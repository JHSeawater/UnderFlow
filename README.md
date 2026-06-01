# UNDERFLOW

터미널 기반 멀티플레이어 경제·소셜 시뮬레이션. 다크웹의 정보 브로커가 되어 독점 기밀 문서를 선점하고 NPC 고객에게 경쟁자보다 먼저 팔아넘겨 빚을 갚고 탈출하는 게임.

- **개발 언어**: 순수 C (POSIX 시스템 콜 + ncurses)
- **목표 환경**: Ubuntu 24.04, 터미널 (최소 해상도 100x30)
- **구조**: 멀티스레드 TCP 서버 1개 + 클라이언트 N개. 워치독 모니터 1개.

---

## 빌드

```bash
make            # server / client_ui / watchdog 일괄 빌드
make clean      # 산출물 정리
```

### 빌드 의존성 (필수)
- `gcc`, `make`
- `libncursesw5-dev` (UTF-8 와이드 문자 ncurses)
- `pthread` (glibc 기본 포함, `-pthread`로 링크)

```bash
sudo apt install build-essential libncursesw5-dev
```

### 실행 의존성 — BGM (선택)
BGM 재생은 아래 플레이어 중 **하나라도 설치돼 있으면 자동 감지·사용**. 우선순위는 `mpg123` → `ffplay`(ffmpeg) → `cvlc`(vlc). **하나도 없으면 무음으로 정상 구동**되며 게임 진행에는 전혀 지장이 없음.

```bash
sudo apt install mpg123        # 권장 (가장 가볍고 안정적)
```

> 동봉된 `bgm/*.mp3`는 전 곡 44.1kHz로 통일돼 있어 사용자가 별도 변환할 필요가 없다. (WSL2/WSLg PulseAudio 환경에서는 스케줄링 지터로 미세 크래클이 남을 수 있으나, 코드에서 재생 버퍼를 키워 완화한다.)

빌드 산출물:
- `server`       — 게임 서버 (포트 8080)
- `client_ui`    — ncurses 6분할 UI 클라이언트 (플레이·시연용)
- `watchdog`     — 서버 자동 재시작 모니터

---

## 실행

### 워치독으로 서버 띄우기 (권장)
```bash
./watchdog ./server
```
자식 서버가 비정상 종료되면 1초 후 자동 재시작. 정상 종료(exit 0) 또는 `Ctrl+C` 시 워치독도 종료.

### 단독 서버
```bash
./server
```

### UI 클라이언트 (시연 권장)
```bash
./client_ui <KEY>
```
- `<KEY>`는 32자 이내의 임의 문자열. 동일 KEY로 두 번 접속 시 두 번째는 거절됨.
- **반드시 외부 터미널에서 실행할 것** (IDE 내장 터미널은 ncurses 렌더링 한계로 깨질 수 있음).

---

## 인게임 명령어

| 명령 | 의미 |
|---|---|
| `/buy <doc_id>` | 매물 구매 (선착순 독점) |
| `/sell <npc_id> <doc_id> [<doc_id> ...]` | 여러 문서를 묶어 NPC에게 매각. 태그 합집합이 요구 조합을 부분집합으로 포함해야 함 |
| `/dispose <doc_id>` | 보유 문서를 0원에 강제 파기 |
| `/inventory` (`/inv`) | 보유 문서/잔고 조회 |
| `/rumor <target_key>` | 대상에게 글리치 사보타주 (수수료 $500, 서버 공유 쿨다운 30초) |
| `/payoff` | 채무 청산 후 탈출 (잔고 ≥ 현재 목표 상환액일 때) |
| `/quit` | 클라이언트 종료 |
| (그 외) | 전역 채팅으로 송신 |

---

## 단축키 (UI 클라이언트)

| 키 | 동작 |
|---|---|
| `Ctrl+C` | 경찰 레이드 패닉 모드 진입 (암호 입력 미니게임) |
| **`Ctrl+\`** | 안전 탈출 (`endwin` 호출 후 즉시 종료) |
| `Esc` | 입력 버퍼 초기화 |
| `Backspace` | 입력 한 글자 삭제 |
| 창 크기 변경 | 자동 리사이즈 (`KEY_RESIZE` → `delwin` → 재렌더) |

`Ctrl+\`는 UI가 먹통이 됐을 때를 대비한 강제 복구 경로다. `SIGQUIT` 기본 동작(코어덤프)을 가로채 `endwin`으로 터미널을 복원한 뒤 즉시 종료하도록 재매핑한다.

---

## 게임 흐름

1. 접속 → 초기자본 $1,000, 채무 $10,000.
2. 이벤트 스레드가 약 10초마다 무작위 태그 매물을 자동 스폰. 약 15초마다 [속보] 힌트 → 8초 후 NPC 의뢰 스폰 (의뢰 수명은 태그 수에 따라 140/110/80초).
3. 약 120초마다 35% 확률로 **공중파 유출** 발생 → 무작위 태그가 3~5분간 동결. 그동안 해당 태그 매물은 신규 스폰/매매 모두 차단.
4. 6~10분 간격으로 **경찰 정기 단속** — 전원에게 15초 제한 암호 입력 미니게임이 발사되고, 시간 내 정답을 제출하지 못한 계정은 **영구 소각**(자산 -1 마커 + 샌드박스 일괄 삭제).
5. 한 명이 `/payoff` 성공 시 모든 유저의 목표 상환액이 +$2,000 인상 (상한 $14,000).
6. 누적 3명 탈출 또는 상한 도달 5분 경과 시 60초 카운트다운 후 **라운드 자동 리셋** — 활성 유저는 초기자본/채무 복원, 마켓·NPC·샌드박스 재시드.
7. 자금 0 + 인벤토리 0이면 **파산** — `users.dat`에 -1 마커 박혀 영구 차단.

---

## 디렉토리 구조

```
.
├── README.md  Makefile  .gitignore
├── src/
│   ├── common/      protocol.{c,h}(패킷·엔디안/널종단 송수신) + 공용 유틸
│   ├── server/      서버 진입점·매물/NPC·이벤트·샌드박스 모듈
│   ├── client/      ui_client.c(ncurses UI) + audio(BGM) 모듈
│   └── tools/       watchdog.c (서버 자동 재시작 모니터)
├── userdb/          users.dat 영속성 (오프셋 기반 record I/O)
├── bgm/             BGM 음원 (*.mp3)
└── data/            런타임 데이터 (자동 생성)
    ├── users.dat        유저 레코드 (오프셋 기반 R/W)
    ├── master/          마스터 매물 파일 (doc_*.dat)
    └── users/<KEY>/     유저별 샌드박스
