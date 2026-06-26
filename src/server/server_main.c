// 서버 메인

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include "protocol.h"
#include <fcntl.h>
#include "userdb/userdb.h"
#include "src/server/market.h"
#include "src/server/sandbox.h"
#include "src/server/events.h"

#define PORT             8080
#define MAX_CLIENTS      10
// INITIAL_MONEY, GOAL_MONEY는 protocol.h에서 공유
#define DEBT_RAISE_STEP  2000    // GDD §F: 탈출자 1명 발생 시 +2,000원 인상
#define DEBT_CAP         14000   // GDD §F: 목표 상환액 상한선
#define RUMOR_FEE          500
#define RUMOR_COOLDOWN_SEC  30   // GDD §B: 서버 공유 쿨다운 (연속 사용 차단)

int client_sockets[MAX_CLIENTS];
char active_keys[MAX_CLIENTS][MAX_KEY_LEN];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static int count_bits(uint32_t v) {
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

void broadcast_packet(Packet *pkt);
int  unicast_to_key(const char *target_key, Packet *pkt);
static void send_error(int sock, int32_t code, const char *reason);

// 라운드 전역 상태 (락 순서: g_round_mutex → clients_mutex)
pthread_mutex_t g_round_mutex = PTHREAD_MUTEX_INITIALIZER;
int32_t g_goal_money       = GOAL_MONEY;
static int     g_escaped_count    = 0;
static int     g_round_end_active = 0;   // 라운드 종료 카운트다운 진행 중 (중복 트리거 가드)
time_t  g_cap_reached_at   = 0;   // DEBT_CAP 최초 도달 시각 (0 = 미도달). 5분 경과 시 라운드 리셋 트리거.
#define ROUND_END_COUNTDOWN_SEC 60       // GDD §F: 스코어보드/에필로그 노출 시간

// GDD §F 탈출 로그 (g_round_mutex 보호)
typedef struct {
    char    key[MAX_KEY_LEN];
    int32_t money_at_escape;
    int32_t escape_order;
} EscapeLogEntry;
static EscapeLogEntry g_escape_log[MAX_SCORE_ESCAPED];

// /rumor 서버 공유 쿨다운 (GDD §B). g_round_mutex와 의미가 달라 별도 락 사용.
// 락 순서: g_rumor_mutex → g_userdb_mutex (수수료 차감 시).
static pthread_mutex_t g_rumor_mutex   = PTHREAD_MUTEX_INITIALIZER;
static time_t          g_rumor_cd_until = 0;

// §D 주기 경찰 레이드 (라운드 종료 §F와 완전 독립)
// 락 순서: g_periodic_raid_mutex → clients_mutex (스냅샷용)
#define PERIODIC_RAID_TIME_LIMIT_SEC  15
#define PERIODIC_RAID_MIN_INTERVAL    360   // 6분
#define PERIODIC_RAID_MAX_INTERVAL    600   // 10분
static pthread_mutex_t g_periodic_raid_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t          g_periodic_raid_deadline = 0;   // 0 = no active raid
static char            g_periodic_raid_pending_keys[MAX_CLIENTS][MAX_KEY_LEN];
static char            g_periodic_raid_passcode[MAX_TEXT_LEN]; // 현재 레이드 우회 암호 (위 뮤텍스 보호)

// 레이드마다 무작위로 하나 선택되는 우회 암호 후보 (난이도 ↑)
static const char *RAID_PASSCODES[] = {
    "pUrge", "OverRidE", "BLAckOUt", "FiRewALL",
    "GHOsT", "ZeROdAy", "LoCkDOWN", "SHadOW", "UNdErFLOW", "pASSWORD", "DEADLINE", "dEBUG", 
    "WhITeHaT", "BLAcKHAt", "BLAckBoX", "WhITeBOx"
};
#define RAID_PASSCODE_COUNT ((int)(sizeof(RAID_PASSCODES) / sizeof(RAID_PASSCODES[0])))

static const char* pick_raid_passcode(void) {
    return RAID_PASSCODES[rand() % RAID_PASSCODE_COUNT];
}

// 시드 데이터 재투입은 파일 하단에 정의돼 있어 전방 선언이 필요함
static void seed_market_and_npc(void);

// ============================================================
// 라운드 리셋 시퀀스 (GDD §F):
//   "1분 후 모든 유저 데이터(빚 10,000원, 초기자본 1,000원)와 마켓을
//    원래 상태로 깨끗이 리셋하여 새로운 라운드를 자동 개시"
// 호출 경로:
//   1) 누적 3명 탈출 → fire_round_end_countdown → round_end_timer_thread → 60초 후
//   2) DEBT_CAP 도달 5분 경과 → event_thread → fire_round_end_countdown → 60초 후
// 클라이언트는 endgame.message의 "__ROUND_RESET__|" 마커로 종료가 아닌
// 라운드 리셋임을 구분한다.
// ============================================================
static void trigger_round_reset(void) {
    // 1. 실물 파일(마스터/샌드박스 doc_*.dat) 일괄 청소
    sandbox_global_reset();

    // 2. users.dat 활성 레코드 → 초기자본 복원 (소각 -1은 보존)
    userdb_reset_round(INITIAL_MONEY);

    // 3. 인메모리 시장/NPC/동결 마스크 초기화
    market_init();

    // 4. 라운드 상태 변수 복원
    pthread_mutex_lock(&g_round_mutex);
    g_goal_money       = GOAL_MONEY;
    g_escaped_count    = 0;
    g_round_end_active = 0;
    g_cap_reached_at   = 0;
    memset(g_escape_log, 0, sizeof(g_escape_log));   // 탈출 로그 초기화
    pthread_mutex_unlock(&g_round_mutex);

    // /rumor 쿨다운도 라운드와 함께 초기화
    pthread_mutex_lock(&g_rumor_mutex);
    g_rumor_cd_until = 0;
    pthread_mutex_unlock(&g_rumor_mutex);

    // 5. 시드 매물·NPC 재투입 (라이브 매물이 한 개도 없는 빈 마켓 방지)
    seed_market_and_npc();

    // 6. 클라이언트에 라운드 리셋 마커 broadcast
    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_EVT_GAME_OVER;
    snprintf(pkt.body.endgame.message, MAX_TEXT_LEN,
             "__ROUND_RESET__|새 라운드가 시작되었습니다. 모든 자산이 초기 상태로 복원되었습니다.");
    broadcast_packet(&pkt);

    // 7. 시드된 매물·NPC를 모든 접속 클라에 동기화 broadcast
    {
        MarketSlot mkt_items[MAX_MARKET_SLOTS];
        int mkt_count = 0;
        market_snapshot(mkt_items, MAX_MARKET_SLOTS, &mkt_count);
        for (int i = 0; i < mkt_count; i++) {
            Packet sp;
            memset(&sp, 0, sizeof(Packet));
            sp.type = PKT_EVT_MARKET_SPAWN;
            sp.body.market_spawn.doc_id     = mkt_items[i].doc_id;
            sp.body.market_spawn.tags       = mkt_items[i].tags;
            sp.body.market_spawn.base_price = mkt_items[i].base_price;
            strncpy(sp.body.market_spawn.name, mkt_items[i].name, MAX_NAME_LEN - 1);
            broadcast_packet(&sp);
        }

        NPCSlot npc_items[MAX_NPC_SLOTS];
        int npc_count = 0;
        npc_snapshot(npc_items, MAX_NPC_SLOTS, &npc_count);
        for (int i = 0; i < npc_count; i++) {
            Packet sp;
            memset(&sp, 0, sizeof(Packet));
            sp.type = PKT_EVT_NPC_SPAWN;
            sp.body.npc_spawn.npc_id        = npc_items[i].npc_id;
            sp.body.npc_spawn.required_tags = npc_items[i].required_tags;
            sp.body.npc_spawn.bounty        = npc_items[i].bounty;
            broadcast_packet(&sp);
        }
    }

    printf("[Server] === ROUND RESET === goal=$%d, escaped=0\n", GOAL_MONEY);
}

// 라운드 종료 카운트다운 종료 후 자동 라운드 리셋 (GDD §F).
// 호출 전 g_round_end_active=1을 트리거 측이 락 안에서 세팅해 둠.
static void* round_end_timer_thread(void* arg) {
    (void)arg;
    sleep(ROUND_END_COUNTDOWN_SEC);

    int should_fire = 0;
    pthread_mutex_lock(&g_round_mutex);
    if (g_round_end_active) {
        should_fire = 1;
        // g_round_end_active 리셋은 trigger_round_reset이 수행
    }
    pthread_mutex_unlock(&g_round_mutex);

    if (should_fire) {
        printf("[Server] Round End Countdown expired -> ROUND RESET\n");
        trigger_round_reset();
    }
    return NULL;
}

// ============================================================
// §D 주기 경찰 레이드 — 개별 처리 미니게임
// ============================================================

// 특정 KEY의 계정/샌드박스를 영구 소각하고 단일 GAME_OVER를 송신.
// 호출자는 어떤 락도 보유하지 말 것 (userdb/sandbox 자체 락 사용).
static void burn_user_by_key(const char *key) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) == 0) {
        userdb_burn_at(offset);    // money=-1 + 인벤 클리어
    }
    sandbox_purge_user(key);       // 실제 doc_*.dat 일괄 unlink

    Packet go;
    memset(&go, 0, sizeof(Packet));
    go.type = PKT_EVT_GAME_OVER;
    snprintf(go.body.endgame.message, MAX_TEXT_LEN,
             "경찰 추적망에 포착되었습니다! 계정 데이터가 영구 소각됩니다.");
    unicast_to_key(key, &go);

    printf("[Server] BURN user='%s' (raid failure)\n", key);
}

