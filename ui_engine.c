#include <ncurses.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MIN_WIDTH 100
#define MIN_HEIGHT 30
#define INPUT_MAX 256
#define QUICK_HELP "[ /help | ESC: cancel | /quit: exit]"

typedef struct{
    WINDOW *news;
    WINDOW *market;
    WINDOW *bounty;
    WINDOW *inventory;
    WINDOW *log;
    WINDOW **prompt;
}UI;

void delete_windows(UI *ui)
{
    if(ui->news)delwin(ui->news);
    if(ui->market)delwin(ui->market);
    if(ui->bounty)delwin(ui->bounty);
    if(ui->inventory)delwin(ui->inventory);
    if(ui->log)delwin(ui->log);
    if(ui->prompt)delwin(ui->prompt);

    memset(ui,0,sizeof(UI));
}
int check_size(void)
{
    int h, w;
    getmaxyx(stdscr,h,w);
     if (w < MIN_WIDTH || h < MIN_HEIGHT) {
        clear();
        mvprintw(h / 2 - 1, 2, "해상도를 더 키우고 실행해 주세요.");
        mvprintw(h / 2, 2, "현재 크기: %dx%d / 최소 크기: %dx%d",
                 w, h, MIN_WIDTH, MIN_HEIGHT);
        mvprintw(h / 2 + 2, 2, "아무 키나 누르면 종료됩니다.");
        refresh();
        getch();
        return 0;
    }

    return 1;
}

void draw_title(WINDOW *win, const char *title) {
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " %s ", title);
}

