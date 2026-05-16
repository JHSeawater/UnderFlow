#include "handlers.h"

#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "userdb/userdb.h"
#include "sandbox/sandbox.h"
#include "market/market.h"
#include "npc/npc.h"

// /rumor 글로벌 쿨다운 (단순 카운터 기반. 정확한 wall-clock 쿨다운은 추후 시간 도입)
#define RUMOR_FEE         500
#define RUMOR_COOLDOWN    1     // 동시 호출 1회만 허용하는 단순 게이트

static pthread_mutex_t g_rumor_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_rumor_busy  = 0;


// =================================================================
// 응답 헬퍼
// =================================================================

static void send_error(int sock, ErrorCode code, const char *reason){
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_ERROR;
    res.body.error.error_code = code;
    if (reason) strncpy(res.body.error.reason, reason, MAX_TEXT_LEN - 1);
    packet_send(sock, &res);
}


// =================================================================
// /buy 핸들러
// =================================================================

static void handle_buy(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    int32_t doc_id = pkt->body.buy.doc_id;

    // 1) 매물 락 (doc_id 단건이라 lock_many 안 써도 됨)
    market_doc_lock(doc_id);

    // 2) 매물 인수 (시장에서 제거하고 메타 복사)
    DocMeta doc;
    int rc = market_buy(doc_id, &doc);
    if (rc == -1) {
        market_doc_unlock(doc_id);
        send_error(sock, ERR_DOC_NOT_FOUND, "존재하지 않는 매물입니다.");
        return;
    }
    if (rc == -2) {
        market_doc_unlock(doc_id);
        send_error(sock, ERR_DOC_FROZEN, "동결된 매물은 구매할 수 없습니다.");
        return;
    }

    // 3) 유저 DB 갱신
    UserRecord rec;
    if (userdb_find(my_key, &rec, NULL) != 0) {
        // 매물을 시장에 되돌려놓고 에러
        market_spawn(&doc);
        market_doc_unlock(doc_id);
        send_error(sock, ERR_INVALID_SESSION, "사용자 레코드를 찾을 수 없습니다.");
        return;
    }

    if (userdb_inventory_full(&rec)) {
        market_spawn(&doc);
        market_doc_unlock(doc_id);
        send_error(sock, ERR_INVENTORY_FULL, "인벤토리가 가득 찼습니다.");
        return;
    }
    if (rec.money < doc.base_price) {
        market_spawn(&doc);
        market_doc_unlock(doc_id);
        send_error(sock, ERR_NOT_ENOUGH_MONEY, "자본금이 부족합니다.");
        return;
    }

    rec.money -= doc.base_price;
    userdb_inventory_add(&rec, doc.doc_id);

    if (userdb_update_at(my_offset, &rec) != 0) {
        market_spawn(&doc);
        market_doc_unlock(doc_id);
        send_error(sock, ERR_INVALID_SESSION, "DB 갱신 실패.");
        return;
    }

    // 4) 실물 파일 이동 (./data/master/<id>.dat → ./data/users/<key>/<id>.dat)
    if (sandbox_buy_move(doc_id, my_key) != 0) {
        // 파일 이동 실패 → 자본금/인벤토리 롤백
        rec.money += doc.base_price;
        userdb_inventory_remove(&rec, doc.doc_id);
        userdb_update_at(my_offset, &rec);
        market_spawn(&doc);
        market_doc_unlock(doc_id);
        send_error(sock, ERR_DOC_NOT_FOUND, "매물 파일 이동 실패.");
        return;
    }

    market_doc_unlock(doc_id);

    // 5) 구매자에게 응답
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_BUY_OK;
    res.body.buy_ok.doc_id          = doc.doc_id;
    res.body.buy_ok.remaining_money = rec.money;
    res.body.buy_ok.tags            = doc.tags;
    strncpy(res.body.buy_ok.name, doc.name, MAX_NAME_LEN - 1);
    packet_send(sock, &res);

    // 6) 전 유저에게 매물 제거 알림
    Packet evt;
    memset(&evt, 0, sizeof(Packet));
    evt.type = PKT_EVT_MARKET_REMOVE;
    evt.body.market_remove.doc_id = doc.doc_id;
    broadcast_packet(&evt);

    // 7) 자본금 0 도달 시 파산 처리 (자본금 다 까먹은 경우)
    if (rec.money == 0) {
        userdb_burn_at(my_offset);
    }
}


