// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "src/common/network.h"
#include "src/server/userdb/userdb.h"

#define PORT 8080
#define MAX_CLIENTS 10

int client_sockets[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// 패킷 방송
void broadcast_packet(Packet *pkt) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0) {
            send_packet(client_sockets[i], pkt);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 클라이언트 핸들러
void* client_handler(void* arg) {
    int sock = *(int*)arg;
    free(arg);
    
    Packet pkt;
    while (1) {
        if (recv_packet(sock, &pkt) < 0) {
            printf("[Server] Client socket %d disconnected.\n", sock);
            break;
        }
        
        if (pkt.type == PKT_REQ_CHAT) {
            printf("[Server] Chat received from user: %s\n", pkt.text);
            Packet broadcast_pkt;
            memset(&broadcast_pkt, 0, sizeof(Packet));
            broadcast_pkt.type = PKT_EVT_CHAT;
            strncpy(broadcast_pkt.text, pkt.text, MAX_TEXT_LEN - 1);
            
            // 모든 클라이언트에 메세지 전송
            broadcast_packet(&broadcast_pkt);
        }
    }
    
    // 멀티스레딩 경쟁 방어
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] == sock) {
            client_sockets[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    close(sock);
    return NULL;
}

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_size;
    
    memset(client_sockets, 0, sizeof(client_sockets));
    
    // 유저 데이터베이스 초기화
    if (userdb_init() < 0) {
        perror("Failed to initialize user database");
        exit(1);
    }
    
    // 소켓 생성
    server_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("socket error");
        exit(1);
    }
    
    // 소켓 옵션 설정
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 주소 재사용
    
    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);
    
    // 서버 소켓 바인딩
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind error");
        exit(1);
    }
    
    // 소켓 리스닝
    if (listen(server_sock, 5) == -1) {
        perror("listen error");
        exit(1);
    }
    
    printf("[Server] Server initialized.\n");
    printf("[Server] Listening on port %d... (Max Clients: %d)\n", PORT, MAX_CLIENTS);
    
    // 클라이언트 접속 수락
    while (1) {
        client_addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_size);
        if (client_sock == -1) {
            continue;
        }
        
        // 스레드 추가
        pthread_mutex_lock(&clients_mutex);
        int added = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] == 0) {
                client_sockets[i] = client_sock;
                added = 1;
                
                int* sock_ptr = malloc(sizeof(int));
                *sock_ptr = client_sock;
                pthread_t t_id;
                pthread_create(&t_id, NULL, client_handler, sock_ptr);
                pthread_detach(t_id);
                printf("[Server] New client connected. Socket: %d\n", client_sock);
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        
        // 연결 거부
        if (!added) {
            printf("[Server] Connection rejected. Max clients reached.\n");
            close(client_sock);
        }
    }
    
    close(server_sock);
    return 0;
}
