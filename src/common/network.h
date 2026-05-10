#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"

// Send a packet via TCP socket, automatically converting endianness
int send_packet(int sock, const Packet *pkt);

// Receive a packet from TCP socket, automatically converting endianness
// Returns -1 on disconnect or error
int recv_packet(int sock, Packet *pkt);

#endif
