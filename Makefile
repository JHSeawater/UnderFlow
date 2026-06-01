# 컴파일러 및 기본 옵션
# -MMD -MP : 컴파일 시 각 .o의 헤더 의존성을 .d 파일로 자동 생성 (헤더 수정 → 해당 .c 재빌드)
CC = gcc
CFLAGS = -Wall -Wextra -g -I. -Isrc/common -Isrc/client -MMD -MP

# 링커 옵션 (라이브러리 연결)
LDFLAGS_SERVER = -pthread
#LDFLAGS_CLIENT = -pthread -lncurses # 한글 깨짐 시 -lncursesw 로 변경
LDFLAGS_CLIENT = -pthread -lncursesw

# 소스 파일 자동 탐색
SRCS_COMMON = $(wildcard src/common/*.c)
SRCS_SERVER = $(wildcard src/server/*.c userdb/*.c)

# 오브젝트 파일 변환
OBJS_COMMON = $(SRCS_COMMON:.c=.o)
OBJS_SERVER = $(SRCS_SERVER:.c=.o)
AUDIO_OBJ   = src/client/audio.o      # ncurses UI 클라이언트가 쓰는 공유 오디오 모듈

# 최종 실행 파일 이름
TARGET_SERVER   = server
TARGET_UI       = client_ui
TARGET_WATCHDOG = watchdog

# 모든 .o에 대응하는 .d(헤더 의존성) 파일 목록
DEPS = $(OBJS_COMMON:.o=.d) $(OBJS_SERVER:.o=.d) $(AUDIO_OBJ:.o=.d) src/client/ui_client.d watchdog.d

# 기본 타겟
all: $(TARGET_SERVER) $(TARGET_UI) $(TARGET_WATCHDOG)

# -MMD가 생성한 헤더 의존성 자동 포함 (있을 때만; 최초 빌드 전엔 무시됨)
-include $(DEPS)

# 서버 빌드 규칙 (공통 모듈 포함)
$(TARGET_SERVER): $(OBJS_COMMON) $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_SERVER)

# ncurses UI 클라이언트 (공유 오디오 모듈 포함)
$(TARGET_UI): $(OBJS_COMMON) $(AUDIO_OBJ) src/client/ui_client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_CLIENT)

# 워치독 (단독 프로세스, 외부 라이브러리 불요)
$(TARGET_WATCHDOG): src/tools/watchdog.c
	$(CC) $(CFLAGS) -o $@ $<

# 개별 .c → .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 빌드 산출물 일괄 정리 (.d 의존성 파일 포함)
clean:
	rm -f $(OBJS_COMMON) $(OBJS_SERVER) $(AUDIO_OBJ) src/client/ui_client.o \
	      $(DEPS) \
	      $(TARGET_SERVER) $(TARGET_UI) $(TARGET_WATCHDOG)

# clean + 런타임 생성물(마스터/샌드박스 문서, 유저DB, 로그)까지 일괄 정리.
# .gitkeep은 보존(쉘 글롭이 숨김파일 미포함)하여 디렉토리 구조 유지.
distclean: clean
	rm -f *.log
	rm -f data/master/*.dat data/users.dat
	rm -rf data/users/*

.PHONY: all clean distclean
