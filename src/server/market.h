#ifndef MARKET_H
#define MARKET_H

#include <stdint.h>
#include "protocol.h"

#define MAX_MARKET_SLOTS 32
#define MAX_NPC_SLOTS    16

typedef struct {
    int32_t  doc_id;
    uint32_t tags;
    int32_t  base_price;
    char     name[MAX_NAME_LEN];
    int      active;
    int      is_frozen;
} MarketSlot;

typedef struct {
    int32_t  npc_id;
    uint32_t required_tags;
    int32_t  bounty;
    int      active;
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

// 공중파 유출 (Role A의 동결 타이머에서 호출 예정)
void freeze_tags(uint32_t tags);
void unfreeze_tags(uint32_t tags);
int  is_tag_frozen(uint32_t tags);

#endif
