#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

// 타이밍 상수
#define EVT_TICK_SEC           5
#define EVT_MARKET_EVERY       1   // 매물 스폰 주기 = 1틱(5s). 보드를 더 빠르게 채워 매칭 매물 확보 용이
#define EVT_NPC_EVERY          3   // 새 의뢰 스폰 주기 = 3틱(15s). 매칭 의뢰가 더 자주 등장
#define EVT_NPC_HINT_DELAY     8
#define EVT_NPC_MAX_AGE_SEC  140   // 1태그 의뢰 수명 (기본). 태그 1개 늘 때마다 STEP만큼 단축
#define EVT_NPC_AGE_STEP_SEC  30   // 요구 태그 1개당 수명 단축폭 → 2태그 110s, 3태그 80s (하한 60s)

// 밸런스 옵션
#define MARKET_PRICE_PER_TAG  400

// 공중파 유출 상수
#define EVT_LEAK_EVERY        24
#define LEAK_PROB_PCT         35
#define LEAK_DUR_MIN_SEC     180
#define LEAK_DUR_MAX_SEC     300

// 주기 경찰 레이드 상수
#define PERIODIC_RAID_MIN_INTERVAL    360   // 6분
#define PERIODIC_RAID_MAX_INTERVAL    600   // 10분

// 상한선 도달 후 5분 경과 여부 측정 상수
#define CAP_GRACE_SEC 300

// =============================================================
// 공개 API 함수 선언
// =============================================================

// 비동기 이벤트 스레드를 백그라운드 구동합니다.
int events_start_thread(void);

// 특정 공중파 유출을 수동/디버그로 즉시 격발시킵니다. (성공 시 1, 실패 시 0)
int fire_public_leak(void);

// 현재 동결 마스크를 피해서 난수 기반의 태그 조합(1~3개)을 빌드합니다.
uint32_t event_random_tags(void);

#endif // EVENTS_H