// raid 발사 후 제한 시간이 지나면 미응답자를 일괄 소각한다.
// 호출자가 락 안에서 g_periodic_raid_deadline 세팅 + pending_keys 채워둠.
static void* periodic_raid_timeout_thread(void* arg) {
    (void)arg;
    sleep(PERIODIC_RAID_TIME_LIMIT_SEC);

    // pending list 스냅샷 (락 안에서) → 해제 후 소각 처리
    char victims[MAX_CLIENTS][MAX_KEY_LEN];
    int  victim_cnt = 0;

    pthread_mutex_lock(&g_periodic_raid_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_periodic_raid_pending_keys[i][0] != 0) {
            strncpy(victims[victim_cnt], g_periodic_raid_pending_keys[i], MAX_KEY_LEN - 1);
            victims[victim_cnt][MAX_KEY_LEN - 1] = '\0';
            victim_cnt++;
            g_periodic_raid_pending_keys[i][0] = 0;
        }
    }
    g_periodic_raid_deadline = 0;
    pthread_mutex_unlock(&g_periodic_raid_mutex);

    for (int i = 0; i < victim_cnt; i++) {
        burn_user_by_key(victims[i]);
    }
    return NULL;
}

// event_thread가 6~10분마다 호출. 이미 진행 중인 raid가 있으면 skip.
void fire_periodic_police_raid(void) {
    char passcode[MAX_TEXT_LEN];
    pthread_mutex_lock(&g_periodic_raid_mutex);
    if (g_periodic_raid_deadline != 0) {
        pthread_mutex_unlock(&g_periodic_raid_mutex);
        return;
    }
    g_periodic_raid_deadline = time(NULL) + PERIODIC_RAID_TIME_LIMIT_SEC;
    strncpy(g_periodic_raid_passcode, pick_raid_passcode(), MAX_TEXT_LEN - 1);
    g_periodic_raid_passcode[MAX_TEXT_LEN - 1] = '\0';
    strncpy(passcode, g_periodic_raid_passcode, MAX_TEXT_LEN - 1);
    passcode[MAX_TEXT_LEN - 1] = '\0';

    // 현재 active 유저 KEY 스냅샷 → pending 목록
    memset(g_periodic_raid_pending_keys, 0, sizeof(g_periodic_raid_pending_keys));
    pthread_mutex_lock(&clients_mutex);
    int pending_cnt = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && active_keys[i][0] != 0) {
            strncpy(g_periodic_raid_pending_keys[pending_cnt],
                    active_keys[i], MAX_KEY_LEN - 1);
            g_periodic_raid_pending_keys[pending_cnt][MAX_KEY_LEN - 1] = '\0';
            pending_cnt++;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    pthread_mutex_unlock(&g_periodic_raid_mutex);

    // 접속자 0명이면 raid 의미 없음 — 롤백
    if (pending_cnt == 0) {
        pthread_mutex_lock(&g_periodic_raid_mutex);
        g_periodic_raid_deadline = 0;
        pthread_mutex_unlock(&g_periodic_raid_mutex);
        return;
    }

    // 락 밖에서 broadcast + 타이머 스레드 파생
    Packet raid;
    memset(&raid, 0, sizeof(Packet));
    raid.type = PKT_EVT_POLICE_RAID;
    strncpy(raid.body.police_raid.passcode, passcode, MAX_TEXT_LEN - 1);
    raid.body.police_raid.time_limit_sec = PERIODIC_RAID_TIME_LIMIT_SEC;
    broadcast_packet(&raid);

    Packet hint;
    memset(&hint, 0, sizeof(Packet));
    hint.type = PKT_EVT_CHAT;
    strncpy(hint.body.chat_evt.sender_key, "[ALERT]", MAX_KEY_LEN - 1);
    snprintf(hint.body.chat_evt.text, MAX_TEXT_LEN - 1,
             "경찰 추적망 활성화! %d초 안에 우회 암호 '%.32s'를 정확히 입력하지 못하면 계정이 영구 소각됩니다.",
             PERIODIC_RAID_TIME_LIMIT_SEC, passcode);
    broadcast_packet(&hint);

    pthread_t tid;
    if (pthread_create(&tid, NULL, periodic_raid_timeout_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        // 파생 실패 — raid 상태 롤백 (영구 미응답 방지)
        pthread_mutex_lock(&g_periodic_raid_mutex);
        g_periodic_raid_deadline = 0;
        memset(g_periodic_raid_pending_keys, 0, sizeof(g_periodic_raid_pending_keys));
        pthread_mutex_unlock(&g_periodic_raid_mutex);
    }

    printf("[Event] Periodic Police Raid fired — %d users pending (%ds)\n",
           pending_cnt, PERIODIC_RAID_TIME_LIMIT_SEC);
}

// 디버그/시연용: 요청자 자신만 대상으로 §D 경찰 레이드를 즉시 발사한다.
// fire_periodic_police_raid와 동일한 공유 상태(g_periodic_raid_*)를 사용하므로
// 이미 진행 중인 레이드가 있으면 skip한다. 이후 PURGE 탈출·오답 소각·타임아웃은
// 모두 기존 handle_minigame_submit + periodic_raid_timeout_thread 경로를 그대로 탄다.
static void fire_self_police_raid(int sock, const char *key) {
    char passcode[MAX_TEXT_LEN];
    pthread_mutex_lock(&g_periodic_raid_mutex);
    if (g_periodic_raid_deadline != 0) {
        pthread_mutex_unlock(&g_periodic_raid_mutex);
        send_error(sock, ERR_INVALID_SESSION, "이미 진행 중인 경찰 레이드가 있습니다.");
        return;
    }
    g_periodic_raid_deadline = time(NULL) + PERIODIC_RAID_TIME_LIMIT_SEC;
    strncpy(g_periodic_raid_passcode, pick_raid_passcode(), MAX_TEXT_LEN - 1);
    g_periodic_raid_passcode[MAX_TEXT_LEN - 1] = '\0';
    strncpy(passcode, g_periodic_raid_passcode, MAX_TEXT_LEN - 1);
    passcode[MAX_TEXT_LEN - 1] = '\0';
    memset(g_periodic_raid_pending_keys, 0, sizeof(g_periodic_raid_pending_keys));
    strncpy(g_periodic_raid_pending_keys[0], key, MAX_KEY_LEN - 1);
    g_periodic_raid_pending_keys[0][MAX_KEY_LEN - 1] = '\0';
    pthread_mutex_unlock(&g_periodic_raid_mutex);

    Packet raid;
    memset(&raid, 0, sizeof(Packet));
    raid.type = PKT_EVT_POLICE_RAID;
    strncpy(raid.body.police_raid.passcode, passcode, MAX_TEXT_LEN - 1);
    raid.body.police_raid.time_limit_sec = PERIODIC_RAID_TIME_LIMIT_SEC;
    packet_send(sock, &raid);

    pthread_t tid;
    if (pthread_create(&tid, NULL, periodic_raid_timeout_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        // 파생 실패 — raid 상태 롤백 (영구 미응답 방지)
        pthread_mutex_lock(&g_periodic_raid_mutex);
        g_periodic_raid_deadline = 0;
        memset(g_periodic_raid_pending_keys, 0, sizeof(g_periodic_raid_pending_keys));
        pthread_mutex_unlock(&g_periodic_raid_mutex);
        send_error(sock, ERR_INVALID_SESSION, "레이드 타이머 생성 실패.");
        return;
    }

    printf("[Debug] Self police raid fired for '%s' (%ds)\n",
           key, PERIODIC_RAID_TIME_LIMIT_SEC);
}

// GDD §F 라운드 종료 카운트다운: 스코어보드 데이터 수집 + broadcast + 60초 후 라운드 리셋.
// 중복 발사 방지를 위해 g_round_end_active를 내부에서 선점한다(첫 호출만 진행).
// 스레드 파생 실패 시 g_round_end_active를 롤백한다.
// §D 주기 경찰 레이드(PKT_EVT_POLICE_RAID + 미니게임)와는 완전 별개 시퀀스.
void fire_round_end_countdown(const char *announce_text) {
    // 0. 중복 발사 방지: g_round_end_active를 선점한 첫 호출만 진행 (나머지는 즉시 반환).
    //    payoff(3인 탈출)·DEBT_CAP 만기·디버그 트리거가 겹쳐 들어와도 한 번만 발동된다.
    pthread_mutex_lock(&g_round_mutex);
    if (g_round_end_active) {
        pthread_mutex_unlock(&g_round_mutex);
        return;
    }
    g_round_end_active = 1;
    pthread_mutex_unlock(&g_round_mutex);

    // 1. 탈출 로그 스냅샷 (g_round_mutex 한 번만 잡음)
    ScoreEscaped escaped_snap[MAX_SCORE_ESCAPED];
    int escaped_snap_cnt = 0;
    pthread_mutex_lock(&g_round_mutex);
    escaped_snap_cnt = g_escaped_count < MAX_SCORE_ESCAPED
                       ? g_escaped_count : MAX_SCORE_ESCAPED;
    for (int i = 0; i < escaped_snap_cnt; i++) {
        memset(&escaped_snap[i], 0, sizeof(ScoreEscaped));
        strncpy(escaped_snap[i].key, g_escape_log[i].key, MAX_KEY_LEN - 1);
        escaped_snap[i].money_at_escape = g_escape_log[i].money_at_escape;
        escaped_snap[i].escape_order    = g_escape_log[i].escape_order;
    }
    pthread_mutex_unlock(&g_round_mutex);

    // 2. 접속자 KEY 스냅샷 (clients_mutex 보유 최소화)
    char snap_keys[MAX_CLIENTS][MAX_KEY_LEN];
    int  snap_cnt = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_sockets[i] != 0 && active_keys[i][0] != 0 && snap_cnt < MAX_CLIENTS) {
            strncpy(snap_keys[snap_cnt], active_keys[i], MAX_KEY_LEN - 1);
            snap_keys[snap_cnt][MAX_KEY_LEN - 1] = '\0';
            snap_cnt++;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    // 3. 잔액 조회 — 탈출자는 제외 (GDD §F: 스코어보드는 남은 플레이어 전용)
    //    클라 disconnect 처리 전이라 active_keys에 탈출자가 잔존할 수 있는 race
    //    window를 명시적으로 차단.
    ScoreRemaining rank[MAX_CLIENTS];
    int rank_cnt = 0;
    for (int i = 0; i < snap_cnt; i++) {
        int is_escaped = 0;
        for (int j = 0; j < escaped_snap_cnt; j++) {
            if (strncmp(snap_keys[i], escaped_snap[j].key, MAX_KEY_LEN) == 0) {
                is_escaped = 1;
                break;
            }
        }
        if (is_escaped) continue;

        UserRecord rec;
        off_t offset;
        if (userdb_find(snap_keys[i], &rec, &offset) == 0 && !userdb_is_burned(&rec)) {
            strncpy(rank[rank_cnt].key, snap_keys[i], MAX_KEY_LEN - 1);
            rank[rank_cnt].key[MAX_KEY_LEN - 1] = '\0';
            rank[rank_cnt].money = rec.money;
            rank_cnt++;
        }
    }

    // 4. 잔액 내림차순 정렬 (selection sort — N≤10이라 충분)
    for (int i = 0; i + 1 < rank_cnt; i++) {
        int max_idx = i;
        for (int j = i + 1; j < rank_cnt; j++) {
            if (rank[j].money > rank[max_idx].money) max_idx = j;
        }
        if (max_idx != i) {
            ScoreRemaining t = rank[i]; rank[i] = rank[max_idx]; rank[max_idx] = t;
        }
    }

    // 5. 스코어보드 패킷 작성
    Packet sb;
    memset(&sb, 0, sizeof(Packet));
    sb.type = PKT_EVT_SCOREBOARD;
    sb.body.scoreboard.countdown_sec = ROUND_END_COUNTDOWN_SEC;

    sb.body.scoreboard.escaped_count = escaped_snap_cnt;
    for (int i = 0; i < escaped_snap_cnt; i++) {
        sb.body.scoreboard.escaped[i] = escaped_snap[i];
    }

    int r_cnt = rank_cnt < MAX_SCORE_REMAINING ? rank_cnt : MAX_SCORE_REMAINING;
    sb.body.scoreboard.remaining_count = r_cnt;
    for (int i = 0; i < r_cnt; i++) {
        sb.body.scoreboard.remaining[i] = rank[i];
    }

    broadcast_packet(&sb);

    // 5. 안내 채팅
    Packet hint;
    memset(&hint, 0, sizeof(Packet));
    hint.type = PKT_EVT_CHAT;
    strncpy(hint.body.chat_evt.sender_key, "[ALERT]", MAX_KEY_LEN - 1);
    strncpy(hint.body.chat_evt.text, announce_text, MAX_TEXT_LEN - 1);
    broadcast_packet(&hint);

    // 6. 60초 타이머 파생
    pthread_t tid;
    if (pthread_create(&tid, NULL, round_end_timer_thread, NULL) == 0) {
        pthread_detach(tid);
    } else {
        pthread_mutex_lock(&g_round_mutex);
        g_round_end_active = 0;
        pthread_mutex_unlock(&g_round_mutex);
    }

    printf("[Server] Round End Countdown fired: %d escaped, %d remaining\n", escaped_snap_cnt, r_cnt);
}

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

    // 제출 문서 ID를 오름차순 정렬해 처리 순서를 결정적으로 고정한다.
    int32_t doc_ids[MAX_INVEN_SIZE];
    memcpy(doc_ids, pkt->body.sell.doc_ids, count * sizeof(int32_t));
    market_doc_sort_ids(doc_ids, count);

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

    // 3. 부분집합 검증 (GDD §A): NPC 요구 태그 ⊆ 제출 태그 합집합
    //    초과 태그가 있어도 통과하되, 5단계에서 해당 문서까지 함께 소모됨
    if ((tag_union & npc.required_tags) != npc.required_tags) {
        send_error(sock, ERR_TAG_MISMATCH, "태그 조합이 요구 사항을 충족하지 않습니다.");
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

    // 7. 자금 업데이트
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
}

// ============================================================
// 핸들러: /payoff — 수동 채무 상환 및 누적 3명 시 경찰 습격 트리거
// ----------------------------------------------------------------
// 락 순서 규약: g_round_mutex 안에서만 라운드 상태 R/W. broadcast 등
// 외부 I/O는 락 해제 후 수행하여 보유 시간 최소화.
// ============================================================
static void handle_payoff(int sock, const char *key) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "유저 정보 조회 실패");
        return;
    }

    int32_t prev_goal, next_goal;
    int     escaped_now;
    int     should_trigger_raid = 0;

    pthread_mutex_lock(&g_round_mutex);

    if (rec.money < g_goal_money) {
        int32_t goal_snapshot = g_goal_money;
        pthread_mutex_unlock(&g_round_mutex);
        char err_msg[MAX_TEXT_LEN];
        snprintf(err_msg, sizeof(err_msg), "상환금 부족: 목표 청산액은 $%d 이나, 당신은 $%d 보유하고 있습니다.",
                 goal_snapshot, rec.money);
        send_error(sock, ERR_NOT_ENOUGH_MONEY, err_msg);
        return;
    }

    prev_goal = g_goal_money;
    next_goal = prev_goal + DEBT_RAISE_STEP;
    if (next_goal > DEBT_CAP) next_goal = DEBT_CAP;
    g_goal_money = next_goal;

    // 상한선 최초 도달 시각 기록 (event_thread가 +5분 후 라운드 리셋 발사)
    if (next_goal == DEBT_CAP && g_cap_reached_at == 0) {
        g_cap_reached_at = time(NULL);
    }

    g_escaped_count++;
    escaped_now = g_escaped_count;

    // GDD §F 탈출 로그 기록 (최대 3명)
    if (escaped_now <= MAX_SCORE_ESCAPED) {
        int idx = escaped_now - 1;
        strncpy(g_escape_log[idx].key, key, MAX_KEY_LEN - 1);
        g_escape_log[idx].key[MAX_KEY_LEN - 1] = '\0';
        g_escape_log[idx].money_at_escape = prev_goal;   // 청산 직전 잔액 = 도달했던 목표액
        g_escape_log[idx].escape_order    = escaped_now;
    }

    if (escaped_now >= 3) {
        should_trigger_raid = 1;   // 실제 중복 발사 가드는 fire_round_end_countdown 내부에서 처리
    }

    pthread_mutex_unlock(&g_round_mutex);

    // 탈출 완료 → 계정을 다음 판을 위해 초기화 (userdb/sandbox 자체 락 — 라운드 락 밖).
    // 인벤토리 전량 파기 + 잔고 초기자본 복원 → 같은 키로 재접속 시 깨끗한 새 게임으로 시작.
    sandbox_purge_user(key);
    rec.money = INITIAL_MONEY;
    userdb_update_at(offset, &rec);

    // 1. 본인에게 승리 탈출 전송
    Packet victory;
    memset(&victory, 0, sizeof(Packet));
    victory.type = PKT_EVT_VICTORY;
    snprintf(victory.body.endgame.message, MAX_TEXT_LEN,
             "축하합니다! $%d의 채무를 성공적으로 청산하고 다크웹을 탈출하셨습니다.", prev_goal);
    packet_send(sock, &victory);

    // 2. 전역 안내 브로드캐스트
    Packet evt;
    memset(&evt, 0, sizeof(Packet));
    evt.type = PKT_EVT_CHAT;
    strncpy(evt.body.chat_evt.sender_key, "[SYSTEM]", MAX_KEY_LEN - 1);
    if (next_goal == prev_goal) {
        snprintf(evt.body.chat_evt.text, MAX_TEXT_LEN - 1,
                 "해커 '%s'님이 $%d의 빚을 갚고 탈출했습니다! 목표 상환액은 상한선 $%d에 고정됩니다.",
                 key, prev_goal, DEBT_CAP);
    } else {
        snprintf(evt.body.chat_evt.text, MAX_TEXT_LEN - 1,
                 "해커 '%s'님이 $%d의 빚을 갚고 탈출했습니다! 은행 보안 강화로 다음 목표 상환액이 $%d로 인상됩니다.",
                 key, prev_goal, next_goal);
    }
    broadcast_packet(&evt);

    // 인상된 목표 상환액을 전 클라이언트에 구조화 전송 → 남은 유저 UI 목표액 즉시 갱신
    Packet goal_evt;
    memset(&goal_evt, 0, sizeof(Packet));
    goal_evt.type = PKT_EVT_GOAL_UPDATE;
    goal_evt.body.goal_update.goal_money = next_goal;
    broadcast_packet(&goal_evt);

    printf("[Server] User '%s' paid off! Escaped count: %d, New Goal: %d\n",
           key, escaped_now, next_goal);

    // 3. 누적 3명 탈출 시 라운드 종료 카운트다운 (락 안에서 should_trigger_raid 결정됨)
    if (should_trigger_raid) {
        fire_round_end_countdown(
            "누적 3명 탈출로 라운드가 종료됩니다! "
            "60초 뒤 라운드가 강제 리셋됩니다. 마지막 에필로그를 나누십시오.");
    }
}