```

---

## 시연 시나리오 (예시 — 클라 3개)

1. `./watchdog ./server` 띄우기.
2. 외부 터미널 3개에서 각각 `./client_ui alice` / `./client_ui bob` / `./client_ui carol` 실행.
3. 시드된 매물 #124·#111·#47·#70 + 시드 NPC #102, #356 표시 확인.
4. alice: `/buy 124` → `/sell 102 124` (B기업+금융 의뢰, 보상 $1,200 회수).
5. 약 1분 후 공중파 유출 발생 시 마켓 보드에 `FRZ` 표기 + 뉴스 패널 갱신 확인.
6. 한 명이 `/payoff` → 다른 두 명의 목표액 $12,000으로 인상 안내.
7. 3명 탈출 또는 5분 cap 경과 시 빨강 점멸 박스 → 60초 후 라운드 리셋 (보드 비워졌다 재시드).

---

## 사용 시스템 콜

필수 요건(서로 다른 시스템 콜 5개 이상)을 크게 상회하며, 모든 호출은 게임 로직·운영체제 제어에 실제로 쓰인다.

| 분류 | 시스템 콜 | 용도 |
|---|---|---|
| **소켓/네트워크** | `socket` `bind` `listen` `accept` `connect` `send` `recv` `setsockopt` | TCP 서버-클라이언트 통신, `SO_REUSEADDR`, `MSG_NOSIGNAL` |
| **프로세스 제어** | `fork` `execv`/`execvp` `waitpid` `kill` | 워치독의 서버 재시작 루프, 좀비 차단 |
| **스레드(동시성)** | `pthread_create` `pthread_detach` `pthread_mutex_lock`/`unlock` | 스레드-퍼-클라이언트, 매물 독점 락 |
| **시그널** | `signal` | `SIGINT`/`SIGQUIT` 패닉·안전종료, `SIGPIPE` 무시 |
| **파일 I/O·영속성** | `open` `close` `read` `write` `pread` `pwrite` `lseek` | `users.dat` 오프셋 기반 레코드 R/W, 문서 파일 |
| **파일시스템·디렉토리** | `mkdir` `rename` `unlink` `opendir` `readdir` `closedir` `chmod` `stat` | 마스터/샌드박스 격리, 원자적 `rename`, 소각(unlink) |

## 핵심 기능 (3개 이상 — 가산점 5개 이상)

1. **매물 선점 거래** — 선착순 독점 구매(`/buy`), 락 기반 동시성 방어.
2. **NPC 의뢰 매각** — 태그 부분집합 매칭으로 묶음 판매(`/sell`).
3. **공중파 유출 동결** — 랜덤 태그를 3~5분간 매매 차단하는 시장 충격.
4. **경찰 단속 미니게임** — 15초 제한 암호 입력, 실패 시 계정 영구 소각.
5. **루머 사보타주** — 타 유저 화면 글리치(`/rumor`), 서버 공유 쿨다운.
6. **채무 상환·탈출 / 라운드 리셋** — `/payoff` 누적 3인 탈출 시 라운드 자동 리셋.
7. **워치독 무중단 운영** — 서버 비정상 종료 시 자동 재시작.

> **가산점 요소**: 멀티스레드(`pthread`) + 소켓 네트워크 통신 + ncurses 6분할 UI 모두 충족.

---

## 구현 특징

- **데드락 원천 차단**: 다수 문서 자원 접근 시 ID 오름차순 정렬 강제 (`market_doc_lock_many` — `src/server/market.c`).
- **EXDEV 회피**: 마스터·샌드박스를 `./data` 하위 단일 파일시스템에 묶어 `rename()` 항상 원자적.
- **시그널 안전성 분기**: `SIGINT`는 패닉 플래그만 세팅(`volatile sig_atomic_t`), `SIGQUIT`은 `endwin` + `_exit` 안전 경로.
- **무중단 운영**: `watchdog` 부모 프로세스 + `fork/execv/waitpid` 재시작 루프.
- **레이스 방어**: `g_round_mutex`로 라운드 상태 직렬화, `g_rumor_mutex`로 사보타주 쿨다운 원자적 검사·예약, 매물 점유는 `market_take` 단일 임계영역.
- **TOCTOU 방어**: `/buy` 시점에 `slot.is_frozen` + `market_take` 결과 두 단계 검증.

---

## 주의

- 서버를 중간에 죽이면 마스터/샌드박스의 doc 파일과 `users.dat`이 그대로 남는다. 완전 초기화는 `rm -rf data/` 후 재실행.
- `client_ui`는 외부 터미널 권장. WSL 환경에서는 Windows Terminal 같은 GPU 가속 터미널 추천.
