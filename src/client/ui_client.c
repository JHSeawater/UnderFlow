#include <ncurses.h>
#include <locale.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include "protocol.h"
#include "audio.h"

#define MIN_WIDTH 100
#define MIN_HEIGHT 30
#define INPUT_MAX 256
#define SB_CHAT_LINES 5      // 라운드 종료(에필로그) 스코어보드 하단에 보여줄 채팅 줄 수
#define QUICK_HELP "[ /quit | ESC: 취소 | 일반채팅 전송 ]"

// 사보타주 글리치 연출: 지속시간 + 프레임 간격(ms)
#define GLITCH_DURATION_SEC 12
#define GLITCH_FRAME_MS     90

typedef enum {
    STATE_NORMAL,
    STATE_PANIC,
    STATE_ROUND_END,    // GDD §F: 스코어보드 카운트다운 (채팅만 허용)
    STATE_GAME_OVER
} ClientMode;

typedef struct{
    WINDOW *news;
    WINDOW *market;
    WINDOW *bounty;
    WINDOW *inventory;
    WINDOW *log_border;
    WINDOW *log;
    WINDOW *prompt;
}UI;

// 라이브 보드를 그리기 위한 클라이언트 인메모리 상태
#define UI_MAX_MARKET 32
#define UI_MAX_NPC    16

typedef struct {
    int32_t  doc_id;
    uint32_t tags;
    int32_t  base_price;
    char     name[MAX_NAME_LEN];
    int      is_frozen;
} UiMarket;

typedef struct {
    int32_t  npc_id;
    uint32_t required_tags;
    int32_t  bounty;
} UiNPC;

typedef struct {
    UiMarket  mkt[UI_MAX_MARKET];
    int       mkt_count;
    UiNPC     npc[UI_MAX_NPC];
    int       npc_count;
    InvenItem inv[MAX_INVEN_SIZE];
    int       inv_count;
    int32_t   my_money;
    int32_t   my_goal;
    uint32_t  frozen_mask;
    char      last_news[MAX_TEXT_LEN];   // 마지막 속보 헤드라인

    // GDD §F 라운드 종료 스코어보드
    int            sb_escaped_count;
    ScoreEscaped   sb_escaped[MAX_SCORE_ESCAPED];
    int            sb_remaining_count;
    ScoreRemaining sb_remaining[MAX_SCORE_REMAINING];

    // 에필로그(라운드 종료) 채팅 링버퍼 — 스코어보드 하단에 표시
    char sb_chat[SB_CHAT_LINES][MAX_TEXT_LEN + MAX_KEY_LEN + 8];
    int  sb_chat_count;
} ClientState;

volatile sig_atomic_t g_quit_req = 0;   // 평상시 Ctrl+C 정상 종료 요청
volatile sig_atomic_t g_in_raid  = 0;   // 경찰 레이드 중이면 1 (Ctrl+C 차단)

void safe_endwin(void)
{
    audio_shutdown();
    if(!isendwin()) {
        endwin();
    }
}

// 평상시 Ctrl+C는 정상 종료 요청. 단, 경찰 레이드(STATE_PANIC) 중에는
// 차단하여 Ctrl+\(SIGQUIT) 비상 탈출로만 빠져나가도록 강제한다.
void handle_sigint(int sig)
{
    (void)sig;
    if (g_in_raid) return;     // 레이드 중 Ctrl+C 무시
    g_quit_req = 1;            // 평상시 종료 요청 플래그
}

