// 네트워크

#include "src/common/network.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

// 패킷 전송
int send_packet(int sock, const Packet *pkt) {
    Packet temp;
    memcpy(&temp, pkt, sizeof(Packet));

    // 엔디안 변환
    temp.type = htonl(temp.type);
    temp.user_id = htonl(temp.user_id);
    temp.item_count = htonl(temp.item_count);
    for (int i = 0; i < MAX_INVEN_SIZE; i++) {
        temp.item_ids[i] = htonl(temp.item_ids[i]);
    }
    temp.target_npc_id = htonl(temp.target_npc_id);
    temp.value = htonl(temp.value);

    // 데이터를 소켓에 작성
    size_t total_written = 0;
    const char *buf = (const char *)&temp;
    while (total_written < sizeof(Packet)) { // 패킷 크기만큼 쓰기 반복
        ssize_t w = write(sock, buf + total_written, sizeof(Packet) - total_written);
        if (w <= 0) return -1;
        total_written += w;
    }
    return 0;
}

// 패킷 수신
int recv_packet(int sock, Packet *pkt) {
    size_t total_read = 0;
    char *buf = (char *)pkt;
    
    // 패킷 크기만큼 읽기 반복
    while (total_read < sizeof(Packet)) {
        ssize_t r = read(sock, buf + total_read, sizeof(Packet) - total_read);
        if (r <= 0) return -1;
        total_read += r;
    }

    // 엔디안 변환
    pkt->type = ntohl(pkt->type);
    pkt->user_id = ntohl(pkt->user_id);
    pkt->item_count = ntohl(pkt->item_count);
    for (int i = 0; i < MAX_INVEN_SIZE; i++) {
        pkt->item_ids[i] = ntohl(pkt->item_ids[i]);
    }
    pkt->target_npc_id = ntohl(pkt->target_npc_id);
    pkt->value = ntohl(pkt->value);

    return 0;
}
