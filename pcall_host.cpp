#include "pcall_host.h"

#include <sys/time.h>

void BasePeer::ServiceOne() {
    InvalidateUnreliableFragmentCachePackets();
    ResendReliablePacketFragments();
}

void BasePeer::InvalidateUnreliableFragmentCachePackets() {
    std::vector<u32> pk_seq_cache_invalid;
    for (auto& [seq_num, fragment_store] : unreliable_packet_recv_store) {
        if (PC_Base::DTime(fragment_store.timestamp) > max_unreliable_cache_pk_timeout_ms) {
            pk_seq_cache_invalid.push_back(seq_num);
        }
    }
    for (auto seq_num : pk_seq_cache_invalid) {
        unreliable_packet_recv_store.erase(seq_num);
    }
}

void BasePeer::ResendReliablePacketFragments() {
    std::scoped_lock lock(reliable_packet_send_store_mut);
    for (auto& [seq_num, fragment_store] : reliable_packet_send_store) {
        if (PC_Base::DTime(fragment_store.timestamp) > max_reliable_packet_retry_begin_ms &&
            PC_Base::DTime(fragment_store.last_reliable_resend_timestamp) > max_reliable_packet_retry_interval_ms) {
            
            // resend the raw unacknowledged fragments
            for (auto& frag : fragment_store.frags) {
                if (!frag.acknowledged) {
                    Send(frag.packet);
                }
            }
            fragment_store.last_reliable_resend_timestamp = PC_Base::GetCurrentTimeMs();
        }
    }
}

void BasePeer::HandlePacketFragment(Network::Packet& packet) {
    u32 seq_num;
    u32 flags;
    u32 fragment_index;
    u32 total_fragments;
    u32 sequenced_packet_id;
    packet >> seq_num;
    packet >> flags;
    packet >> fragment_index;
    packet >> total_fragments;
    packet >> sequenced_packet_id;
    /*printf("Handling packet fragment seq num: %u, "
           "flags: %u, fragment index: %d, total fragments: "
           "%d, sequenced %s packet id: %u\n",
           seq_num, flags, fragment_index, total_fragments,
           ((flags & PC_SEND_FLAG_RELIABLE) ? "reliable" : "unreliable"),
           sequenced_packet_id);*/
    
    Network::Packet packet_body;
    packet_body.Append(reinterpret_cast<const u8*>(packet.GetData()) + packet_header_size, packet.GetDataSize() - packet_header_size);
    
    if (flags & PC_SEND_FLAG_NO_FRAGMENT) {
        received_ready_packets.emplace_back(packet_body, flags);
    } else { // we have a fragmented packet
        if (flags & PC_SEND_FLAG_RELIABLE) {
            [&]() {
                auto recv_cache_it = reliable_packet_recv_ready_seq_cache.find(seq_num);
                if (recv_cache_it != reliable_packet_recv_ready_seq_cache.end()) {
                    return; // block reliable packets that have already been received
                }
                auto& fragment_store = reliable_packet_recv_store[seq_num];
                if (fragment_store.num_fragments_total == 0) {
                    fragment_store.num_fragments_total = total_fragments;
                } else if (fragment_store.num_fragments_total != total_fragments) {
                    printf("NETPC error: fragment total mismatch from last fragment for this packet: %u != %u\n", fragment_store.num_fragments_total, total_fragments);
                }
                // make sure we haven't already received this fragment
                // this can happen if this is a reliable packet and we
                // have high ping
                auto it = std::find_if(fragment_store.frags.begin(), fragment_store.frags.end(), [fragment_index](auto frag) {
                    return frag.frag_index == fragment_index;
                });
                if (it != fragment_store.frags.end()) {
                    // we already received this packet
                    return;
                }
                fragment_store.frags.emplace(packet, true, fragment_index);
                // check if we have received all the fragments
                if (fragment_store.frags.size() != total_fragments) {
                    return;
                }
                // recover full packet
                Network::Packet full_packet;
                for (auto& frag : fragment_store.frags) {
                    full_packet.Append(reinterpret_cast<const u8*>(frag.packet.GetData()) + packet_header_size, frag.packet.GetDataSize() - packet_header_size);
                }
                if (flags & PC_SEND_FLAG_SEQUENCED) {
                    reliable_seq_packet_store.emplace(full_packet, sequenced_packet_id, PC_Host::GetCurrentTimeMs(), flags);
                    AddValidAwaitingSequencedPacketsToReady(reliable_seq_packet_store, reliable_sequenced_packet_recv_seq);
                } else {
                    received_ready_packets.emplace_back(full_packet, flags);
                }
                reliable_packet_recv_ready_seq_cache.insert({seq_num, true});
                reliable_packet_recv_store.erase(seq_num);
            }();
        } else {
            auto& fragment_store = unreliable_packet_recv_store[seq_num];
            fragment_store.frags.emplace(packet, false, fragment_index);
            fragment_store.timestamp = PC_Base::GetCurrentTimeMs();
            // check if we have received all the fragments
            if (fragment_store.frags.size() == total_fragments) {
                // recover full packet
                Network::Packet full_packet;
                for (auto& frag : fragment_store.frags) {
                    full_packet.Append(reinterpret_cast<const u8*>(frag.packet.GetData()) + packet_header_size, frag.packet.GetDataSize() - packet_header_size);
                }
                if (flags & PC_SEND_FLAG_SEQUENCED) {
                    unreliable_seq_packet_store.emplace(full_packet, sequenced_packet_id, PC_Host::GetCurrentTimeMs(), flags);
                    AddValidAwaitingUnreliableSequencedPacketsToReady();
                } else {
                    received_ready_packets.emplace_back(full_packet, flags);
                }
                unreliable_packet_recv_store.erase(seq_num);
            }
        }
    }
    
    if (flags & PC_SEND_FLAG_RELIABLE) {
        // send packet acknowledgement
        SendPacketAcknowledgement(seq_num, fragment_index);
    }
}