// GDD §D: SIGQUIT(Ctrl+\)을 안전 탈출 경로로 매핑.
// 비상시 터미널 복구 후 즉시 종료. (endwin은 엄밀히 signal-safe는 아니지만 통상 패턴)
void handle_sigquit(int sig)
{
    (void)sig;
    if (!isendwin()) endwin();
    _exit(0);
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

// 태그 표시 순서. 기업 5종(아사사카/바이오젠/넥서스/밀리테크/레이븐)을 앞으로 모으고
// 나머지 분류 태그를 뒤에 둔다. 밀리테크/레이븐은 나중에 추가한 비트라 비트 순서로 돌면
// 뒤로 밀려서, 여기서 순서를 직접 지정한다.
static const uint32_t TAG_DISPLAY_ORDER[] = {
    TAG_CORP_A, TAG_CORP_B, TAG_CORP_C, TAG_CORP_D, TAG_CORP_E,
    TAG_CUSTOMER, TAG_FINANCE, TAG_MILITARY, TAG_GOVERNMENT,
    TAG_MEDICAL, TAG_RESEARCH, TAG_PERSONAL,
};
#define TAG_DISPLAY_COUNT (int)(sizeof(TAG_DISPLAY_ORDER) / sizeof(TAG_DISPLAY_ORDER[0]))

void render_tags(WINDOW *win, int y, int x, uint32_t mask)
{
    int first = 1;
    wmove(win, y, x);
    for (int i = 0; i < TAG_DISPLAY_COUNT; i++) {
        uint32_t bit = TAG_DISPLAY_ORDER[i];
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

static void tags_to_str(uint32_t mask, char *out, size_t out_sz) {
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < TAG_DISPLAY_COUNT; i++) {
        uint32_t bit = TAG_DISPLAY_ORDER[i];
        if (mask & bit) {
            int w = snprintf(out + len, out_sz - len, "[%s]", get_tag_name(bit));
            if (w > 0) len += (size_t)w;
        }
    }
    if (len == 0) snprintf(out, out_sz, "[NO_TAG]");
}

static void draw_title(WINDOW *win, const char *title);   // forward

static void render_market(UI *ui, ClientState *st) {
    if (!ui->market) return;
    werase(ui->market);
    draw_title(ui->market, "글로벌 마켓 (Market)");
    int h, w; getmaxyx(ui->market, h, w); (void)w;
    if (st->mkt_count == 0) {
        mvwprintw(ui->market, 1, 2, "(매물 없음 — 이벤트 스레드 대기 중)");
    } else {
        mvwprintw(ui->market, 1, 2, "ID    가격     상태   태그");
        int row = 2;
        for (int i = 0; i < st->mkt_count && row < h - 1; i++) {
            char tagbuf[128];
            tags_to_str(st->mkt[i].tags, tagbuf, sizeof(tagbuf));
            const char *status = st->mkt[i].is_frozen ? "FRZ" : "   ";
            if (st->mkt[i].is_frozen) wattron(ui->market, A_DIM);
            mvwprintw(ui->market, row, 2, "%-4d  $%-7d %-5s %s",
                      st->mkt[i].doc_id, st->mkt[i].base_price, status, tagbuf);
            if (st->mkt[i].is_frozen) wattroff(ui->market, A_DIM);
            row++;
        }
    }
    wrefresh(ui->market);
}

static void render_bounty(UI *ui, ClientState *st) {
    if (!ui->bounty) return;
    werase(ui->bounty);
    draw_title(ui->bounty, "VIP NPC 의뢰 보드 (Bounty)");
    int h, w; getmaxyx(ui->bounty, h, w); (void)w;
    if (st->npc_count == 0) {
        mvwprintw(ui->bounty, 1, 2, "(의뢰 없음 — 속보를 주시하라)");
    } else {
        int row = 1;
        for (int i = 0; i < st->npc_count && row + 2 < h - 1; i++) {
            char tagbuf[128];
            tags_to_str(st->npc[i].required_tags, tagbuf, sizeof(tagbuf));
            // 요구 태그가 동결되면 해당 의뢰는 사실상 수행 불가 → 힌트 표시
            int npc_frz = (st->npc[i].required_tags & st->frozen_mask) ? 1 : 0;
            if (npc_frz) wattron(ui->bounty, A_DIM);
            mvwprintw(ui->bounty, row,     2, "#%-4d  보상:$%d%s",
                      st->npc[i].npc_id, st->npc[i].bounty,
                      npc_frz ? "  [동결]" : "");
            mvwprintw(ui->bounty, row + 1, 4, "요구: %s", tagbuf);
            if (npc_frz) wattroff(ui->bounty, A_DIM);
            row += 3;
        }
    }
    wrefresh(ui->bounty);
}

static void render_inventory(UI *ui, ClientState *st) {
    if (!ui->inventory) return;
    werase(ui->inventory);
    char title[160];
    snprintf(title, sizeof(title), "내 샌드박스 (%d/%d) [잔고: $%d | 목표: $%d]",
             st->inv_count, MAX_INVEN_SIZE, st->my_money, st->my_goal);
    draw_title(ui->inventory, title);
    int h, w; getmaxyx(ui->inventory, h, w); (void)w;
    if (st->inv_count == 0) {
        mvwprintw(ui->inventory, 1, 2, "(보유 문서 없음)");
    } else {
        for (int i = 0; i < st->inv_count && (1 + i) < h - 1; i++) {
            char tagbuf[128];
            tags_to_str(st->inv[i].tags, tagbuf, sizeof(tagbuf));
            const char *frz = st->inv[i].is_frozen ? " [동결]" : "";
            if (st->inv[i].is_frozen) wattron(ui->inventory, A_DIM);
            mvwprintw(ui->inventory, 1 + i, 2, "[%d] ID:%-4d %s %s%s",
                      i + 1, st->inv[i].doc_id, st->inv[i].name, tagbuf, frz);
            if (st->inv[i].is_frozen) wattroff(ui->inventory, A_DIM);
        }
    }
    wrefresh(ui->inventory);
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 한 윈도우를 무작위 글리치 문자로 덮는다. (srand는 호출자/시작 시 1회만 — 프레임마다 달라짐)
static void glitch_window(WINDOW *win)
{
    if (!win) return;
    int h, w;
    getmaxyx(win, h, w);
    if (w < 8) return;
    for (int i = 0; i < h; i++) {
        int spots = 1 + rand() % 3;   // 줄당 손상 지점 수
        for (int s = 0; s < spots; s++) {
            int color = 1 + rand() % 5;
            int x = rand() % (w - 6);
            wattron(win, COLOR_PAIR(color) | A_REVERSE);
            mvwprintw(win, i, x, "#@!%c%c%d",
                      'A' + rand() % 26, 'a' + rand() % 26, rand() % 100);
            wattroff(win, COLOR_PAIR(color) | A_REVERSE);
        }
    }
    wnoutrefresh(win);
}

// 사보타주 글리치 한 프레임 — 데이터 패널 전체를 손상시킨다 (로그·프롬프트는 보존).
void render_glitch(UI *ui)
{
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_RED, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_CYAN, -1);
        init_pair(4, COLOR_MAGENTA, -1);
        init_pair(5, COLOR_YELLOW, -1);
    }
    glitch_window(ui->news);
    glitch_window(ui->market);
    glitch_window(ui->bounty);
    glitch_window(ui->inventory);
    doupdate();   // 4개 패널을 한 번에 갱신 (프레임 단위 깜빡임 최소화)
}

// 글리치 종료 시 손상된 데이터 패널만 원상 복구 (로그·프롬프트는 건드리지 않음).
static void recover_from_glitch(UI *ui, ClientState *st)
{
    render_market(ui, st);
    render_bounty(ui, st);
    render_inventory(ui, st);
    werase(ui->news);
    draw_title(ui->news, "다크웹 중앙 뉴스망 / 브로드캐스트");
    if (st->last_news[0]) mvwprintw(ui->news, 1, 2, "> %s", st->last_news);
    else                  mvwprintw(ui->news, 1, 2, "> 속보 대기 중...");
    wrefresh(ui->news);
}

// 패닉 박스를 그린다. 화면 clear는 호출자가 진입 시 1회만 수행하고,
// 카운트다운 틱·키 입력마다 이 함수가 박스만 제자리에 다시 그려 입력 echo를 유지한다.
// (clear를 매번 하지 않으므로 점멸에 입력창이 지워지지 않는다.)
void render_panic_mode(UI *ui, const char *passcode, int time_limit, const char *input)
{
    (void)ui;

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(20, COLOR_WHITE, COLOR_RED);   // 박스 본문 (흰 글자 / 빨강 배경)
        init_pair(21, COLOR_RED,   -1);          // 박스 테두리 (빨강 / 기본)
    }

    int h, w;
    getmaxyx(stdscr, h, w);
    (void)h;

    WINDOW *panic_win = newwin(14, 60, 5, (w - 60) / 2);

    if (has_colors()) wbkgd(panic_win, COLOR_PAIR(20));

    if (has_colors()) wattron(panic_win, COLOR_PAIR(21));
    box(panic_win, 0, 0);
    if (has_colors()) wattroff(panic_win, COLOR_PAIR(21));

    // 타이틀: 빨강 점멸 + 반전
    wattron(panic_win, A_BLINK | A_REVERSE | A_BOLD);
    mvwprintw(panic_win, 1, 15, " !!! POLICE RAID DETECTED !!! ");
    wattroff(panic_win, A_BLINK | A_REVERSE | A_BOLD);

    mvwprintw(panic_win, 4, 5, "긴급 서버 수색 및 연결 파기 시도 중...");
    mvwprintw(panic_win, 6, 5, "제한 시간: %-4d 초", time_limit);
    mvwprintw(panic_win, 8, 5, "수색 무력화 우회 암호: %s", passcode);
    mvwprintw(panic_win, 10, 5, "주의: 긴급 암호를 신속히 입력하십시오!");

    // 입력 echo (박스 내부 — 화면 점멸에도 유지). %-30s 패딩으로 백스페이스 잔상 제거.
    mvwprintw(panic_win, 12, 5, "입력 > %-30s", input ? input : "");

    // 커서를 입력 끝에 위치 ("입력 > " 표시폭 7). 암호는 ASCII라 strlen=표시폭.
    wmove(panic_win, 12, 5 + 7 + (int)strlen(input ? input : ""));
    wrefresh(panic_win);
    delwin(panic_win);
}

