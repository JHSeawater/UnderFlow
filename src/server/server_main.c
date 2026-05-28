// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "userdb/userdb.h"
#include "src/server/market.h"
#include "src/server/sandbox.h"

#define PORT           8080
#define MAX_CLIENTS    10
#define INITIAL_MONEY  1000
#define GOAL_MONEY     10000
#define RUMOR_FEE      500

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

// 특정 키에게만 단일 전송 (사보타주 글리치 등에서 사용)
int unicast_to_key(const char *target_key, Packet *pkt) {
    int sent = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && strncmp(active_keys[i], target_key, MAX_KEY_LEN) == 0) {
            packet_send(client_sockets[i], pkt);
            sent = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return sent;
}

// 에러 응답 헬퍼
static void send_error(int sock, int32_t code, const char *reason) {
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_ERROR;
    res.body.error.error_code = code;
    strncpy(res.body.error.reason, reason, MAX_TEXT_LEN - 1);
    packet_send(sock, &res);
}

// doc_id 오름차순 정렬 (락 순서 규약 — Circular Wait 봉쇄)
static int cmp_int32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

// =============================================================
// 이벤트 스레드 — 비동기 매물/NPC 스폰 엔진
// =============================================================

// 타이밍 상수 (조정 가능)
#define EVT_TICK_SEC       10   // 기본 틱 간격 (초)
#define EVT_MARKET_EVERY    2   // 매물 스폰 주기 (틱 수) → 20초
#define EVT_NPC_EVERY       5   // NPC 시퀀스 주기 (틱 수) → 50초
#define EVT_NPC_HINT_DELAY  8   // 힌트 후 NPC 실제 등장까지 지연 (초)
#define EVT_NPC_MAX_AGE_SEC 180 // 미해결 NPC 만료 수명 (초) → 보드 고착 방지

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

    DocFile doc;
    memset(&doc, 0, sizeof(DocFile));
    doc.doc_id     = ++g_doc_id_counter;
    doc.tags       = tags;
    doc.base_price = (rand() % 8 + 1) * 100;   // 100~800원
    make_doc_name(tags, doc.name, sizeof(doc.name));

    // 물리 파일 생성 먼저, 실패 시 인메모리 등록하지 않음
    if (master_write_doc(&doc) != 0) {
        fprintf(stderr, "[Event] master_write_doc 실패: doc_id=%d\n", doc.doc_id);
        return;
    }
    // 인메모리 등록 실패 시 물리 파일 정리
    if (market_add(doc.doc_id, doc.tags, doc.base_price, doc.name) != 0) {
        master_remove_doc(doc.doc_id);
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
    if (npc_is_full()) {
        printf("[Event] NPC 보드 가득참 — 스폰 건너뜀\n");
        return;
    }

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
    int32_t npc_id = ++g_npc_id_counter;
    // 현상금: 태그 수 비례 + 랜덤 (태그 1개 → 500~3000, 3개 → 2000~4000)
    int32_t bounty = (rand() % 5 + count_bits(req_tags)) * 500;

    if (npc_add(npc_id, req_tags, bounty) != 0) {
        printf("[Event] NPC 보드 가득참 — 스폰 건너뜀\n");
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type                       = PKT_EVT_NPC_SPAWN;
    pkt.body.npc_spawn.npc_id        = npc_id;
    pkt.body.npc_spawn.required_tags = req_tags;
    pkt.body.npc_spawn.bounty        = bounty;
    broadcast_packet(&pkt);

    printf("[Event] NPC 스폰: ID=%d req_tags=0x%03X bounty=%d\n",
           npc_id, req_tags, bounty);
}

// ── 의뢰 에이징 — 오래 미해결 NPC 만료 (GDD 2.B) ──────────────
static void event_age_npcs(void) {
    int32_t expired[MAX_NPC_SLOTS];
    int n = 0;
    npc_despawn_aged(EVT_NPC_MAX_AGE_SEC, expired, MAX_NPC_SLOTS, &n);

    for (int i = 0; i < n; i++) {
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_EVT_NPC_DESPAWN;
        pkt.body.npc_despawn.npc_id = expired[i];
        broadcast_packet(&pkt);
        printf("[Event] NPC 만료(에이징): ID=%d\n", expired[i]);
    }
}

void* event_thread(void* arg) {
    (void)arg;
    srand(time(NULL));
    
    printf("[Event Engine] 비동기 이벤트 스레드 구동 시작.\n");
    int tick = 0;
    
    while (1) {
        sleep(EVT_TICK_SEC);
        tick++;
        
        // 1. NPC 의뢰 에이징 체크 (매 틱마다)
        event_age_npcs();
        
        // 2. 시장 매물 스폰
        if (tick % EVT_MARKET_EVERY == 0) {
            event_spawn_market();
        }
        
        // 3. NPC 의뢰 스폰 시퀀스 (별도 유예 지연 때문에 스레드 파생)
        if (tick % EVT_NPC_EVERY == 0) {
            pthread_t npc_tid;
            if (pthread_create(&npc_tid, NULL, (void*(*)(void*))event_do_npc_sequence, NULL) == 0) {
                pthread_detach(npc_tid);
            }
        }
    }
    return NULL;
}

// ============================================================
// 핸들러: /buy
// ============================================================
static void handle_buy(int sock, const char *key, Packet *pkt) {
    int32_t doc_id = pkt->body.buy.doc_id;

    // 1. 매물 존재 확인
    MarketSlot slot;
    if (market_find(doc_id, &slot) != 0) {
        send_error(sock, ERR_DOC_NOT_FOUND, "매물이 존재하지 않습니다.");
        return;
    }

    // 2. 동결 검사
    if (slot.is_frozen) {
        send_error(sock, ERR_DOC_FROZEN, "동결된 매물입니다.");
        return;
    }

    // 3. 인벤토리 용량 제한
    if (sandbox_count(key) >= MAX_INVEN_SIZE) {
        send_error(sock, ERR_INVENTORY_FULL, "인벤토리가 가득 찼습니다 (최대 5칸).");
        return;
    }

    // 4. 자금 확인
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "유저 정보 조회 실패");
        return;
    }
    if (rec.money < slot.base_price) {
        send_error(sock, ERR_NOT_ENOUGH_MONEY, "잔액 부족");
        return;
    }

    // 5. 매물 원자적 점유 (선착순 독점 + 동결 TOCTOU 동시 방어)
    int take_rc = market_take(doc_id, &slot);
    if (take_rc == -2) {
        send_error(sock, ERR_DOC_FROZEN, "동결된 매물입니다.");
        return;
    } else if (take_rc != 0) {
        send_error(sock, ERR_DOC_NOT_FOUND, "이미 다른 유저가 구매했습니다.");
        return;
    }

    // 6. 실물 파일 이동 (master → sandbox)
    if (sandbox_buy(key, doc_id) != 0) {
        // 롤백: 매물 복구
        market_add(slot.doc_id, slot.tags, slot.base_price, slot.name);
        send_error(sock, ERR_DOC_NOT_FOUND, "파일 이동 실패 (시스템 에러)");
        return;
    }

    // 7. 자산 차감
    rec.money -= slot.base_price;
    userdb_update_at(offset, &rec);

    // 8. 응답
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_BUY_OK;
    res.body.buy_ok.doc_id = doc_id;
    res.body.buy_ok.remaining_money = rec.money;
    res.body.buy_ok.tags = slot.tags;
    strncpy(res.body.buy_ok.name, slot.name, MAX_NAME_LEN - 1);
    packet_send(sock, &res);

    // 9. 시장에서 매물 사라짐 전체 방송
    Packet evt;
    memset(&evt, 0, sizeof(Packet));
    evt.type = PKT_EVT_MARKET_REMOVE;
    evt.body.market_remove.doc_id = doc_id;
    broadcast_packet(&evt);
}

