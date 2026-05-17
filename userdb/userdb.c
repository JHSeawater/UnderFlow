#include "userdb.h"

#include <sys/stat.h>    // mkdir, mode 상수, S_IRUSR 등
#include <sys/types.h>   // mode_t (이미 sys/stat.h가 포함하지만 명시적으로)
#include <fcntl.h>       // open, O_RDWR, O_CREAT 등
#include <unistd.h>      // close
#include <errno.h>       // errno, EEXIST 등
#include <pthread.h>
#include <string.h>
#include <stdio.h>       // snprintf

static pthread_mutex_t g_userdb_mutex = PTHREAD_MUTEX_INITIALIZER;

// data / master / users 디렉토리와 users.dat 일괄 보장. 서버 시작 시 한 번 호출.
int userdb_init(void) {
    // 1. ./data/ 자체
    if (mkdir(USERDB_DIR, 0700) == -1) {
        if (errno != EEXIST) {
            return -1;   // 진짜 에러 (권한 없음, 디스크 풀 등)
        }
        // EEXIST는 정상 — 이미 있는 거니까 통과
    }

    // 2. ./data/master/ (매물 마스터 보관함)
    if (mkdir(MASTER_ROOT, 0700) == -1) {
        if (errno != EEXIST) return -1;
    }

    // 3. ./data/users/ (유저 샌드박스 루트)
    if (mkdir(SANDBOX_ROOT, 0700) == -1) {
        if (errno != EEXIST) return -1;
    }

    // 4. users.dat 파일 보장
    int fd = open(USERDB_PATH, O_RDWR | O_CREAT, 0600);
    if (fd == -1) {
        return -1;
    }
    close(fd);

    // init 성공. 이 시점부터 ./data/users.dat은 존재한다고 확정.
    return 0;
}

int userdb_find(const char *key, UserRecord *out, off_t *out_offset){
    UserRecord rec; // 매 루프마다 받아올 유저 데이터

    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH,O_RDONLY);
    if(fd == -1){
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    ssize_t n;
    off_t offset = 0;
    int found = 0;
    while((n = read(fd,&rec,sizeof(UserRecord))) == sizeof(UserRecord)){ //읽어온 값의 크기가 정확히 UserRecord크기 일때만 반복

        if(strncmp(key,rec.key, MAX_KEY_LEN) == 0){
            //찾았을때
            if(out != NULL) *out = rec;
            if(out_offset != NULL) { *out_offset = offset; }
            found = 1;
            break;
        }
        offset += n;
    }
    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return found ? 0 : -1;
}

int userdb_find_active(const char *key, UserRecord *out, off_t *out_offset){
    UserRecord tmp;
    off_t off;
    if (userdb_find(key, &tmp, &off) != 0) return -1;
    if (userdb_is_burned(&tmp)) return -1;   // 소각된 계정은 못 찾은 걸로 취급
    if (out)        *out = tmp;
    if (out_offset) *out_offset = off;
    return 0;
}

