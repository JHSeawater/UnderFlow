#ifndef USERDB_H
#define USERDB_H

#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

#define USERDB_DIR  "./data/"
#define USERDB_PATH USERDB_DIR "users.dat"

typedef struct {
    char    key[MAX_KEY_LEN];                   //유저 식별 문자열 키
    int32_t money;                              //돈(-1이면 파산/소각)
    int32_t inventory_count;                    //인벤토리 보유 개수 (0 ~ MAX_INVEN_SIZE)
                                                //inventory_doc_ids[0 .. count-1]만 유효
    int32_t inventory_doc_ids[MAX_INVEN_SIZE];  //인벤토리 슬롯 (앞에서부터 채움)
} UserRecord;

// users.dat / data 디렉토리가 없으면 생성. 서버 시작 시 한 번 호출.
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

// 새 유저 레코드를 파일 끝에 덧붙임. 추가된 위치를 out_offset으로 돌려줌.
// 호출자는 이 offset을 들고 있다가 update에 재사용 가능 (find 다시 안 해도 됨).
int userdb_append(const UserRecord *rec, off_t *out_offset);

// 특정 위치의 레코드를 통째로 덮어쓰기. find로 받은 offset 재활용.
int userdb_update_at(off_t offset, const UserRecord *rec);

// 계정 소각 처리 (money = -1 마킹).
// 소각 정책이 바뀌면 이 함수 안만 고치면 됨.
int userdb_burn_at(off_t offset);

// 이 레코드가 소각된 계정인지 확인.
// 소각 마커가 -1에서 다른 값으로 바뀌면 이 함수 안만 고치면 됨.
int userdb_is_burned(const UserRecord *rec);

// 라운드 리셋: 소각(-1) 마커가 박힌 레코드는 그대로 두고,
// 그 외 모든 활성 레코드를 (money=initial_money, inventory=비어있음) 으로 복원.
int userdb_reset_round(int32_t initial_money);

#endif