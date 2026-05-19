#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_KEY_LEN     32
#define MAX_TEXT_LEN    256
#define MAX_NAME_LEN    64
#define MAX_INVEN_SIZE  5    // GDD 2-C: 인벤토리 최대 한도

// =============================================================
// 태그 비트마스크 정의 (최대 32개까지 확장 가능)
// =============================================================
typedef enum {
    TAG_CORP_A     = 1U << 0,    // 기업A
    TAG_CORP_B     = 1U << 1,    // 기업B
    TAG_CORP_C     = 1U << 2,    // 기업C
    TAG_CUSTOMER   = 1U << 3,    // 고객정보
    TAG_FINANCE    = 1U << 4,    // 금융
    TAG_MILITARY   = 1U << 5,    // 군사무기
    TAG_GOVERNMENT = 1U << 6,    // 정부기관
    TAG_MEDICAL    = 1U << 7,    // 의료
    TAG_RESEARCH   = 1U << 8,    // 연구개발
    TAG_PERSONAL   = 1U << 9,    // 사적정보
} Tag;

// 클라이언트 표시용 태그 라벨 매핑 (안전한 함수 형태)
const char* get_tag_name(uint32_t tag_bit);

// =============================================================
// 에러 코드
// =============================================================
typedef enum {
    ERR_NONE = 0,
    ERR_NOT_ENOUGH_MONEY,
    ERR_INVENTORY_FULL,
    ERR_DOC_NOT_FOUND,
    ERR_TAG_MISMATCH,
    ERR_NPC_NOT_FOUND,
    ERR_NPC_ALREADY_TAKEN,
    ERR_DOC_FROZEN,
    ERR_RUMOR_COOLDOWN,
    ERR_INVALID_SESSION,
    ERR_DUPLICATE_LOGIN,
    ERR_KEY_BURNED,
} ErrorCode;

// =============================================================
// 패킷 타입 정의
// =============================================================
typedef enum {
    PKT_REQ_LOGIN          = 100,
    PKT_REQ_CHAT,
    PKT_REQ_BUY,
    PKT_REQ_SELL,
    PKT_REQ_DISPOSE,
    PKT_REQ_INVEN,
    PKT_REQ_RUMOR,
    PKT_REQ_MINIGAME_SUBMIT,

    PKT_RES_LOGIN_OK       = 200,
    PKT_RES_LOGIN_FAIL,
    PKT_RES_INVEN_INFO,
    PKT_RES_BUY_OK,
    PKT_RES_SELL_OK,
    PKT_RES_DISPOSE_OK,
    PKT_RES_ERROR,
    PKT_RES_MINIGAME_OK,

    PKT_EVT_CHAT           = 300,
    PKT_EVT_MARKET_SPAWN,
    PKT_EVT_MARKET_REMOVE,
    PKT_EVT_NPC_SPAWN,
    PKT_EVT_NPC_DESPAWN,
    PKT_EVT_NEWS_LEAK,
    PKT_EVT_NEWS_RECOVER,
    PKT_EVT_RUMOR_GLITCH,
    PKT_EVT_POLICE_RAID,
    PKT_EVT_GAME_OVER,
    PKT_EVT_VICTORY,
    PKT_EVT_SCOREBOARD,
} PacketType;

// =============================================================
// 구조체 정의 (패딩 방지)
// =============================================================
#pragma pack(push, 1)

// 인벤토리 항목
typedef struct {
    int32_t  doc_id;
    uint32_t tags;
    char     name[MAX_NAME_LEN];
    int32_t  is_frozen;
} InvenItem;

// 메인 패킷 구조체
typedef struct {
    int32_t type;
    int32_t session_id;

    union {
        // ---------- 클라 → 서버 요청 ----------
        struct { char key[MAX_KEY_LEN]; } login;
        struct { char text[MAX_TEXT_LEN]; } chat;
        struct { int32_t doc_id; } buy;
        
        struct {
            int32_t npc_id;
            int32_t count;
            int32_t doc_ids[MAX_INVEN_SIZE];
        } sell;

        struct { int32_t doc_id; } dispose;
        // PKT_REQ_INVEN: body 없음
        struct { char passcode[MAX_TEXT_LEN]; } minigame;
        struct { char target_key[MAX_KEY_LEN]; } rumor; // 수정 반영

        // ---------- 서버 → 클라 응답 ----------
        struct {
            int32_t assigned_session_id;
            int32_t money;
            int32_t goal_money;
        } login_ok;

        struct {
            int32_t   count;
            InvenItem items[MAX_INVEN_SIZE];
            int32_t   money;
        } inven_info;

        struct {
            int32_t  doc_id;
            int32_t  remaining_money;
            uint32_t tags;
            char     name[MAX_NAME_LEN];
        } buy_ok;

        struct {
            int32_t bounty;
            int32_t new_money;
        } sell_ok;

        struct {
            int32_t error_code;
            char    reason[MAX_TEXT_LEN];
        } error;

        // ---------- 서버 → 클라 이벤트 ----------
        struct {
            char sender_key[MAX_KEY_LEN];
            char text[MAX_TEXT_LEN];
        } chat_evt;

        struct {
            int32_t  doc_id;
            uint32_t tags;
            int32_t  base_price;
            char     name[MAX_NAME_LEN];
        } market_spawn;

        struct { int32_t doc_id; } market_remove;

        struct {
            int32_t  npc_id;
            uint32_t required_tags;
            int32_t  bounty;
        } npc_spawn;

        struct { int32_t npc_id; } npc_despawn;

        struct {
            uint32_t frozen_tags;
            int32_t  duration_sec;
            char     headline[MAX_TEXT_LEN];
        } news_leak;

        struct { uint32_t recovered_tags; } news_recover;

        // PKT_EVT_RUMOR_GLITCH: body 없음

        struct {
            char     passcode[MAX_TEXT_LEN];
            int32_t  time_limit_sec;
        } police_raid;

        struct { char message[MAX_TEXT_LEN]; } endgame;

        struct {
            int32_t rank;           // 이 클라이언트의 순위 (1=우승)
            int32_t final_money;    // 최종 자산
            char    winner_key[MAX_KEY_LEN];
        } scoreboard;
    } body;
} Packet;

#pragma pack(pop)

// =============================================================
// 송수신 래퍼 함수 (구현부는 protocol.c에 캡슐화)
// =============================================================
int packet_send(int sockfd, const Packet *pkt);
int packet_recv(int sockfd, Packet *pkt);

#endif // PROTOCOL_H