int userdb_append(const UserRecord *rec, off_t *out_offset){
    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH, O_RDWR);
    if(fd == -1){
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    off_t end_pos = lseek(fd, 0, SEEK_END);
    if(end_pos == -1){
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    ssize_t written = write(fd, rec, sizeof(UserRecord));
    if(written != sizeof(UserRecord)){
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if(out_offset != NULL){ *out_offset = end_pos; }

    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return 0;
}

int userdb_signup(const char *key, UserRecord *out, off_t *out_offset){
    UserRecord rec;
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.key, key, MAX_KEY_LEN - 1);
    rec.money = INITIAL_MONEY;
    rec.inventory_count = 0;
    // inventory_doc_ids는 memset으로 이미 0

    if (userdb_append(&rec, out_offset) != 0) return -1;
    if (out) *out = rec;
    return 0;
}

int userdb_update_at(off_t offset, const UserRecord *rec){
    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH,O_RDWR);
    if(fd == -1){
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if(lseek(fd,offset,SEEK_SET) == -1){
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if(write(fd,rec,sizeof(UserRecord)) != sizeof(UserRecord)){
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return 0;
}

int userdb_burn_at(off_t offset) {
    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH, O_RDWR);
    if (fd == -1) {
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == -1) {
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    UserRecord rec;
    if (read(fd, &rec, sizeof(UserRecord)) != sizeof(UserRecord)) {
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    // 파산처리
    rec.money = -1;

    if (lseek(fd, offset, SEEK_SET) == -1) {
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if (write(fd, &rec, sizeof(UserRecord)) != sizeof(UserRecord)) {
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return 0;
}

int userdb_is_burned(const UserRecord *rec){
    return rec->money == -1 ? 1 : 0;
}

int userdb_find_or_create(const char *key, int initial_money,
                          UserRecord *out, off_t *out_offset) {
    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH, O_RDWR);
    if (fd == -1) {
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    UserRecord rec;
    off_t offset = 0;
    ssize_t n;
    int found = 0;
    while ((n = read(fd, &rec, sizeof(UserRecord))) == sizeof(UserRecord)) {
        if (strncmp(key, rec.key, MAX_KEY_LEN) == 0) {
            found = 1;
            break;
        }
        offset += n;
    }

    if (found) {
        if (out)        *out        = rec;
        if (out_offset) *out_offset = offset;
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return 0;
    }

    // 못 찾음 → 락을 보유한 채로 append (TOCTOU 방지)
    memset(&rec, 0, sizeof(UserRecord));
    strncpy(rec.key, key, MAX_KEY_LEN - 1);
    rec.money = initial_money;

    off_t end_pos = lseek(fd, 0, SEEK_END);
    if (end_pos == -1 || write(fd, &rec, sizeof(UserRecord)) != sizeof(UserRecord)) {
        close(fd);
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    if (out)        *out        = rec;
    if (out_offset) *out_offset = end_pos;
    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return 1;
}

// =================================================================
// 신규: 샌드박스, Key 검증, 인벤토리 메모리 ops
// =================================================================

int userdb_key_is_safe(const char *key){
    if (!key || !*key) return 0;
    size_t len = strnlen(key, MAX_KEY_LEN);
    if (len == 0 || len >= MAX_KEY_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = key[i];
        int ok = (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') ||
                 c == '_';
        if (!ok) return 0;
    }
    return 1;
}

int userdb_create_sandbox(const char *key){
    if (!userdb_key_is_safe(key)) return -1;

    // SANDBOX_ROOT 자체는 userdb_init에서 만들지만, 방어적으로 한 번 더
    if (mkdir(SANDBOX_ROOT, 0700) == -1 && errno != EEXIST) return -1;

    char path[sizeof(SANDBOX_ROOT) + MAX_KEY_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", SANDBOX_ROOT, key);

    if (mkdir(path, 0700) == -1 && errno != EEXIST) return -1;
    return 0;
}

int userdb_inventory_add(UserRecord *rec, int32_t doc_id){
    if (rec->inventory_count >= MAX_INVEN_SIZE) return -1;
    rec->inventory_doc_ids[rec->inventory_count++] = doc_id;
    return 0;
}

int userdb_inventory_remove(UserRecord *rec, int32_t doc_id){
    for (int i = 0; i < rec->inventory_count; i++) {
        if (rec->inventory_doc_ids[i] == doc_id) {
            // 발견한 위치부터 뒤 슬롯을 한 칸씩 앞으로 당김
            for (int j = i; j < rec->inventory_count - 1; j++) {
                rec->inventory_doc_ids[j] = rec->inventory_doc_ids[j + 1];
            }
            rec->inventory_doc_ids[rec->inventory_count - 1] = 0;
            rec->inventory_count--;
            return 0;
        }
    }
    return -1;
}

int userdb_inventory_full(const UserRecord *rec){
    return rec->inventory_count >= MAX_INVEN_SIZE ? 1 : 0;
}

int userdb_inventory_has(const UserRecord *rec, int32_t doc_id){
    for (int i = 0; i < rec->inventory_count; i++) {
        if (rec->inventory_doc_ids[i] == doc_id) return 1;
    }
    return 0;
}
