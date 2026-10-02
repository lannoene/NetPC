#include "udp_socket.hpp"

#define MAX_MSG_BUF_SIZE 2000
#ifndef _WIN32
#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

UDPSocket::UDPSocket() {
    PROFILE_FUNCTION;
	s_type = "UDP";
	if ((_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
		printf("udp socket() failed with error code: %d\n", GetLastError());
	}
    recv_buffer = new char[MAX_MSG_BUF_SIZE];
}

UDPSocket::~UDPSocket() {
    PROFILE_FUNCTION;
#if defined(_WIN32) || defined(__3DS__)
	closesocket(_socket);
#else
    close(_socket);
#endif
    delete[] recv_buffer;
}

void UDPSocket::Init(uint16_t port) {
	
}

int UDPSocket::ReceiveFrom(NetAddress_t& packet_from, Network::Packet& buf) {
    PROFILE_FUNCTION;
	struct sockaddr_in from;
#ifdef _WIN32
	int nFromLen = sizeof(from);
#else
    socklen_t nFromLen = sizeof(from);
#endif
	int bytesRead = recvfrom(_socket, recv_buffer, MAX_MSG_BUF_SIZE, 0, reinterpret_cast<sockaddr*>(&from), &nFromLen);
	packet_from.SetFromSockadr(&from);
	if (bytesRead == SOCKET_ERROR) {
		int error = GetLastError();
		if (error != WSAEWOULDBLOCK) {
            printf("Receive from error: %s\n", strerror(error));
			if (errCallback[error] != nullptr) {
				errCallback[error](packet_from);
			}
            return -1;
		}
        return 0;
	}
	buf.Append(recv_buffer, bytesRead);
	//char ipAsString[50];
	//packet_from.GetIpAsString(ipAsString, sizeof(ipAsString));
	//std::cout << "received " << bytesRead << " bytes from " << ipAsString << std::endl;
	//if (buf.GetByte() != 4 || buf.GetByte() != 73 || buf.GetByte() != 92 || buf.GetByte() != 224) { // bad head meaning packet is probably not fully transmitted. ignore it.
    //    printf("Threw away msg\n");
	//	return false;
	//}
	//if (int bytesExpected; (bytesExpected = buf.GetInt()) != bytesRead) {
    //    printf("Threw away message. expected %d bytes got %d bytes\n", bytesExpected, bytesRead);
	//	return false;
	//}
	//buf.TruncateReadBytes();
	return bytesRead;
}

bool UDPSocket::SendTo(const Network::Packet& bufn, NetAddress_t *recipient) {
    PROFILE_FUNCTION;
	struct sockaddr_in dest;
	recipient->ToSockadr(&dest);
	//AssertPanicError(buf.GetSeek() <= 65535, "Socket buffer overflow.");
    int ret = sendto(_socket, reinterpret_cast<const char*>(bufn.GetData()), bufn.GetDataSize(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(struct sockaddr_in));
    if (ret >= 0) {
        return static_cast<size_t>(ret) == bufn.GetDataSize();
    }
    return false;
}