// ============================================================
// 핸들러: /sell
// ============================================================
static void handle_sell(int sock, const char *key, Packet *pkt) {
    int32_t npc_id = pkt->body.sell.npc_id;
    int32_t count  = pkt->body.sell.count;

    if (count <= 0 || count > MAX_INVEN_SIZE) {
        send_error(sock, ERR_TAG_MISMATCH, "잘못된 매각 요청 (개수 오류)");
        return;
    }

    // 락 순서 규약 — doc_id 오름차순 정렬로 Circular Wait 원천 차단
    int32_t doc_ids[MAX_INVEN_SIZE];
    memcpy(doc_ids, pkt->body.sell.doc_ids, count * sizeof(int32_t));
    qsort(doc_ids, count, sizeof(int32_t), cmp_int32);

    // 1. NPC 존재 확인
    NPCSlot npc;
    if (npc_find(npc_id, &npc) != 0) {
        send_error(sock, ERR_NPC_NOT_FOUND, "NPC 의뢰가 존재하지 않습니다.");
        return;
    }

    // 2. 유저 보유 검증 + 태그 합집합 계산 + 동결 차단
    uint32_t tag_union = 0;
    for (int i = 0; i < count; i++) {
        if (!sandbox_has(key, doc_ids[i])) {
            send_error(sock, ERR_DOC_NOT_FOUND, "보유하지 않은 문서가 포함됨");
            return;
        }
        DocFile df;
        if (sandbox_read_doc(key, doc_ids[i], &df) != 0) {
            send_error(sock, ERR_DOC_NOT_FOUND, "문서 메타 읽기 실패");
            return;
        }
        if (is_tag_frozen(df.tags)) {
            send_error(sock, ERR_DOC_FROZEN, "동결 태그 문서는 매각할 수 없습니다.");
            return;
        }
        tag_union |= df.tags;
    }

    // 3. 태그 조합 100% 일치 검증
    if (tag_union != npc.required_tags) {
        send_error(sock, ERR_TAG_MISMATCH, "태그 조합이 요구 사항과 일치하지 않습니다.");
        return;
    }

    // 4. NPC 원자적 점유 (선착순 매각 충돌 방어)
    if (npc_take(npc_id, &npc) != 0) {
        send_error(sock, ERR_NPC_ALREADY_TAKEN, "다른 유저가 먼저 매각했습니다.");
        return;
    }

    // 5. 보유 문서 파쇄 (정렬된 순서대로)
    for (int i = 0; i < count; i++) {
        sandbox_dispose(key, doc_ids[i]);
    }

    // 6. 보상 적립
    UserRecord rec;
    off_t offset;
    userdb_find(key, &rec, &offset);
    rec.money += npc.bounty;

    // 7. 승리 트리거 — 목표액 도달 시 자금 회수 + 게임 종료 브로드캐스트
    int is_winner = (rec.money >= GOAL_MONEY);
    if (is_winner) {
        rec.money = 0;  // 자금 강제 회수
    }
    userdb_update_at(offset, &rec);

    // 8. 응답
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_SELL_OK;
    res.body.sell_ok.bounty = npc.bounty;
    res.body.sell_ok.new_money = rec.money;
    packet_send(sock, &res);

    // 9. NPC 소멸 방송
    Packet despawn;
    memset(&despawn, 0, sizeof(Packet));
    despawn.type = PKT_EVT_NPC_DESPAWN;
    despawn.body.npc_despawn.npc_id = npc_id;
    broadcast_packet(&despawn);

    // 10. 승리자 발생 시 게임 오버 전체 방송 (Role A 트리거)
    if (is_winner) {
        Packet victory;
        memset(&victory, 0, sizeof(Packet));
        victory.type = PKT_EVT_VICTORY;
        snprintf(victory.body.endgame.message, MAX_TEXT_LEN,
                 "모든 빚을 갚았군. 약속대로 넌 이제 자유다.");
        unicast_to_key(key, &victory);

        Packet gameover;
        memset(&gameover, 0, sizeof(Packet));
        gameover.type = PKT_EVT_GAME_OVER;
        snprintf(gameover.body.endgame.message, MAX_TEXT_LEN,
                 "약속을 지키지 못했군. 남은 빚은 목숨으로 받지. (승리자: %s)", key);
        broadcast_packet(&gameover);
    }
}

