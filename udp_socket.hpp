#pragma once

#include "socket_base.hpp"
#ifdef __3DS__
#include "../light_mutex.hpp"
#endif
#include "packet.h"

class UDPSocket;

//------------------------------------------
// socket container
//------------------------------------------
class UDPSocket : public BaseSocket {
public:
	UDPSocket();
	~UDPSocket();
	void Init(uint16_t port);
	int ReceiveFrom(NetAddress_t &packet_from, NetPC::Packet& buf);
	bool SendTo(const NetPC::Packet&, NetAddress_t *recipient);
private:
    char *recv_buffer;
#ifdef __3DS__
    // input: ip
    // output: mac address
    LightMutex arp_map_mut;
    std::map<u32, std::array<u8, 6>> arp_mac_addr_map;
    u32 my_ip_addr = 0;
    u16 my_port_num = 0;
    struct in_addr netmask;
    size_t num_arp_entries_last = 128;
    size_t num_routing_entries_last = 32;
    std::atomic<u16> next_packet_id;
    bool last_send_errored = false;
#endif
};