void get_time_string(char *buf, int size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    snprintf(buf, size, "%02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);
}

void log_message(UI *ui, const char *msg) {
    char time_buf[16];

    get_time_string(time_buf, sizeof(time_buf));

    wprintw(ui->log, "\n[%s] %s", time_buf, msg);
    wrefresh(ui->log);
}

int input_limit_width(void) {
    int h, w;
    int help_len = strlen(QUICK_HELP);

    getmaxyx(stdscr, h, w);

    return w - help_len - 5;
}

void restore_prompt_cursor(UI *ui, const char *input, int len) {
    int h, w;
    int help_len = strlen(QUICK_HELP);

    werase(ui->prompt);
    mvwprintw(ui->prompt, 0, 0, "> %s", input);

    if (w > help_len + 2) {
        mvwprintw(ui->prompt, 0, w - help_len - 1, "%s", QUICK_HELP);
    }

    wmove(ui->prompt, 0, len + 2);
    wrefresh(ui->prompt);
}

void draw_ui(UI *ui) {
    int h, w;
    int y = 0;

    getmaxyx(stdscr, h, w);

    clear();
    refresh();

    /*
        6분할 구조

        news
        market | bounty
        inventory
        log
        prompt
    */

    int news_h = 4;
    int prompt_h = 1;
    int inventory_h = 5;

    int middle_h = h * 45 / 100;

    /*
        잔여 공간 보정:
        마지막 세로 영역인 log_h는 전체 높이에서
        이미 사용한 높이를 뺀 값으로 계산한다.
    */
    int log_h = h - news_h - middle_h - inventory_h - prompt_h;

    int market_w = w * 50 / 100;

    /*
        잔여 공간 보정:
        bounty_w는 w * 50 / 100이 아니라
        전체 폭에서 market_w를 뺀 나머지를 사용한다.
    */
    int bounty_w = w - market_w;

    ui->news = newwin(news_h, w, y, 0);
    y += news_h;

    ui->market = newwin(middle_h, market_w, y, 0);
    ui->bounty = newwin(middle_h, bounty_w, y, market_w);
    y += middle_h;

    ui->inventory = newwin(inventory_h, w, y, 0);
    y += inventory_h;

    ui->log = newwin(log_h, w, y, 0);
    y += log_h;

    ui->prompt = newwin(prompt_h, w, y, 0);

    keypad(ui->prompt, TRUE);
    nodelay(ui->prompt, TRUE);

    scrollok(ui->log, TRUE);
    scrollok(ui->prompt, FALSE);

    draw_title(ui->news, "다크웹 중앙 뉴스망 / 브로드캐스트 브리핑");
    mvwprintw(ui->news, 1, 2, "> [속보] A정치인 비자금 스캔들 관련 기밀 가치 상승 전망");
    mvwprintw(ui->news, 2, 2, "> [SYSTEM] 시장 내 악성 재고 만료 처리가 곧 진행됩니다.");

    draw_title(ui->market, "글로벌 마켓 (Market)");
    mvwprintw(ui->market, 1, 2, "ID  | 태그 조합 및 기밀 정보        | 가치");
    mvwprintw(ui->market, 2, 2, "201 | [B기업][재무제표]             | $300");
    mvwprintw(ui->market, 3, 2, "202 | [국방부][기밀도면]            | $500");
    mvwprintw(ui->market, 4, 2, "203 | [민간][주민번호]              | $100");
    mvwprintw(ui->market, 5, 2, "204 | [A정치인][녹취록]             | $800");
    mvwprintw(ui->market, middle_h - 2, 2, "> 독점 매물 12개가 스폰됨.");

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

    draw_title(ui->log, "시스템 알림 및 채팅 로그");
    mvwprintw(ui->log, 1, 2, "[SYSTEM] UNDERFLOW UI started.");
    mvwprintw(ui->log, 2, 2, "[SYSTEM] Resize-safe ncurses layout enabled.");
    mvwprintw(ui->log, 3, 2, "[TIP] 터미널 창 크기를 조절해보세요.");

    restore_prompt_cursor(ui, "", 0);

    wrefresh(ui->news);
    wrefresh(ui->market);
    wrefresh(ui->bounty);
    wrefresh(ui->inventory);
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

int main(void) {
    UI ui = {0};

    char input[INPUT_MAX];
    int len = 0;
    int running = 1;

    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    timeout(0);

    memset(input, 0, sizeof(input));

    if (!check_size()) {
        endwin();
        return 1;
    }

    draw_ui(&ui);

    while (running) {
        int ch = wgetch(ui.prompt);

        if (ch == ERR) {
            continue;
        }

        if (ch == KEY_RESIZE) {
            len = 0;
            memset(input, 0, sizeof(input));
            redraw(&ui);
            restore_prompt_cursor(&ui, input, len);
            continue;
        }

        if (ch == '\n') {
            input[len] = '\0';

            if (strcmp(input, "/quit") == 0) {
                running = 0;
                continue;
            }

            if (len > 0) {
                char msg[INPUT_MAX + 16];
                snprintf(msg, sizeof(msg), "[YOU] %s", input);
                log_message(&ui, msg);
            }

            len = 0;
            memset(input, 0, sizeof(input));
            restore_prompt_cursor(&ui, input, len);
            continue;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                input[len] = '\0';
            }

            restore_prompt_cursor(&ui, input, len);
            continue;
        }

        if (ch == 27) {
            len = 0;
            memset(input, 0, sizeof(input));
            log_message(&ui, "[SYSTEM] 입력이 취소되었습니다.");
            restore_prompt_cursor(&ui, input, len);
            continue;
        }

        if (ch >= 32 && ch <= 126) {
            int limit = input_limit_width();

            /*
                Buffer Overflow 차단:
                입력창 가용 너비를 초과하는 타건은
                input 배열에 저장하지 않고 무시한다.
            */
            if (len < INPUT_MAX - 1 && len < limit) {
                input[len] = ch;
                len++;
                input[len] = '\0';
            }

            restore_prompt_cursor(&ui, input, len);
            continue;
        }

        restore_prompt_cursor(&ui, input, len);
    }

    delete_windows(&ui);
    endwin();

    printf("UNDERFLOW UI safely closed.\n");

    return 0;

}