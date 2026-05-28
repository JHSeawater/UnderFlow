#include "market.h"
#include <string.h>
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

int npc_despawn_aged(int max_age_sec, int32_t *out_ids, int max_out, int *out_count) {
    pthread_mutex_lock(&g_npc_mutex);
    time_t now = time(NULL);
    int count = 0;
    for (int i = 0; i < MAX_NPC_SLOTS; i++) {
        if (g_npc[i].active && (now - g_npc[i].spawn_time) > max_age_sec) {
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
