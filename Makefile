# 컴파일러 및 기본 옵션
CC = gcc
CFLAGS = -Wall -Wextra -g -I.

# 링커 옵션 (라이브러리 연결)
LDFLAGS_SERVER = -pthread
#LDFLAGS_CLIENT = -pthread -lncurses # 한글 깨짐 시 -lncursesw 로 변경
LDFLAGS_CLIENT = -pthread -lncursesw

# 소스 파일 자동 탐색
SRCS_COMMON = $(wildcard src/common/*.c) protocol.c
SRCS_SERVER = $(wildcard src/server/*.c userdb/*.c handlers/*.c market/*.c npc/*.c sandbox/*.c)
SRCS_CLIENT = $(wildcard src/client/*.c)

# 오브젝트 파일 변환
OBJS_COMMON = $(SRCS_COMMON:.c=.o)
OBJS_SERVER = $(SRCS_SERVER:.c=.o)
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

# 최종 실행 파일 이름
TARGET_SERVER = server
TARGET_CLIENT = client
TARGET_UI = client_ui

# 기본 타겟: make를 치면 둘 다 빌드됨
all: $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_UI)

# 헤더 파일 의존성 강제 (protocol.h 수정 시 모든 .c 파일 재빌드)
$(OBJS_COMMON) $(OBJS_SERVER) $(OBJS_CLIENT) ui_client.o: protocol.h

# 서버 빌드 규칙 (공통 모듈 포함)
$(TARGET_SERVER): $(OBJS_COMMON) $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_SERVER)

# 클라이언트 빌드 규칙 (공통 모듈 포함)
$(TARGET_CLIENT): $(OBJS_COMMON) $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_CLIENT)

$(TARGET_UI): $(OBJS_COMMON) ui_client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_CLIENT)

# 개별 .c 파일을 .o 파일로 컴파일하는 규칙
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 빌드 결과물 청소 (make clean)
clean:
	rm -f $(OBJS_COMMON) $(OBJS_SERVER) $(OBJS_CLIENT) ui_client.o \
	      $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_UI)

.PHONY: all clean