void BasePeer::SendProtocolRaw(const Network::Packet& p, u32 flags) {
    u32 sequenced_packet_id = 0;
    if (flags & PC_SEND_FLAG_SEQUENCED) {
        if (flags & PC_SEND_FLAG_RELIABLE) {
            sequenced_packet_id = reliable_sequenced_packet_send_seq++;
        } else {
            sequenced_packet_id = unreliable_sequenced_packet_send_seq++;
        }
    }
    auto send_one_fragment = [&](u32 pk_seq_id, u32 fragment_index, u32 total_fragments) {
        // add header
        bool use_protocol = true;
        Network::Packet packet;
        packet << use_protocol;
        packet << pk_seq_id;
        packet << flags;
        packet << fragment_index;
        packet << total_fragments;
        packet << sequenced_packet_id;
        if (packet.GetDataSize() != packet_header_size) {
            printf("NETPC internal error: Packet header size %zu did not match the expected value of %zu\n", packet.GetDataSize(), packet_header_size);
            abort(); // we assert here because this is a programmer error
        }
        // append body
        auto pk_off = max_fragment_size * fragment_index;
        packet.Append(reinterpret_cast<const u8*>(p.GetData()) + pk_off, std::min<u32>(p.GetDataSize() - pk_off, max_fragment_size));
        Send(packet);
        if (flags & PC_SEND_FLAG_RELIABLE) {
            std::scoped_lock lock(reliable_packet_send_store_mut);
            auto& fragment_store = reliable_packet_send_store[pk_seq_id];
            fragment_store.frags.emplace(packet, false, fragment_index);
            fragment_store.timestamp = PC_Base::GetCurrentTimeMs();
        }
    };
    auto align_up = [](auto num, auto align) {
        auto mod = num % align;
        if (mod == 0) {
            return num;
        } else {
            return num + (align - mod);
        }
    };
    size_t num_fragments = align_up(p.GetDataSize(), max_fragment_size) / max_fragment_size;
    auto seq_id = next_seq_num++;
    for (size_t i = 0; i < num_fragments; i++) {
        send_one_fragment(seq_id, i, num_fragments);
    }
}

void BasePeer::AddValidAwaitingSequencedPacketsToReady(std::set<SequencePacketStore>& seq_packet_store, u32& sequenced_packet_recv_seq) {
    u32 num_valid_packets = 0;
    u32 cur_id_seq = sequenced_packet_recv_seq;
    for (auto& seq_packet : seq_packet_store) {
        if (seq_packet.sequence_id != cur_id_seq) {
            break;
        }
        received_ready_packets.emplace_back(seq_packet.packet, seq_packet.flags);
        num_valid_packets++;
        cur_id_seq++;
    }
    sequenced_packet_recv_seq += num_valid_packets;
    // disgusting.
    auto range_end = seq_packet_store.begin();
    std::advance(range_end, num_valid_packets);
    seq_packet_store.erase(seq_packet_store.begin(), range_end);
}

void BasePeer::AddValidAwaitingUnreliableSequencedPacketsToReady() {
    // go through unreliable packet receive queue and remove any packets that are too old
    u32 num_too_old = 0;
    for (auto& seq_packet : unreliable_seq_packet_store) {
        if (PC_Base::DTime(seq_packet.timestamp) < max_unreliable_sequenced_packet_wait_ms) {
            break;
        }
        num_too_old++;
        // set this to skip over the dropped packets
        unreliable_sequenced_packet_recv_seq = seq_packet.sequence_id + 1;
    }
    if (num_too_old) {
        printf("Dropped %d unreliable sequenced packets that were too old\n", num_too_old);
        auto range_end = unreliable_seq_packet_store.begin();
        std::advance(range_end, num_too_old);
        unreliable_seq_packet_store.erase(unreliable_seq_packet_store.begin(), range_end);
    }
    AddValidAwaitingSequencedPacketsToReady(unreliable_seq_packet_store, unreliable_sequenced_packet_recv_seq);
}

