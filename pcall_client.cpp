#include "netpc/pcall_client.h"

std::shared_ptr<PC_Client::Server> PC_Client::Connect(const NetAddress_t& addr, u16 ms_timeout) {
    PROFILE_FUNCTION;
    server = std::make_shared<Server>();
    server->addr = addr;
    server->client = this;
    server->authenticated = false;
    NetPC::Packet packet;
    bool uses_protocol = false;
    u8 msg_type = PK_CLT_AUTH_REQ;
    packet << uses_protocol;
    packet << msg_type;
    server->Send(packet);
    
    PC_Event ev;
    Service(ev, ms_timeout);
    
    if (ev.type != PC_EV_TYPE_CONNECT) {
        return server = nullptr;
    }
    
    return server;
}

void PC_Client::ReceivePackets() {
    PROFILE_FUNCTION;
    NetAddress_t addr;
    NetPC::Packet packet;
    if (int rval = socket.ReceiveFrom(addr, packet)) {
        if (rval < 0) { // internal error occurred
            printf("NETPC error: UDPSocket::ReceiveFrom returned error val\n");
            // not sure what to do here...
            if (server) {
                AddDefaultEvent(PC_EV_TYPE_DISCONNECT);
                server.reset();
            }
            return;
        }
        if (!server || addr != server->addr) {
            // ignore everything not from our server
            return;
        }
        if (!packet.GetDataSize()) {
            return;
        }
        bool uses_protocol;
        packet >> uses_protocol;
        if (!uses_protocol) {
            u8 msg_type;
            packet >> msg_type;
            if (!server->authenticated && msg_type != PK_SRV_AUTH_ACK) {
                return; // don't accept any packets while we're not authenticated
            }
            switch (msg_type) {
            case PK_SRV_AUTH_ACK: {
                u8 resp_code;
                packet >> resp_code;
                if (resp_code == PK_RESP_OK) {
                    server->authenticated = true;
                    server->time_since_last_ping_packet = GetCurrentTimeMs();
                    AddDefaultEvent(PC_EV_TYPE_CONNECT);
                }
                break;
            }
            case PK_SRV_PING_ACK: {
                auto [ping_id, ack_list_save] = server->HandlePingPacket(packet);
                server->cur_ack_list_save = ack_list_save;
                auto it = server->outgoing_pings.find(ping_id);
                if (it == server->outgoing_pings.end()) {
                    printf("NETPC debug: Incoming ping ack was not found in outgoing ping list! (id: %u)\n", ping_id);
                    break;
                }
                u64 round_trip_ms = DTime(it->second.ping_send_time_ms);
                printf("NETPC debug: Current round trip time: %llu ms, ping: %f ms\n", round_trip_ms, static_cast<float>(round_trip_ms) / 2);
                server->outgoing_pings.erase(it);
                server->time_since_last_ping_packet = GetCurrentTimeMs();
                break;
            }
            case PK_SRV_PACKET_ACK: {
                server->HandleFragmentAcknowledgement(packet);
                break;
            }
            case PK_SRV_EJECT: {
                // ...we are disconnected...
                break;
            }
            }
            return;
        }
        if (!server->authenticated) {
            return;
        }
        server->HandlePacketFragment(packet);
        if (server->NumPacketsReady()) {
            auto ready_packet = server->PopNextReadyPacket();
            auto& packet = ready_packet.packet;
            u8 msg_type;
            packet >> msg_type;
            switch (msg_type) {
            case PK_SRV_DATA: {
                NetPC::Packet pk;
                pk.Append(reinterpret_cast<const u8*>(packet.GetData()) + 1, packet.GetDataSize() - 1);
                AddDataPacketEvent(pk, server, ready_packet.flags);
                break;
            }
            case PK_SRV_DISCONNECT_ACK: {
                
                break;
            }
            }
        }
    }
}

void PC_Client::SendProtocolPackets() {
    PROFILE_FUNCTION;
    if (!server || !server->authenticated) {
        return;
    }
    server->ServiceOne();
}

void PC_Client::CheckPeerTimeouts() {
    if (!server || !server->authenticated) {
        return;
    }
    if (DTime(server->time_since_last_ping_packet) > max_ping_timeout_ms) {
        auto& ev = AddDefaultEvent(PC_EV_TYPE_DISCONNECT);
        ev.peer = server;
        server.reset();
        printf("NETPC debug: Server timed out\n");
    }
}