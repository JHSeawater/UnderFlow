#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#define MARKET_DIR "./data/market"
#define USER_DIR "./data/users"
#define USER1_SANDBOX "./data/users/user_1"
#define ITEM_FILE_MARKET "./data/market/item_101.dat"
#define ITEM_FILE_USER1 "./data/users/user_1/item_101.dat"

// 샌드박스 디렉토리 무결성 검증 및 생성 함수
void check_and_create_dir(const char* path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == 0) {
            printf("[System] 디렉토리 생성 완료: %s\n", path);
        } else {
            perror("[Error] 디렉토리 생성 실패");
            exit(EXIT_FAILURE);
        }
    } else {
        printf("[System] 디렉토리 확인 완료: %s\n", path);
    }
}

int main() {
    printf("=== UNDERFLOW 샌드박스 시스템 동작 검증 ===\n\n");

    // 1. 디렉토리 구조 검증 및 생성 (Fail-Safe)
    check_and_create_dir("./data");
    check_and_create_dir(MARKET_DIR);
    check_and_create_dir(USER_DIR);
    check_and_create_dir(USER1_SANDBOX);

    printf("\n--- 1단계: 시장 매물 스폰 (Event Thread 시뮬레이션) ---\n");
    int fd = open(ITEM_FILE_MARKET, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        perror("파일 생성 실패");
        return 1;
    }
    const char* tags = "[군사무기]+[보안]";
    write(fd, tags, strlen(tags));
    close(fd);
    printf("[Market] 아이템(101번)이 상점에 등록되었습니다. (태그: %s)\n", tags);

    printf("\n--- 2단계: 선착순 구매 (Atomic Rename) ---\n");
    // 유저가 구매 시도: 원자적 이름 변경을 통해 권한 획득 (경쟁 조건 방어)
    if (rename(ITEM_FILE_MARKET, ITEM_FILE_USER1) == 0) {
        printf("[User 1] 구매 성공! 아이템이 인벤토리(샌드박스)로 안전하게 이동되었습니다.\n");
    } else {
        perror("[User 1] 구매 실패 (누군가 먼저 샀거나 에러)");
    }

    // 파일 존재 검증 (Fail-Safe)
    if (access(ITEM_FILE_USER1, F_OK) == 0) {
        printf("\n--- 3단계: 인벤토리 조회 및 태그 검증 (/inventory) ---\n");
        char buffer[256] = {0};
        int read_fd = open(ITEM_FILE_USER1, O_RDONLY);
        if (read_fd != -1) {
            read(read_fd, buffer, sizeof(buffer) - 1);
            close(read_fd);
            printf("[System] user_1의 인벤토리에서 아이템 101번 발견!\n");
            printf("[System] 파일 내용(태그) 분석 결과: %s\n", buffer);
        }
    }

    printf("\n--- 4단계: 아이템 파기 (/dispose) ---\n");
    if (unlink(ITEM_FILE_USER1) == 0) {
        printf("[System] 아이템 파쇄 완료. 인벤토리 공간이 확보되었습니다.\n");
    } else {
        perror("[System] 아이템 파쇄 실패");
    }

    printf("\n=== 샌드박스 검증 완료 ===\n");
    return 0;
}
