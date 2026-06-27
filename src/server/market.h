#ifndef MARKET_H
#define MARKET_H

#include <stdint.h>
#include <time.h>
#include "protocol.h"

#define MAX_MARKET_SLOTS 32
#define MAX_NPC_SLOTS    16

// 매물 자연 소멸 수명 범위 (익명 구매자 컨셉) — market_add가 이 범위 내 무작위 수명 부여
#define MARKET_LIFE_MIN_SEC 40
#define MARKET_LIFE_MAX_SEC 90

typedef struct {
    int32_t  doc_id;
    uint32_t tags;
    int32_t  base_price;
    char     name[MAX_NAME_LEN];
    int      active;
    int      is_frozen;
    time_t   spawn_time;
    int      lifespan_sec;   // 이 시간 경과 시 익명 구매로 소멸
} MarketSlot;

typedef struct {
    int32_t  npc_id;
    uint32_t required_tags;
    int32_t  bounty;
    int      active;
    time_t   spawn_time;
} NPCSlot;

void market_init(void);

// Role A가 이벤트 스레드에서 호출. B 측은 시드용으로만 사용.
int  market_add(int32_t doc_id, uint32_t tags, int32_t price, const char *name);
int  npc_add(int32_t npc_id, uint32_t required_tags, int32_t bounty);

// Role B가 /buy /sell 핸들러에서 사용.
int  market_find(int32_t doc_id, MarketSlot *out);
int  market_take(int32_t doc_id, MarketSlot *out);   // 원자적 제거 (선착순 독점)
int  npc_find(int32_t npc_id, NPCSlot *out);
int  npc_take(int32_t npc_id, NPCSlot *out);         // 원자적 점유 (선착순 매각)

// 이벤트 스레드 및 늦은 접속자 동기화 헬퍼
int  market_is_full(void);
int  npc_is_full(void);
int  npc_despawn_aged(int base_age_sec, int step_sec, int32_t *out_ids, int max_out, int *out_count);
int  market_despawn_aged(int32_t *out_ids, int max_out, int *out_count);  // 수명 만료 매물 소멸
uint32_t market_frozen_mask(void);
uint32_t market_active_tags(void);   // 현재 활성 매물에 존재하는 태그 합집합
int  market_snapshot(MarketSlot *out_array, int max_size, int *out_count);
int  npc_snapshot(NPCSlot *out_array, int max_size, int *out_count);

// 공중파 유출 — 태그 동결/해제
void freeze_tags(uint32_t tags);
void unfreeze_tags(uint32_t tags);
int  is_tag_frozen(uint32_t tags);

// 다수 문서 ID를 in-place 오름차순 정렬한다. count<=1이면 no-op.
// 묶음 매각 시 문서 처리 순서를 결정적으로 고정해 두는 헬퍼.
void market_doc_sort_ids(int32_t *ids, int count);

#endif
