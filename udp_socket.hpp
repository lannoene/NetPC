#pragma once

#include "socket_base.hpp"
#ifdef __3DS__
#include "../azahar_network/packet.h"
#else
#include "packet.h"
#endif

class UDPSocket;

//------------------------------------------
// socket container
//------------------------------------------
class UDPSocket : public BaseSocket {
public:
	UDPSocket();
	~UDPSocket();
	void Init(uint16_t port);
	int ReceiveFrom(NetAddress_t &packet_from, Network::Packet& buf);
	bool SendTo(const Network::Packet&, NetAddress_t *recipient);
private:
    char *recv_buffer;
};