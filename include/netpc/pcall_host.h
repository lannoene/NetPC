#pragma once

#include "udp_socket.hpp"

#include <queue>
#include <memory>
#include <list>
#include <set>
#include <mutex>
#include <algorithm>
#include <atomic>

#ifndef __3DS__
using PC_Mutex = std::mutex;
#else
#include "../light_mutex.hpp"
using PC_Mutex = LightMutex;
#endif

constexpr static inline size_t max_fragment_size = 1600;
constexpr static inline size_t packet_header_size = sizeof(bool) + sizeof(u32) + sizeof(u32) + sizeof(u32) + sizeof(u32) + sizeof(u32);
constexpr static inline size_t reliable_packet_retry_ms = 200;
constexpr static inline u64    max_unreliable_cache_pk_timeout_ms = 3000;
constexpr static inline u64    ping_interval_ms = 5000;
constexpr static inline u64    max_reliable_packet_retry_begin_ms = 400;
constexpr static inline u64    max_reliable_packet_retry_interval_ms = 60;
constexpr static inline u64    max_unreliable_sequenced_packet_wait_ms = 120;
constexpr static inline u64    max_ping_cache_wait_ms = 30000;
constexpr static inline u64    max_ping_timeout_ms = 60000;
constexpr static inline u32    default_sndbuf_size = 0x40000;
constexpr static inline u32    default_recvbuf_size = 0x40000;

enum _pc_type {
    PC_EV_TYPE_NONE = 0,
    PC_EV_TYPE_CONNECT,
    PC_EV_TYPE_DISCONNECT,
    PC_EV_TYPE_RECEIVE,
};

enum _pc_send_flags {
    PC_SEND_FLAG_RELIABLE = 1, // resends the packet over and over until it gets acked
    PC_SEND_FLAG_SEQUENCED = 1 << 1, // waits for ack before sending next packet
    PC_SEND_FLAG_NO_FRAGMENT = 1 << 2, // errors if fragment is above max_fragment_size,
                                       // removes much of the processing done for fragments
    //PC_SEND_FLAG_NONBLOCKING = 1 << 3, // does all of this without blocking on send
};

class PC_Host;
class PC_Client;

struct BasePeer {
    void SendPacketType(u8 msg_type, const NetPC::Packet& p, u32 flags = 0) {
        NetPC::Packet packet;
        packet << msg_type;
        packet.Append(p.GetData(), p.GetDataSize());
        SendProtocolRaw(packet, flags);
    }
    void SendProtocolRaw(const NetPC::Packet& p, u32 flags = 0);
    struct ReadyPacket {
        NetPC::Packet packet;
        u32 flags;
    };
    ReadyPacket PopNextReadyPacket() {
        if (!received_ready_packets.size()) {
            printf("NETPC internal error: Attempted to pop ready packet from empty queue");
            abort();
        }
        auto ready_packet = received_ready_packets.front();
        received_ready_packets.pop_front();
        return ready_packet;
    }
    size_t NumPacketsReady() {
        return received_ready_packets.size();
    }
    virtual ~BasePeer() = default;
private:
    // sends 1 raw UDP packet
    virtual bool Send(const NetPC::Packet& p) = 0;
    virtual void ServiceOne();
    void HandlePacketFragment(NetPC::Packet& packet);
    virtual void SendPacketAcknowledgement(u32 seq_num, u32 fragment_index) = 0;
    void InvalidateUnreliableFragmentCachePackets();
    void ResendReliablePacketFragments();
    //void AddUnacknowledgedFragmentsToPacket(NetPC::Packet& packet, std::map<u32, PacketFragmentStore>& packet_frag_store) {
        