// ============================================================
// 핸들러: /fake rich (디버그/시연) — 잔액을 목표 상환액까지 채워 즉시 /payoff 가능하게 함
// ============================================================
static void handle_fake_rich(int sock, const char *key) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "유저 정보 조회 실패");
        return;
    }

    pthread_mutex_lock(&g_round_mutex);
    int32_t goal = g_goal_money;
    pthread_mutex_unlock(&g_round_mutex);

    rec.money = goal;                 // 목표액만큼 채움 → 즉시 /payoff 가능
    userdb_update_at(offset, &rec);

    // 잔액/인벤 갱신 응답 (클라가 INVEN_INFO로 잔고 표시를 갱신)
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_INVEN_INFO;
    res.body.inven_info.count = sandbox_list(key, res.body.inven_info.items, MAX_INVEN_SIZE);
    for (int i = 0; i < res.body.inven_info.count; i++) {
        res.body.inven_info.items[i].is_frozen =
            is_tag_frozen(res.body.inven_info.items[i].tags);
    }
    res.body.inven_info.money = rec.money;
    packet_send(sock, &res);

    // 본인에게 안내 채팅
    Packet note;
    memset(&note, 0, sizeof(Packet));
    note.type = PKT_EVT_CHAT;
    strncpy(note.body.chat_evt.sender_key, "[DEBUG]", MAX_KEY_LEN - 1);
    snprintf(note.body.chat_evt.text, MAX_TEXT_LEN - 1,
             "잔액을 $%d로 채웠습니다. /payoff 로 탈출을 테스트하세요.", rec.money);
    packet_send(sock, &note);

    printf("[Debug] fake rich: '%s' money set to %d\n", key, rec.money);
}

