// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "userdb/userdb.h"
#include "market/market.h"
#include "npc/npc.h"
#include "handlers/handlers.h"

#define PORT 8080
#define MAX_CLIENTS 10

int client_sockets[MAX_CLIENTS];
char active_keys[MAX_CLIENTS][MAX_KEY_LEN];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_session_counter = 0;

// 패킷 방송 (handlers.c가 extern으로 호출)
void broadcast_packet(Packet *pkt) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && active_keys[i][0] != '\0') {
            packet_send(client_sockets[i], pkt);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 승리 트리거 stub — A 영역. handlers.c의 /sell이 승리 감지 시 호출.
// 일단 단순 브로드캐스트로 시연하고 A가 실제 PKT_EVT_VICTORY/PKT_EVT_GAME_OVER로 분리.
void server_trigger_victory(int winner_sock) {
    Packet vic;
    memset(&vic, 0, sizeof(Packet));
    vic.type = PKT_EVT_VICTORY;
    strncpy(vic.body.endgame.message,
            "모든 빚을 갚았군. 약속대로 넌 이제 자유다.",
            MAX_TEXT_LEN - 1);
    packet_send(winner_sock, &vic);

    Packet over;
    memset(&over, 0, sizeof(Packet));
    over.type = PKT_EVT_GAME_OVER;
    strncpy(over.body.endgame.message,
            "약속을 지키지 못했군. 남은 빚은 목숨으로 받지.",
            MAX_TEXT_LEN - 1);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && client_sockets[i] != winner_sock) {
            packet_send(client_sockets[i], &over);
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
    off_t my_offset = 0;
    int logged_in = 0;

    // 1. 로그인 단계 대기
    while (!logged_in) {
        if (packet_recv(sock, &pkt) <= 0) {
            goto disconnect;
        }

        if (pkt.type == PKT_REQ_LOGIN) {
            // Key 안전성 검사 (디렉토리 탈출 차단)
            if (!userdb_key_is_safe(pkt.body.login.key)) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_INVALID_SESSION;
                strncpy(res.body.error.reason, "Key 문자가 허용되지 않습니다.", MAX_TEXT_LEN - 1);
                packet_send(sock, &res);
                goto disconnect;
            }

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

            // DB 조회 및 생성 (find+create 원자적 수행으로 TOCTOU 방지)
            UserRecord rec;
            off_t offset;
            int rc = userdb_find_or_create(pkt.body.login.key, 1000, &rec, &offset);
            if (rc < 0) {
                goto disconnect;
            }

            // 소각 계정 차단
            if (userdb_is_burned(&rec)) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_KEY_BURNED;
                strncpy(res.body.error.reason, "파산/소각된 계정입니다.", MAX_TEXT_LEN - 1);
                packet_send(sock, &res);
                goto disconnect;
            }

            // 개인 샌드박스 폴더 생성 (이미 있으면 EEXIST는 정상)
            if (userdb_create_sandbox(pkt.body.login.key) != 0) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_INVALID_SESSION;
                strncpy(res.body.error.reason, "샌드박스 디렉토리 생성 실패.", MAX_TEXT_LEN - 1);
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
            my_offset = offset;
            logged_in = 1;

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_LOGIN_OK;
            res.body.login_ok.assigned_session_id = ++g_session_counter;
            res.body.login_ok.money = rec.money;
            res.body.login_ok.goal_money = GOAL_MONEY;
            packet_send(sock, &res);

            printf("[Server] User '%s' logged in. (offset=%ld, money=%d)\n",
                   my_key, (long)my_offset, rec.money);
        }
    }

    // 2. 메인 루프 — 채팅은 직접 처리, 그 외 명령은 handlers로 디스패치
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
        } else {
            // B 영역: /buy /sell /dispose /inventory /rumor 등
            handlers_dispatch(sock, &pkt, my_key, my_offset);
        }

        // 파산(Permadeath) 즉시 처형 — 핸들러 안에서 userdb_burn_at 호출됐을 가능성 검사
        UserRecord post;
        if (userdb_find(my_key, &post, NULL) == 0 && userdb_is_burned(&post)) {
            Packet over;
            memset(&over, 0, sizeof(Packet));
            over.type = PKT_EVT_GAME_OVER;
            strncpy(over.body.endgame.message,
                    "파산. 계정이 영구 소각되었습니다.",
                    MAX_TEXT_LEN - 1);
            packet_send(sock, &over);
            printf("[Server] User '%s' bankrupted — forced disconnect.\n", my_key);
            break;
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

    // 시장 / NPC 자료구조 초기화
    market_init();
    npc_init();

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