    //}
    void SendPingPacket(u8 msg_type, u32 ping_id, std::vector<u32> ack_list_save) {
        NetPC::Packet resp;
        bool uses_protocol = false;
        resp << uses_protocol;
        resp << msg_type;
        resp << ping_id;
        // add fully received and acknowledged seq packets
        u32 num_packets = reliable_packet_recv_ready_seq_cache.size();
        resp << num_packets;
        for (auto& [seq_num, is_finished] : reliable_packet_recv_ready_seq_cache) {
            if (!is_finished) { // this shouldn't be here
                printf("NETPC internal error: bad data in reliable_packet_recv_ready_seq_cache");
                abort();
            }
            resp << seq_num;
        }
        // tell it to remove these packets from its packet blocker
        u32 ack_list_save_len = ack_list_save.size();
        resp << ack_list_save_len;
        for (auto seq_id : ack_list_save) {
            resp << seq_id;
        }
        Send(resp);
    }
    std::tuple<u32, std::vector<u32>> HandlePingPacket(NetPC::Packet& packet) {
        u32 ping_id;
        u32 cl_num_packets;
        packet >> ping_id;
        // decode fully received and acknowledged seq packets
        std::vector<u32> ack_list_save;
        packet >> cl_num_packets;
        for (u32 i = 0; i < cl_num_packets; i++) {
            u32 seq_id;
            packet >> seq_id;
            ack_list_save.push_back(seq_id);
            std::scoped_lock lock(reliable_packet_send_store_mut);
            if (auto it = reliable_packet_send_store.find(seq_id); it != reliable_packet_send_store.end()) {
                reliable_packet_send_store.erase(it);
            } else {
                printf("NETPC debug: Packet already popped %d\n", seq_id);
            }
        }
        // decode the acked list of our acknowledged packets
        u32 cl_num_await_ack;
        packet >> cl_num_await_ack;
        for (u32 i = 0; i < cl_num_await_ack; i++) {
            u32 seq_id;
            packet >> seq_id;
            if (auto it = reliable_packet_recv_ready_seq_cache.find(seq_id); it != reliable_packet_recv_ready_seq_cache.end()) {
                reliable_packet_recv_ready_seq_cache.erase(it);
            } else {
                printf("NETPC debug: already known that peer has received ack for packet %d\n", seq_id);
            }
        }
        return std::make_tuple(ping_id, ack_list_save);
    }
    void HandleFragmentAcknowledgement(NetPC::Packet& packet) {
        u32 seq_num;
        u32 fragment_index;
        packet >> seq_num;
        packet >> fragment_index;
        std::scoped_lock lock(reliable_packet_send_store_mut);
        auto it = reliable_packet_send_store.find(seq_num);
        if (it == reliable_packet_send_store.end()) {
            printf("NETPC debug: could not locate seq num %d in reliable_packet_send_store for frag ack\n", seq_num);
            return;
        }
        if (fragment_index >= it->second.frags.size()) {
            printf("NETPC error: fragment index of %u was out of bounds\n", fragment_index);
            return;
        }
        if (auto frag_it = std::find_if(it->second.frags.begin(), it->second.frags.end(), [fragment_index](auto f) {
            return fragment_index == f.frag_index;
        }); frag_it != it->second.frags.end()) {
            frag_it->acknowledged = true;
        } else {
            printf("NETPC error: couldn't locate fragment index of %u in list of received fragments\n", fragment_index);
        }
        u32 num_acknowledged = 0;
        for (auto& frag : it->second.frags) {
            if (frag.acknowledged) {
                num_acknowledged++;
            }
        }
        if (num_acknowledged == it->second.num_fragments_total) {
            reliable_packet_send_store.erase(seq_num);
        }
    }
    friend class PC_Host;
    friend class PC_Client;
    u32 next_seq_num = 1;
    std::list<ReadyPacket> received_ready_packets;
    struct PacketFragmentStore {
        struct PacketFragment {
            NetPC::Packet packet;
            mutable bool acknowledged;
            u32 frag_index;
            bool operator<(const PacketFragment& o) const {
                return frag_index < o.frag_index;
            }
        };
        std::set<PacketFragment> frags;
        u64 timestamp; // timestamp of last send or recv
        u32 num_fragments_total;
        u64 last_reliable_resend_timestamp = 0; // timestamp of last reliable resend
    };
    std::map<u32, PacketFragmentStore> reliable_packet_recv_store;
    std::map<u32, PacketFragmentStore> reliable_packet_send_store;
    PC_Mutex reliable_packet_send_store_mut; // protects reliable_packet_send_store
    std::map<u32, PacketFragmentStore> unreliable_packet_recv_store;
    // stores information about received reliable packets
    // until the next successful ping, where we can clear
    // this
    std::map<u32, bool> reliable_packet_recv_ready_seq_cache;
    std::atomic<u32> reliable_sequenced_packet_send_seq{}; // protocol starts at 1 and it keeps track of only sequenced packets
    u32 reliable_sequenced_packet_recv_seq{}; // keeps track of the last sequenced packet id we've added to the ready queue
    struct SequencePacketStore {
        NetPC::Packet packet;
        u32 sequence_id;
        u64 timestamp;
        u32 flags;
        bool operator<(const SequencePacketStore& o) const {
            return sequence_id < o.sequence_id;
        }
    };
    void AddValidAwaitingSequencedPacketsToReady(std::set<SequencePacketStore>& seq_packet_store, u32& sequenced_packet_recv_seq);
    void AddValidAwaitingUnreliableSequencedPacketsToReady();
    std::set<SequencePacketStore> reliable_seq_packet_store; // stores sequenced packets that we received out of order for later
    std::atomic<u32> unreliable_sequenced_packet_send_seq{};
    u32 unreliable_sequenced_packet_recv_seq{};
    std::set<SequencePacketStore> unreliable_seq_packet_store; // stores sequenecd packets like the reliable version,
                                                               // but this version contains a timeout.
    u64 time_since_last_ping_packet; // time since last ping send or ack
};

