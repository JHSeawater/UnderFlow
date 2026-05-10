// 클라이언트 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "src/common/network.h"

#define PORT 8080

// 수신 스레드
void* recv_thread(void* arg) {
    int sock = *(int*)arg;
    Packet pkt;
    
    while (1) {
        // 패킷 수신
        if (recv_packet(sock, &pkt) < 0) {
            printf("\n[Client] Server disconnected.\n");
            exit(0);
        }

        // 패킷 타입 처리
        if (pkt.type == PKT_EVT_CHAT) {
            printf("\r\033[K[Broadcast]: %s\n", pkt.text);
            printf("Input > ");
            fflush(stdout);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    // IP 인자 없으면 로컬호스트
    const char *ip = (argc == 2) ? argv[1] : "127.0.0.1";
    
    // 소켓 생성
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket error");
        exit(1);
    }

    // 서버 주소 설정
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    serv_addr.sin_port = htons(PORT);
    
    printf("[Client] Attempting to connect to server %s:%d...\n", ip, PORT);
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("connect error");
        exit(1);
    }
    
    printf("[Client] Connected to server successfully.\n");
    
    // 수신 스레드 생성
    pthread_t r_thread;
    pthread_create(&r_thread, NULL, recv_thread, (void*)&sock);
    pthread_detach(r_thread);
    
    // 입력 버퍼
    char buf[MAX_TEXT_LEN];
    while (1) {
        printf("Input > ");
        fflush(stdout);
        if (fgets(buf, sizeof(buf), stdin) == NULL) break;
        
        // 개행 문자 제거
        buf[strcspn(buf, "\n")] = 0;
        if (strlen(buf) == 0) continue;
        
        // 패킷 생성
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_REQ_CHAT;
        strncpy(pkt.text, buf, MAX_TEXT_LEN - 1);
        
        if (send_packet(sock, &pkt) < 0) {
            printf("[Client] Failed to send packet.\n");
            break;
        }
    }
    
    close(sock);
    return 0;
}
