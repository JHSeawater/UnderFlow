#ifndef NPC_H
#define NPC_H

#include <stdint.h>

/*
 * npc 모듈 — NPC 의뢰 보드 인메모리 자료구조.
 *
 *   - npc_spawn / npc_despawn : A의 이벤트 스레드가 등록/만료
 *   - npc_take                : /sell 핸들러가 호출 — 선착순 1명만 성공
 *   - npc_lookup              : 정보 조회만 (제거 안 함)
 */

#define MAX_NPC_BOARD 16

typedef struct {
    int32_t  npc_id;
    uint32_t required_tags;   // 비트마스크 (정확히 일치해야 매각 가능)
    int32_t  bounty;
} NpcOrder;

int  npc_init(void);

// NPC 등록 (A 호출). 반환: 0 성공, -1 가득참/중복
int  npc_spawn(const NpcOrder *npc);

// NPC 강제 만료 (A의 aging).
int  npc_despawn(int32_t npc_id);

// /sell 시 선착순 매각 — 보드에서 즉시 제거하고 *out에 메타 반환.
// 반환: 0=성공, -1=없음/이미 다른 사람이 가져감
int  npc_take(int32_t npc_id, NpcOrder *out);

// 정보만 조회 (제거 안 함).
int  npc_lookup(int32_t npc_id, NpcOrder *out);

#endif