bool PC_Base::Service(PC_Event& ev, s64 timeout) {
    PROFILE_FUNCTION;
    ev.type = PC_EV_TYPE_NONE;
    ev.peer = nullptr;
    ev.pk = std::move(Network::Packet{});
    s64 time_start = GetCurrentTimeMs();
    bool can_recv = socket.Poll(POLL_WAIT_RECEIVE, 0) & POLL_WAIT_RECEIVE;
    do {
        if (can_recv) {
            ReceivePackets();
        }
        SendProtocolPackets();
        if (event_queue.size()) {
            auto _ev = event_queue.front();
            event_queue.pop();
            ev = _ev;
            return true;
        }
        auto dtime = static_cast<s64>(GetCurrentTimeMs()) - time_start;
        if (dtime < timeout) {
            can_recv = socket.Poll(POLL_WAIT_RECEIVE, timeout - dtime) & POLL_WAIT_RECEIVE;
            if (static_cast<s64>(GetCurrentTimeMs()) - time_start >= timeout) {
                break;
            }
        } else {
            break;
        }
    } while (true);
    return false;
}

u64 PC_Base::GetCurrentTimeMs() {
    PROFILE_FUNCTION;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool PC_Base::ProtocolValidatePacket(const Network::Packet& packet) {
    return packet.GetDataSize() >= packet_header_size;
}

void PC_Host::ReceivePackets() {
    PROFILE_FUNCTION;
    NetAddress_t addr;
    Network::Packet packet;
    if (int rval = socket.ReceiveFrom(addr, packet)) {
        if (rval < 0) { // internal error occurred
            printf("NETPC error: UDPSocket::ReceiveFrom returned error val");
            // we have to stop hosting a server
            // TODO: add return int
            // return -1;
            return;
        }
        bool uses_protocol;
        packet >> uses_protocol;
        if (uses_protocol && !ProtocolValidatePacket(packet)) {
            return;
        }
        auto client = GetAddrClient(addr);
        if (!client) {
            // client is not authenticated yet
            if (uses_protocol) {
                // reject all protocol packets because
                // we haven't established a pipe to the client
                // yet
                return;
            }
            u8 msg_type;
            packet >> msg_type;
            if (msg_type != PK_CLT_AUTH_REQ) {
                return;
            }
            if (HandleClientAuthRequestPacket(addr, packet)) {
                AddDefaultEvent(PC_EV_TYPE_CONNECT);
            }
            return;
        }
        if (!uses_protocol) {
            // handle all non-protocol packets here
            // these are simpily UDP packets, where there's
            // no guarentee of reliability
            u8 msg_type;
            packet >> msg_type;
            switch (msg_type) {
            case PK_CLT_PING: {
                auto [ping_id, ack_list_save] = client->HandlePingPacket(packet);
                client->SendPingPacket(PK_SRV_PING_ACK, ping_id, ack_list_save);
                break;
            }
            case PK_CLT_PACKET_ACK: {
                client->HandleFragmentAcknowledgement(packet);
                break;
            }
            }
            return;
        }
        // all other packets are expected to
        // follow protocol
        client->HandlePacketFragment(packet);
        if (client->NumPacketsReady()) {
            auto ready_packet = client->PopNextReadyPacket();
            auto& packet = ready_packet.packet;
            u8 msg_type;
            packet >> msg_type;
            switch (msg_type) {
            case PK_CLT_DATA: {
                Network::Packet pk;
                pk.Append(reinterpret_cast<const u8*>(packet.GetData()) + 1, packet.GetDataSize() - 1);
                AddDataPacketEvent(pk, client, ready_packet.flags);
                break;
            }
            }
        }
    }
}

void PC_Host::SendProtocolPackets() {
    
}

bool PC_Host::Init(u16 port) {
    PROFILE_FUNCTION;
    socket.BindToPort(port);
    return true;
}

bool PC_Host::HandleClientAuthRequestPacket(NetAddress_t& addr, Network::Packet& packet) {
    PROFILE_FUNCTION;
    auto client = GetAddrClient(addr);
    if (client) { // client is already connected
        return false;
    }
    // add client to array
    auto new_clt = std::make_shared<Client>();
    new_clt->addr = addr;
    new_clt->host = this;
    clients.push_back(new_clt);
    
    Network::Packet pk_resp;
    bool use_protocol = false;
    u8 msg_type = PK_SRV_AUTH_ACK;
    u8 resp_code = PK_RESP_OK;
    pk_resp << use_protocol;
    pk_resp << msg_type;
    pk_resp << resp_code;
    new_clt->Send(pk_resp);
    return true;
}

void PC_Host::BroadcastPacket(Network::Packet& pk, u32 send_flags, std::shared_ptr<Client> exclude) {
    PROFILE_FUNCTION;
    for (auto cl : clients) {
        if (cl == exclude) {
            continue;
        }
        cl->SendPacket(pk, send_flags);
    }
}