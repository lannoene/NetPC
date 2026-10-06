#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif
#include <map>
#include <functional>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef __3DS__
#include "../debug_log.hpp"
#else
#define PROFILE_FUNCTION
#endif

typedef enum {
	NA_NULL = 0,
	NA_LOOPBACK,
	NA_BROADCAST,
	NA_IP,
} NetAddrType_t;

enum {
    POLL_WAIT_RECEIVE = 1,
    POLL_WAIT_SEND = 1 << 1,
};

class UDPSocket;
class TCPSocket;

//-------------------------------------------
// determine sources of net addreses
//-------------------------------------------
typedef struct NetAddress_s {
	friend UDPSocket;
	friend TCPSocket;
public:
	NetAddress_s() {};
	NetAddress_s(unsigned char ip[4], uint16_t port) {
		memcpy(m_ip, ip, 4);
		m_port = htons(port);
	}
    NetAddress_s(const char *ip, uint16_t port) {
        SetFromAddr(ip, port);
    }
	/*NetAddress_s(NetAddress_s& oAddr) {
		SetType(oAddr.GetType());
		oAddr.GetAddress(m_ip, &m_port);
	}*/
	~NetAddress_s() = default;
	void GetAddress(uint8_t ip[4], uint16_t *port) {
		memcpy(ip, m_ip, 4);
		*port = m_port;
	}
	void SetFromAddr(const char *ip, uint16_t port) {
		sockaddr_in sockInfo;
	
		sockInfo.sin_family = AF_INET;
		sockInfo.sin_port = htons(port);
#ifdef _WIN32
		sockInfo.sin_addr.S_un.S_addr = inet_addr(ip);
#else
        inet_aton(ip, &sockInfo.sin_addr);
#endif
		
		SetFromSockadr(&sockInfo);
	}
	uint16_t GetPort() {return ntohs(m_port);}
	uint8_t *GetIp() {return m_ip;}
	void GetIpAsString(char *inpString, size_t maxChars) {
		snprintf(inpString, maxChars, "%d.%d.%d.%d:%d", m_ip[0], m_ip[1], m_ip[2], m_ip[3], m_port);
	}
	NetAddrType_t GetType() {return m_type;}
	bool operator==(NetAddress_s& addr) {
		return (m_type == addr.GetType() && memcmp(m_ip, addr.GetIp(), 4) == 0 && m_port == addr.m_port);
	}
	void SetType(NetAddrType_t type) {m_type = type;}
private:
	void SetFromSockadr(const struct sockaddr_in *sockAddr) {
		if (sockAddr->sin_family == AF_INET) {
			m_type = NA_IP;
			*(int *)&m_ip = ((struct sockaddr_in *)sockAddr)->sin_addr.s_addr;
			m_port = ((struct sockaddr_in *)sockAddr)->sin_port;
		}
	}
	void ToSockadr(struct sockaddr_in *s) {
		memset(s, 0, sizeof(struct sockaddr_in));

		if (m_type == NA_BROADCAST) {
			((struct sockaddr_in*)s)->sin_family = AF_INET;
			((struct sockaddr_in*)s)->sin_port = m_port;
			((struct sockaddr_in*)s)->sin_addr.s_addr = INADDR_BROADCAST;
		} else if (m_type == NA_IP) {
			((struct sockaddr_in*)s)->sin_family = AF_INET;
			((struct sockaddr_in*)s)->sin_addr.s_addr = *(int *)&m_ip;
			((struct sockaddr_in*)s)->sin_port = m_port;
		} else if (m_type == NA_LOOPBACK) {
			((struct sockaddr_in*)s)->sin_family = AF_INET;
			((struct sockaddr_in*)s)->sin_port = m_port;
			((struct sockaddr_in*)s)->sin_addr.s_addr = INADDR_LOOPBACK;
		}
	}
	NetAddrType_t m_type;
	uint8_t m_ip[4] = {0};
	uint16_t m_port = 0;
} NetAddress_t;