// GDD §F 라운드 종료 카운트다운 + 스코어보드 모달.
// 기본 6분할 보드는 보존한 채 중앙에 박스만 띄움 (clear 호출 없음).
void render_scoreboard(ClientState *st, int countdown_sec)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    int box_h = 22;
    int box_w = 70;
    if (box_h > h - 2) box_h = h - 2;
    if (box_w > w - 4) box_w = w - 4;

    WINDOW *sb = newwin(box_h, box_w, (h - box_h) / 2, (w - box_w) / 2);
    wbkgd(sb, A_NORMAL);
    box(sb, 0, 0);

    wattron(sb, A_BOLD);
    mvwprintw(sb, 0, 3, " ROUND END — 라운드 종료 카운트다운 ");
    wattroff(sb, A_BOLD);

    mvwprintw(sb, 1, 2, "남은 시간: %02d초  (자동 라운드 리셋까지)", countdown_sec);
    mvwprintw(sb, 2, 2, "──────────────────────────────────────────────────────────");

    mvwprintw(sb, 3, 2, "[탈출자 명단]");
    int y = 4;
    if (st->sb_escaped_count == 0) {
        mvwprintw(sb, y, 4, "(탈출자 없음)");
        y++;
    } else {
        for (int i = 0; i < st->sb_escaped_count; i++) {
            mvwprintw(sb, y + i, 4, "#%d. %-28s  탕감액: $%d",
                      st->sb_escaped[i].escape_order,
                      st->sb_escaped[i].key,
                      st->sb_escaped[i].money_at_escape);
        }
        y += st->sb_escaped_count;
    }

    y++;
    mvwprintw(sb, y, 2, "──────────────────────────────────────────────────────────"); y++;
    mvwprintw(sb, y, 2, "[남은 플레이어 잔액 순위]"); y++;
    if (st->sb_remaining_count == 0) {
        mvwprintw(sb, y, 4, "(남은 플레이어 없음)");
        y++;
    } else {
        int rem_limit = box_h - 4 - SB_CHAT_LINES;   // 하단 에필로그 채팅 영역 자리 확보
        for (int i = 0; i < st->sb_remaining_count && y < rem_limit; i++) {
            mvwprintw(sb, y, 4, "%d. %-28s  잔액: $%d",
                      i + 1,
                      st->sb_remaining[i].key,
                      st->sb_remaining[i].money);
            y++;
        }
    }

    // 에필로그 채팅 영역 (하단 고정) — 라운드 종료 중 주고받은 채팅 표시
    int chat_label_y = box_h - 3 - SB_CHAT_LINES;
    mvwprintw(sb, chat_label_y, 2, "[에필로그 채팅] (게임 명령 잠금, 채팅만 허용)");
    int shown = st->sb_chat_count < SB_CHAT_LINES ? st->sb_chat_count : SB_CHAT_LINES;
    int first = st->sb_chat_count - shown;
    for (int i = 0; i < shown; i++) {
        int idx = (first + i) % SB_CHAT_LINES;
        mvwprintw(sb, chat_label_y + 1 + i, 4, "%.*s", box_w - 7, st->sb_chat[idx]);
    }

    wrefresh(sb);
    delwin(sb);
}

// UTF-8 문자열을 표시 폭(col_w) 기준으로 줄바꿈하며 (start_y,x)부터 출력. 사용한 줄 수 반환.
// 한글 등 3바이트 문자는 2칸, ASCII는 1칸으로 계산해 박스 경계를 넘지 않게 한다.
static int mvw_print_wrapped(WINDOW *win, int start_y, int x, int col_w, const char *s)
{
    int y = start_y, col = 0;
    char line[256];
    int li = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int blen, cw;
        if      (*p < 0x80)         { blen = 1; cw = 1; }   // ASCII
        else if ((*p >> 5) == 0x6)  { blen = 2; cw = 1; }   // 110xxxxx
        else if ((*p >> 4) == 0xE)  { blen = 3; cw = 2; }   // 1110xxxx (한글 등 와이드)
        else if ((*p >> 3) == 0x1E) { blen = 4; cw = 2; }   // 11110xxx
        else                        { blen = 1; cw = 1; }   // 깨진 바이트 방어
        if (col + cw > col_w || li > (int)sizeof(line) - 8) {
            line[li] = '\0';
            mvwprintw(win, y, x, "%s", line);
            y++; li = 0; col = 0;
        }
        for (int k = 0; k < blen && p[k]; k++) line[li++] = (char)p[k];
        p += blen; col += cw;
    }
    if (li > 0) { line[li] = '\0'; mvwprintw(win, y, x, "%s", line); y++; }
    return y - start_y;
}

