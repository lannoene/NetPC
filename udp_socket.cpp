#include "udp_socket.hpp"

#define MAX_MSG_BUF_SIZE 2000
#ifndef _WIN32
#define WSAEWOULDBLOCK EWOULDBLOCK
#endif

#ifdef __3DS__
#include <list>
#include <mutex>
#include "../itypes.h"
#include "../nwm_net.hpp"
#include "../ctr_objects.hpp"
#define DISABLE_FAST_PACKETS 1
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

int UDPSocket::ReceiveFrom(NetAddress_t& packet_from, NetPC::Packet& buf) {
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

bool UDPSocket::SendTo(const NetPC::Packet& bufn, NetAddress_t *recipient) {
    PROFILE_FUNCTION;
    auto regular_send = [](auto _socket, const NetPC::Packet& bufn, NetAddress_t *recipient) {
        struct sockaddr_in dest;
        recipient->ToSockadr(&dest);
        //AssertPanicError(buf.GetSeek() <= 65535, "Socket buffer overflow.");
        int ret = sendto(_socket, reinterpret_cast<const char*>(bufn.GetData()), bufn.GetDataSize(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(struct sockaddr_in));
        if (ret >= 0) {
            return static_cast<size_t>(ret) == bufn.GetDataSize();
        }
        return false;
    };
#if !defined(__3DS__) || DISABLE_FAST_PACKETS == 1
    return regular_send(_socket, bufn, recipient);
#else
    perror("Called UDPSocket::SendTo");
    DEFER_LITE([] {
        perror("Exited UDPSocket::SendTo");
    });
    perror("W\n");
    // Thank you PabloMK7 for the suggestion!
    std::array<u8, 6> dest_mac_addr;
    {TAGGED_SCOPE("dddddd");
        perror("X\n");
        if (!my_port_num || !my_ip_addr || last_send_errored) {
            struct sockaddr_in sin;
            socklen_t len = sizeof(sin);
            if (getsockname(_socket, (struct sockaddr *)&sin, &len) < 0) {
                perror("UDPSocket::SendTo: getsockname failed!\n");
            }
            my_port_num = sin.sin_port;
            R_ASSERT(SOCU_GetIPInfo(nullptr, &netmask, nullptr));
            my_ip_addr = gethostid();
            //perror(ScrappyFormat("Got IP address %s").c_str());
            if (!sin.sin_port) {
                // we have to send a regular packet to give
                // ourself a port
                return regular_send(_socket, bufn, recipient);
            }
        }
        perror("Y\n");
        std::scoped_lock lock{arp_map_mut};
        perror("Z\n");
        last_send_errored = false;
        auto peer_ip = *reinterpret_cast<u32*>(recipient->GetIp());
        auto it = arp_mac_addr_map.find(peer_ip);
        perror("L\n");
        if (it == arp_mac_addr_map.end()) {
            perror("K\n");
            /* POSSIBLE IMPLEMENTATION: CLOSEST TO NTR-CFW
            // we need to send a regular packet and add the arp
            // mac address to the map by looking through the
            // frame payload and picking out the necessary information
            extern void InjectBranchLinkExchange(void*, void*);
            extern int asm_Hook_NWMGetValParam(const u8 *, int) asm("asm_Hook_NWMGetValParam");
            extern void AddNWMValCallback(std::function<void(UDS::MacAddress, u32, u16)>, u32);
            InjectBranchLinkExchange((void*)0x00120f9c, (void*)asm_Hook_NWMGetValParam);
            auto ip_find = *reinterpret_cast<u32*>(recipient->GetIp());
            perror("Attempting to send regular packet\n");
            AddNWMValCallback([this, ip_find](std::array<u8, 6> mac_addr, u32 my_ip, u16 my_port) {
                arp_mac_addr_map[ip_find] = mac_addr;
                my_ip_addr = my_ip;
                my_port_num = my_port;
            }, ip_find);
            return regular_send(_socket, bufn, recipient);*/
            // other implementation: general, doesn't require hooks! a little bit slower...
            
            // check if we are connecting to a global ip address
            auto recip_ip = *reinterpret_cast<u32*>(recipient->GetIp());
            u32 routing_ip = recip_ip;
            //printf("My ip addr %p net mask %p recip ip %p, %p != %p, %hu\n", my_ip_addr, netmask.s_addr, recip_ip, (my_ip_addr & netmask.s_addr), (recip_ip & netmask.s_addr), ntohs(my_port_num));
            if ((my_ip_addr & netmask.s_addr) != (recip_ip & netmask.s_addr)) {
                perror("A\n");
                // global... so we have to find the gateway IP
                auto routing_entries = new SOCU_RoutingTableEntry[num_routing_entries_last];
                perror("B\n");
                socklen_t entries_buf_size = num_routing_entries_last * sizeof(SOCU_RoutingTableEntry);
                R_ASSERT(SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_ROUTING_TABLE, routing_entries, &entries_buf_size));
                perror("C\n");
                size_t num_entries = entries_buf_size / sizeof(SOCU_RoutingTableEntry);
                bool found_entry = false;
                u32 gateway_ip;
                for (size_t i = 0; i < num_entries; i++) {
                    auto& entry = routing_entries[i];
                    if (entry.dest_ip.s_addr == recip_ip || true) { // ... just pick the first gateway address ...
                        found_entry = true;
                        gateway_ip = entry.gateway.s_addr;
                        break;
                    }
                }
                perror("D\n");
                delete[] routing_entries;
                perror("E\n");
                if (num_entries == num_routing_entries_last) {
                    perror("Saturated routing entries list\n");
                    num_routing_entries_last *= 2;
                }
                if (!found_entry) {
                    perror("Unable to locate routing entry for global IP\n");
                    return regular_send(_socket, bufn, recipient);
                } else {
                    perror("Found routing entry!\n");
                    routing_ip = gateway_ip;
                }
            }
            auto arp_entries = new SOCU_ARPTableEntry[num_arp_entries_last];
            socklen_t entries_buf_size = num_arp_entries_last * sizeof(SOCU_ARPTableEntry);
            R_ASSERT(SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_ARP_TABLE, arp_entries, &entries_buf_size));
            size_t num_entries = entries_buf_size / sizeof(SOCU_ARPTableEntry);
            bool found_arp = false;
            for (size_t i = 0; i < num_entries; i++) {
                // go through all entries and find our
                // arp entry
                auto& entry = arp_entries[i];
                if (entry.ip.s_addr == routing_ip && memcmp(entry.mac, std::array<u8, 6>{}.data(), sizeof(entry.mac)) != 0) {
                    found_arp = true;
                    auto& mac = arp_mac_addr_map[entry.ip.s_addr];
                    memcpy(&mac, entry.mac, sizeof(entry.mac));
                    memcpy(dest_mac_addr.data(), entry.mac, sizeof(entry.mac));
                    break;
                }
            }
            delete[] arp_entries;
            // isn't it ridiculous that there's no way to get the
            // current size of the list???
            if (num_entries == num_arp_entries_last) {
                perror("Saturated arp entries list\n");
                num_arp_entries_last *= 2;
            }
            if (!found_arp) {
                // we need to call regular send to
                // let SOC:U route at least 1 packet
                // by itself, and we can try again
                // in the next call
                perror("Unable to locate arp entry\n");
                return regular_send(_socket, bufn, recipient);
            } else {
                perror(ScrappyFormat("Found arp entry! %02x:%02x:%02x:%02x:%02x:%02x\n", dest_mac_addr[0], dest_mac_addr[1], dest_mac_addr[2], dest_mac_addr[3], dest_mac_addr[4], dest_mac_addr[5]).c_str());
            }
        } else {
            dest_mac_addr = it->second;
        }
    }
    perror("Sending fast packet\n");
    constexpr u32 max_udp_packet_fragment_size = 4000;// ntr uses 1448;
    auto UDP_Init_Buffers = [&](const void *buf, size_t size) -> std::list<std::vector<u8>> {
        std::list<std::vector<u8>> out;
        u32 offset = 0;
        while (offset < size) {
            auto calculate_ipv4_checksum = [](void* vdata, size_t length) -> u16_be {
                // Cast the data pointer to one that can be indexed.
                char *data = (char*)vdata;
                // Initialise the accumulator.
                uint32_t acc = 0xffff;

                // Handle complete 16-bit blocks.
                for (size_t i = 0; i + 1 < length; i += 2) {
                    auto word = *reinterpret_cast<u16*>(data + i);
                    acc += word;
                    if (acc > 0xffff) {
                        acc -= 0xffff;
                    }
                }

                // Handle any partial block at the end of the data.
                if (length & 1) {
                    uint16_t word = *reinterpret_cast<u8*>(data + length - 1);
                    acc += word;
                    if (acc > 0xffff) {
                        acc -= 0xffff;
                    }
                }
                
                return u16_be().Load(~acc);
            };
            #pragma pack(push, 2)
            struct NWMEth2PacketHeader {
                std::array<u8, 6> mac_addr_dest;
                std::array<u8, 6> mac_addr_src;
                u16_be packet_size;
                u16 unk1;
                u32_be unk2;
                u16_be type;
            };
            #pragma pack(pop)
            static_assert(sizeof(NWMEth2PacketHeader) == 22);
            struct IPv4PacketHeader {
                u8 about;
                u8 dsf; // ignore
                u16_be ip_length; // length of this + after
                u16_be identification;
                u16_be flags;
                u8 ttl;
                u8 protocol;
                u16_be checksum;
                std::array<u8, 4> ipv4_src;
                std::array<u8, 4> ipv4_dest;
            };
            static_assert(sizeof(IPv4PacketHeader) == 20);
            struct UDPPacketHeader {
                u16_be source_port;
                u16_be destination_port;
                u16_be udp_length; // length of this + after
                u16_be checksum;
            };
            static_assert(sizeof(UDPPacketHeader) == 8);
            constexpr size_t udp_frame_header_size = sizeof(NWMEth2PacketHeader) + sizeof(IPv4PacketHeader) + sizeof(UDPPacketHeader);
            static_assert(max_udp_packet_fragment_size > udp_frame_header_size);
            static_assert(udp_frame_header_size == 50);
            auto cur_frag_size = std::min<u32>(size - offset + udp_frame_header_size, max_udp_packet_fragment_size);
            constexpr u16 IPv4HeaderType = 0x0800;
            auto& cb = out.emplace_back(cur_frag_size);
            auto heth2 = reinterpret_cast<NWMEth2PacketHeader*>(cb.data());
            heth2->mac_addr_src = NWM_Net::GetConsoleMacAddress();
            heth2->mac_addr_dest = dest_mac_addr;
            heth2->packet_size = cur_frag_size;
            heth2->unk1 = 0xAAAA;
            heth2->unk2 = 0x03000000;
            heth2->type = IPv4HeaderType;
            auto hipv4 = reinterpret_cast<IPv4PacketHeader*>(heth2 + 1);
            constexpr u8 hipv4_about_version_4 = 4,
                         hipv4_header_length = 5,
                         hipv4_ttl_num = 64;
            constexpr u8 hipv4_protocol_udp = IPPROTO_UDP;
            hipv4->about = (hipv4_about_version_4 << 4) | hipv4_header_length;
            hipv4->dsf = 0;
            hipv4->ip_length = cb.size() - sizeof(NWMEth2PacketHeader);
            hipv4->identification = next_packet_id++;
            hipv4->flags = 0;
            hipv4->ttl = hipv4_ttl_num;
            hipv4->protocol = hipv4_protocol_udp;
            memcpy(hipv4->ipv4_src.data(), &my_ip_addr, sizeof(my_ip_addr));
            memcpy(hipv4->ipv4_dest.data(), recipient->GetIp(), sizeof(u32));
            hipv4->checksum = 0;
            hipv4->checksum = calculate_ipv4_checksum(hipv4, sizeof(IPv4PacketHeader));
            auto hudp  = reinterpret_cast<UDPPacketHeader*>(hipv4 + 1);
            hudp->source_port.Load(my_port_num);
            hudp->destination_port = recipient->GetPort();
            hudp->udp_length = hipv4->ip_length - sizeof(IPv4PacketHeader);
            hudp->checksum = 0; // ignore
            auto body  = reinterpret_cast<u8*>(hudp + 1);
            auto body_length = hudp->udp_length - sizeof(UDPPacketHeader);
            memcpy(body, reinterpret_cast<const u8*>(buf) + offset, body_length);
            offset += body_length;
        }
        return out;
    };
    auto bufs = UDP_Init_Buffers(bufn.GetData(), bufn.GetDataSize());
    
    for (auto& buf : bufs) {
        perror("Attempting to send packet\n");
        if (int ret = NWM_Net::SendPacketFrame(buf.data(), buf.size(), buf.size(), (const void*)2); ret < 0) {
            ResultCode r{.raw = static_cast<u32>(ret)};
            perror(ScrappyFormat("NWM_Net::SendPacketFrame error! %p, description: %ld, summary %ld, module %ld, level %ld, sent buf of size: %ld\n", ret, r.description, r.summary, r.module, r.level, buf.size()).c_str());
            last_send_errored = true;
            return false;
        }
    }
    perror("NWM_Net::SendPacketFrame succeeded!\n");
    return true;
#endif
}
