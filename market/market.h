#ifndef MARKET_H
#define MARKET_H

#include <stdint.h>
#include "sandbox/sandbox.h"   // DocMeta

/*
 * market 모듈 — 시장에 떠 있는 매물 상태와 동결 태그를 관리하는 인메모리 자료구조.
 *
 * 책임 분리:
 *   - market_spawn / market_remove : 매물 등록/제거 (A의 이벤트 스레드가 호출)
 *   - market_buy                   : /buy 핸들러가 호출 (선착순 1명만 성공)
 *   - market_freeze / recover      : 공중파 유출 이벤트 (A 호출)
 *   - market_doc_lock_many / unlock: /sell 다중 매물 잠금 — ID 오름차순 강제로 Circular Wait 차단
 */

#define MAX_MARKET_ITEMS 64
#define MAX_DOC_ID       1024     // doc_id별 mutex 배열 크기. doc_id는 1..MAX_DOC_ID-1 권장.

int  market_init(void);

// 매물 등록 (A의 이벤트 스레드 호출). 동시에 master/<id>.dat 파일도 생성.
// 반환: 0 성공, -1 시장 가득참/이미 존재
int  market_spawn(const DocMeta *doc);

// 매물 강제 제거(만료, 또는 buy 후 정리).
int  market_remove(int32_t doc_id);

// /buy 진입점. 매물이 있으면 시장에서 제거하고 *out에 메타 반환.
// 반환: 0=성공, -1=없음, -2=동결 매물(구매 불가)
int  market_buy(int32_t doc_id, DocMeta *out);

// 시장에 매물이 있는지만 검사 + 메타 복사 (제거하지 않음).
int  market_lookup(int32_t doc_id, DocMeta *out);

// 시장 가득 찼는지 (A가 스폰 전에 체크).
int  market_is_full(void);

// 현재 시장 매물 전체를 items 배열에 복사. 늦은 접속자 동기화용.
// g_market_mutex 잡고 복사 후 즉시 release — 락 외부에서 packet_send 해야 함.
// 반환: 0=성공, -1=인자 오류
int  market_snapshot(DocMeta *items, int max_items, int *out_count);

// =================================================================
// 동결 태그 관리 (공중파 유출 / 재상장)
// =================================================================

// 해당 비트마스크 태그를 동결 마스크에 OR. 그 태그를 가진 매물은 buy/sell 거절.
void market_freeze_tags(uint32_t tags);

// 동결 마스크에서 해당 태그 비트 해제.
void market_recover_tags(uint32_t tags);

// 특정 doc의 태그 중 하나라도 동결 마스크와 겹치는지. 1=동결, 0=정상
int  market_is_doc_frozen(uint32_t doc_tags);

// 현재 동결 마스크 조회 (UI 렌더링 보조용).
uint32_t market_frozen_mask(void);

// =================================================================
// doc_id별 락 — /sell의 다중 매물 잠금 시 사용
// =================================================================

// 단일 doc_id 락 (raw API).
void market_doc_lock(int32_t doc_id);
void market_doc_unlock(int32_t doc_id);

/*
 * 여러 doc_id를 한꺼번에 잠금.
 * - 내부에서 슬롯 인덱스(doc_id % MAX_DOC_ID)로 매핑 후 중복 제거,
 *   오름차순 정렬한 순서로 lock을 획득한다 → Circular Wait 원천 차단.
 * - unlock_many는 같은 doc_ids를 그대로 넘기면 됨 (내부에서 동일 중복 제거).
 */
void market_doc_lock_many(const int32_t *doc_ids, int n);
void market_doc_unlock_many(const int32_t *doc_ids, int n);

#endif
