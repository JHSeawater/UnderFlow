#include "sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

static void user_dir_path(const char *key, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s", USERS_DIR, key);
}

static void master_doc_path(int32_t doc_id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/doc_%d.dat", MASTER_DIR, doc_id);
}

static void user_doc_path(const char *key, int32_t doc_id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s/doc_%d.dat", USERS_DIR, key, doc_id);
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == -1 && errno != EEXIST) return -1;
    return 0;
}

int sandbox_global_init(void) {
    if (ensure_dir(DATA_ROOT)  != 0) return -1;
    if (ensure_dir(MASTER_DIR) != 0) return -1;
    if (ensure_dir(USERS_DIR)  != 0) return -1;
    return 0;
}

int sandbox_user_init(const char *key) {
    char path[512];
    user_dir_path(key, path, sizeof(path));
    return ensure_dir(path);
}

int sandbox_count(const char *key) {
    char path[512];
    user_dir_path(key, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        // OS 기본 숨김 폴더 예외처리
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strncmp(ent->d_name, "doc_", 4) == 0) count++;
    }
    closedir(d);
    return count;
}

int sandbox_has(const char *key, int32_t doc_id) {
    char path[512];
    user_doc_path(key, doc_id, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int sandbox_buy(const char *key, int32_t doc_id) {
    char from[512], to[512];
    master_doc_path(doc_id, from, sizeof(from));
    user_doc_path(key, doc_id, to, sizeof(to));

    // 원본 존재 확인
    struct stat st;
    if (stat(from, &st) != 0) return -1;

    // 같은 파일시스템(./data) 내부이므로 EXDEV 없이 원자적 rename
    if (rename(from, to) != 0) return -1;

    // 소유권 통제: 일반 유저가 직접 수정 못 하도록 read-only 처리
    chmod(to, 0400);
    return 0;
}

int sandbox_dispose(const char *key, int32_t doc_id) {
    char path[512];
    user_doc_path(key, doc_id, path, sizeof(path));
    return unlink(path) == 0 ? 0 : -1;
}

int sandbox_read_doc(const char *key, int32_t doc_id, DocFile *out) {
    char path[512];
    user_doc_path(key, doc_id, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    ssize_t n = read(fd, out, sizeof(DocFile));
    close(fd);
    return n == (ssize_t)sizeof(DocFile) ? 0 : -1;
}

int sandbox_list(const char *key, InvenItem *out, int max) {
    char path[512];
    user_dir_path(key, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strncmp(ent->d_name, "doc_", 4) != 0) continue;

        int32_t doc_id = atoi(ent->d_name + 4);
        DocFile df;
        if (sandbox_read_doc(key, doc_id, &df) == 0) {
            memset(&out[count], 0, sizeof(InvenItem));
            out[count].doc_id = df.doc_id;
            out[count].tags = df.tags;
            strncpy(out[count].name, df.name, MAX_NAME_LEN - 1);
            out[count].is_frozen = 0;   // 호출 측에서 market 모듈 조회 후 채움
            count++;
        }
    }
    closedir(d);
    return count;
}

int master_write_doc(const DocFile *doc) {
    char path[512];
    master_doc_path(doc->doc_id, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return -1;
    ssize_t n = write(fd, doc, sizeof(DocFile));
    close(fd);
    return n == (ssize_t)sizeof(DocFile) ? 0 : -1;
}

int master_remove_doc(int32_t doc_id) {
    char path[512];
    master_doc_path(doc_id, path, sizeof(path));
    return unlink(path) == 0 ? 0 : -1;
}

// 디렉토리 하나의 doc_*.dat을 전부 unlink. 디렉토리 자체는 유지.
static void purge_docs_in(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "doc_", 4) != 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        unlink(path);
    }
    closedir(d);
}

int sandbox_purge_user(const char *key) {
    char path[512];
    user_dir_path(key, path, sizeof(path));
    purge_docs_in(path);
    return 0;
}

int sandbox_global_reset(void) {
    // 1. 마스터 폴더 청소
    purge_docs_in(MASTER_DIR);

    // 2. 각 유저 샌드박스 청소 (USERS_DIR 하위 디렉토리 순회)
    DIR *u = opendir(USERS_DIR);
    if (!u) return 0;
    struct dirent *ent;
    while ((ent = readdir(u)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char user_path[512];
        snprintf(user_path, sizeof(user_path), "%s/%s", USERS_DIR, ent->d_name);
        struct stat st;
        if (stat(user_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            purge_docs_in(user_path);
        }
    }
    closedir(u);
    return 0;
}
