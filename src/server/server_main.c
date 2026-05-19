// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>
#include "protocol.h"
#include "userdb/userdb.h"
#include "market/market.h"
#include "npc/npc.h"
#include "sandbox/sandbox.h"
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

// =============================================================
// 이벤트 스레드 — 비동기 매물/NPC 스폰 엔진
// =============================================================

// 타이밍 상수 (조정 가능)
#define EVT_TICK_SEC       10   // 기본 틱 간격 (초)
#define EVT_MARKET_EVERY    2   // 매물 스폰 주기 (틱 수) → 20초
#define EVT_NPC_EVERY       5   // NPC 시퀀스 주기 (틱 수) → 50초
#define EVT_NPC_HINT_DELAY  8   // 힌트 후 NPC 실제 등장까지 지연 (초)

// doc_id / npc_id 발급 카운터 (이벤트 스레드 단독 사용)
static int32_t g_doc_id_counter = 0;
static int32_t g_npc_id_counter = 0;

// 태그 테이블 (protocol.h Tag enum 순서와 동기화)
static const uint32_t TAG_LIST[10] = {
    TAG_CORP_A, TAG_CORP_B, TAG_CORP_C,
    TAG_CUSTOMER, TAG_FINANCE, TAG_MILITARY,
    TAG_GOVERNMENT, TAG_MEDICAL, TAG_RESEARCH, TAG_PERSONAL
};
#define TAG_COUNT 10

static const char *TAG_SHORT[10] = {
    "A기업", "B기업", "C기업",
    "고객정보", "금융", "군사무기",
    "정부기관", "의료", "연구개발", "사적정보"
};

static const char *DOC_SUFFIX[] = {
    "내부 문건", "비밀 도면", "녹취록",
    "거래 장부", "고객 명단", "보고서",
};
#define DOC_SUFFIX_COUNT 6

// NPC 등장 전 힌트 (TAG_LIST 인덱스와 1:1 대응)
static const char *NPC_HINT[10] = {
    "[속보] A기업 내부 비리 의혹 문건 유출설",
    "[속보] B기업 주가 폭락 — 내부 문건 해커 손에",
    "[속보] C기업 CEO 비자금 거래 포착",
    "[속보] 대규모 고객정보 유출 사태 확산",
    "[속보] 금융 당국, 불법 거래 내역 추적 중",
    "[속보] 국방부 신무기 도면 유출 의혹 제기",
    "[속보] 정부 기관 내부 문건 암시장 유통 확인",
    "[속보] 제약사 미공개 임상 데이터 거래 포착",
    "[속보] 국책 연구소 기밀 자료 외부 유출",
    "[속보] 유명인 사생활 영상·정보 거래 포착",
};