struct PC_Event {
    enum _pc_type type;
    NetPC::Packet pk;
    u32 pk_flags;
    std::shared_ptr<BasePeer> peer;
};

class PC_Base {
public:
    bool Service(PC_Event& ev, s64 timeout);
    static u64 GetCurrentTimeMs();
    static u64 DTime(u64 start_time) {return GetCurrentTimeMs() - start_time;}
protected:
    PC_Base();
    UDPSocket socket;
    std::list<NetPC::Packet> reliable_packets_receive_queue;
    std::queue<PC_Event> event_queue;
    
    virtual void ReceivePackets() = 0;
    virtual void SendProtocolPackets() = 0;
    virtual void CheckPeerTimeouts() = 0;
    bool ProtocolValidatePacket(const NetPC::Packet& packet);
    
    PC_Event& AddDefaultEvent(enum _pc_type ev_type) {
        auto& ev = event_queue.emplace();
        ev.type = ev_type;
        return ev;
    }
    void AddDataPacketEvent(NetPC::Packet& p, std::shared_ptr<BasePeer> peer, u32 packet_flags) {
        auto& ev = AddDefaultEvent(PC_EV_TYPE_RECEIVE);
        ev.pk = p;
        ev.peer = peer;
        ev.pk_flags = packet_flags;
    }
};

enum PK_PC_SRV_TYPE {
    PK_SRV_AUTH_ACK = 0,
    PK_SRV_AUTH_LEAVE_ACK = 1,
    PK_SRV_DATA,
    PK_SRV_PING_ACK,
    PK_SRV_PACKET_ACK,
    PK_SRV_RELIABLE_RECV_LIST, // TODO
    PK_SRV_DISCONNECT_ACK,
    PK_SRV_EJECT,
};

enum PK_PC_CLT_TYPE {
    PK_CLT_AUTH_REQ = 0,
    PK_CLT_AUTH_LEAVE = 1,
    PK_CLT_DATA,
    PK_CLT_PING,
    PK_CLT_PACKET_ACK,
    PK_CLT_RELIABLE_RECV_LIST,
    PK_CLT_DISCONNECT,
};

enum PK_RESP_ERROR_CODE {
    PK_RESP_OK = 0,
    PK_RESP_UNKNOWN_ERROR = 1,
};

class PC_Host : public PC_Base {
public:
    bool Init(u16 port);
    struct Client : public BasePeer {
        NetAddress_t addr;
        PC_Host *host;
        virtual void SendPacket(const NetPC::Packet& p, u32 flags = 0) {
            SendPacketType(PK_SRV_DATA, p, flags);
        }
    private:
        virtual bool Send(const NetPC::Packet& p) {
            return host->socket.SendTo(p, &addr);
        }
        virtual void SendPacketAcknowledgement(u32 seq_num, u32 fragment_index) {
            NetPC::Packet packet_ack;
            bool use_protocol = false;
            u8 msg_type = PK_SRV_PACKET_ACK;
            packet_ack << use_protocol;
            packet_ack << msg_type;
            packet_ack << seq_num;
            packet_ack << fragment_index;
            Send(packet_ack);
        }
        friend class PC_Host;
        friend class PC_Client;
    };
    void BroadcastPacket(NetPC::Packet& pk, u32 send_flags, std::shared_ptr<Client> exclude = nullptr);
private:
    std::list<std::shared_ptr<Client>> clients;
    
    std::shared_ptr<Client> GetAddrClient(NetAddress_t& addr) {
        for (auto c : clients) {
            if (addr == c->addr) {
                return c;
            }
        }
        return nullptr;
    }
    
    virtual void ReceivePackets();
    virtual void SendProtocolPackets();
    virtual void CheckPeerTimeouts();
    
    // packet handlers
    bool HandleClientAuthRequestPacket(NetAddress_t& addr, NetPC::Packet& packet);
};