// ============================================================
// 핸들러: /fake endround (디버그/시연) — 라운드 종료 카운트다운 즉시 발사
// ============================================================
static void handle_fake_endround(void) {
    // 중복 발사 가드는 fire_round_end_countdown 내부에서 처리 (이미 진행 중이면 무시됨)
    fire_round_end_countdown(
        "(DEBUG) 강제 라운드 종료가 발동되었습니다! 60초 뒤 라운드가 리셋됩니다.");
    printf("[Debug] fake endround requested\n");
}

// ============================================================
// 핸들러: /minigame_submit (경찰 습격 방탈출 제출)
// ============================================================
static void handle_minigame_submit(int sock, const char *key, Packet *pkt) {
    // ── §D 주기 경찰 레이드 우선 처리 (개별 응답) ──────────────
    char current_passcode[MAX_TEXT_LEN];
    current_passcode[0] = '\0';
    pthread_mutex_lock(&g_periodic_raid_mutex);
    int periodic_active = (g_periodic_raid_deadline != 0
                           && time(NULL) < g_periodic_raid_deadline);
    int pending_idx = -1;
    if (periodic_active) {
        strncpy(current_passcode, g_periodic_raid_passcode, MAX_TEXT_LEN - 1);
        current_passcode[MAX_TEXT_LEN - 1] = '\0';
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_periodic_raid_pending_keys[i][0] != 0 &&
                strncmp(g_periodic_raid_pending_keys[i], key, MAX_KEY_LEN) == 0) {
                pending_idx = i;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_periodic_raid_mutex);

    if (periodic_active && pending_idx >= 0) {
        if (strcmp(pkt->body.minigame.passcode, current_passcode) == 0) {
            pthread_mutex_lock(&g_periodic_raid_mutex);
            g_periodic_raid_pending_keys[pending_idx][0] = 0;
            pthread_mutex_unlock(&g_periodic_raid_mutex);

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_MINIGAME_OK;
            packet_send(sock, &res);

            printf("[Server] User '%s' resolved periodic raid (passcode '%s' OK)\n",
                   key, current_passcode);
        } else {
            // 오답 → 해당 유저만 즉시 영구 소각 (GDD §D)
            pthread_mutex_lock(&g_periodic_raid_mutex);
            g_periodic_raid_pending_keys[pending_idx][0] = 0;
            pthread_mutex_unlock(&g_periodic_raid_mutex);

            burn_user_by_key(key);
            // burn_user_by_key가 PKT_EVT_GAME_OVER 단일 전송. 클라가 종료하면 client_handler가 자연 정리.
        }
        return;
    }

    // §D 외에는 미니게임 입력이 의미 없음. §F 라운드 종료 카운트다운에는
    // PURGE 우회 경로가 존재하지 않으므로(미니게임 = §D 전용) 단순 거절.
    send_error(sock, ERR_INVALID_SESSION, "현재 활성화된 미니게임이 없습니다.");
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
// 핸들러: /rumor — 서버 공유 쿨다운 + 수수료 차감 + 타겟 글리치 단일 전송
// 락 순서: g_rumor_mutex → g_userdb_mutex (userdb_find/update 내부)
// ============================================================
static void handle_rumor(int sock, const char *key, Packet *pkt) {
    char target_key[MAX_KEY_LEN];
    strncpy(target_key, pkt->body.rumor.target_key, MAX_KEY_LEN - 1);
    target_key[MAX_KEY_LEN - 1] = '\0';

    pthread_mutex_lock(&g_rumor_mutex);

    time_t now = time(NULL);
    if (now < g_rumor_cd_until) {
        int remain = (int)(g_rumor_cd_until - now);
        pthread_mutex_unlock(&g_rumor_mutex);
        char msg[MAX_TEXT_LEN];
        snprintf(msg, sizeof(msg),
                 "사보타주 쿨다운 중입니다. %d초 후 다시 시도하십시오.", remain);
        send_error(sock, ERR_RUMOR_COOLDOWN, msg);
        return;
    }

    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) {
        pthread_mutex_unlock(&g_rumor_mutex);
        send_error(sock, ERR_INVALID_SESSION, "유저 정보 조회 실패");
        return;
    }
    if (rec.money < RUMOR_FEE) {
        pthread_mutex_unlock(&g_rumor_mutex);
        send_error(sock, ERR_NOT_ENOUGH_MONEY, "사보타주 수수료가 부족합니다.");
        return;
    }

    rec.money -= RUMOR_FEE;
    userdb_update_at(offset, &rec);
    g_rumor_cd_until = now + RUMOR_COOLDOWN_SEC;

    pthread_mutex_unlock(&g_rumor_mutex);

    // 락 밖에서 타겟에게 글리치 패킷 단일 전송 (clients_mutex와 nested 회피)
    Packet glitch;
    memset(&glitch, 0, sizeof(Packet));
    glitch.type = PKT_EVT_RUMOR_GLITCH;
    unicast_to_key(target_key, &glitch);

    printf("[Server] %s paid %d for /rumor against %s (cooldown %ds).\n",
           key, RUMOR_FEE, target_key, RUMOR_COOLDOWN_SEC);

    // 수수료 차감 결과(잔고)를 요청자에게 즉시 반영 (인벤 정보 재전송으로 my_money 갱신)
    handle_inventory(sock, key);
}

