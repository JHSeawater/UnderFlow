#include "market.h"

#include <string.h>
#include <pthread.h>

// =================================================================
// 인메모리 시장 상태
// =================================================================

typedef struct {
    int32_t  in_use;       // 0=빈 슬롯, 1=매물 있음
    DocMeta  doc;
} MarketSlot;

static MarketSlot       g_market[MAX_MARKET_ITEMS];
static int              g_market_count = 0;
static pthread_mutex_t  g_market_mutex = PTHREAD_MUTEX_INITIALIZER;

// 동결 태그 비트마스크
static uint32_t         g_frozen_mask = 0;
static pthread_mutex_t  g_frozen_mutex = PTHREAD_MUTEX_INITIALIZER;

// doc_id별 락 — /sell 다중 매물 잠금에 사용
static pthread_mutex_t  g_doc_locks[MAX_DOC_ID];
static int              g_doc_locks_inited = 0;

// =================================================================
// 초기화
// =================================================================

int market_init(void){
    memset(g_market, 0, sizeof(g_market));
    g_market_count = 0;

    g_frozen_mask = 0;

    if (!g_doc_locks_inited) {
        for (int i = 0; i < MAX_DOC_ID; i++) {
            pthread_mutex_init(&g_doc_locks[i], NULL);
        }
        g_doc_locks_inited = 1;
    }
    return 0;
}

// =================================================================
// 매물 슬롯 헬퍼 (g_market_mutex 잡힌 상태에서 호출)
// =================================================================

static int find_slot_by_doc_id(int32_t doc_id){
    for (int i = 0; i < MAX_MARKET_ITEMS; i++) {
        if (g_market[i].in_use && g_market[i].doc.doc_id == doc_id) return i;
    }
    return -1;
}

static int find_empty_slot(void){
    for (int i = 0; i < MAX_MARKET_ITEMS; i++) {
        if (!g_market[i].in_use) return i;
    }
    return -1;
}

// =================================================================
// 공개 API
// =================================================================

int market_spawn(const DocMeta *doc){
    if (!doc) return -1;
    pthread_mutex_lock(&g_market_mutex);

    if (find_slot_by_doc_id(doc->doc_id) >= 0) {
        // 이미 있는 doc_id — 스폰 거절
        pthread_mutex_unlock(&g_market_mutex);
        return -1;
    }
    int idx = find_empty_slot();
    if (idx < 0) {
        pthread_mutex_unlock(&g_market_mutex);
        return -1;
    }
    g_market[idx].in_use = 1;
    g_market[idx].doc    = *doc;
    g_market_count++;

    pthread_mutex_unlock(&g_market_mutex);
    return 0;
}

int market_remove(int32_t doc_id){
    pthread_mutex_lock(&g_market_mutex);
    int idx = find_slot_by_doc_id(doc_id);
    if (idx < 0) {
        pthread_mutex_unlock(&g_market_mutex);
        return -1;
    }
    g_market[idx].in_use = 0;
    memset(&g_market[idx].doc, 0, sizeof(DocMeta));
    g_market_count--;
    pthread_mutex_unlock(&g_market_mutex);
    return 0;
}

int market_buy(int32_t doc_id, DocMeta *out){
    pthread_mutex_lock(&g_market_mutex);
    int idx = find_slot_by_doc_id(doc_id);
    if (idx < 0) {
        pthread_mutex_unlock(&g_market_mutex);
        return -1;
    }
    DocMeta snapshot = g_market[idx].doc;
    pthread_mutex_unlock(&g_market_mutex);

    // 동결 검사 (별도 락이라 잡았다 풀고 재검사)
    if (market_is_doc_frozen(snapshot.tags)) return -2;

    // 다시 잡고 in_use 토글 + 카운트 감소 (선착순 1명만 성공)
    pthread_mutex_lock(&g_market_mutex);
    idx = find_slot_by_doc_id(doc_id);
    if (idx < 0) {
        // 다른 스레드가 가로챔
        pthread_mutex_unlock(&g_market_mutex);
        return -1;
    }
    if (out) *out = g_market[idx].doc;
    g_market[idx].in_use = 0;
    memset(&g_market[idx].doc, 0, sizeof(DocMeta));
    g_market_count--;
    pthread_mutex_unlock(&g_market_mutex);
    return 0;
}

