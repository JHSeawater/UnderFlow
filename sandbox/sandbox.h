#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdint.h>
#include "protocol.h"

/*
 * sandbox 모듈 — 매물 파일과 유저 샌드박스 파일을 실제로 조작하는 파일시스템 ops.
 *
 * 모든 경로는 ./data/ 트리 내부로 고정 (EXDEV 회피).
 *   - 매물 마스터: ./data/master/<doc_id>.dat
 *   - 유저 사물함: ./data/users/<key>/<doc_id>.dat
 *
 * 각 .dat 파일은 DocMeta 구조체 하나만 바이너리로 저장한다.
 */

#pragma pack(push, 1)
typedef struct {
    int32_t  doc_id;
    uint32_t tags;        // 비트마스크 (protocol.h의 Tag enum)
    int32_t  base_price;
    char     name[MAX_NAME_LEN];
} DocMeta;
#pragma pack(pop)

// 매물 파일 생성: ./data/master/<doc_id>.dat 에 DocMeta 기록. (A가 매물 스폰 시 호출)
int sandbox_master_write(const DocMeta *doc);

// 매물 파일을 유저 샌드박스로 이동(rename). master → users/<key>/.
// 동일 파티션 내 이동이므로 EXDEV 발생하지 않음.
int sandbox_buy_move(int32_t doc_id, const char *key);

// 유저 인벤토리에서 doc_id 파일 삭제(unlink). /sell, /dispose 공용.
int sandbox_user_unlink(const char *key, int32_t doc_id);

// 마스터 폴더에서 매물 파일 삭제(매물 만료 등).
int sandbox_master_unlink(int32_t doc_id);

// 유저 인벤토리 디렉토리 순회 (opendir/readdir, ./.. 필터).
// items에 최대 max_items까지 DocMeta 채우고, *out_count에 실제 개수.
// 반환: 0=성공, -1=오류
int sandbox_inventory_scan(const char *key,
                           DocMeta *items, int max_items,
                           int *out_count);

// 특정 유저 폴더의 특정 doc_id 메타데이터만 읽기.
int sandbox_user_read_meta(const char *key, int32_t doc_id, DocMeta *out);

// 마스터 폴더의 특정 doc_id 메타데이터만 읽기.
int sandbox_master_read_meta(int32_t doc_id, DocMeta *out);

#endif
