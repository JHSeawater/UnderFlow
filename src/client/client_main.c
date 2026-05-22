// 클라이언트 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"

#define PORT 8080

void format_tags(uint32_t tags, char *buf, size_t buf_size) {
    buf[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t bit = 1U << i;
        if (tags & bit) {
            const char *name = get_tag_name(bit);
            if (strcmp(name, "알수없음") != 0) {
                int written = snprintf(buf + len, buf_size - len, "[%s]", name);
                if (written > 0) len += written;
            }
        }
    }
}

// 수신 스레드
void* recv_thread(void* arg) {
    int sock = *(int*)arg;
    Packet pkt;
    
    while (1) {
        // 패킷 수신
        if (packet_recv(sock, &pkt) <= 0) {
            printf("\n[Client] Server disconnected.\n");
            exit(0);
        }

        // 패킷 타입 처리
        switch (pkt.type) {
            case PKT_EVT_CHAT:
                printf("\r\033[K[Broadcast from %s]: %s\n", pkt.body.chat_evt.sender_key, pkt.body.chat_evt.text);
                break;
            case PKT_EVT_MARKET_SPAWN: {
                char tags_buf[128];
                format_tags(pkt.body.market_spawn.tags, tags_buf, sizeof(tags_buf));
                printf("\r\033[K[Market] 스폰! ID:%d | 태그:0x%03X %s | 가격:%d | 이름:%s\n", 
                    pkt.body.market_spawn.doc_id, pkt.body.market_spawn.tags, tags_buf,
                    pkt.body.market_spawn.base_price, pkt.body.market_spawn.name);
                break;
            }
            case PKT_EVT_NPC_SPAWN: {
                char tags_buf[128];
                format_tags(pkt.body.npc_spawn.required_tags, tags_buf, sizeof(tags_buf));
                printf("\r\033[K[NPC] 의뢰 스폰! NPC_ID:%d | 요구태그:0x%03X %s | 보상:%d\n", 
                    pkt.body.npc_spawn.npc_id, pkt.body.npc_spawn.required_tags, tags_buf, pkt.body.npc_spawn.bounty);
                break;
            }
            case PKT_RES_BUY_OK:
                printf("\r\033[K[System] 구매 성공! 남은 돈: %d\n", pkt.body.buy_ok.remaining_money);
                break;
            case PKT_RES_SELL_OK:
                printf("\r\033[K[System] 매각 성공! 보상: %d | 현재 돈: %d\n", pkt.body.sell_ok.bounty, pkt.body.sell_ok.new_money);
                break;
            case PKT_RES_DISPOSE_OK:
                printf("\r\033[K[System] 파기 완료.\n");
                break;
            case PKT_RES_INVEN_INFO: {
                printf("\r\033[K[Inventory] 내 자금: %d | 보유 아이템 수: %d\n", pkt.body.inven_info.money, pkt.body.inven_info.count);
                for(int i=0; i<pkt.body.inven_info.count; i++) {
                    char tags_buf[128];
                    format_tags(pkt.body.inven_info.items[i].tags, tags_buf, sizeof(tags_buf));
                    printf("\r\033[K  -> ID:%d | 이름:%s | 태그:0x%03X %s\n", 
                        pkt.body.inven_info.items[i].doc_id, pkt.body.inven_info.items[i].name, pkt.body.inven_info.items[i].tags, tags_buf);
                }
                break;
            }
            case PKT_RES_ERROR:
                printf("\r\033[K[Error] %s (코드: %d)\n", pkt.body.error.reason, pkt.body.error.error_code);
                break;
            case PKT_EVT_MARKET_REMOVE:
                printf("\r\033[K[Market] 매물 삭제됨 (ID:%d)\n", pkt.body.market_remove.doc_id);
                break;
            case PKT_EVT_NPC_DESPAWN:
                printf("\r\033[K[NPC] 의뢰 만료됨 (NPC_ID:%d)\n", pkt.body.npc_despawn.npc_id);
                break;
            case PKT_EVT_GAME_OVER:
                printf("\r\033[K[Game Over] %s\n", pkt.body.endgame.message);
                exit(0);
                break;
            case PKT_EVT_VICTORY:
                printf("\r\033[K[Victory] %s\n", pkt.body.endgame.message);
                break;
            default:
                // 다른 패킷 무시
                break;
        }
        printf("Input > ");
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <user_key> [server_ip]\n", argv[0]);
        return 1;
    }
    const char *user_key = argv[1];
    const char *ip = (argc > 2) ? argv[2] : "127.0.0.1";
    
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
    
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_REQ_LOGIN;
    strncpy(pkt.body.login.key, user_key, MAX_KEY_LEN - 1);
    packet_send(sock, &pkt);

    if (packet_recv(sock, &pkt) <= 0 || pkt.type != PKT_RES_LOGIN_OK) {
        printf("Login Failed.\n");
        close(sock);
        return 1;
    }
    printf("[Client] Logged in successfully!\n");
    
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
        
        // 패킷 생성 및 명령어 파싱
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));

        if (strncmp(buf, "/buy ", 5) == 0) {
            pkt.type = PKT_REQ_BUY;
            pkt.body.buy.doc_id = atoi(buf + 5);
        } else if (strncmp(buf, "/dispose ", 9) == 0) {
            pkt.type = PKT_REQ_DISPOSE;
            pkt.body.dispose.doc_id = atoi(buf + 9);
        } else if (strcmp(buf, "/inventory") == 0 || strcmp(buf, "/inv") == 0) {
            pkt.type = PKT_REQ_INVEN;
        } else if (strncmp(buf, "/rumor", 6) == 0) {
            pkt.type = PKT_REQ_RUMOR;
        } else if (strncmp(buf, "/sell ", 6) == 0) {
            // 사용법: /sell npc_id doc_id1 doc_id2 ...
            pkt.type = PKT_REQ_SELL;
            char *token = strtok(buf + 6, " ");
            if (token) pkt.body.sell.npc_id = atoi(token);
            
            int count = 0;
            token = strtok(NULL, " ");
            while (token != NULL && count < MAX_INVEN_SIZE) {
                pkt.body.sell.doc_ids[count++] = atoi(token);
                token = strtok(NULL, " ");
            }
            pkt.body.sell.count = count;
        } else {
            pkt.type = PKT_REQ_CHAT;
            strncpy(pkt.body.chat.text, buf, MAX_TEXT_LEN - 1);
        }
        
        if (packet_send(sock, &pkt) < 0) {
            printf("[Client] Failed to send packet.\n");
            break;
        }
    }
    
    close(sock);
    return 0;
}