// 비트 수 카운트 (portable)
static int count_bits(uint32_t v) {
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

// 동결 태그 제외한 랜덤 태그 조합 (1~3개) 생성
// Fisher-Yates partial shuffle로 중복 없이 선택
static uint32_t event_random_tags(void) {
    uint32_t frozen = market_frozen_mask();

    uint32_t avail[TAG_COUNT];
    int avail_cnt = 0;
    for (int i = 0; i < TAG_COUNT; i++) {
        if (!(TAG_LIST[i] & frozen))
            avail[avail_cnt++] = TAG_LIST[i];
    }
    if (avail_cnt == 0) return 0;  // 전부 동결 시 스폰 생략 (0 → event_spawn_market이 건너뜀)

    int pick = rand() % 3 + 1;
    if (pick > avail_cnt) pick = avail_cnt;

    uint32_t pool[TAG_COUNT];
    for (int i = 0; i < avail_cnt; i++) pool[i] = avail[i];
    for (int i = 0; i < pick; i++) {
        int j = i + rand() % (avail_cnt - i);
        uint32_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
    }

    uint32_t result = 0;
    for (int i = 0; i < pick; i++) result |= pool[i];
    return result;
}

// 태그 비트마스크에서 가장 낮은 비트의 TAG_LIST 인덱스 반환
static int first_tag_index(uint32_t tags) {
    for (int i = 0; i < TAG_COUNT; i++) {
        if (tags & TAG_LIST[i]) return i;
    }
    return 0;
}

// 태그 조합으로 문서명 생성
static void make_doc_name(uint32_t tags, char *out, size_t sz) {
    snprintf(out, sz, "%s %s",
             TAG_SHORT[first_tag_index(tags)],
             DOC_SUFFIX[rand() % DOC_SUFFIX_COUNT]);
}

// ── 매물 스폰 루틴 ─────────────────────────────────────────────
static void event_spawn_market(void) {
    if (market_is_full()) return;

    uint32_t tags = event_random_tags();
    if (!tags) return;

    DocMeta doc;
    memset(&doc, 0, sizeof(DocMeta));
    doc.doc_id     = ++g_doc_id_counter;
    doc.tags       = tags;
    doc.base_price = (rand() % 8 + 1) * 100;   // 100~800원
    make_doc_name(tags, doc.name, sizeof(doc.name));

    // 물리 파일 생성 먼저, 실패 시 인메모리 등록하지 않음
    if (sandbox_master_write(&doc) != 0) {
        fprintf(stderr, "[Event] sandbox_master_write 실패: doc_id=%d\n", doc.doc_id);
        return;
    }
    // 인메모리 등록 실패 시 물리 파일 정리
    if (market_spawn(&doc) != 0) {
        sandbox_master_unlink(doc.doc_id);
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type                      = PKT_EVT_MARKET_SPAWN;
    pkt.body.market_spawn.doc_id     = doc.doc_id;
    pkt.body.market_spawn.tags       = doc.tags;
    pkt.body.market_spawn.base_price = doc.base_price;
    strncpy(pkt.body.market_spawn.name, doc.name, MAX_NAME_LEN - 1);
    broadcast_packet(&pkt);

    printf("[Event] 매물 스폰: ID=%d tags=0x%03X price=%d \"%s\"\n",
           doc.doc_id, doc.tags, doc.base_price, doc.name);
}

// ── NPC 힌트 → 유예 → 스폰 시퀀스 ─────────────────────────────
static void event_do_npc_sequence(void) {
    uint32_t req_tags = event_random_tags();
    if (!req_tags) return;

    // 1단계: 속보 힌트 브로드캐스트 (유저 선점 매집 유도)
    Packet hint;
    memset(&hint, 0, sizeof(Packet));
    hint.type = PKT_EVT_CHAT;
    strncpy(hint.body.chat_evt.sender_key, "[속보]", MAX_KEY_LEN - 1);
    strncpy(hint.body.chat_evt.text,
            NPC_HINT[first_tag_index(req_tags)], MAX_TEXT_LEN - 1);
    broadcast_packet(&hint);

    // 2단계: 유저가 힌트를 읽고 매집할 시간
    sleep(EVT_NPC_HINT_DELAY);

    // 3단계: NPC 스폰
    NpcOrder npc;
    memset(&npc, 0, sizeof(NpcOrder));
    npc.npc_id        = ++g_npc_id_counter;
    npc.required_tags = req_tags;
    // 현상금: 태그 수 비례 + 랜덤 (태그 1개 → 500~3000, 3개 → 2000~4000)
    npc.bounty = (rand() % 5 + count_bits(req_tags)) * 500;

    if (npc_spawn(&npc) != 0) {
        printf("[Event] NPC 보드 가득참 — 스폰 건너뜀\n");
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type                       = PKT_EVT_NPC_SPAWN;
    pkt.body.npc_spawn.npc_id        = npc.npc_id;
    pkt.body.npc_spawn.required_tags = npc.required_tags;
    pkt.body.npc_spawn.bounty        = npc.bounty;
    broadcast_packet(&pkt);

    printf("[Event] NPC 스폰: ID=%d req_tags=0x%03X bounty=%d\n",
           npc.npc_id, npc.required_tags, npc.bounty);
}

// ── 이벤트 스레드 진입점 ───────────────────────────────────────
static void* event_thread(void *arg) {
    (void)arg;
    srand((unsigned int)time(NULL));

    int market_tick = 0;
    int npc_tick    = 0;

    printf("[Event] 이벤트 스레드 시작. 틱=%d초 / 매물=%d틱 / NPC=%d틱\n",
           EVT_TICK_SEC, EVT_MARKET_EVERY, EVT_NPC_EVERY);

    while (1) {
        sleep(EVT_TICK_SEC);
        market_tick++;
        npc_tick++;

        if (market_tick >= EVT_MARKET_EVERY) {
            market_tick = 0;
            event_spawn_market();
        }

        if (npc_tick >= EVT_NPC_EVERY) {
            npc_tick = 0;
            event_do_npc_sequence();
        }
    }
    return NULL;
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

            // 접속 성공 처리 — 이중 접속 재검사 + 등록을 단일 락 안에서 원자적 수행
            // (이중 접속 검사와 등록 사이의 TOCTOU 레이스 컨디션 방지)
            int late_dup = 0;
            int32_t sid = 0;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] != 0 && client_sockets[i] != sock &&
                    strncmp(active_keys[i], pkt.body.login.key, MAX_KEY_LEN) == 0) {
                    late_dup = 1;
                    break;
                }
            }
            if (!late_dup) {
                sid = ++g_session_counter;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_sockets[i] == sock) {
                        strncpy(active_keys[i], pkt.body.login.key, MAX_KEY_LEN - 1);
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&clients_mutex);

            if (late_dup) {
                Packet res;
                memset(&res, 0, sizeof(Packet));
                res.type = PKT_RES_LOGIN_FAIL;
                res.body.error.error_code = ERR_DUPLICATE_LOGIN;
                strncpy(res.body.error.reason, "이미 접속 중인 계정입니다.", MAX_TEXT_LEN - 1);
                packet_send(sock, &res);
                goto disconnect;
            }

            strncpy(my_key, pkt.body.login.key, MAX_KEY_LEN - 1);
            my_offset = offset;
            logged_in = 1;

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_LOGIN_OK;
            res.body.login_ok.assigned_session_id = sid;
            res.body.login_ok.money = rec.money;
            res.body.login_ok.goal_money = GOAL_MONEY;
            packet_send(sock, &res);

            // 늦은 접속자 동기화: 현재 시장 매물과 NPC 의뢰를 이 클라이언트에만 유니캐스트.
            // snapshot 함수가 내부 락을 잡고 복사 후 즉시 해제하므로
            // clients_mutex 없이 이 스레드 전용 sock에 바로 전송해도 안전.
            {
                DocMeta mkt_items[MAX_MARKET_ITEMS];
                int mkt_count = 0;
                market_snapshot(mkt_items, MAX_MARKET_ITEMS, &mkt_count);
                for (int i = 0; i < mkt_count; i++) {
                    Packet sync;
                    memset(&sync, 0, sizeof(Packet));
                    sync.type = PKT_EVT_MARKET_SPAWN;
                    sync.body.market_spawn.doc_id     = mkt_items[i].doc_id;
                    sync.body.market_spawn.tags       = mkt_items[i].tags;
                    sync.body.market_spawn.base_price = mkt_items[i].base_price;
                    strncpy(sync.body.market_spawn.name, mkt_items[i].name, MAX_NAME_LEN - 1);
                    packet_send(sock, &sync);
                }

                NpcOrder npc_orders[MAX_NPC_BOARD];
                int npc_count = 0;
                npc_snapshot(npc_orders, MAX_NPC_BOARD, &npc_count);
                for (int i = 0; i < npc_count; i++) {
                    Packet sync;
                    memset(&sync, 0, sizeof(Packet));
                    sync.type = PKT_EVT_NPC_SPAWN;
                    sync.body.npc_spawn.npc_id        = npc_orders[i].npc_id;
                    sync.body.npc_spawn.required_tags = npc_orders[i].required_tags;
                    sync.body.npc_spawn.bounty        = npc_orders[i].bounty;
                    packet_send(sock, &sync);
                }
            }

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

    // 이벤트 스레드 파생 (매물·NPC 자동 스폰)
    pthread_t event_tid;
    if (pthread_create(&event_tid, NULL, event_thread, NULL) == 0) {
        pthread_detach(event_tid);
    } else {
        perror("[Server] 이벤트 스레드 생성 실패");
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

        // malloc 먼저 — 실패 시 mutex 없이 연결 거부 (C2 fix)
        int *sock_ptr = malloc(sizeof(int));
        if (!sock_ptr) {
            fprintf(stderr, "[Server] malloc 실패 — 연결 거부.\n");
            close(client_sock);
            continue;
        }
        *sock_ptr = client_sock;

        // 슬롯 등록만 mutex 내에서 수행 (malloc·pthread_create를 mutex 밖으로 분리, C3 fix)
        pthread_mutex_lock(&clients_mutex);
        int added = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_sockets[i] == 0) {
                client_sockets[i] = client_sock;
                added = 1;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        if (!added) {
            printf("[Server] Connection rejected. Max clients reached.\n");
            free(sock_ptr);
            close(client_sock);
            continue;
        }

        pthread_t t_id;
        pthread_create(&t_id, NULL, client_handler, sock_ptr);
        pthread_detach(t_id);
        printf("[Server] New client connected. Socket: %d\n", client_sock);
    }

    close(server_sock);
    return 0;
}