// =================================================================
// /sell 핸들러 — 다중 매물 묶기 + 태그 조합 검증 + 선착순 매각
// =================================================================

static void handle_sell(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    int32_t npc_id = pkt->body.sell.npc_id;
    int32_t count  = pkt->body.sell.count;
    if (count <= 0 || count > MAX_INVEN_SIZE) {
        send_error(sock, ERR_DOC_NOT_FOUND, "잘못된 매각 요청 (개수 범위 오류).");
        return;
    }

    // 패킷에서 doc_ids 복사
    int32_t doc_ids[MAX_INVEN_SIZE] = {0};
    for (int i = 0; i < count; i++) doc_ids[i] = pkt->body.sell.doc_ids[i];

    // ★ Circular Wait 차단: doc_id별 락을 오름차순 정렬 후 잠금
    market_doc_lock_many(doc_ids, count);

    // 1) NPC 선착순 매각 — 가장 먼저 도달한 1명만 take 성공
    NpcOrder npc;
    if (npc_take(npc_id, &npc) != 0) {
        market_doc_unlock_many(doc_ids, count);
        send_error(sock, ERR_NPC_ALREADY_TAKEN, "이미 거래된 NPC 의뢰입니다.");
        return;
    }

    // 2) 유저 DB 조회
    UserRecord rec;
    if (userdb_find(my_key, &rec, NULL) != 0) {
        // NPC를 보드에 되돌려놓기
        npc_spawn(&npc);
        market_doc_unlock_many(doc_ids, count);
        send_error(sock, ERR_INVALID_SESSION, "사용자 레코드를 찾을 수 없습니다.");
        return;
    }

    // 3) 인벤토리에 doc_ids가 모두 있는지 + 태그 합집합 계산 + 동결 검사
    uint32_t combined_tags = 0;
    for (int i = 0; i < count; i++) {
        if (!userdb_inventory_has(&rec, doc_ids[i])) {
            npc_spawn(&npc);
            market_doc_unlock_many(doc_ids, count);
            send_error(sock, ERR_DOC_NOT_FOUND, "인벤토리에 없는 문서가 포함됨.");
            return;
        }

        // 유저 폴더의 메타 읽어 태그 합집합
        DocMeta meta;
        if (sandbox_user_read_meta(my_key, doc_ids[i], &meta) != 0) {
            npc_spawn(&npc);
            market_doc_unlock_many(doc_ids, count);
            send_error(sock, ERR_DOC_NOT_FOUND, "문서 파일 누락 (메타 읽기 실패).");
            return;
        }

        // 동결된 태그 포함 시 매각 거절
        if (market_is_doc_frozen(meta.tags)) {
            npc_spawn(&npc);
            market_doc_unlock_many(doc_ids, count);
            send_error(sock, ERR_DOC_FROZEN, "동결된 태그가 포함되어 있어 매각 불가.");
            return;
        }

        combined_tags |= meta.tags;
    }

    // 4) ★ 태그 조합 검증: 합집합 == NPC 요구 태그 (한 비트도 어긋나면 거절)
    if (combined_tags != npc.required_tags) {
        npc_spawn(&npc);
        market_doc_unlock_many(doc_ids, count);
        send_error(sock, ERR_TAG_MISMATCH, "태그 조합이 요구사항과 일치하지 않습니다.");
        return;
    }

    // 5) 실물 파일 unlink 먼저 (FS-first 순서)
    //    이미 sandbox_user_read_meta로 존재 확인했고 doc_lock도 잡혀 있어서
    //    unlink 실패는 거의 발생 안 함. 실패 시 NPC는 이미 take되어 롤백 불가 →
    //    로그만 남기고 거래는 계속. 잔여 고스트 파일은 운영/관리자가 정리.
    for (int i = 0; i < count; i++) {
        if (sandbox_user_unlink(my_key, doc_ids[i]) != 0) {
            fprintf(stderr,
                    "[sell] unlink 실패: key=%s doc_id=%d (고스트 파일 가능)\n",
                    my_key, doc_ids[i]);
        }
    }

    // 6) 거래 성립 — 인벤토리에서 제거 + 자본금 += 현상금
    for (int i = 0; i < count; i++) {
        userdb_inventory_remove(&rec, doc_ids[i]);
    }
    rec.money += npc.bounty;

    // 7) users.dat 갱신
    if (userdb_update_at(my_offset, &rec) != 0) {
        // 매우 드문 경우. 파일은 이미 지워졌고 NPC도 take됨 → 복구 불가.
        market_doc_unlock_many(doc_ids, count);
        send_error(sock, ERR_INVALID_SESSION, "DB 갱신 실패 — 운영팀 문의.");
        return;
    }

    market_doc_unlock_many(doc_ids, count);

    // 8) 응답 + 매각 NPC 디스폰 브로드캐스트
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_SELL_OK;
    res.body.sell_ok.bounty    = npc.bounty;
    res.body.sell_ok.new_money = rec.money;
    packet_send(sock, &res);

    Packet evt;
    memset(&evt, 0, sizeof(Packet));
    evt.type = PKT_EVT_NPC_DESPAWN;
    evt.body.npc_despawn.npc_id = npc.npc_id;
    broadcast_packet(&evt);

    // 9) ★ 승리 트리거 검사 — 목표 도달 시 자금 강제 전송 후 게임 종료 신호
    if (rec.money >= GOAL_MONEY) {
        rec.money = 0;
        userdb_update_at(my_offset, &rec);
        server_trigger_victory(sock);   // A 영역 함수
    }
}


