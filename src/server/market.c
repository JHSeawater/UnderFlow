#include "market.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

static MarketSlot g_market[MAX_MARKET_SLOTS];
static NPCSlot    g_npc[MAX_NPC_SLOTS];
static uint32_t   g_frozen_tags = 0;

// 락 순서 규약: market → npc (혹시라도 두 락이 같이 필요할 때 항상 이 순서)
static pthread_mutex_t g_market_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_npc_mutex    = PTHREAD_MUTEX_INITIALIZER;

void market_init(void) {
    pthread_mutex_lock(&g_market_mutex);
    memset(g_market, 0, sizeof(g_market));
    g_frozen_tags = 0;
    pthread_mutex_unlock(&g_market_mutex);

    pthread_mutex_lock(&g_npc_mutex);
    memset(g_npc, 0, sizeof(g_npc));
    pthread_mutex_unlock(&g_npc_mutex);
}

int market_add(int32_t doc_id, uint32_t tags, int32_t price, const char *name) {
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (!g_market[i].active) {
            g_market[i].doc_id = doc_id;
            g_market[i].tags = tags;
            g_market[i].base_price = price;
            strncpy(g_market[i].name, name, MAX_NAME_LEN - 1);
            g_market[i].name[MAX_NAME_LEN - 1] = '\0';
            g_market[i].active = 1;
            g_market[i].is_frozen = (tags & g_frozen_tags) ? 1 : 0;
            g_market[i].spawn_time = time(NULL);
            g_market[i].lifespan_sec = MARKET_LIFE_MIN_SEC
                + rand() % (MARKET_LIFE_MAX_SEC - MARKET_LIFE_MIN_SEC + 1);
            pthread_mutex_unlock(&g_market_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return -1;
}

int market_find(int32_t doc_id, MarketSlot *out) {
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && g_market[i].doc_id == doc_id) {
            if (out) *out = g_market[i];
            pthread_mutex_unlock(&g_market_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return -1;
}

int market_take(int32_t doc_id, MarketSlot *out) {
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && g_market[i].doc_id == doc_id) {
            if (g_market[i].is_frozen) {
                pthread_mutex_unlock(&g_market_mutex);
                return -2;
            }
            if (out) *out = g_market[i];
            g_market[i].active = 0;
            pthread_mutex_unlock(&g_market_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return -1;
}

int npc_add(int32_t npc_id, uint32_t required_tags, int32_t bounty) {
    pthread_mutex_lock(&g_npc_mutex);
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (!g_npc[i].active) {
            g_npc[i].npc_id = npc_id;
            g_npc[i].required_tags = required_tags;
            g_npc[i].bounty = bounty;
            g_npc[i].active = 1;
            g_npc[i].spawn_time = time(NULL);
            pthread_mutex_unlock(&g_npc_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_npc_mutex);
    return -1;
}

int npc_find(int32_t npc_id, NPCSlot *out) {
    pthread_mutex_lock(&g_npc_mutex);
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (g_npc[i].active && g_npc[i].npc_id == npc_id) {
            if (out) *out = g_npc[i];
            pthread_mutex_unlock(&g_npc_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_npc_mutex);
    return -1;
}

int npc_take(int32_t npc_id, NPCSlot *out) {
    pthread_mutex_lock(&g_npc_mutex);
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (g_npc[i].active && g_npc[i].npc_id == npc_id) {
            if (out) *out = g_npc[i];
            g_npc[i].active = 0;
            pthread_mutex_unlock(&g_npc_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_npc_mutex);
    return -1;
}

void freeze_tags(uint32_t tags) {
    pthread_mutex_lock(&g_market_mutex);
    g_frozen_tags |= tags;
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && (g_market[i].tags & tags)) {
            g_market[i].is_frozen = 1;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
}

void unfreeze_tags(uint32_t tags) {
    pthread_mutex_lock(&g_market_mutex);
    g_frozen_tags &= ~tags;
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && (g_market[i].tags & tags) && !(g_market[i].tags & g_frozen_tags)) {
            g_market[i].is_frozen = 0;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
}

int is_tag_frozen(uint32_t tags) {
    pthread_mutex_lock(&g_market_mutex);
    int frozen = (tags & g_frozen_tags) ? 1 : 0;
    pthread_mutex_unlock(&g_market_mutex);
    return frozen;
}

int market_is_full(void) {
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (!g_market[i].active) {
            pthread_mutex_unlock(&g_market_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return 1;
}

int npc_is_full(void) {
    pthread_mutex_lock(&g_npc_mutex);
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (!g_npc[i].active) {
            pthread_mutex_unlock(&g_npc_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_npc_mutex);
    return 1;
}

static int popcount_u32(uint32_t v) {
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

// 요구 태그가 많을수록(어려울수록) 수명을 짧게 잡아, 충족하기 힘든 의뢰가 보드에
// 고착되지 않고 빨리 순환되게 한다. max_age = base - (태그수-1)*step, 하한 60초.
int npc_despawn_aged(int base_age_sec, int step_sec, int32_t *out_ids, int max_out, int *out_count) {
    pthread_mutex_lock(&g_npc_mutex);
    time_t now = time(NULL);
    int count = 0;
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (!g_npc[i].active) continue;
        int tagn = popcount_u32(g_npc[i].required_tags);
        int max_age = base_age_sec - (tagn - 1) * step_sec;
        if (max_age < 60) max_age = 60;
        if ((now - g_npc[i].spawn_time) > max_age) {
            if (count < max_out) {
                out_ids[count++] = g_npc[i].npc_id;
            }
            g_npc[i].active = 0;
        }
    }
    *out_count = count;
    pthread_mutex_unlock(&g_npc_mutex);
    return count;
}

uint32_t market_frozen_mask(void) {
    pthread_mutex_lock(&g_market_mutex);
    uint32_t mask = g_frozen_tags;
    pthread_mutex_unlock(&g_market_mutex);
    return mask;
}

// 수명(lifespan_sec)이 만료된 활성 매물을 소멸시키고 그 doc_id들을 반환한다.
// 동결된 매물은 익명 구매 대상에서 제외(시장 동결 중에는 거래 불가 컨셉 유지).
int market_despawn_aged(int32_t *out_ids, int max_out, int *out_count) {
    pthread_mutex_lock(&g_market_mutex);
    time_t now = time(NULL);
    int count = 0;
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && !g_market[i].is_frozen &&
            (now - g_market[i].spawn_time) > g_market[i].lifespan_sec) {
            if (count < max_out) {
                out_ids[count++] = g_market[i].doc_id;
            }
            g_market[i].active = 0;
        }
    }
    *out_count = count;
    pthread_mutex_unlock(&g_market_mutex);
    return count;
}

uint32_t market_active_tags(void) {
    pthread_mutex_lock(&g_market_mutex);
    uint32_t mask = 0;
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active) mask |= g_market[i].tags;
    }
    pthread_mutex_unlock(&g_market_mutex);
    return mask;
}

int market_snapshot(MarketSlot *out_array, int max_size, int *out_count) {
    pthread_mutex_lock(&g_market_mutex);
    int count = 0;
    for (int i = 0; i < MAX_MARKET_SLOTS; i++) {
        if (g_market[i].active && count < max_size) {
            out_array[count++] = g_market[i];
        }
    }
    *out_count = count;
    pthread_mutex_unlock(&g_market_mutex);
    return 0;
}

int npc_snapshot(NPCSlot *out_array, int max_size, int *out_count) {
    pthread_mutex_lock(&g_npc_mutex);
    int count = 0;
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (g_npc[i].active && count < max_size) {
            out_array[count++] = g_npc[i];
        }
    }
    *out_count = count;
    pthread_mutex_unlock(&g_npc_mutex);
    return 0;
}

// 묶음 매각 시 제출 문서 ID를 오름차순으로 정렬해 처리 순서를 결정적으로 고정한다.
// (실제 동시성 안전은 개별 뮤텍스를 중첩 없이 원자적으로 사용하는 구조에서 보장된다.)
static int cmp_int32_asc(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

void market_doc_sort_ids(int32_t *ids, int count) {
    if (!ids || count <= 1) return;
    qsort(ids, (size_t)count, sizeof(int32_t), cmp_int32_asc);
}