// ============================================================
// 핸들러: /dispose
// ============================================================
static void handle_dispose(int sock, const char *key, Packet *pkt) {
    int32_t doc_id = pkt->body.dispose.doc_id;

    if (!sandbox_has(key, doc_id)) {
        send_error(sock, ERR_DOC_NOT_FOUND, "보유하지 않은 문서입니다.");
        return;
    }

    if (sandbox_dispose(key, doc_id) != 0) {
        send_error(sock, ERR_DOC_NOT_FOUND, "파기 실패 (시스템 에러)");
        return;
    }

    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_DISPOSE_OK;
    packet_send(sock, &res);
}

// ============================================================
// 핸들러: /inventory
// ============================================================
static void handle_inventory(int sock, const char *key) {
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_INVEN_INFO;

    res.body.inven_info.count = sandbox_list(key, res.body.inven_info.items, MAX_INVEN_SIZE);

    // 동결 상태 마킹
    for (int i = 0; i < res.body.inven_info.count; i++) {
        res.body.inven_info.items[i].is_frozen =
            is_tag_frozen(res.body.inven_info.items[i].tags);
    }

    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) == 0) {
        res.body.inven_info.money = rec.money;
    }

    packet_send(sock, &res);
}

// ============================================================
// 핸들러: /rumor — 수수료 차감 후 Role A에게 글리치 브로드캐스트 위임
// ============================================================
static void handle_rumor(int sock, const char *key, Packet *pkt) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "유저 정보 조회 실패");
        return;
    }
    if (rec.money < RUMOR_FEE) {
        send_error(sock, ERR_NOT_ENOUGH_MONEY, "사보타주 수수료가 부족합니다.");
        return;
    }

    rec.money -= RUMOR_FEE;
    userdb_update_at(offset, &rec);

    // 타겟에게 글리치 패킷 단일 전송 (Role A 영역이지만 호출은 여기서)
    Packet glitch;
    memset(&glitch, 0, sizeof(Packet));
    glitch.type = PKT_EVT_RUMOR_GLITCH;
    unicast_to_key(pkt->body.rumor.target_key, &glitch);

    printf("[Server] %s paid %d for /rumor against %s.\n",
           key, RUMOR_FEE, pkt->body.rumor.target_key);
}

