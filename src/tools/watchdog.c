// 워치독 모니터 프로세스 (GDD §3.E)
// ------------------------------------------------------------
// fork/execv 로 자식 서버를 실행하고 waitpid 로 상태를 감시한다.
// 자식이 비정상 종료(시그널 또는 0이 아닌 exit code)되면 1초 backoff 후
// 즉시 재가동하여 무중단 운영을 보장한다.
// SIGINT/SIGTERM 을 받으면 자식에 시그널을 전달한 뒤 자신도 정상 종료.
// ------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

static volatile sig_atomic_t g_stop      = 0;
static volatile pid_t        g_child_pid = -1;

static void on_terminate(int sig) {
    g_stop = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, sig);
    }
}

int main(int argc, char *argv[]) {
    const char *server_path = (argc > 1) ? argv[1] : "./server";

    signal(SIGINT,  on_terminate);
    signal(SIGTERM, on_terminate);
    signal(SIGPIPE, SIG_IGN);

    printf("[Watchdog] Monitoring: %s\n", server_path);
    fflush(stdout);

    while (!g_stop) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("[Watchdog] fork");
            sleep(1);
            continue;
        }

        if (pid == 0) {
            // 자식: 서버 교체 실행
            char *args[] = { (char*)server_path, NULL };
            execv(server_path, args);
            perror("[Watchdog] execv");
            _exit(127);
        }

        // 부모: 자식 감시
        g_child_pid = pid;
        printf("[Watchdog] Server up (pid=%d)\n", pid);
        fflush(stdout);

        int   status = 0;
        pid_t w;
        do {
            w = waitpid(pid, &status, 0);
        } while (w < 0 && errno == EINTR && !g_stop);

        g_child_pid = -1;

        if (g_stop) {
            // 시그널로 종료 요청 — 자식 회수 후 워치독 자신도 종료
            if (w < 0) {
                // 자식 회수 못한 경우 한 번 더 시도 (no-block)
                waitpid(pid, &status, WNOHANG);
            }
            break;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            printf("[Watchdog] Server exited (code=%d)\n", code);
            if (code == 0) {
                // 정상 종료 — 워치독도 같이 종료
                break;
            }
        } else if (WIFSIGNALED(status)) {
            printf("[Watchdog] Server killed by signal %d\n", WTERMSIG(status));
        }

        printf("[Watchdog] Restarting in 1s...\n");
        fflush(stdout);
        sleep(1);
    }

    printf("[Watchdog] shutting down.\n");
    return 0;
}