// =================================================================
// /dispose 핸들러 — 보상 0원 강제 파쇄
// =================================================================

static void handle_dispose(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    int32_t doc_id = pkt->body.dispose.doc_id;

    market_doc_lock(doc_id);

    UserRecord rec;
    if (userdb_find(my_key, &rec, NULL) != 0) {
        market_doc_unlock(doc_id);
        send_error(sock, ERR_INVALID_SESSION, "사용자 레코드를 찾을 수 없습니다.");
        return;
    }
    if (!userdb_inventory_has(&rec, doc_id)) {
        market_doc_unlock(doc_id);
        send_error(sock, ERR_DOC_NOT_FOUND, "인벤토리에 없는 문서입니다.");
        return;
    }

    userdb_inventory_remove(&rec, doc_id);
    userdb_update_at(my_offset, &rec);
    sandbox_user_unlink(my_key, doc_id);

    market_doc_unlock(doc_id);

    // 응답 (보상 없음, money 그대로)
    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_DISPOSE_OK;
    packet_send(sock, &res);
}


// =================================================================
// /inventory 핸들러 — opendir/readdir 순회
// =================================================================

static void handle_inventory(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    (void)pkt;

    UserRecord rec;
    if (userdb_find(my_key, &rec, NULL) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "사용자 레코드를 찾을 수 없습니다.");
        return;
    }
    (void)my_offset;

    // 실물 파일을 읽어 메타 가져옴 (단순히 users.dat의 doc_ids만 신뢰하지 않음)
    DocMeta items[MAX_INVEN_SIZE];
    int real_count = 0;
    if (sandbox_inventory_scan(my_key, items, MAX_INVEN_SIZE, &real_count) != 0) {
        send_error(sock, ERR_INVALID_SESSION, "인벤토리 디렉토리 순회 실패.");
        return;
    }

    Packet res;
    memset(&res, 0, sizeof(Packet));
    res.type = PKT_RES_INVEN_INFO;
    res.body.inven_info.count = real_count;
    res.body.inven_info.money = rec.money;

    for (int i = 0; i < real_count; i++) {
        res.body.inven_info.items[i].doc_id    = items[i].doc_id;
        res.body.inven_info.items[i].tags      = items[i].tags;
        res.body.inven_info.items[i].is_frozen = market_is_doc_frozen(items[i].tags);
        strncpy(res.body.inven_info.items[i].name, items[i].name, MAX_NAME_LEN - 1);
    }
    packet_send(sock, &res);
}


