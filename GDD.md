# Game Design Document (GDD): Black Market Broker

## 1. 게임 개요 (Game Overview)
- **게임명**: Black Market Broker (가제)
- **장르**: 터미널 기반 멀티플레이어 경제/소셜 시뮬레이션
- **타겟 환경**: Ubuntu 24.04, CLI (터미널)
- **개발 언어**: 순수 C 언어 (POSIX 시스템 콜 전면 활용)
- **핵심 로그라인**: "플레이어는 다크웹의 정보 브로커가 되어, 독점 기밀 문서를 선점하고 까다로운 조건의 NPC 고객에게 경쟁자보다 먼저 팔아넘겨 막대한 부를 축적해야 한다."

## 2. 5대 핵심 게임플레이 및 기능 (Core Functionalities)

### A. 다중 속성 기밀 독점 수집 및 NPC 의뢰 판매 (Tag-based NPC Trading)
- **단일 매물 선착순 독점 (Exclusive Rights)**: 시장의 모든 기밀 문서는 서버당 단 1개만 존재하는 독점 매물입니다. 유저가 `/buy`로 구매하는 즉시 상점에서 사라집니다.
- **다중 속성(Tags) 부여**: 모든 정보는 속성 태그(예: `[B기업]`, `[고객정보]`, `[군사무기]`)를 1~N개 지니고 있습니다.
- **글로벌 NPC 고객 의뢰 보드 (Race Condition Selling)**: 특정 속성 태그 합(예: `[B기업]+[고객정보]`)을 요구하며 랜덤한 현상금을 내건 NPC 고객들이 나타납니다.
- **선착순 매각 충돌**: 조건을 충족하는 기밀을 지닌 유저 중 가장 먼저 판매 명령(`/sell [내 기밀 ID 여러 개] [NPC ID]`)을 전송한 단 1명만이 현상금을 독식합니다.
- **정보 폐기망 (Trash Bin)**: 샌드박스가 꽉 차거나 악성 재고가 쌓여 더 이상 경제 활동이 불가능할 때, `/dispose` 명령을 통해 강제로 구매가의 10% 등 헐값에 매각하고 파일 객체를 파기(`unlink`)시켜 유동성을 회복합니다.

### B. 비동기 이벤트 시스템 및 소셜 사보타주 (Events & Sabotage)
- **의뢰 에이징(Aging) 스폰 시스템**: 특정 태그만 몰려서 수요와 공급이 맞지 않는 교착 상태를 막기 위해, 아무도 해결하지 못하고 오래 남은 NPC 의뢰는 이벤트 스레드에 의해 지속적으로 만료(Despawn) 및 페이즈아웃 처리되어 생태계 순환을 돕습니다.
- **시각적 글리치(Glitch) 공격**: 막대한 수수료를 내고 `/rumor` 공작을 띄우면, 다른 유저들의 화면에 떠 있는 NPC 보드의 특정 가격이나 태그 텍스트가 일시적으로 깨지거나 뒤섞이는 UI 글리치 연출이 가해져 상대방의 판단을 물리적으로 흐리게 합니다.

### C. 파일 시스템 기반 "인벤토리 샌드박스" (Inventory Sandbox)
- **실물 파일 이동 및 권한 제어**: 마스터 계층의 문서는 서버 측 단일 파일 시스템 통제 하에 해당 유저 전용 샌드박스 폴더로 물리적 이동(`rename`) 처리됩니다.

### D. 패닉 버튼 "긴급 파기 미니게임" 시그널 흐름 (Panic Escape Sequence)
- **경찰 레이드와 시그널 인과관계 명확화**: 
  1. 서버가 전체 유저에게 "경찰 추적망 활성화!" 패킷 전송.
  2. 클라이언트 렌더러가 화면을 붉게 점멸시킴.
  3. 놀란 유저가 1차 안전장치인 `Ctrl+C` 입력 (하드웨어 `SIGINT` 시그널 발생).
  4. 클라이언트 운영체제 수준 핸들러가 이를 가로채 `volatile sig_atomic_t is_panic = 1;` 세팅.
  5. 메인 루프가 플래그를 읽고 채팅/마켓을 강제 잠금한 뒤, "암구호 타이핑" 창을 띄우며 최후의 방탈출(초기화) 진행.

### E. 로우 레벨 파일 I/O 기반 유저 지속성 (User DB Persistence)
- 로우 레벨 구조체 단위 오프셋 파일 입출력으로 자산 영속성 방어.

## 3. 시스템 아키텍처 및 시스템 콜 활용 계획 (System Architecture)

### A. 네트워크 및 다중 접속 동기화 (Multithreading & Socket Lock)
- 멀티스레드 기반 TCP 소켓 서버 (Thread-per-client 모델).
- **[교착 상태(Deadlock) 원천 차단 룰]**: NPC 의뢰를 맞추기 위해 다수의 정보 파일(ID 3번, 5번 등)에 동시에 `pthread_mutex_lock`을 걸어야 할 경우, 반드시 **[가장 ID 숫자가 작은 파일 자원부터 오름차순으로 Lock을 획득하도록 강제]**하여 순환 대기(Circular Wait)에 의한 운영체제론적 데드락 에러를 미연에 원천 차단합니다.
- **시스템 콜**: `socket()`, `bind()`, `listen()`, `accept()`, `pthread_create()`, `pthread_mutex_lock/unlock()`

### B. 시그널 제어 및 스레드-안전 이벤트 타이머 (Signals & Timers)
- **이벤트 스레드**: 서버에서 별도 스레드가 `sleep()`에 기반해 Aging 처리.
- **시스템 콜**: `sleep()`, `alarm()`, `signal/sigaction()`, `kill()`

### C. 샌드박스 시스템 파일 마운트 무결성 룰 (FileSystem Restrictions)
- **[마운트 EXDEV 제약 요건 방어]**: `rename()` 호출 시 다른 파티션 대역이면 이동이 거부되는 OS 취약점을 피해, 마켓 마스터 폴더와 모든 유저의 개인 샌드박스는 반드시 부모 폴더를 공유하는 로컬 트리 구조(예: `./data/market` 과 `./data/users`) 내에 상대 경로로 묶여 있도록 설계합니다.
- **시스템 콜**: `mkdir()`, `rename()`, `link()`, `unlink()`, `opendir()`, `readdir()`, `stat()`, `chmod()`

### D. 유저 DB 저장 동기화 (File I/O Persistence & Lock)
- 데이터 갱신 시 `fcntl()` 레코드 락 적용.

### E. 워치독 서버 모니터 (Watchdog Server Monitor)
- `monitor` 부모 프로세스와 `waitpid()` 로 좀비 처리.

## 4. 유저 인터페이스 (User Interface) 및 UI 렌더링 룰
- **WINDOW 분할 확장 (`newwin`)**: 5개 독립 객체 구조 및 렌더링 락.
- **수동 라인 버퍼링**: `cbreak()`, `timeout(0)`, `noecho()`.
- **UI 사보타주 시각 이펙트**: `standout()` 및 컬러 뒤섞음을 통해 찌라시/해킹 피드백 직관적 제시.

## 5. 프로젝트 평가 기준 방어 및 체크리스트 (Evaluation Defense Checklist)
- Deadlock 방지 락 체인(순환대기 해결 오름차순 알고리즘 구현), EXDEV 회피 파일 마운트 제한, Async-Signal-Safety 로직 분기 등 상업적 규모에서도 가장 난해한 C언어/운영체제론적 크리티컬 에러들을 아키텍처 설계 단계부터 선제적으로 완벽 방어함.
