#include <ncurses.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include "protocol.h"

#define MIN_WIDTH 100
#define MIN_HEIGHT 30
#define INPUT_MAX 256
#define QUICK_HELP "[ /quit | ESC: 취소 | 일반채팅 전송 ]"

typedef enum {
    STATE_NORMAL,
    STATE_PANIC,
    STATE_GAME_OVER
} ClientState;

typedef struct{
    WINDOW *news;
    WINDOW *market;
    WINDOW *bounty;
    WINDOW *inventory;
    WINDOW *log_border;
    WINDOW *log;
    WINDOW *prompt;
}UI;

volatile sig_atomic_t is_panic = 0;

void safe_endwin(void)
{
    if(!isendwin()) {
        endwin();
    }
}

void handle_sigint(int sig)
{
    (void)sig;
    is_panic = 1;
}

void get_time_string(char *buf, int size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(buf, size, "%02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);
}

void log_message(UI *ui, const char *msg)
{
    char time_buf[16];
    get_time_string(time_buf, sizeof(time_buf));
    wprintw(ui->log, "\n[%s] %s", time_buf, msg);
    wrefresh(ui->log);
}

void delete_windows(UI *ui)
{
    if(ui->news)delwin(ui->news);
    if(ui->market)delwin(ui->market);
    if(ui->bounty)delwin(ui->bounty);
    if(ui->inventory)delwin(ui->inventory);
    if(ui->log)delwin(ui->log);
    if(ui->log_border)delwin(ui->log_border);
    if(ui->prompt)delwin(ui->prompt);

    memset(ui,0,sizeof(UI));
}

void render_tags(WINDOW *win, int y, int x, uint32_t mask)
{
    int first = 1;
    wmove(win, y, x);
    for (int i = 0; i < 10; i++) {
        uint32_t bit = (1U << i);
        if (mask & bit) {
            if (!first) {
                wprintw(win, "+");
            }
            wprintw(win, "[%s]", get_tag_name(bit));
            first = 0;
        }
    }
    if (first) {
        wprintw(win, "[NO_TAG]");
    }
}

void render_glitch(UI *ui)
{
    int h, w;
    getmaxyx(ui->bounty, h, w);

    start_color();
    use_default_colors();

    init_pair(1, COLOR_RED, -1);
    init_pair(2, COLOR_GREEN, -1);
    init_pair(3, COLOR_CYAN, -1);
    init_pair(4, COLOR_MAGENTA, -1);
    init_pair(5, COLOR_YELLOW, -1);

    srand(time(NULL));

    for (int i = 1; i < h - 1; i++) {
        int color = 1 + rand() % 5;
        int x = 2 + rand() % (w - 10);

        wattron(ui->bounty, COLOR_PAIR(color));
        wattron(ui->bounty, A_REVERSE);

        mvwprintw(ui->bounty, i, x, "#@!%c%c%d",
                  'A' + rand() % 26,
                  'a' + rand() % 26,
                  rand() % 100);

        wattroff(ui->bounty, A_REVERSE);
        wattroff(ui->bounty, COLOR_PAIR(color));
    }

    wrefresh(ui->bounty);
    log_message(ui, "[경고] /rumor 사보타주 발생: VIP 보드가 일시 손상되었습니다.");
}

void render_panic_mode(UI *ui, const char *passcode, int time_limit)
{
    (void)ui;
    clear();
    refresh();

    int h, w;
    getmaxyx(stdscr, h, w);
    (void)h;

    WINDOW *panic_win = newwin(13, 60, 5, (w - 60) / 2);
    box(panic_win, 0, 0);
    wattron(panic_win, A_REVERSE);
    mvwprintw(panic_win, 1, 15, " !!! POLICE RAID DETECTED !!! ");
    wattroff(panic_win, A_REVERSE);

    mvwprintw(panic_win, 4, 5, "긴급 서버 수색 및 연결 파기 시도 중...");
    mvwprintw(panic_win, 6, 5, "제한 시간: %d 초", time_limit);
    mvwprintw(panic_win, 8, 5, "수색 무력화 우회 암호: %s", passcode);
    mvwprintw(panic_win, 10, 5, "주의: 긴급 암호를 신속히 입력하십시오!");

    wrefresh(panic_win);
    delwin(panic_win);
}

void render_game_over(const char *msg)
{
    clear();
    refresh();

    int h, w;
    getmaxyx(stdscr, h, w);

    WINDOW *go_win = newwin(10, 60, (h - 10) / 2, (w - 60) / 2);
    box(go_win, 0, 0);
    wattron(go_win, A_REVERSE);
    mvwprintw(go_win, 1, 23, " GAME OVER ");
    wattroff(go_win, A_REVERSE);

    mvwprintw(go_win, 4, 4, "사유: %s", msg);
    mvwprintw(go_win, 7, 14, "아무 키나 누르면 종료됩니다.");
    wrefresh(go_win);
    getch();
    delwin(go_win);

    safe_endwin();
    exit(0);
}

int check_size(void)
{
    int h,w;
    getmaxyx(stdscr, h, w);

    if(w<MIN_WIDTH||h<MIN_HEIGHT)
    {
        clear();
        mvprintw(h/2-1, 2, "해상도를 더 키우고 실행해주세요.");
        mvprintw(h/2,2,"현재 크기: %dx%d / 최소 크기: %dx%d", w, h, MIN_WIDTH, MIN_HEIGHT);
        mvprintw(h/2+2, 2, "아무 키나 누르면 종료됩니다.");
        refresh();
        getch();
        return 0;
    }
    return 1;
}
void draw_title(WINDOW *win, const char *title)
{
    box(win, 0, 0);
    mvwprintw(win,0,2," %s ", title);
}
void draw_ui(UI*ui)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    clear();//현재 화면 내용 전부 지우고
    refresh();//지금까지 바뀐 내용을 실제 터미널 화면에 반영(메모리버퍼 내용 출력)->새 레이아웃 다시 그리기
    int news_h=4;//뉴스
    int prompt_h=1;//입력창
    int inventory_h=5;//인벤토리
    int middle_h=h*45/100;//터미널 전체 높이 중 45% market,bounty 영역 높이에 사용
    int log_h=h-news_h-middle_h-inventory_h-prompt_h;//나머지 로그창

    int market_w=w/2;
    int bounty_w=w-market_w;

    int y=0;
    ui->news=newwin(news_h,w,y,0);
    y+=news_h;

    ui->market=newwin(middle_h, market_w, y, 0);
    ui->bounty=newwin(middle_h, bounty_w, y, market_w);
    y+=middle_h;

    ui->inventory=newwin(inventory_h, w, y, 0);
    y+=inventory_h;

    ui->log_border=newwin(log_h, w, y, 0);
    ui->log=derwin(ui->log_border, log_h - 2, w - 2, 1, 1);
    y+=log_h;

    ui->prompt=newwin(prompt_h, w, h-1, 0);

    keypad(ui->prompt, TRUE);
    nodelay(ui->prompt, TRUE);
    scrollok(ui->log, TRUE);
    draw_title(ui->news, "다크웹 중앙 뉴스망 / 브로드캐스트 브리핑");
    mvwprintw(ui->news, 1, 2, "> [속보] A정치인 비자금 스캔들 관련 기밀 가치 상승 전망");
    mvwprintw(ui->news, 2, 2, "> [SYSTEM] 시장 내 악성 재고 만료 처리가 곧 진행됩니다.");

    draw_title(ui->market, "글로벌 마켓 (Market)");
    mvwprintw(ui->market, 1, 2, "ID  | 태그 조합 및 기밀 정보        | 가치");
    mvwprintw(ui->market, 2, 2, "201 | [B기업][재무제표]             | $300");
    mvwprintw(ui->market, 3, 2, "202 | [국방부][기밀도면]            | $500");
    mvwprintw(ui->market, 4, 2, "203 | [민간][주민번호]              | $100");
    mvwprintw(ui->market, 5, 2, "204 | [A정치인][녹취록]             | $800");
    mvwprintw(ui->market, 7, 2, "> 독점 매물 12개가 스폰됨.");


    draw_title(ui->bounty, "VIP NPC 의뢰 보드 (Bounty)");
    mvwprintw(ui->bounty, 1, 2, "#011 익명 공매도 세력");
    mvwprintw(ui->bounty, 2, 2, "[목표]: [B기업] + [재무제표]");
    mvwprintw(ui->bounty, 3, 2, "[보상]: $1,200 | [제한 시간]: 1m 20s");

    mvwprintw(ui->bounty, 5, 2, "#012 해외 용병단");
    mvwprintw(ui->bounty, 6, 2, "[목표]: [국방부] + [기밀도면]");
    mvwprintw(ui->bounty, 7, 2, "[보상]: $4,500 | [제한 시간]: 0m 45s");

    draw_title(ui->inventory, "내 샌드박스 인벤토리 (운용 가능 슬롯: 5) [잔고: $1,450]");
    mvwprintw(ui->inventory, 1, 2, "[01] ID:088 | [국방부][GPS좌표] | 정상 보관중");
    mvwprintw(ui->inventory, 2, 2, "[02] ID:154 | [A정치인][비자금] | 동결 재고");
    mvwprintw(ui->inventory, 3, 2, "[03] 빈 슬롯 | [04] 빈 슬롯 | [05] 빈 슬롯");

    draw_title(ui->log_border, "시스템 알림 및 채팅 로그");
    wprintw(ui->log, "[SYSTEM] UNDERFLOW UI started.\n");
    wprintw(ui->log, "[SYSTEM] Resize-safe ncurses layout enabled.\n");
    wprintw(ui->log, "[TIP] 터미널 창 크기를 조절해보세요.");

    mvwprintw(ui->prompt, 0, 0, "> ");
    if (w >= 45) {
        mvwprintw(ui->prompt, 0, w - 40, "[ /help | ESC: 취소 | /quit: 종료 ]");
    }

    wrefresh(ui->news);
    wrefresh(ui->market);
    wrefresh(ui->bounty);
    wrefresh(ui->inventory);
    wrefresh(ui->log_border);
    wrefresh(ui->log);
    wrefresh(ui->prompt);
}
void redraw(UI *ui) {
    delete_windows(ui);
    clear();
    refresh();

    if (!check_size()) {
        endwin();
        exit(0);
    }

    draw_ui(ui);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <user_key>\n", argv[0]);
        return 1;
    }
    const char *user_key = argv[1];

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(8080);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        perror("connect error");
        return 1;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_REQ_LOGIN;
    strncpy(pkt.body.login.key, user_key, MAX_KEY_LEN - 1);
    packet_send(sock, &pkt);

    if (packet_recv(sock, &pkt) <= 0 || pkt.type != PKT_RES_LOGIN_OK) {
        printf("Login Failed.\n");
        close(sock);
        return 1;
    }

    atexit(safe_endwin);
    signal(SIGINT, handle_sigint);

    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    UI ui = {0};

    if (!check_size()) {
        safe_endwin();
        return 1;
    }

    draw_ui(&ui);

    char input[INPUT_MAX] = {0};
    int len = 0;
    int running = 1;
    int server_lost = 0;
    ClientState state = STATE_NORMAL;

    while (running) {
        if (is_panic) {
            is_panic = 0;
            state = STATE_PANIC;
            render_panic_mode(&ui, "PURGE", 30);
            len = 0;
            memset(input, 0, sizeof(input));
        }

        int ch = wgetch(ui.prompt);

        if (ch == ERR) {
            struct timeval tv = {0, 0};
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            if (select(sock + 1, &readfds, NULL, NULL, &tv) > 0) {
                int recv_rc = packet_recv(sock, &pkt);
                if (recv_rc <= 0) {
                    server_lost = 1;
                    running = 0;
                    break;
                }
                
                if (pkt.type == PKT_EVT_CHAT) {
                    wprintw(ui.log, "\n[Broadcast from %s] %s", pkt.body.chat_evt.sender_key, pkt.body.chat_evt.text);
                    wrefresh(ui.log);
                }
                else if (pkt.type == PKT_EVT_RUMOR_GLITCH) {
                    render_glitch(&ui);
                }
                else if (pkt.type == PKT_EVT_POLICE_RAID) {
                    state = STATE_PANIC;
                    render_panic_mode(&ui, pkt.body.police_raid.passcode, pkt.body.police_raid.time_limit_sec);
                    len = 0;
                    memset(input, 0, sizeof(input));
                }
                else if (pkt.type == PKT_RES_MINIGAME_OK) {
                    state = STATE_NORMAL;
                    redraw(&ui);
                    log_message(&ui, "[SYSTEM] 수색 무력화 성공! 정상 마켓 화면으로 복원되었습니다.");
                }
                else if (pkt.type == PKT_EVT_GAME_OVER) {
                    state = STATE_GAME_OVER;
                    render_game_over(pkt.body.endgame.message);
                }
                else if (pkt.type == PKT_EVT_VICTORY) {
                    state = STATE_GAME_OVER;
                    render_game_over(pkt.body.endgame.message);
                }
                else if (pkt.type == PKT_EVT_MARKET_SPAWN) {
                    char msg[MAX_TEXT_LEN];
                    snprintf(msg, sizeof(msg), "[MARKET] 신규 매물 스폰! ID:%d | 가격:$%d | 명칭: %s",
                             pkt.body.market_spawn.doc_id, pkt.body.market_spawn.base_price, pkt.body.market_spawn.name);
                    log_message(&ui, msg);
                }
                else if (pkt.type == PKT_EVT_MARKET_REMOVE) {
                    char msg[MAX_TEXT_LEN];
                    snprintf(msg, sizeof(msg), "[MARKET] 매물 판매/소거 완료: ID:%d", pkt.body.market_remove.doc_id);
                    log_message(&ui, msg);
                }
                else if (pkt.type == PKT_EVT_NPC_SPAWN) {
                    char msg[MAX_TEXT_LEN];
                    snprintf(msg, sizeof(msg), "[NPC] 의뢰 등록! ID:%d | 보상:$%d",
                             pkt.body.npc_spawn.npc_id, pkt.body.npc_spawn.bounty);
                    log_message(&ui, msg);
                }
                else if (pkt.type == PKT_EVT_NPC_DESPAWN) {
                    char msg[MAX_TEXT_LEN];
                    snprintf(msg, sizeof(msg), "[NPC] 의뢰 수명만료: ID:%d", pkt.body.npc_despawn.npc_id);
                    log_message(&ui, msg);
                }
            }
            continue;
        }

        if (ch == KEY_RESIZE) {
            len = 0;
            memset(input, 0, sizeof(input));
            if (state == STATE_NORMAL) {
                redraw(&ui);
            } else if (state == STATE_PANIC) {
                render_panic_mode(&ui, "PURGE", 30);
            }
            continue;
        }

        if (ch == '\n') {
            input[len] = '\0';

            if (strcmp(input, "/quit") == 0) {
                running = 0;
                continue;
            }

            if (state == STATE_PANIC) {
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_MINIGAME_SUBMIT;
                strncpy(pkt.body.minigame.passcode, input, MAX_TEXT_LEN - 1);
                packet_send(sock, &pkt);

                len = 0;
                memset(input, 0, sizeof(input));
                continue;
            }

            if (strcmp(input, "/fake rumor") == 0) {
                render_glitch(&ui);
            }
            else if (strcmp(input, "/fake raid") == 0) {
                state = STATE_PANIC;
                render_panic_mode(&ui, "PURGE", 30);
            }
            else if (strcmp(input, "/fake ok") == 0) {
                state = STATE_NORMAL;
                redraw(&ui);
                log_message(&ui, "[SYSTEM] 로컬 강제 복구 완료.");
            }
            else if (strcmp(input, "/fake over") == 0) {
                state = STATE_GAME_OVER;
                render_game_over("로컬 강제 게임오버 테스트");
            }
            else {
                wprintw(ui.log, "\n[YOU] %s", input);
                wrefresh(ui.log);

                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_CHAT;
                strncpy(pkt.body.chat.text, input, MAX_TEXT_LEN - 1);
                packet_send(sock, &pkt);
            }

            len = 0;
            memset(input, 0, sizeof(input));
            continue;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                input[len] = '\0';
            }
        }
        else if (ch == 27) {
            len = 0;
            memset(input, 0, sizeof(input));
        }
        else if (ch >= 32 && ch <= 126) {
            int h, w;
            getmaxyx(stdscr, h, w);
            (void)h;

            if (len < INPUT_MAX - 1 && len < w - 45) {
                input[len] = ch;
                len++;
                input[len] = '\0';
            }
        }

        if (state == STATE_NORMAL) {
            int h, w;
            getmaxyx(stdscr, h, w);
            (void)h;

            werase(ui.prompt);
            mvwprintw(ui.prompt, 0, 0, "> %s", input);

            if (w >= 45) {
                mvwprintw(ui.prompt, 0, w - 40,
                          "[ /quit: 종료 | 일반채팅 전송 ]");
            }
            wrefresh(ui.prompt);
        } else if (state == STATE_PANIC) {
            mvprintw(12, (COLS - 60) / 2 + 5, "> 입력: %-25s", input);
            clrtoeol();
            refresh();
        }
    }

    delete_windows(&ui);
    safe_endwin();

    if (server_lost) {
        printf("[Disconnected] 서버와의 연결이 끊어졌습니다.\n");
    } else {
        printf("UNDERFLOW UI safely closed.\n");
    }
    return 0;
}