// ============================================================
// 파산 처리: GDD §F — 자금(money)==0 AND 인벤토리==비어있음 일 때만 발동
// (자금 0이라도 매각 가능한 문서가 남아 있으면 파산으로 보지 않음)
// ============================================================
static int check_bankruptcy(int sock, const char *key) {
    UserRecord rec;
    off_t offset;
    if (userdb_find(key, &rec, &offset) != 0) return 0;
    if (rec.money > 0) return 0;

    // 인벤토리에 매각 가능한 문서가 남아 있으면 파산 아님 (GDD §F)
    int inv = sandbox_count(key);
    if (inv > 0) return 0;
    // opendir 실패(inv<0)는 안전하게 파산 보류로 간주

    if (inv < 0) return 0;

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

            pthread_mutex_lock(&g_round_mutex);
            int32_t goal_snapshot = g_goal_money;
            pthread_mutex_unlock(&g_round_mutex);

            Packet res;
            memset(&res, 0, sizeof(Packet));
            res.type = PKT_RES_LOGIN_OK;
            res.body.login_ok.assigned_session_id = sock;
            res.body.login_ok.money = rec.money;
            res.body.login_ok.goal_money = goal_snapshot;
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

                // 현재 동결 상태 동기화: 진행 중인 공중파 유출이 있으면 늦은 접속자도
                // 즉시 FRZ를 보도록 NEWS_LEAK(현재 mask)을 단일 전송한다.
                // (위 MARKET_SPAWN/NPC_SPAWN 다음에 보내야 클라가 재마킹할 수 있음)
                uint32_t cur_frozen = market_frozen_mask();
                if (cur_frozen != 0) {
                    Packet fz;
                    memset(&fz, 0, sizeof(Packet));
                    fz.type = PKT_EVT_NEWS_LEAK;
                    fz.body.news_leak.frozen_tags  = cur_frozen;
                    fz.body.news_leak.duration_sec = 0;
                    strncpy(fz.body.news_leak.headline,
                            "현재 동결 상태 동기화 (진행 중인 공중파 유출)", MAX_TEXT_LEN - 1);
                    packet_send(sock, &fz);
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
                if (strcmp(pkt.body.chat.text, "/payoff") == 0) {
                    handle_payoff(sock, my_key);
                } else {
                    Packet evt;
                    memset(&evt, 0, sizeof(Packet));
                    evt.type = PKT_EVT_CHAT;
                    strncpy(evt.body.chat_evt.text, pkt.body.chat.text, MAX_TEXT_LEN - 1);
                    strncpy(evt.body.chat_evt.sender_key, my_key, MAX_KEY_LEN - 1);
                    broadcast_packet(&evt);
                }
                break;
            }
            case PKT_REQ_MINIGAME_SUBMIT:
                handle_minigame_submit(sock, my_key, &pkt);
                break;
            case PKT_REQ_TRIGGER_RAID:
                fire_self_police_raid(sock, my_key);
                break;
            case PKT_REQ_TRIGGER_LEAK:
                // 디버그/시연: 공중파 유출을 즉시 발사 (전체 broadcast — 실제 이벤트와 동일)
                if (fire_public_leak() == 0) {
                    send_error(sock, ERR_DOC_NOT_FOUND,
                               "동결할 매물 태그가 없습니다 (매물이 없거나 이미 전부 동결).");
                }
                break;
            case PKT_REQ_TRIGGER_RICH:
                handle_fake_rich(sock, my_key);
                break;
            case PKT_REQ_TRIGGER_ENDROUND:
                handle_fake_endround();
                break;
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
        const char *name;
    } docs[] = {
        { 124, TAG_CORP_B   | TAG_FINANCE,    "바이오젠 기밀폐기 재무제표" },
        { 111, TAG_GOVERNMENT | TAG_MILITARY, "연방정부 전술 드론 설계도" },
        { 47, TAG_PERSONAL | TAG_CUSTOMER,   "퇴직 요원 신상정보" },
        { 70, TAG_CORP_A   | TAG_PERSONAL,   "아사사카 임직원 비리 증거" },
    };
    for (size_t i = 0; i < sizeof(docs)/sizeof(docs[0]); i++) {
        // 레버 A: 시드도 동일 공식 (지터 없이 결정적). 4종 모두 2태그 → $800
        int32_t price = count_bits(docs[i].tags) * MARKET_PRICE_PER_TAG;
        DocFile df;
        memset(&df, 0, sizeof(df));
        df.doc_id = docs[i].doc_id;
        df.tags = docs[i].tags;
        df.base_price = price;
        strncpy(df.name, docs[i].name, MAX_NAME_LEN - 1);
        master_write_doc(&df);
        market_add(docs[i].doc_id, docs[i].tags, price, docs[i].name);
    }

    // NPC 의뢰 2건 (보상은 런타임 공식 (rand()%5+태그수)*500 범위 내로: 2태그 → $1,000~3,000)
    npc_add(102, TAG_CORP_B   | TAG_FINANCE,                1200);   // 2태그 저~중
    npc_add(356, TAG_GOVERNMENT | TAG_MILITARY,             2500);   // 2태그 중~상 (프리미엄 타깃)

    printf("[Server] Seeded %zu market docs and 2 NPC bounties.\n",
           sizeof(docs)/sizeof(docs[0]));
}

// SIGTERM/SIGINT 진입 시 정상 종료 경로(atexit + leak 리포트 트리거).
static void on_shutdown_signal(int sig) {
    (void)sig;
    exit(0);
}

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_size;

    signal(SIGTERM, on_shutdown_signal);
    signal(SIGINT,  on_shutdown_signal);
    signal(SIGPIPE, SIG_IGN);   // EPIPE는 send 반환값으로 처리 (MSG_NOSIGNAL과 이중 방어)

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
    if (events_start_thread() != 0) {
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
                int* sock_ptr = malloc(sizeof(int));
                if (sock_ptr == NULL) break;          // OOM — added=0 유지 → 아래서 거부 처리
                *sock_ptr = client_sock;

                pthread_t t_id;
                if (pthread_create(&t_id, NULL, client_handler, sock_ptr) != 0) {
                    free(sock_ptr);                   // 스레드 생성 실패 — 슬롯 미점유, 메모리 회수
                    break;                            // added=0 유지 → 소켓 close
                }
                pthread_detach(t_id);

                // 핸들러 파생 성공 후에만 슬롯 점유 (실패 시 슬롯/소켓 누수 방지)
                client_sockets[i] = client_sock;
                added = 1;
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
