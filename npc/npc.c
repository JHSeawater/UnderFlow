#include "npc.h"

#include <string.h>
#include <pthread.h>

typedef struct {
    int32_t   in_use;
    NpcOrder  order;
} NpcSlot;

static NpcSlot          g_board[MAX_NPC_BOARD];
static pthread_mutex_t  g_npc_mutex = PTHREAD_MUTEX_INITIALIZER;

int npc_init(void){
    memset(g_board, 0, sizeof(g_board));
    return 0;
}

static int find_slot_by_id(int32_t npc_id){
    for (int i = 0; i < MAX_NPC_BOARD; i++) {
        if (g_board[i].in_use && g_board[i].order.npc_id == npc_id) return i;
    }
    return -1;
}

static int find_empty_slot(void){
    for (int i = 0; i < MAX_NPC_BOARD; i++) {
        if (!g_board[i].in_use) return i;
    }
    return -1;
}

int npc_spawn(const NpcOrder *npc){
    if (!npc) return -1;
    pthread_mutex_lock(&g_npc_mutex);

    if (find_slot_by_id(npc->npc_id) >= 0) {
        pthread_mutex_unlock(&g_npc_mutex);
        return -1;
    }
    int idx = find_empty_slot();
    if (idx < 0) {
        pthread_mutex_unlock(&g_npc_mutex);
        return -1;
    }
    g_board[idx].in_use = 1;
    g_board[idx].order  = *npc;

    pthread_mutex_unlock(&g_npc_mutex);
    return 0;
}

int npc_despawn(int32_t npc_id){
    pthread_mutex_lock(&g_npc_mutex);
    int idx = find_slot_by_id(npc_id);
    if (idx < 0) {
        pthread_mutex_unlock(&g_npc_mutex);
        return -1;
    }
    g_board[idx].in_use = 0;
    memset(&g_board[idx].order, 0, sizeof(NpcOrder));
    pthread_mutex_unlock(&g_npc_mutex);
    return 0;
}

int npc_take(int32_t npc_id, NpcOrder *out){
    pthread_mutex_lock(&g_npc_mutex);
    int idx = find_slot_by_id(npc_id);
    if (idx < 0) {
        // 다른 유저가 이미 가져갔거나 만료됨
        pthread_mutex_unlock(&g_npc_mutex);
        return -1;
    }
    if (out) *out = g_board[idx].order;
    g_board[idx].in_use = 0;
    memset(&g_board[idx].order, 0, sizeof(NpcOrder));
    pthread_mutex_unlock(&g_npc_mutex);
    return 0;
}

int npc_lookup(int32_t npc_id, NpcOrder *out){
    pthread_mutex_lock(&g_npc_mutex);
    int idx = find_slot_by_id(npc_id);
    int ret = -1;
    if (idx >= 0) {
        if (out) *out = g_board[idx].order;
        ret = 0;
    }
    pthread_mutex_unlock(&g_npc_mutex);
    return ret;
}

int npc_snapshot(NpcOrder *orders, int max_orders, int *out_count){
    if (!orders || !out_count || max_orders <= 0) return -1;
    pthread_mutex_lock(&g_npc_mutex);
    int count = 0;
    for (int i = 0; i < MAX_NPC_BOARD && count < max_orders; i++) {
        if (g_board[i].in_use) orders[count++] = g_board[i].order;
    }
    *out_count = count;
    pthread_mutex_unlock(&g_npc_mutex);
    return 0;
}
