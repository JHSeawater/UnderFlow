#ifndef USERDB_H
#define USERDB_H

#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

#define USERDB_DIR    "./data/"
#define USERDB_PATH   USERDB_DIR "users.dat"
#define SANDBOX_ROOT  USERDB_DIR "users"
#define MASTER_ROOT   USERDB_DIR "master"

// 게임 상수 (단톡 합의 후 조정)
#define INITIAL_MONEY 1000
#define GOAL_MONEY    10000

typedef struct {
    char    key[MAX_KEY_LEN];                   //유저 식별 문자열 키
    int32_t money;                              //돈(-1이면 파산/소각)
    int32_t inventory_count;                    //인벤토리 보유 개수 (0 ~ MAX_INVEN_SIZE)
                                                //inventory_doc_ids[0 .. count-1]만 유효
    int32_t inventory_doc_ids[MAX_INVEN_SIZE];  //인벤토리 슬롯 (앞에서부터 채움)
} UserRecord;

// users.dat / data / master / users 디렉토리 일괄 보장. 서버 시작 시 한 번 호출.
int userdb_init(void);

// key로 유저 찾기. 찾으면 0, 못 찾으면 -1 반환.
// out 또는 out_offset에 NULL 넘기면 그 부분은 채우지 않음.
int userdb_find(const char *key, UserRecord *out, off_t *out_offset);
/*
    users.dat
    ---------------------------------
    offset 0   ~ 55  : A 레코드
    offset 56  ~ 111 : B 레코드
    ...
    오프셋 값으로 특정 유저 레코드에 직접 접근
*/

// 활성 계정만 반환 (소각된 계정은 못 찾은 걸로 취급 → Key 영구 차단).
int userdb_find_active(const char *key, UserRecord *out, off_t *out_offset);

// 새 유저 레코드를 파일 끝에 덧붙임. 추가된 위치를 out_offset으로 돌려줌.
int userdb_append(const UserRecord *rec, off_t *out_offset);

// 신규 가입 헬퍼: money = INITIAL_MONEY, 인벤토리 0 초기화 + append
int userdb_signup(const char *key, UserRecord *out, off_t *out_offset);

// 특정 위치의 레코드를 통째로 덮어쓰기. find로 받은 offset 재활용.
int userdb_update_at(off_t offset, const UserRecord *rec);

// 계정 소각 처리 (money = -1 마킹).
int userdb_burn_at(off_t offset);

// 이 레코드가 소각된 계정인지 확인.
int userdb_is_burned(const UserRecord *rec);

// key로 찾되 없으면 initial_money로 신규 레코드 생성. find→append가 단일 락 안에서 원자적으로 수행됨.
// 반환값: 0=기존 유저, 1=신규 생성, -1=에러
int userdb_find_or_create(const char *key, int initial_money,
                          UserRecord *out, off_t *out_offset);

// ./data/users/<key>/ 디렉토리 생성. 이미 존재(EEXIST)는 정상으로 0 반환.
int userdb_create_sandbox(const char *key);

// Key 문자 안전성 검사 ([A-Za-z0-9_]만 허용). 1=안전, 0=거부.
// 디렉토리 탈출(./.., 슬래시) 공격 방어.
int userdb_key_is_safe(const char *key);

// =================================================================
// 인벤토리 메모리 조작 (디스크 쓰기 안 함, 호출자가 update_at으로 영속화)
// =================================================================

// 인벤토리에 doc_id 추가. 0=성공, -1=가득참(MAX_INVEN_SIZE 초과)
int userdb_inventory_add(UserRecord *rec, int32_t doc_id);

// 인벤토리에서 doc_id 제거(뒤 슬롯 앞으로 당김). 0=성공, -1=없음
int userdb_inventory_remove(UserRecord *rec, int32_t doc_id);

// 인벤토리 가득 찼는지. 1=가득, 0=여유
int userdb_inventory_full(const UserRecord *rec);

// 인벤토리에 doc_id 있는지. 1=있음, 0=없음
int userdb_inventory_has(const UserRecord *rec, int32_t doc_id);

#endif
