#pragma once

#include <map>

#include "pcall_host.h"

class PC_Client : public PC_Base {
public:
    struct Server : public BasePeer {
        NetAddress_t addr;
        virtual void SendPacket(const Network::Packet& p, u32 flags = 0) {
            SendPacketType(PK_CLT_DATA, p, flags);
        }
        PC_Client *client;
        bool authenticated;
    private:
        virtual bool Send(const Network::Packet& p) {
            return client->socket.SendTo(p, &addr);
        }
        virtual void SendPacketAcknowledgement(u32 seq_num, u32 fragment_index) {
            Network::Packet packet_ack;
            bool use_protocol = false;
            u8 msg_type = PK_CLT_PACKET_ACK;
            packet_ack << use_protocol;
            packet_ack << msg_type;
            packet_ack << seq_num;
            packet_ack << fragment_index;
            Send(packet_ack);
        }
        virtual void ServiceOne() {
            BasePeer::ServiceOne();
            { // remove old pings
                std::vector<u32> invalidate_pings;
                for (auto& [ping_id, ping] : outgoing_pings) {
                    if (PC_Base::DTime(ping.ping_send_time_ms) > max_ping_cache_wait_ms) {
                        invalidate_pings.push_back(ping_id);
                    }
                }
                for (auto ping_id : invalidate_pings) {
                    outgoing_pings.erase(ping_id);
                }
            }
            if (PC_Base::DTime(last_ping_time_ms) > ping_interval_ms && authenticated) {
                u64 cur_time_ms = PC_Base::GetCurrentTimeMs();
                auto cur_ping_id = next_ping_id++;
                SendPingPacket(PK_CLT_PING, cur_ping_id, cur_ack_list_save);
                auto& outgoing_ping = outgoing_pings[cur_ping_id];
                outgoing_ping.ping_send_time_ms = cur_time_ms;
                last_ping_time_ms = cur_time_ms;
            }
        }
        u64 last_ping_time_ms = 0;
        friend class PC_Host;
        friend class PC_Client;
        struct OutgoingPing {
            u64 ping_send_time_ms;
        };
        std::map<u32, OutgoingPing> outgoing_pings;
        // this saves the total ack list
        std::vector<u32> cur_ack_list_save;
        u32 next_ping_id = 0;
    };
    std::shared_ptr<Server> Connect(const NetAddress_t& addr, u16 ms_timeout);
    void Disconnect(std::shared_ptr<Server> server);
private:
    
    virtual void ReceivePackets();
    virtual void SendProtocolPackets();
    
    std::shared_ptr<Server> server;
};