class BaseSocket {
public:
	BaseSocket() = default;
#ifdef _WIN32
	BaseSocket(SOCKET s) : _socket(s) {}
#else
    BaseSocket(int s) : _socket(s) {}
#endif
	void SetErrorCallback(int error, std::function<void(NetAddress_t& addr)> callback) {
		errCallback[error] = callback;
	}
	bool BindToPort(uint16_t port) {
		sockaddr_in m_srv;
		m_srv.sin_family = AF_INET;
		m_srv.sin_port = htons(port);
#ifdef _WIN32
		m_srv.sin_addr.S_un.S_addr = INADDR_ANY;
#else
        m_srv.sin_addr.s_addr = INADDR_ANY;
#endif
		
		if (bind(_socket, (struct sockaddr*)&m_srv, sizeof(m_srv)) == SOCKET_ERROR) {
#ifdef _WIN32
            printf("Couldn't bind socket: %d\n", WSAGetLastError());
#else
            printf("Couldn't bind socket: %s\n", strerror(errno));
#endif
			return false;
		}
		
		return true;
	}
	int GetLastError() {
#ifdef _WIN32
            return WSAGetLastError();
#else
            return errno;
#endif
	}
    int Poll(int events, int timeout) {
        int poll_count;
#ifndef _WIN32
        struct pollfd pollst;
#else
        WSAPOLLFD pollst;
#endif
        
        pollst.fd = _socket;
        pollst.events = 0;

        if (events & POLL_WAIT_RECEIVE) {
            pollst.events |= POLLIN;
        }
        if (events & POLL_WAIT_SEND) {
            pollst.events |= POLLOUT;
        }

#ifndef _WIN32
        poll_count = poll(&pollst, 1, timeout);
#else
        poll_count = WSAPoll(&pollst, 1, timeout);
#endif

        if (poll_count < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                //* condition = ENET_SOCKET_WAIT_INTERRUPT;

                return 0;
            }
#else
            // ... check this?
            printf("WSAPoll returned SOCKET_ERROR %d\n", WSAGetLastError());
#endif

            return -1;
        }

        //* condition = ENET_SOCKET_WAIT_NONE;

        if (poll_count == 0)
            return 0;
        
        int out = 0;

        if (pollst.revents & POLLOUT) {
            out |= POLL_WAIT_SEND;
        }
        if (pollst.revents & POLLIN) {
            out |= POLL_WAIT_RECEIVE;
        }

        return out;
    }
    static void InitNetworking() {
#ifdef _WIN32
        static WSAData wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            puts("could not init wsadata");
        }
        timeBeginPeriod(1);
#endif
    }
    static void DeinitNetworking() {
#ifdef _WIN32
        WSACleanup();
        timeEndPeriod(1);
#endif
    }
    enum class BlockingType {
        BLOCKING,
        NONBLOCKING
    };
    void SetBlockingType(BlockingType bl) {
        if (bl == BlockingType::BLOCKING) {
#ifdef _WIN32
            unsigned long i_true = true;
            if (ioctlsocket(_socket, FIONBIO, &i_true) == SOCKET_ERROR) {
                printf("Could not set nonblocking sockets");
            }
#else
            if (fcntl(_socket, F_SETFL, O_NONBLOCK) != 0) {
                printf("Could not set nonblocking sockets.\n");
            }
#endif
        } else {
#ifdef _WIN32
            unsigned long i_false = false;
            if (ioctlsocket(_socket, FIONBIO, &i_false) == SOCKET_ERROR) {
                printf("Could not set blocking sockets\n");
            }
#else
            const int flags = fcntl(_socket, F_GETFL, 0);
            if (fcntl(_socket, F_SETFL, flags & (~O_NONBLOCK)) != 0) {
                printf("Could not set blocking sockets.\n");
            }
#endif
        }
    }
    bool SetRecvBufferSize(int value) {
        return setsockopt(_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&value), sizeof(int)) >= 0;
    }
    bool SetSendBufferSize(int value) {
        return setsockopt(_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&value), sizeof(int)) >= 0;
    }
    int GetRecvBufferSize() {
        int value;
#ifndef _WIN32
        socklen_t len;
#else
        int len;
#endif
        len = sizeof(value);
        getsockopt(_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&value), &len);
        return value;
    }
    int GetSendBufferSize() {
        int value;
#ifndef _WIN32
        socklen_t len;
#else
        int len;
#endif
        len = sizeof(value);
        getsockopt(_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&value), &len);
        return value;
    }
protected:
	std::map<int, std::function<void(NetAddress_t&)>> errCallback;
#ifdef _WIN32
	SOCKET _socket = INVALID_SOCKET;
#else
    int _socket = -1;
#endif
	std::string s_type;
};