// =================================================================
// /rumor 핸들러 — 수수료 차감 + (브로드캐스트는 A 영역)
// =================================================================

static void handle_rumor(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    (void)pkt;  // target_key는 향후 A의 unicast 구현 시 사용. 현재는 broadcast.
    // 글로벌 쿨다운 체크 (간이)
    pthread_mutex_lock(&g_rumor_mutex);
    if (g_rumor_busy) {
        pthread_mutex_unlock(&g_rumor_mutex);
        send_error(sock, ERR_RUMOR_COOLDOWN, "글리치 쿨다운 중입니다.");
        return;
    }
    g_rumor_busy = 1;
    pthread_mutex_unlock(&g_rumor_mutex);

    UserRecord rec;
    if (userdb_find(my_key, &rec, NULL) != 0) {
        pthread_mutex_lock(&g_rumor_mutex);
        g_rumor_busy = 0;
        pthread_mutex_unlock(&g_rumor_mutex);
        send_error(sock, ERR_INVALID_SESSION, "사용자 레코드 없음.");
        return;
    }
    if (rec.money < RUMOR_FEE) {
        pthread_mutex_lock(&g_rumor_mutex);
        g_rumor_busy = 0;
        pthread_mutex_unlock(&g_rumor_mutex);
        send_error(sock, ERR_NOT_ENOUGH_MONEY, "수수료가 부족합니다.");
        return;
    }

    rec.money -= RUMOR_FEE;
    userdb_update_at(my_offset, &rec);

    // 브로드캐스트(특정 target에게 PKT_EVT_RUMOR_GLITCH 단일 전송)는 A 영역.
    // 일단 일반 broadcast로 시연하고, A가 target_key 기반 unicast로 교체.
    Packet evt;
    memset(&evt, 0, sizeof(Packet));
    evt.type = PKT_EVT_RUMOR_GLITCH;
    broadcast_packet(&evt);

    // 쿨다운 해제 (실제 wall-clock 기반은 추후)
    pthread_mutex_lock(&g_rumor_mutex);
    g_rumor_busy = 0;
    pthread_mutex_unlock(&g_rumor_mutex);

    // 파산 트리거 (수수료로 잔액 0 되면 소각)
    if (rec.money == 0) {
        userdb_burn_at(my_offset);
    }
}


// =================================================================
// 디스패처
// =================================================================

void handlers_dispatch(int sock, Packet *pkt, const char *my_key, off_t my_offset){
    switch (pkt->type) {
        case PKT_REQ_BUY:     handle_buy(sock, pkt, my_key, my_offset);       break;
        case PKT_REQ_SELL:    handle_sell(sock, pkt, my_key, my_offset);      break;
        case PKT_REQ_DISPOSE: handle_dispose(sock, pkt, my_key, my_offset);   break;
        case PKT_REQ_INVEN:   handle_inventory(sock, pkt, my_key, my_offset); break;
        case PKT_REQ_RUMOR:   handle_rumor(sock, pkt, my_key, my_offset);     break;
        default:
            // PKT_REQ_CHAT 등은 server_main.c에서 별도 처리.
            // PKT_REQ_MINIGAME_SUBMIT은 C 영역(시그널 미니게임) 책임.
            break;
    }
}
