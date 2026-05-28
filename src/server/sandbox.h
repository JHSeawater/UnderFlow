#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdint.h>
#include "protocol.h"

#define DATA_ROOT  "./data"
#define MASTER_DIR "./data/master"
#define USERS_DIR  "./data/users"

// 마스터 / 샌드박스 모두 ./data 하위에 두어 EXDEV (Cross-device link) 차단.
// rename()은 같은 파일시스템 내에서만 원자적으로 작동하기 때문.
typedef struct {
    int32_t  doc_id;
    uint32_t tags;
    int32_t  base_price;
    char     name[MAX_NAME_LEN];
} DocFile;

int  sandbox_global_init(void);                                          // ./data, master, users 디렉토리 생성
int  sandbox_user_init(const char *key);                                 // ./data/users/<key>/ 생성
int  sandbox_count(const char *key);                                     // 보유 파일 개수 (".", ".." 예외처리)
int  sandbox_has(const char *key, int32_t doc_id);                       // 보유 여부 (stat)
int  sandbox_buy(const char *key, int32_t doc_id);                       // 마스터 → 샌드박스 (rename + chmod)
int  sandbox_dispose(const char *key, int32_t doc_id);                   // unlink
int  sandbox_read_doc(const char *key, int32_t doc_id, DocFile *out);    // 문서 메타 읽기
int  sandbox_list(const char *key, InvenItem *out, int max);             // opendir + readdir 순회

// 마스터 측 (Role A의 이벤트 스레드용 인터페이스, B의 시드용도 겸함)
int  master_write_doc(const DocFile *doc);
int  master_remove_doc(int32_t doc_id);

#endif
