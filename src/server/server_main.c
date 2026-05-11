// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "userdb/userdb.h"

#define PORT 8080
#define MAX_CLIENTS 10

int client_sockets[MAX_CLIENTS];
char active_keys[MAX_CLIENTS][MAX_KEY_LEN];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// 패킷 방송
void broadcast_packet(Packet *pkt) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0) {
            packet_send(client_sockets[i], pkt);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 클라이언트 핸들러
void* client_handler(void* arg) {
    int sock = *(int*)arg;
    free(arg);
    
    Packet pkt;
    char my_key[MAX_KEY_LEN] = {0};
    int logged_in = 0;

    // 1. 로그인 단계 대기
    while (!logged_in) {
        if (packet_recv(sock, &pkt) <= 0) {
            goto disconnect;
        }

        if (pkt.type == PKT_REQ_LOGIN) {
            // 이중 접속 검사
            pthread_mutex_lock(&clients_mutex);
            int duplicate = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] != 0 && client_sockets[i] != sock && 
                    strncmp(active_keys[i], pkt.body.login.key, MAX_KEY_LEN) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);

            if (duplicate) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_DUPLICATE_LOGIN;
                strncpy(res.body.error.reason, "이미 접속 중인 계정입니다.", MAX_TEXT_LEN - 1);
                packet_send(sock, &res);
                goto disconnect;
            }

            // DB 조회 및 생성
            UserRecord rec;
            off_t offset;
            if (userdb_find(pkt.body.login.key, &rec, &offset) != 0) {
                memset(&rec, 0, sizeof(UserRecord));
                strncpy(rec.key, pkt.body.login.key, MAX_KEY_LEN - 1);
                rec.money = 1000;
                userdb_append(&rec, &offset);
            }

            if (userdb_is_burned(&rec)) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_KEY_BURNED;
                strncpy(res.body.error.reason, "파산/소각된 계정입니다.", MAX_TEXT_LEN - 1);
                packet_send(sock, &res);
                goto disconnect;
            }

            // 접속 성공 처리
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] == sock) {
                    strncpy(active_keys[i], pkt.body.login.key, MAX_KEY_LEN - 1);
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);

            strncpy(my_key, pkt.body.login.key, MAX_KEY_LEN - 1);
            logged_in = 1;

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_LOGIN_OK;
            res.body.login_ok.assigned_session_id = sock;
            res.body.login_ok.money = rec.money;
            res.body.login_ok.goal_money = 10000;
            packet_send(sock, &res);
            
            printf("[Server] User '%s' logged in.\n", my_key);
        }
    }

    // 2. 메인 채팅 루프
    while (1) {
        if (packet_recv(sock, &pkt) <= 0) {
            break;
        }
        
        if (pkt.type == PKT_REQ_CHAT) {
            printf("[Server] Chat received from %s: %s\n", my_key, pkt.body.chat.text);
            Packet broadcast_pkt;
            memset(&broadcast_pkt, 0, sizeof(Packet));
            broadcast_pkt.type = PKT_EVT_CHAT;
            strncpy(broadcast_pkt.body.chat_evt.text, pkt.body.chat.text, MAX_TEXT_LEN - 1);
            strncpy(broadcast_pkt.body.chat_evt.sender_key, my_key, MAX_KEY_LEN - 1);
            
            broadcast_packet(&broadcast_pkt);
        }
    }

disconnect:
    printf("[Server] Client socket %d disconnected.\n", sock);
    
    // 멀티스레딩 경쟁 방어 및 퇴장 처리
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] == sock) {
            client_sockets[i] = 0;
            memset(active_keys[i], 0, MAX_KEY_LEN);
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
    memset(active_keys, 0, sizeof(active_keys));
    
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
