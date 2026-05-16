#include "sandbox.h"
#include "userdb/userdb.h"     // SANDBOX_ROOT, MASTER_ROOT, userdb_key_is_safe

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

// 경로 버퍼 크기 — root prefix + key + doc_id + 안전 여유
#define PATH_BUF_SIZE 256

// ===== 내부 경로 빌더 =====
static int make_master_path(int32_t doc_id, char *buf, size_t buf_sz){
    int n = snprintf(buf, buf_sz, "%s/%d.dat", MASTER_ROOT, doc_id);
    return (n > 0 && (size_t)n < buf_sz) ? 0 : -1;
}

static int make_user_path(const char *key, int32_t doc_id, char *buf, size_t buf_sz){
    if (!userdb_key_is_safe(key)) return -1;   // 디렉토리 탈출 방어
    int n = snprintf(buf, buf_sz, "%s/%s/%d.dat", SANDBOX_ROOT, key, doc_id);
    return (n > 0 && (size_t)n < buf_sz) ? 0 : -1;
}

static int make_user_dir(const char *key, char *buf, size_t buf_sz){
    if (!userdb_key_is_safe(key)) return -1;
    int n = snprintf(buf, buf_sz, "%s/%s", SANDBOX_ROOT, key);
    return (n > 0 && (size_t)n < buf_sz) ? 0 : -1;
}

// ===== DocMeta 파일 I/O =====
static int read_meta_from_path(const char *path, DocMeta *out){
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    ssize_t n = read(fd, out, sizeof(DocMeta));
    close(fd);
    return (n == sizeof(DocMeta)) ? 0 : -1;
}

static int write_meta_to_path(const char *path, const DocMeta *doc){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) return -1;
    ssize_t n = write(fd, doc, sizeof(DocMeta));
    close(fd);
    return (n == sizeof(DocMeta)) ? 0 : -1;
}

// ===== 공개 API =====

int sandbox_master_write(const DocMeta *doc){
    if (!doc) return -1;
    char path[PATH_BUF_SIZE];
    if (make_master_path(doc->doc_id, path, sizeof(path)) != 0) return -1;
    return write_meta_to_path(path, doc);
}

int sandbox_buy_move(int32_t doc_id, const char *key){
    char src[PATH_BUF_SIZE], dst[PATH_BUF_SIZE];
    if (make_master_path(doc_id, src, sizeof(src)) != 0) return -1;
    if (make_user_path(key, doc_id, dst, sizeof(dst)) != 0) return -1;

    // 유저 샌드박스 디렉토리가 없으면 만들고 시도 (방어)
    char dir[PATH_BUF_SIZE];
    if (make_user_dir(key, dir, sizeof(dir)) == 0) {
        if (mkdir(dir, 0700) == -1 && errno != EEXIST) return -1;
    }

    // 같은 ./data/ 트리 안이라 EXDEV는 발생하지 않음.
    if (rename(src, dst) == -1) return -1;

    // 권한 차단(소유자 외 접근 거절): 평가 항목의 chmod 시연.
    chmod(dst, 0600);
    return 0;
}

int sandbox_user_unlink(const char *key, int32_t doc_id){
    char path[PATH_BUF_SIZE];
    if (make_user_path(key, doc_id, path, sizeof(path)) != 0) return -1;
    return unlink(path) == 0 ? 0 : -1;
}

int sandbox_master_unlink(int32_t doc_id){
    char path[PATH_BUF_SIZE];
    if (make_master_path(doc_id, path, sizeof(path)) != 0) return -1;
    return unlink(path) == 0 ? 0 : -1;
}

int sandbox_inventory_scan(const char *key, DocMeta *items, int max_items, int *out_count){
    if (!items || !out_count || max_items <= 0) return -1;
    if (!userdb_key_is_safe(key)) return -1;

    char dir[PATH_BUF_SIZE];
    if (make_user_dir(key, dir, sizeof(dir)) != 0) return -1;

    DIR *dp = opendir(dir);
    if (!dp) return -1;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL && count < max_items) {
        const char *name = entry->d_name;

        // 운영체제 기본 숨김 폴더 필터 (. 와 ..)
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        // <doc_id>.dat 파싱 — 파일명 전체가 정확히 매칭돼야 통과
        // (%n으로 소비 바이트 수 확인 → "123abc.dat" 같은 변종 차단)
        int32_t doc_id = 0;
        int     consumed = 0;
        if (sscanf(name, "%d.dat%n", &doc_id, &consumed) != 1) continue;
        if (consumed != (int)strlen(name)) continue;

        char path[PATH_BUF_SIZE];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        if (read_meta_from_path(path, &items[count]) == 0) {
            count++;
        }
    }
    closedir(dp);

    *out_count = count;
    return 0;
}

int sandbox_user_read_meta(const char *key, int32_t doc_id, DocMeta *out){
    char path[PATH_BUF_SIZE];
    if (make_user_path(key, doc_id, path, sizeof(path)) != 0) return -1;
    return read_meta_from_path(path, out);
}

int sandbox_master_read_meta(int32_t doc_id, DocMeta *out){
    char path[PATH_BUF_SIZE];
    if (make_master_path(doc_id, path, sizeof(path)) != 0) return -1;
    return read_meta_from_path(path, out);
}
