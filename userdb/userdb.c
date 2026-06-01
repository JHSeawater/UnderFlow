#include "userdb.h"



#include <sys/stat.h>    // mkdir, mode 상수, S_IRUSR 등
#include <sys/types.h>   // mode_t (이미 sys/stat.h가 포함하지만 명시적으로)
#include <fcntl.h>       // open, O_RDWR, O_CREAT 등
#include <unistd.h>      // close
#include <errno.h>       // errno, EEXIST 등
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_userdb_mutex = PTHREAD_MUTEX_INITIALIZER;



// users.dat / data 디렉토리가 없으면 생성. 서버 시작 시 한 번 호출.
int userdb_init(void) {
    // 1. 디렉토리 보장
    if (mkdir(USERDB_DIR, 0700) == -1) {
        if (errno != EEXIST) {
            return -1;   // 진짜 에러 (권한 없음, 디스크 풀 등)
        }
        // EEXIST는 정상 — 이미 있는 거니까 통과
    }
    
    // 2. 파일 보장
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

    // 영구 소각: money 마커 + 인벤토리 일관성 클리어
    // (재접속 차단은 money==-1 검사로 작동하지만, 인벤 필드도 0으로 정리해 두면
    //  혹시 burn 마커 정책이 바뀌어도 잔여 doc_id 참조가 남지 않는다.)
    rec.money = -1;
    rec.inventory_count = 0;
    memset(rec.inventory_doc_ids, 0, sizeof(rec.inventory_doc_ids));

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

int userdb_reset_round(int32_t initial_money) {
    pthread_mutex_lock(&g_userdb_mutex);

    int fd = open(USERDB_PATH, O_RDWR);
    if (fd == -1) {
        pthread_mutex_unlock(&g_userdb_mutex);
        return -1;
    }

    UserRecord rec;
    off_t offset = 0;
    ssize_t n;
    while ((n = pread(fd, &rec, sizeof(UserRecord), offset)) == sizeof(UserRecord)) {
        if (rec.money != -1) {
            rec.money = initial_money;
            rec.inventory_count = 0;
            memset(rec.inventory_doc_ids, 0, sizeof(rec.inventory_doc_ids));
            if (pwrite(fd, &rec, sizeof(UserRecord), offset) != sizeof(UserRecord)) {
                close(fd);
                pthread_mutex_unlock(&g_userdb_mutex);
                return -1;
            }
        }
        offset += sizeof(UserRecord);
    }

    close(fd);
    pthread_mutex_unlock(&g_userdb_mutex);
    return 0;
}