void render_game_over(const char *msg)
{
    clear();
    refresh();

    int h, w;
    getmaxyx(stdscr, h, w);

    int box_w = 64;
    if (box_w > w - 2) box_w = w - 2;            // 터미널 폭 초과 방지
    int box_h   = 11;
    int inner_x = 3;
    int col_w   = box_w - 6;                      // 좌우 여백 (경계 침범 방지)

    WINDOW *go_win = newwin(box_h, box_w, (h - box_h) / 2, (w - box_w) / 2);
    box(go_win, 0, 0);
    wattron(go_win, A_REVERSE);
    mvwprintw(go_win, 1, (box_w - 11) / 2, " GAME OVER ");
    wattroff(go_win, A_REVERSE);

    // 사유 문구를 박스 내부 폭에 맞춰 줄바꿈 출력 (긴 한글도 경계 안에 정확히 들어가도록)
    char full[MAX_TEXT_LEN + 16];
    snprintf(full, sizeof(full), "사유: %s", msg);
    mvw_print_wrapped(go_win, 3, inner_x, col_w, full);

    mvwprintw(go_win, box_h - 2, (box_w - 28) / 2, "아무 키나 누르면 종료됩니다.");
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
static void draw_title(WINDOW *win, const char *title)
{
    box(win, 0, 0);
    mvwprintw(win,0,2," %s ", title);
}
static void draw_ui(UI *ui, ClientState *st)
{
    int h, w;
    getmaxyx(stdscr, h, w);

    clear();
    refresh();

    int news_h      = 4;
    int prompt_h    = 1;
    int inventory_h = 9;                  // 7칸 + 박스/제목
    int middle_h    = h * 45 / 100;       // 마켓/바운티 영역
    int log_h       = h - news_h - middle_h - inventory_h - prompt_h;
    int market_w    = w / 2;
    int bounty_w    = w - market_w;       // 잔여 보정

    int y = 0;
    ui->news       = newwin(news_h, w, y, 0); y += news_h;
    ui->market     = newwin(middle_h, market_w, y, 0);
    ui->bounty     = newwin(middle_h, bounty_w, y, market_w); y += middle_h;
    ui->inventory  = newwin(inventory_h, w, y, 0); y += inventory_h;
    ui->log_border = newwin(log_h, w, y, 0);
    ui->log        = derwin(ui->log_border, log_h - 2, w - 2, 1, 1);
    y += log_h;
    ui->prompt     = newwin(prompt_h, w, h - 1, 0);

    keypad(ui->prompt, TRUE);
    nodelay(ui->prompt, TRUE);
    scrollok(ui->log, TRUE);

    // 뉴스 패널 — 마지막 속보 1줄 표시
    draw_title(ui->news, "다크웹 중앙 뉴스망 / 브로드캐스트");
    if (st->last_news[0]) {
        mvwprintw(ui->news, 1, 2, "> %s", st->last_news);
    } else {
        mvwprintw(ui->news, 1, 2, "> 속보 대기 중...");
    }

    // 로그 박스
    draw_title(ui->log_border, "시스템 알림 및 채팅 로그");

    // 프롬프트 + 퀵헬프 (입력 프롬프트를 마지막에 그려 커서가 입력 캐럿에 남도록)
    if (w >= 45) {
        mvwprintw(ui->prompt, 0, w - 40, "[ /help | ESC: 취소 | /quit: 종료 ]");
    }
    mvwprintw(ui->prompt, 0, 0, "> ");

    wrefresh(ui->news);
    wrefresh(ui->log_border);
    wrefresh(ui->log);
    wrefresh(ui->prompt);

    // 라이브 보드
    render_market(ui, st);
    render_bounty(ui, st);
    render_inventory(ui, st);
}

static void redraw(UI *ui, ClientState *st) {
    delete_windows(ui);
    clear();
    refresh();

    if (!check_size()) {
        endwin();
        exit(0);
    }

    draw_ui(ui, st);
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

    int32_t login_money = pkt.body.login_ok.money;
    int32_t login_goal  = pkt.body.login_ok.goal_money;

    audio_init("bgm");

    atexit(safe_endwin);
    signal(SIGINT,  handle_sigint);
    signal(SIGQUIT, handle_sigquit);   // GDD §D: Ctrl+\ 안전 탈출

    setlocale(LC_ALL, "");
    srand(time(NULL) ^ getpid());   // 글리치 등 연출 난수 시드 (세션마다 다르게)

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    UI ui = {0};
    ClientState state = {0};
    state.my_money = login_money;
    state.my_goal  = login_goal;

    if (!check_size()) {
        safe_endwin();
        return 1;
    }

    draw_ui(&ui, &state);

    // 로그인 직후 자동 /inventory — 재접속/라운드 리셋 직후 인벤 초기 동기화
    {
        Packet req;
        memset(&req, 0, sizeof(Packet));
        req.type = PKT_REQ_INVEN;
        packet_send(sock, &req);
    }

    char input[INPUT_MAX] = {0};
    int len = 0;
    int running = 1;
    int server_lost = 0;
    ClientMode mode = STATE_NORMAL;

    // §D 패닉 카운트다운 상태
    time_t panic_deadline    = 0;    // 0 = 패닉 모드 비활성
    int    last_panic_remain = -1;   // 마지막으로 그린 남은 초 (재그리기 throttle)
    char   panic_passcode[MAX_TEXT_LEN] = "PURGE";  // 서버가 지정한 현재 레이드 암호

    // §F 라운드 종료 카운트다운 상태
    time_t round_end_deadline    = 0;
    int    last_round_end_remain = -1;

    // 사보타주 글리치 연출 상태 (지속 애니메이션)
    time_t glitch_until    = 0;    // 0 = 글리치 비활성
    long   last_glitch_ms  = 0;    // 마지막 프레임 시각 (throttle)

    while (running) {
        // 시그널 핸들러용 레이드 상태 동기화: 레이드 중이면 Ctrl+C가 차단된다.
        g_in_raid = (mode == STATE_PANIC);

        // 평상시 Ctrl+C 정상 종료 요청 처리
        if (g_quit_req) {
            running = 0;
            break;
        }

        int ch = wgetch(ui.prompt);

        if (ch == ERR) {
            // 키 입력 없을 때 30ms 타임아웃으로 패킷을 대기 (CPU 100% 점유 방지)
            struct timeval tv = {0, 30000};
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
                    const char *sender = pkt.body.chat_evt.sender_key;
                    int is_news = (strstr(sender, "속보") || strstr(sender, "ALERT") ||
                                   strstr(sender, "SYSTEM")) ? 1 : 0;
                    if (is_news) {
                        strncpy(state.last_news, pkt.body.chat_evt.text, MAX_TEXT_LEN - 1);
                        state.last_news[MAX_TEXT_LEN - 1] = '\0';
                    }
                    // 패닉/라운드종료 등 오버레이 화면을 뚫지 않도록 일반 모드에서만 로그에 그린다.
                    if (mode == STATE_NORMAL) {
                        wprintw(ui.log, "\n[%s] %s",
                                pkt.body.chat_evt.sender_key, pkt.body.chat_evt.text);
                        wrefresh(ui.log);
                        if (is_news) {
                            werase(ui.news);
                            draw_title(ui.news, "다크웹 중앙 뉴스망 / 브로드캐스트");
                            mvwprintw(ui.news, 1, 2, "> %s", state.last_news);
                            wrefresh(ui.news);
                        }
                    }
                    // 에필로그(라운드 종료) 중에는 스코어보드 하단 채팅 영역에 누적 표시
                    else if (mode == STATE_ROUND_END) {
                        snprintf(state.sb_chat[state.sb_chat_count % SB_CHAT_LINES],
                                 sizeof(state.sb_chat[0]), "[%s] %s",
                                 pkt.body.chat_evt.sender_key, pkt.body.chat_evt.text);
                        state.sb_chat_count++;
                        int remain = round_end_deadline > time(NULL)
                                     ? (int)(round_end_deadline - time(NULL)) : 0;
                        render_scoreboard(&state, remain);
                    }
                }
                else if (pkt.type == PKT_EVT_RUMOR_GLITCH) {
                    // 사보타주: 데이터 패널 전체가 GLITCH_DURATION_SEC 동안 손상 애니메이션.
                    // (평상시에만 발동 — 패닉/라운드종료 화면은 침범하지 않음)
                    if (mode == STATE_NORMAL) {
                        glitch_until   = time(NULL) + GLITCH_DURATION_SEC;
                        last_glitch_ms = 0;
                        render_glitch(&ui);
                        log_message(&ui,
                            "[경고] /rumor 사보타주 피격! 시스템 전반이 손상되었습니다...");
                    }
                }
                else if (pkt.type == PKT_EVT_POLICE_RAID) {
                    audio_play_alarm();
                    mode = STATE_PANIC;
                    panic_deadline    = time(NULL) + pkt.body.police_raid.time_limit_sec;
                    last_panic_remain = -1;
                    len = 0;
                    memset(input, 0, sizeof(input));
                    // 서버가 무작위로 지정한 암호를 저장 → 이후 재그리기에 재사용
                    strncpy(panic_passcode, pkt.body.police_raid.passcode, MAX_TEXT_LEN - 1);
                    panic_passcode[MAX_TEXT_LEN - 1] = '\0';
                    // 진입 시 1회만 화면을 비우고(뒤 보드 숨김), 이후엔 박스만 갱신.
                    clear();
                    refresh();
                    render_panic_mode(&ui, panic_passcode,
                                      pkt.body.police_raid.time_limit_sec, input);
                }
                else if (pkt.type == PKT_RES_MINIGAME_OK) {
                    mode = STATE_NORMAL;
                    panic_deadline    = 0;
                    last_panic_remain = -1;
                    redraw(&ui, &state);
                    log_message(&ui, "[SYSTEM] 수색 무력화 성공! 정상 마켓 화면으로 복원되었습니다.");
                }
                else if (pkt.type == PKT_EVT_GAME_OVER) {
                    if (strncmp(pkt.body.endgame.message, "__ROUND_RESET__", 15) == 0) {
                        const char *bar = strchr(pkt.body.endgame.message, '|');
                        const char *announce = bar ? bar + 1 : "새 라운드 시작.";
                        state.mkt_count          = 0;
                        state.npc_count          = 0;
                        state.inv_count          = 0;
                        state.frozen_mask        = 0;
                        state.my_money           = INITIAL_MONEY;
                        state.my_goal            = GOAL_MONEY;
                        state.last_news[0]       = '\0';
                        state.sb_escaped_count   = 0;
                        state.sb_remaining_count = 0;
                        state.sb_chat_count      = 0;
                        round_end_deadline       = 0;
                        last_round_end_remain    = -1;
                        mode = STATE_NORMAL;
                        redraw(&ui, &state);
                        log_message(&ui, "[ROUND RESET] 라운드가 리셋되었습니다.");
                        log_message(&ui, announce);
                    } else {
                        mode = STATE_GAME_OVER;
                        render_game_over(pkt.body.endgame.message);
                    }
                }
                else if (pkt.type == PKT_EVT_VICTORY) {
                    mode = STATE_GAME_OVER;
                    render_game_over(pkt.body.endgame.message);
                }
                else if (pkt.type == PKT_EVT_SCOREBOARD) {
                    // GDD §F: 라운드 종료 진입. 게임 명령 잠금 + 카운트다운/스코어보드 표시
                    state.sb_escaped_count = pkt.body.scoreboard.escaped_count;
                    for (int i = 0; i < state.sb_escaped_count && i < MAX_SCORE_ESCAPED; i++) {
                        state.sb_escaped[i] = pkt.body.scoreboard.escaped[i];
                    }
                    state.sb_remaining_count = pkt.body.scoreboard.remaining_count;
                    for (int i = 0; i < state.sb_remaining_count && i < MAX_SCORE_REMAINING; i++) {
                        state.sb_remaining[i] = pkt.body.scoreboard.remaining[i];
                    }
                    int cd = pkt.body.scoreboard.countdown_sec;
                    mode = STATE_ROUND_END;
                    round_end_deadline    = time(NULL) + cd;
                    last_round_end_remain = -1;
                    state.sb_chat_count   = 0;   // 새 에필로그 채팅 로그 시작
                    render_scoreboard(&state, cd);
                }
                else if (pkt.type == PKT_EVT_MARKET_SPAWN) {
                    if (state.mkt_count < UI_MAX_MARKET) {
                        UiMarket *m = &state.mkt[state.mkt_count++];
                        m->doc_id     = pkt.body.market_spawn.doc_id;
                        m->tags       = pkt.body.market_spawn.tags;
                        m->base_price = pkt.body.market_spawn.base_price;
                        strncpy(m->name, pkt.body.market_spawn.name, MAX_NAME_LEN - 1);
                        m->name[MAX_NAME_LEN - 1] = '\0';
                        m->is_frozen = (m->tags & state.frozen_mask) ? 1 : 0;
                    }
                    if (mode == STATE_NORMAL) {
                        render_market(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[MARKET] 신규 매물 ID:%d 가격:$%d %s",
                                 pkt.body.market_spawn.doc_id,
                                 pkt.body.market_spawn.base_price,
                                 pkt.body.market_spawn.name);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_EVT_MARKET_REMOVE) {
                    for (int i = 0; i < state.mkt_count; i++) {
                        if (state.mkt[i].doc_id == pkt.body.market_remove.doc_id) {
                            state.mkt[i] = state.mkt[--state.mkt_count];
                            break;
                        }
                    }
                    if (mode == STATE_NORMAL) {
                        render_market(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[MARKET] 매물 소멸 ID:%d",
                                 pkt.body.market_remove.doc_id);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_EVT_NPC_SPAWN) {
                    if (state.npc_count < UI_MAX_NPC) {
                        UiNPC *n = &state.npc[state.npc_count++];
                        n->npc_id        = pkt.body.npc_spawn.npc_id;
                        n->required_tags = pkt.body.npc_spawn.required_tags;
                        n->bounty        = pkt.body.npc_spawn.bounty;
                    }
                    if (mode == STATE_NORMAL) {
                        render_bounty(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[NPC] 신규 의뢰 #%d 보상:$%d",
                                 pkt.body.npc_spawn.npc_id, pkt.body.npc_spawn.bounty);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_EVT_NPC_DESPAWN) {
                    for (int i = 0; i < state.npc_count; i++) {
                        if (state.npc[i].npc_id == pkt.body.npc_despawn.npc_id) {
                            state.npc[i] = state.npc[--state.npc_count];
                            break;
                        }
                    }
                    if (mode == STATE_NORMAL) {
                        render_bounty(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[NPC] 의뢰 만료 #%d",
                                 pkt.body.npc_despawn.npc_id);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_EVT_NEWS_LEAK) {
                    state.frozen_mask |= pkt.body.news_leak.frozen_tags;
                    for (int i = 0; i < state.mkt_count; i++) {
                        if (state.mkt[i].tags & state.frozen_mask)
                            state.mkt[i].is_frozen = 1;
                    }
                    // 인벤토리·바운티도 즉시 갱신 (속보 시점에 표시 일치)
                    for (int i = 0; i < state.inv_count; i++) {
                        state.inv[i].is_frozen =
                            (state.inv[i].tags & state.frozen_mask) ? 1 : 0;
                    }
                    strncpy(state.last_news, pkt.body.news_leak.headline, MAX_TEXT_LEN - 1);
                    state.last_news[MAX_TEXT_LEN - 1] = '\0';
                    if (mode == STATE_NORMAL) {
                        render_market(&ui, &state);
                        render_inventory(&ui, &state);
                        render_bounty(&ui, &state);
                        werase(ui.news);
                        draw_title(ui.news, "다크웹 중앙 뉴스망 / 브로드캐스트");
                        mvwprintw(ui.news, 1, 2, "> %s", state.last_news);
                        wrefresh(ui.news);

                        char tagbuf[128];
                        tags_to_str(pkt.body.news_leak.frozen_tags, tagbuf, sizeof(tagbuf));
                        char msg[MAX_TEXT_LEN * 2];
                        snprintf(msg, sizeof(msg), "[News-Leak] %s | 동결: %s | %d초간",
                                 pkt.body.news_leak.headline, tagbuf,
                                 pkt.body.news_leak.duration_sec);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_EVT_NEWS_RECOVER) {
                    state.frozen_mask &= ~pkt.body.news_recover.recovered_tags;
                    for (int i = 0; i < state.mkt_count; i++) {
                        state.mkt[i].is_frozen =
                            (state.mkt[i].tags & state.frozen_mask) ? 1 : 0;
                    }
                    for (int i = 0; i < state.inv_count; i++) {
                        state.inv[i].is_frozen =
                            (state.inv[i].tags & state.frozen_mask) ? 1 : 0;
                    }
                    if (mode == STATE_NORMAL) {
                        render_market(&ui, &state);
                        render_inventory(&ui, &state);
                        render_bounty(&ui, &state);
                        char tagbuf[128];
                        tags_to_str(pkt.body.news_recover.recovered_tags, tagbuf, sizeof(tagbuf));
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[News-Recover] 동결 해제: %s", tagbuf);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_RES_BUY_OK) {
                    state.my_money = pkt.body.buy_ok.remaining_money;
                    if (mode == STATE_NORMAL) {
                        render_inventory(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[OK] 구매 완료 ID:%d %s 잔액:$%d",
                                 pkt.body.buy_ok.doc_id, pkt.body.buy_ok.name,
                                 pkt.body.buy_ok.remaining_money);
                        log_message(&ui, msg);
                    }
                    Packet req;
                    memset(&req, 0, sizeof(Packet));
                    req.type = PKT_REQ_INVEN;
                    packet_send(sock, &req);
                }
                else if (pkt.type == PKT_RES_SELL_OK) {
                    state.my_money = pkt.body.sell_ok.new_money;
                    if (mode == STATE_NORMAL) {
                        render_inventory(&ui, &state);
                        char msg[MAX_TEXT_LEN];
                        snprintf(msg, sizeof(msg), "[OK] 매각 완료 보상:$%d 잔액:$%d",
                                 pkt.body.sell_ok.bounty, pkt.body.sell_ok.new_money);
                        log_message(&ui, msg);
                    }
                    Packet req;
                    memset(&req, 0, sizeof(Packet));
                    req.type = PKT_REQ_INVEN;
                    packet_send(sock, &req);
                }
                else if (pkt.type == PKT_RES_DISPOSE_OK) {
                    if (mode == STATE_NORMAL) log_message(&ui, "[OK] 폐기 완료.");
                    Packet req;
                    memset(&req, 0, sizeof(Packet));
                    req.type = PKT_REQ_INVEN;
                    packet_send(sock, &req);
                }
                else if (pkt.type == PKT_RES_ERROR) {
                    if (mode == STATE_NORMAL) {
                        char msg[MAX_TEXT_LEN * 2];
                        snprintf(msg, sizeof(msg), "[ERROR %d] %s",
                                 pkt.body.error.error_code, pkt.body.error.reason);
                        log_message(&ui, msg);
                    }
                }
                else if (pkt.type == PKT_RES_INVEN_INFO) {
                    state.inv_count = pkt.body.inven_info.count;
                    for (int i = 0; i < state.inv_count; i++) {
                        state.inv[i] = pkt.body.inven_info.items[i];
                    }
                    state.my_money = pkt.body.inven_info.money;
                    if (mode == STATE_NORMAL) render_inventory(&ui, &state);
                }
                else if (pkt.type == PKT_EVT_GOAL_UPDATE) {
                    // 타 유저 탈출로 목표 상환액이 인상됨 → 내 화면 목표액 갱신
                    state.my_goal = pkt.body.goal_update.goal_money;
                    if (mode == STATE_NORMAL) render_inventory(&ui, &state);
                }
            }

            // §D 패닉 카운트다운 1초 단위 재그림 (남은 시간 변할 때만)
            if (mode == STATE_PANIC && panic_deadline > 0) {
                time_t now = time(NULL);
                int remain = panic_deadline > now ? (int)(panic_deadline - now) : 0;
                if (remain != last_panic_remain) {
                    render_panic_mode(&ui, panic_passcode, remain, input);
                    last_panic_remain = remain;
                }
            }

            // §F 라운드 종료 카운트다운 1초 단위 재그림
            if (mode == STATE_ROUND_END && round_end_deadline > 0) {
                time_t now = time(NULL);
                int remain = round_end_deadline > now ? (int)(round_end_deadline - now) : 0;
                if (remain != last_round_end_remain) {
                    render_scoreboard(&state, remain);
                    last_round_end_remain = remain;
                }
            }

            // 사보타주 글리치 애니메이션 (GLITCH_FRAME_MS 간격으로 새 프레임, 만료 시 복구)
            if (mode == STATE_NORMAL && glitch_until > 0) {
                if (time(NULL) < glitch_until) {
                    long ms = now_ms();
                    if (ms - last_glitch_ms >= GLITCH_FRAME_MS) {
                        render_glitch(&ui);
                        last_glitch_ms = ms;
                    }
                } else {
                    glitch_until = 0;
                    recover_from_glitch(&ui, &state);
                    log_message(&ui, "[복구] 사보타주 손상 복구 완료 — 화면 정상화.");
                }
            }

            continue;
        }

        if (ch == KEY_RESIZE) {
            len = 0;
            memset(input, 0, sizeof(input));
            if (mode == STATE_NORMAL) {
                redraw(&ui, &state);
            } else if (mode == STATE_PANIC) {
                int remain = panic_deadline > time(NULL)
                             ? (int)(panic_deadline - time(NULL)) : 0;
                clear();
                refresh();
                last_panic_remain = remain;
                render_panic_mode(&ui, panic_passcode, remain, input);
            } else if (mode == STATE_ROUND_END) {
                redraw(&ui, &state);
                int remain = round_end_deadline > time(NULL)
                             ? (int)(round_end_deadline - time(NULL)) : 0;
                render_scoreboard(&state, remain);
                last_round_end_remain = remain;
            }
            continue;
        }

        if (ch == '\n') {
            input[len] = '\0';

            if (strcmp(input, "/quit") == 0) {
                running = 0;
                continue;
            }

            if (mode == STATE_PANIC) {
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_MINIGAME_SUBMIT;
                strncpy(pkt.body.minigame.passcode, input, MAX_TEXT_LEN - 1);
                packet_send(sock, &pkt);

                len = 0;
                memset(input, 0, sizeof(input));
                continue;
            }

            if (mode == STATE_ROUND_END) {
                // GDD §F: 게임 명령 잠금, 채팅만 허용
                if (input[0] == '/') {
                    log_message(&ui, "[SYSTEM] 라운드 종료 중에는 게임 명령이 잠금 상태입니다. 채팅만 가능합니다.");
                } else if (len > 0) {
                    memset(&pkt, 0, sizeof(Packet));
                    pkt.type = PKT_REQ_CHAT;
                    strncpy(pkt.body.chat.text, input, MAX_TEXT_LEN - 1);
                    packet_send(sock, &pkt);
                }
                len = 0;
                memset(input, 0, sizeof(input));
                continue;
            }

            if (strcmp(input, "/fake rumor") == 0) {
                // 로컬 테스트: 지속 글리치 연출 발동
                if (mode == STATE_NORMAL) {
                    glitch_until   = time(NULL) + GLITCH_DURATION_SEC;
                    last_glitch_ms = 0;
                    render_glitch(&ui);
                    log_message(&ui, "[경고] (로컬) 사보타주 글리치 테스트.");
                }
            }
            else if (strcmp(input, "/fake raid") == 0) {
                // 서버에 자기 자신 대상 §D 레이드 트리거를 요청한다.
                // 서버가 PKT_EVT_POLICE_RAID로 응답하면 기존 핸들러가 패닉 진입을 처리.
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_TRIGGER_RAID;
                packet_send(sock, &pkt);
                log_message(&ui, "[DEBUG] 자기 자신 대상 경찰 레이드 트리거 요청 전송.");
            }
            else if (strcmp(input, "/fake leak") == 0) {
                // 서버에 공중파 유출(동결) 즉시 발사를 요청한다 (전체 broadcast).
                // 서버가 PKT_EVT_NEWS_LEAK로 응답하면 기존 핸들러가 FRZ 마킹을 처리.
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_TRIGGER_LEAK;
                packet_send(sock, &pkt);
                log_message(&ui, "[DEBUG] 공중파 유출(동결) 트리거 요청 전송.");
            }
            else if (strcmp(input, "/fake rich") == 0) {
                // 서버에 잔액을 목표 상환액까지 채워달라고 요청 (이후 /payoff 로 탈출 테스트)
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_TRIGGER_RICH;
                packet_send(sock, &pkt);
                log_message(&ui, "[DEBUG] 잔액 채우기 요청 전송 — 이후 /payoff 로 탈출 테스트.");
            }
            else if (strcmp(input, "/fake endround") == 0) {
                // 서버에 라운드 종료 카운트다운(스코어보드+60초+리셋) 즉시 발사 요청
                memset(&pkt, 0, sizeof(Packet));
                pkt.type = PKT_REQ_TRIGGER_ENDROUND;
                packet_send(sock, &pkt);
                log_message(&ui, "[DEBUG] 라운드 종료 카운트다운 트리거 요청 전송.");
            }
            else if (strcmp(input, "/fake ok") == 0) {
                mode = STATE_NORMAL;
                redraw(&ui, &state);
                log_message(&ui, "[SYSTEM] 로컬 강제 복구 완료.");
            }
            else if (strcmp(input, "/fake over") == 0) {
                mode = STATE_GAME_OVER;
                render_game_over("로컬 강제 게임오버 테스트");
            }
            else {
                // [YOU] 로컬 echo 제거: 서버가 발신자 본인에게도 [닉네임]으로 되돌려주므로
                // 중복 표시를 막는다. 명령어는 각자 서버 응답([BUY OK] 등)으로 피드백된다.
                memset(&pkt, 0, sizeof(Packet));

                if (strncmp(input, "/buy ", 5) == 0) {
                    pkt.type = PKT_REQ_BUY;
                    pkt.body.buy.doc_id = atoi(input + 5);
                } else if (strncmp(input, "/dispose ", 9) == 0) {
                    pkt.type = PKT_REQ_DISPOSE;
                    pkt.body.dispose.doc_id = atoi(input + 9);
                } else if (strcmp(input, "/inventory") == 0 || strcmp(input, "/inv") == 0) {
                    pkt.type = PKT_REQ_INVEN;
                } else if (strncmp(input, "/rumor ", 7) == 0) {
                    pkt.type = PKT_REQ_RUMOR;
                    strncpy(pkt.body.rumor.target_key, input + 7, MAX_KEY_LEN - 1);
                } else if (strncmp(input, "/sell ", 6) == 0) {
                    pkt.type = PKT_REQ_SELL;
                    char temp[INPUT_MAX];
                    strncpy(temp, input, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';

                    char *token = strtok(temp + 6, " ");
                    if (token) pkt.body.sell.npc_id = atoi(token);

                    int count = 0;
                    token = strtok(NULL, " ");
                    while (token != NULL && count < MAX_INVEN_SIZE) {
                        pkt.body.sell.doc_ids[count++] = atoi(token);
                        token = strtok(NULL, " ");
                    }
                    pkt.body.sell.count = count;
                } else {
                    pkt.type = PKT_REQ_CHAT;
                    strncpy(pkt.body.chat.text, input, MAX_TEXT_LEN - 1);
                }

                packet_send(sock, &pkt);
            }

            len = 0;
            memset(input, 0, sizeof(input));
            // continue 생략: 하단 프롬프트 재그리기 블록으로 떨어져 입력창을 '> '로 비운다.
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                // UTF-8 연속 바이트(10xxxxxx)면 선두 바이트까지 마저 지워 한 글자 단위로 삭제
                while (len > 0 && ((unsigned char)input[len] & 0xC0) == 0x80)
                    len--;
                input[len] = '\0';
            }
        }
        else if (ch == 27) {
            len = 0;
            memset(input, 0, sizeof(input));
        }
        else if (ch >= 32 && ch <= 255) {
            // ASCII(32~126) + UTF-8 멀티바이트 바이트(0x80~0xFF)를 그대로 누적.
            // wgetch는 한글을 바이트 단위로 돌려주므로, 연속 바이트를 UTF-8로 모아 보관하면
            // 표시(ncursesw)·전송 모두 정상 동작한다. (함수키는 256↑이라 자동 제외)
            int h, w;
            getmaxyx(stdscr, h, w);
            (void)h;

            if (len < INPUT_MAX - 1 && len < w - 45) {
                input[len] = (char)ch;
                len++;
                input[len] = '\0';
            }
        }

        if (mode == STATE_NORMAL) {
            int h, w;
            getmaxyx(stdscr, h, w);
            (void)h;

            werase(ui.prompt);
            if (w >= 45) {
                mvwprintw(ui.prompt, 0, w - 40,
                          "[ /quit: 종료 | 일반채팅 전송 ]");
            }
            // 입력을 '마지막'에 그려 하드웨어 커서가 입력 캐럿(박스 안)에 남도록 한다.
            // (힌트를 나중에 그리면 커서가 우하단 힌트 끝에 남아, 조합된 한글 글자가
            //  그 위치에 떴다가 한 박자 늦게 입력칸에 표시되는 문제가 생긴다.)
            mvwprintw(ui.prompt, 0, 0, "> %s", input);
            wrefresh(ui.prompt);
        } else if (mode == STATE_PANIC) {
            // 키 입력마다 박스만 제자리에 다시 그려 입력 echo 갱신 (clear 없음 → 점멸 무관).
            int remain = panic_deadline > time(NULL)
                         ? (int)(panic_deadline - time(NULL)) : 0;
            render_panic_mode(&ui, panic_passcode, remain, input);
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