// ============================================================
// 파산 처리: 자금이 0 이하가 되면 계정 소각 + 강제 퇴출
// ============================================================
static int check_bankruptcy(int sock, const char *key) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) return 0;
    if (rec.money > 0) return 0;

    userdb_burn_at(offset);

    Packet bust;
    memset(&bust, 0, sizeof(Packet));
    bust.type = PKT_EVT_GAME_OVER;
    snprintf(bust.body.endgame.message, MAX_TEXT_LEN,
             "파산했습니다. 약속을 지키지 못했군.");
    packet_send(sock, &bust);
    return 1;
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
                rec.money = INITIAL_MONEY;
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

            // 개인 샌드박스 생성
            sandbox_user_init(my_key);

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_LOGIN_OK;
            res.body.login_ok.assigned_session_id = sock;
            res.body.login_ok.money = rec.money;
            res.body.login_ok.goal_money = GOAL_MONEY;
            packet_send(sock, &res);

            // 늦은 접속자 동기화: 현재 시장 매물과 NPC 의뢰를 이 클라이언트에만 유니캐스트.
            {
                MarketSlot mkt_items[MAX_MARKET_SLOTS];
                int mkt_count = 0;
                market_snapshot(mkt_items, MAX_MARKET_SLOTS, &mkt_count);
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

                NPCSlot npc_orders[MAX_NPC_SLOTS];
                int npc_count = 0;
                npc_snapshot(npc_orders, MAX_NPC_SLOTS, &npc_count);
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

            printf("[Server] User '%s' logged in.\n", my_key);
        }
    }

    // 2. 메인 루프 — 게임 명령 디스패치
    while (1) {
        if (packet_recv(sock, &pkt) <= 0) {
            break;
        }

        switch (pkt.type) {
            case PKT_REQ_CHAT: {
                printf("[Server] Chat from %s: %s\n", my_key, pkt.body.chat.text);
                Packet evt;
                memset(&evt, 0, sizeof(Packet));
                evt.type = PKT_EVT_CHAT;
                strncpy(evt.body.chat_evt.text, pkt.body.chat.text, MAX_TEXT_LEN - 1);
                strncpy(evt.body.chat_evt.sender_key, my_key, MAX_KEY_LEN - 1);
                broadcast_packet(&evt);
                break;
            }
            case PKT_REQ_BUY:
                handle_buy(sock, my_key, &pkt);
                if (check_bankruptcy(sock, my_key)) goto disconnect;
                break;
            case PKT_REQ_SELL:
                handle_sell(sock, my_key, &pkt);
                break;
            case PKT_REQ_DISPOSE:
                handle_dispose(sock, my_key, &pkt);
                break;
            case PKT_REQ_INVEN:
                handle_inventory(sock, my_key);
                break;
            case PKT_REQ_RUMOR:
                handle_rumor(sock, my_key, &pkt);
                if (check_bankruptcy(sock, my_key)) goto disconnect;
                break;
            default:
                printf("[Server] Unknown packet type %d from %s\n", pkt.type, my_key);
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

// ============================================================
// 시드 데이터 — Role A의 이벤트 스레드가 완성되기 전까지 임시.
// A가 백그라운드 스레드를 붙이면 이 함수는 제거 가능.
// ============================================================
static void seed_market_and_npc(void) {
    // 마스터 디렉토리에 실제 파일 4개 생성 + 인메모리 매물 등록
    struct {
        int32_t  doc_id;
        uint32_t tags;
        int32_t  price;
        const char *name;
    } docs[] = {
        { 201, TAG_CORP_B   | TAG_FINANCE,  300, "재무제표 (B기업)" },
        { 202, TAG_GOVERNMENT | TAG_MILITARY, 500, "기밀도면 (국방부)" },
        { 203, TAG_PERSONAL | TAG_CUSTOMER, 100, "주민번호 (민간)" },
        { 204, TAG_CORP_A   | TAG_PERSONAL, 800, "녹취록 (A정치인)" },
    };
    for (size_t i = 0; i < sizeof(docs)/sizeof(docs[0]); i++) {
        DocFile df;
        memset(&df, 0, sizeof(df));
        df.doc_id = docs[i].doc_id;
        df.tags = docs[i].tags;
        df.base_price = docs[i].price;
        strncpy(df.name, docs[i].name, MAX_NAME_LEN - 1);
        master_write_doc(&df);
        market_add(docs[i].doc_id, docs[i].tags, docs[i].price, docs[i].name);
    }

    // NPC 의뢰 2건
    npc_add(11, TAG_CORP_B   | TAG_FINANCE,                1200);
    npc_add(12, TAG_GOVERNMENT | TAG_MILITARY,             4500);

    printf("[Server] Seeded %zu market docs and 2 NPC bounties.\n",
           sizeof(docs)/sizeof(docs[0]));
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

    // 샌드박스 디렉토리 초기화 (data, master, users)
    if (sandbox_global_init() < 0) {
        perror("Failed to initialize sandbox dirs");
        exit(1);
    }

    // 시장/NPC 상태 초기화 + 시드
    market_init();
    seed_market_and_npc();

    // 이벤트 스레드 파생 (매물·NPC 자동 스폰 및 에이징)
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
