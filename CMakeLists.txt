project(netpc)

add_library(netpc
	packet.cpp
	include/netpc/packet.h
	pcall_client.cpp
	include/netpc/pcall_client.h
	pcall_host.cpp
	include/netpc/pcall_host.h
	include/netpc/socket_base.hpp
	udp_socket.cpp
	include/netpc/udp_socket.hpp
)

if (WIN32)
	target_link_libraries(netpc ws2_32)
endif()

target_include_directories(netpc PRIVATE include/netpc/)