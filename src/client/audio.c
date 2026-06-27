#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ncurses.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_BGM_FILES 32
#define MAX_PATH_LEN  256

static char g_playlist[MAX_BGM_FILES][MAX_PATH_LEN];
static int  g_bgm_count = 0;
static int  g_current_track = -1;
static volatile int   g_audio_active = 0;
static volatile pid_t g_child_pid = -1;   // 현재 재생 중인 디코더 자식 PID (-1=없음)
static pthread_t g_audio_tid;

// PATH 환경변수의 각 디렉토리를 순회하며 실행 가능한 바이너리를 찾는다.
// 쉘(system/which) 호출 없이 순수 C로 access(X_OK) 검사만 수행한다.
static int is_in_path(const char *bin) {
    const char *path = getenv("PATH");
    if (!path) return 0;

    char dirs[4096];
    snprintf(dirs, sizeof(dirs), "%s", path);

    char *save = NULL;
    for (char *dir = strtok_r(dirs, ":", &save); dir != NULL;
         dir = strtok_r(NULL, ":", &save)) {
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%.200s/%.40s", dir, bin);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

// 현재 리눅스 시스템에 실행 가능한 오디오 커맨드가 있는지 우선순위 프로빙합니다.
static const char* probe_audio_player(void) {
    if (is_in_path("mpg123")) return "mpg123";
    if (is_in_path("ffplay")) return "ffplay";
    if (is_in_path("cvlc"))   return "cvlc";
    return NULL; // 사용 가능한 플레이어 없음 (무음 모드 폴백)
}

// 오디오를 재생하는 자식 프로세스 구동 및 완주 대기 스레드
static void* audio_loop_thread_func(void* arg) {
    (void)arg;
    const char* player = probe_audio_player();
    if (!player) {
        return NULL;   // 가용 플레이어 없음 — 무음 모드 (ncurses 화면 보호 위해 콘솔 출력 없음)
    }

    g_audio_active = 1;

    // WSL2/WSLg PulseAudio 환경에서 버퍼 언더런(크래클링)을 완화하려고
    // 재생 버퍼 지연을 키운다. BGM이라 지연 체감은 무관. (자식이 env 상속)
    int has_pulse = (getenv("PULSE_SERVER") != NULL);
    if (has_pulse) setenv("PULSE_LATENCY_MSEC", "500", 0);

    while (g_audio_active) {
        if (g_bgm_count == 0) break;

        // 곡 선택 (셔플)
        g_current_track = rand() % g_bgm_count;
        char *filepath = g_playlist[g_current_track];

        // 플레이어별 인자 배열 조립 (system() 대신 fork/exec — 자식 PID를 직접 추적해
        // 종료 시 이 클라이언트가 띄운 디코더만 정확히 kill한다)
        // --buffer 2048 / -volume 90: 버퍼 고갈 찌지직 방지 및 음량
        char *pargv[12];
        int ai = 0;
        if (strcmp(player, "mpg123") == 0) {
            pargv[ai++] = "mpg123";
            if (has_pulse) {
                pargv[ai++] = "-o"; pargv[ai++] = "pulse";  // alsa 거치지 말고 pulse로 바로
                // bgm은 전부 44100Hz라서 -r로 리샘플 강제 안 해도 됨
            }
            pargv[ai++] = "--buffer"; pargv[ai++] = "8192";  // 디코드 선행 버퍼 확대(언더런 완충)
            pargv[ai++] = "-q";
        } else if (strcmp(player, "ffplay") == 0) {
            pargv[ai++] = "ffplay"; pargv[ai++] = "-nodisp"; pargv[ai++] = "-autoexit";
            pargv[ai++] = "-loglevel"; pargv[ai++] = "quiet";
            pargv[ai++] = "-volume"; pargv[ai++] = "90";
        } else { // cvlc
            pargv[ai++] = "cvlc"; pargv[ai++] = "--play-and-exit"; pargv[ai++] = "--quiet";
        }
        pargv[ai++] = filepath;
        pargv[ai]   = NULL;

        pid_t pid = fork();
        if (pid == 0) {
            // 자식: 표준 입출력을 /dev/null로 무음화 후 디코더 exec
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            execvp(pargv[0], pargv);
            _exit(127);  // exec 실패
        } else if (pid > 0) {
            // 부모: PID 기록 후 한 곡이 끝날 때까지 대기 (동기 블로킹)
            g_child_pid = pid;
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* 재시도 */ }
            g_child_pid = -1;
        } else {
            usleep(200000);  // fork 실패 — 잠시 쉬고 재시도
        }

        // 종료 요청 시 다음 곡을 띄우지 않고 즉시 탈출 (셧다운 지연 최소화)
        if (!g_audio_active) break;
        usleep(300000);  // 곡 간 짧은 간격
    }
    return NULL;
}

int audio_init(const char *bgm_dir_path) {
    // 믹서 채널 경합 방지: 이미 오디오 엔진이 돌고 있으면 중복 가동 차단
    if (g_audio_active) {
        return 0;
    }

    g_bgm_count = 0;
    DIR *dir = opendir(bgm_dir_path);
    if (!dir) {
        return 0;   // bgm 디렉토리 없음 — 무음 기동 (콘솔 출력 없음: ncurses 화면 보호)
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            // 확장자가 .mp3 인지 검증
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".mp3") == 0) {
                // 경로가 버퍼(256)를 넘지 않게 %s마다 길이를 잘라서 합침
                snprintf(g_playlist[g_bgm_count], MAX_PATH_LEN, "%.180s/%.70s",
                         bgm_dir_path, entry->d_name);
                g_bgm_count++;
                if (g_bgm_count >= MAX_BGM_FILES) break;
            }
        }
    }
    closedir(dir);

    if (g_bgm_count == 0) {
        return 0;   // 재생 가능한 .mp3 없음 — 무음 기동 (콘솔 출력 없음)
    }

    srand(time(NULL));
    g_audio_active = 1; // 기동 상태 우선 마크
    if (pthread_create(&g_audio_tid, NULL, audio_loop_thread_func, NULL) == 0) {
        return 0;
    }
    g_audio_active = 0; // 스레드 기동 실패 시 마크 해제
    return -1;
}

void audio_shutdown(void) {
    if (!g_audio_active && g_child_pid <= 0) return;  // 미기동/중복 호출 가드

    g_audio_active = 0;   // 루프가 다음 곡을 띄우지 않도록 먼저 마크

    // 이 클라이언트가 띄운 디코더 자식만 정확히 종료 (머신 전체 pkill 금지).
    // active=0을 먼저 세웠으므로 이 자식 이후 새 자식은 생성되지 않는다.
    pid_t pid = g_child_pid;
    if (pid > 0) {
        kill(pid, SIGTERM);   // 자식 종료 → 루프의 waitpid가 반환되고 스레드가 탈출
    }

    pthread_join(g_audio_tid, NULL);
}

void audio_play_alarm(void) {
    beep();
    flash();
}