int market_lookup(int32_t doc_id, DocMeta *out){
    pthread_mutex_lock(&g_market_mutex);
    int idx = find_slot_by_doc_id(doc_id);
    int ret = -1;
    if (idx >= 0) {
        if (out) *out = g_market[idx].doc;
        ret = 0;
    }
    pthread_mutex_unlock(&g_market_mutex);
    return ret;
}

int market_is_full(void){
    pthread_mutex_lock(&g_market_mutex);
    int full = (g_market_count >= MAX_MARKET_ITEMS) ? 1 : 0;
    pthread_mutex_unlock(&g_market_mutex);
    return full;
}

// =================================================================
// 동결 태그
// =================================================================

void market_freeze_tags(uint32_t tags){
    pthread_mutex_lock(&g_frozen_mutex);
    g_frozen_mask |= tags;
    pthread_mutex_unlock(&g_frozen_mutex);
}

void market_recover_tags(uint32_t tags){
    pthread_mutex_lock(&g_frozen_mutex);
    g_frozen_mask &= ~tags;
    pthread_mutex_unlock(&g_frozen_mutex);
}

int market_is_doc_frozen(uint32_t doc_tags){
    pthread_mutex_lock(&g_frozen_mutex);
    int frozen = (g_frozen_mask & doc_tags) ? 1 : 0;
    pthread_mutex_unlock(&g_frozen_mutex);
    return frozen;
}

uint32_t market_frozen_mask(void){
    pthread_mutex_lock(&g_frozen_mutex);
    uint32_t mask = g_frozen_mask;
    pthread_mutex_unlock(&g_frozen_mutex);
    return mask;
}

// =================================================================
// doc_id별 락 (Circular Wait 차단)
// =================================================================

static int doc_id_to_slot(int32_t doc_id){
    // 음수도 안전하게 0..MAX_DOC_ID-1 범위로
    int s = (int)(((uint32_t)doc_id) % (uint32_t)MAX_DOC_ID);
    return s;
}

void market_doc_lock(int32_t doc_id){
    pthread_mutex_lock(&g_doc_locks[doc_id_to_slot(doc_id)]);
}

void market_doc_unlock(int32_t doc_id){
    pthread_mutex_unlock(&g_doc_locks[doc_id_to_slot(doc_id)]);
}

// 슬롯 인덱스 배열을 중복 없이 채우고 오름차순 정렬
static int collect_unique_sorted_slots(const int32_t *doc_ids, int n, int *out_slots){
    int count = 0;
    for (int i = 0; i < n; i++) {
        int s = doc_id_to_slot(doc_ids[i]);
        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (out_slots[j] == s) { dup = 1; break; }
        }
        if (!dup) out_slots[count++] = s;
    }
    // 삽입 정렬 (n <= MAX_INVEN_SIZE = 5이라 충분)
    for (int i = 1; i < count; i++) {
        int key = out_slots[i];
        int j = i - 1;
        while (j >= 0 && out_slots[j] > key) {
            out_slots[j + 1] = out_slots[j];
            j--;
        }
        out_slots[j + 1] = key;
    }
    return count;
}

void market_doc_lock_many(const int32_t *doc_ids, int n){
    if (!doc_ids || n <= 0) return;
    int slots[MAX_INVEN_SIZE];
    int count = collect_unique_sorted_slots(doc_ids, n, slots);
    // ★ 오름차순 정렬된 순서로 lock — Circular Wait 원천 차단
    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&g_doc_locks[slots[i]]);
    }
}

void market_doc_unlock_many(const int32_t *doc_ids, int n){
    if (!doc_ids || n <= 0) return;
    int slots[MAX_INVEN_SIZE];
    int count = collect_unique_sorted_slots(doc_ids, n, slots);
    // unlock은 순서 무관 — 그냥 슬롯별로 해제
    for (int i = 0; i < count; i++) {
        pthread_mutex_unlock(&g_doc_locks[slots[i]]);
    }
}
