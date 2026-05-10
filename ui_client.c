#include <ncurses.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>

#define MIN_WIDTH 100
#define MIN_HEIGHT 30
#define INPUT_MAX 256

typedef struct{
    WINDOW *news;
    WINDOW *market;
    WINDOW *bounty;
    WINDOW *inventory;
    WINDOW *log;
    WINDOW *prompt;
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
    ui->news=newwin(new_h,w,y,0);
    y+=news_h;

    ui->market=newwin(middle_h, market_w, y, 0);
    ui->bounty=newwin(middle_h, bounty_w, y, market_w);
    y+=middle_h;

    ui->inventory=newwin(inventory_h, w, y, 0);
    y+=inventory_h;

    ui->log=newwin(log_h, w, y, 0);
    y+=log_h;

    ui->prompt=newwin(prompt_h, w, h-1, 0);

    keypad(ui->prompt, TRUE);
    nodelay(ui->prompt, TRUE);
    scrollok(ui->log, TRUE);
    draw_title(ui->news, "다크웹 중앙 뉴스망 / 브로드캐스트 브리핑");
    mvwprintw(ui->news, 1, 2, "> [속보] A정치인 비자금 스캔들 관련 기밀 가치 상승 전망");
    mvprintw(ui->news, 2, 2, "> [SYSTEM] 시장 내 악성 재고 만료 처리가 곧 진행됩니다.");

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

    draw_title(ui->log, "시스템 알림 및 채팅 로그");
    mvwprintw(ui->log, 1, 2, "[SYSTEM] UNDERFLOW UI started.");
    mvwprintw(ui->log, 2, 2, "[SYSTEM] Resize-safe ncurses layout enabled.");
    mvwprintw(ui->log, 3, 2, "[TIP] 터미널 창 크기를 조절해보세요.");

    mvwprintw(ui->prompt, 0, 0, "> ");
    if (w >= 45) {
        mvwprintw(ui->prompt, 0, w - 40, "[ /help | ESC: 취소 | /quit: 종료 ]");
    }

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
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    UI ui = {0};

    if (!check_size()) {
        endwin();
        return 1;
    }

    draw_ui(&ui);

    char input[INPUT_MAX] = {0};
    int len = 0;
    int running = 1;

    while (running) {
        int ch = wgetch(ui.prompt);

        if (ch == ERR) {
            continue;
        }

        if (ch == KEY_RESIZE) {
            len = 0;
            memset(input, 0, sizeof(input));
            redraw(&ui);
            continue;
        }

        if (ch == '\n') {
            input[len] = '\0';

            if (strcmp(input, "/quit") == 0) {
                running = 0;
                continue;
            }

            wprintw(ui.log, "\n[YOU] %s", input);
            wrefresh(ui.log);

            len = 0;
            memset(input, 0, sizeof(input));
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

            if (len < INPUT_MAX - 1 && len < w - 45) {
                input[len] = ch;
                len++;
                input[len] = '\0';
            }
        }

        int h, w;
        getmaxyx(stdscr, h, w);

        werase(ui.prompt);
        mvwprintw(ui.prompt, 0, 0, "> %s", input);

        if (w >= 45) {
            mvwprintw(ui.prompt, 0, w - 40,
                      "[ /help | ESC: 취소 | /quit: 종료 ]");
        }

        wrefresh(ui.prompt);
    }

    delete_windows(&ui);
    endwin();

    printf("UNDERFLOW UI safely closed.\n");
    return 0;
}
