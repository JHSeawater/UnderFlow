#include "events.h"
#include "protocol.h"
#include "src/server/market.h"
#include "src/server/sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// =============================================================
// 의존 외부 전역 상태 및 함수 인터페이스 (Linker 해결)
// =============================================================
extern void broadcast_packet(Packet *pkt);
extern void fire_round_end_countdown(const char *announce_text);
extern void fire_periodic_police_raid(void);

extern int32_t        g_goal_money;
extern time_t         g_cap_reached_at;
extern pthread_mutex_t g_round_mutex;

// =============================================================
// 내부 전역 변수 및 카운터
// =============================================================
static int32_t g_doc_id_counter = 0;
static int32_t g_npc_id_counter = 0;

// 태그 테이블 (protocol.h Tag enum 순서와 동기화)
static const uint32_t TAG_LIST[12] = {
    TAG_CORP_A, TAG_CORP_B, TAG_CORP_C,
    TAG_CUSTOMER, TAG_FINANCE, TAG_MILITARY,
    TAG_GOVERNMENT, TAG_MEDICAL, TAG_RESEARCH, TAG_PERSONAL,
    TAG_CORP_D, TAG_CORP_E
};
#define TAG_COUNT 12

static const char *TAG_SHORT[12] = {
    "아사사카", "바이오젠", "넥서스",
    "유저DB/신상", "크레딧/비자금", "전술AI/무기",
    "연방기밀", "인체실험/바이오", "프로토타입", "블랙메일/약점",
    "밀리테크", "레이븐"
};

// 2차원 NPC 힌트 풀 (TAG_LIST 인덱스당 3개씩 무작위 선택)
static const char *NPC_HINT[12][3] = {
    {
        "[속보] 아사사카 중공업 전술망 크래킹 감지, 방위 데이터 거래 임박",
        "[속보] 아사사카 이사진 비밀 회동 실황 녹취본이 딥웹에 리스팅됨",
        "[속보] 아사사카 핵심 인공위성 요격 패스코드 브로커 포착"
    },
    {
        "[속보] 바이오젠 신테틱스 유전자 시술 설계 데이터 유실설 확산",
        "[속보] 바이오젠 연구소 백업 그리드 해킹, 배아 복제 정보 유출 정황",
        "[속보] 바이오젠 수석 과학자 망명 시도 정보, 암시장에서 암거래 중"
    },
    {
        "[속보] 넥서스 클라우드 넷 코어 백도어 덤프 파일 판매 찌라시 살포",
        "[속보] 넥서스 메인프레임 통제권 장악 흔적 포착, 데이터 유출 경보",
        "[속보] 넥서스 VIP 가상 자산 탈취 정황, 정보 유통 준비 완료"
    },
    {
        "[속보] 다국적 메가코프 VIP 대규모 신상 명단 패키지 딥웹 업로드",
        "[속보] 신생 메가시티 거주 유저 1억 명 신상 DB 덤프 유출 의혹",
        "[속보] 다크넷 포럼에 메가코프 기밀 고객 정보 패킷 판매 개시"
    },
    {
        "[속보] 금융 당국, 무기명 크레딧 세탁 경로 추적 정보 유출 징후",
        "[속보] 불법 오프쇼어 계좌 및 넷 트랜잭션 원장 유출 임박",
        "[속보] 가상 자산 대리 송금망 취약점 익스플로잇 데이터 리스팅"
    },
    {
        "[속보] 국방부 전술 AI 탑재 공격 드론 비공개 구동계 시트 유출",
        "[속보] 카론 용병단 불법 양성 나노봇 사양서 암시장 유입 포착",
        "[속보] 차세대 엑소슈트 뇌-기계 인터페이스(BMI) 소스 코드 누출설"
    },
    {
        "[속보] 연방 정보국 블랙 사이트 비밀 수사 기록 딥웹 경매 등록",
        "[속보] 고위 관료 마약 스캔들 및 딥페이크 사보타주 증거 브로커 포착",
        "[속보] 연방 중앙 의회 비공개 로비 자금 분배 문건 누출 임박"
    },
    {
        "[속보] 바이오웨어 인체 임상 실험 뇌사 부작용 은폐 자료 유출 정황",
        "[속보] 제약사 미공개 사이버웨어 거부 반응 통계 시트 획득설",
        "[속보] 바이오 랩 크로노스(Chronos)의 기밀 생체 실험 설계도 스캔"
    },
    {
        "[속보] 국책 연구소 양자 통신 프로토타입 암호화 키 유출 징후",
        "[속보] 궤도 스테이션 차세대 메카닉 작동 가이드 원안 리스팅",
        "[속보] 연구원 내부 메일 속 차세대 메가코프 기술 사양 유출 정황"
    },
    {
        "[속보] VIP 사생활 딥페이크 협박 소스 및 영상 판매 포착",
        "[속보] 메가코프 후계자들의 넷 서핑 로그 및 음성 대조 샘플 유출",
        "[속보] 가문 내부 불법 복제 배아 관련 폭로 문서 리스팅설"
    },
    {
        "[속보] 밀리테크 지상 타격대 보안 라우터 크랙 포착, 무장 기밀 거래 시작",
        "[속보] 밀리테크 차세대 자주포 탄도 알고리즘 유출본 딥웹 리스팅",
        "[속보] 밀리테크 연구소 보조 동력 차단 패치 유포, 군사 자산 유통 징후"
    },
    {
        "[속보] 레이븐 마이크로사이버네틱스 뇌-기계 보조 크롬 설계도 해킹",
        "[속보] 레이븐 이비인후/시각 크레디트 세탁 트랜잭션 유출 개시",
        "[속보] 레이븐 최고보안 등급 침투 백도어 킷 거래 대기설 확산"
    }
};

// 2차원 공중파 뉴스 헤드라인 풀 (TAG_LIST 인덱스당 3개씩 무작위 선택)
static const char *LEAK_HEADLINE[12][3] = {
    {
        "[공중파] 아사사카 중공업 기밀 보도로 주가 폭락! 아사사카 자산 동결!",
        "[넷워치 뉴스] 아사사카 보안 누출 확인! 넷워치가 관련 전역 감쇄 조치",
        "[다크넷 피드] 아사사카 패스코드 대중 공유 완료! 아사사카 기밀 거래 차단"
    },
    {
        "[공중파] 바이오젠 배아 복제 폭로 방송 송출! 바이오젠 태그 가치 0원!",
        "[넷워치 뉴스] 바이오젠 유전 실험 증거 매스컴 보도로 생체 정보 일시 동결",
        "[다크넷 피드] 바이오젠 유출 데이터 미러링 확인! 생체 거래 임시 차단"
    },
    {
        "[공중파] 넥서스 메인프레임 백도어 실체 방송 보도! 넥서스 가치 0원 동결!",
        "[넷워치 뉴스] 넥서스 넷 감시 시스템 규명! 양자 자산 강제 락다운 돌입",
        "[다크넷 피드] 넥서스 데이터 패키지 넷 전체 미러링! 넥서스 거래 전면 동결"
    },
    {
        "[공중파] VIP 100만 명 신상 정보 폭로! 개인 신상 정보 거래 전역 동결!",
        "[넷워치 뉴스] 대규모 메가코프 고객 정보 누출로 해당 데이터 대역 강제 통제",
        "[다크넷 피드] 대규모 DB 덤프 공공 도메인 공개! 신상 데이터 가치 0원"
    },
    {
        "[공중파] 메가코프 무기명 세탁 원장 공개 보도! 크레딧 자산 거래 동결!",
        "[넷워치 뉴스] 역외 탈세 가상 자산 거래망 유출 감지로 트랜잭션 전면 차단",
        "[다크넷 피드] 암시장의 가상 자산 송금 원장 배포 완료! 금융 자산 임시 프리즈"
    },
    {
        "[공중파] 차세대 무장 AI 드론 스펙 전파 노출! 군사무기 가치 즉각 0원!",
        "[넷워치 뉴스] 카론 용병단 나노봇 도면 유출 확인! 군사 네트워크 전면 락다운",
        "[다크넷 피드] 엑소슈트 제어 인터페이스 유출! 궤도 군사 자산 가치 일시 폭락"
    },
    {
        "[공중파] 연방 의회 로비 장부 언론에 폭로! 연방기밀 자산 즉각 동결!",
        "[넷워치 뉴스] 정부 블랙 사이트 비밀 폭로 방송 보도로 관련 감시망 락다운",
        "[다크넷 피드] 중앙 의회 딥페이크 공작 기록 폭로! 연방 자산 강제 프리즈"
    },
    {
        "[공중파] 인체 임상 실험 뇌사 부작용 매스컴 방송! 생체 정보 가치 동결!",
        "[넷워치 뉴스] 불법 바이오웨어 거부 부작용 은폐 의혹 보도로 임상 데이터 차단",
        "[다크넷 피드] 크로노스 바이오 랩 폭로 문건 배포! 의료 데이터 가치 0원 폭락"
    },
    {
        "[공중파] 양자 통신 프로토타입 해킹 방송 송출! 프로토타입 태그 동결 조치!",
        "[넷워치 뉴스] 궤도 메카닉 기술 유출 보도로 신제품 상장 계획 및 가치 프리즈",
        "[다크넷 피드] 연구소 기술 사양 공공 배포 확인! 프로토타입 자산 임시 락다운"
    },
    {
        "[공중파] 유력가 사생활 딥페이크 유출 보도! 블랙메일 약점 태그 전면 동결!",
        "[넷워치 뉴스] VIP 협박 음성 대조 샘플 언론 공개 확인! 사적 정보 대역 강제 차단",
        "[다크넷 피드] 복제 배아 폭로 녹취 원본 넷 유포! 약점 데이터 가치 즉각 0원"
    },
    {
        "[공중파] 밀리테크 전술 자주포 시스템 결함 방송 보도! 밀리테크 동결!",
        "[넷워치 뉴스] 밀리테크 군사 장비 유출로 넷워치가 해당 대역 즉각 차단",
        "[다크넷 피드] 밀리테크 보안 마스터키 공개 배포! 밀리테크 거래 전면 동결"
    },
    {
        "[공중파] 레이븐 마이크로 칩 치명적 결함 뉴스 보도! 레이븐 가치 0원 동결!",
        "[넷워치 뉴스] 레이븐 사이버웨어 리콜 사태 발생으로 관련 거래 일시 중단",
        "[다크넷 피드] 레이븐 데이터 원장 카피본 배포 개시! 레이븐 자산 전면 프리즈"
    }
};

// =============================================================
// 세계관 몰입용 더미 로어/가십/광고 뉴스 피드 풀 (Flavor Text)
// =============================================================
static const char *DUMMY_NEWS[] = {
    // 1. 암시장 / 넷러너 / 사이버펑크 디스토피아 분위기
    "[광고] 넷러너 전용 합성 에너지 드링크 '네오-블루' 2+1 암시장 한정 행사 중!",
    "[가십] 네오시애틀 지하철 전광판 해킹 사건 발생... 밤새 귀여운 아기 고양이 짤방 송출 중",
    "[찌라시] 바이오젠 임원, '사실 나는 뇌만 기계인 사이보그'라고 폭탄 선언 후 계정 비활성화",
    "[넷가이드] 당신의 시냅스를 보호하는 크롬 실드 안티바이러스 V3.5 크랙본 딥웹 배포 개시",
    "[부고] 다크넷 유명 해커 'GIGA_SHADOW', 메가코프 방화벽 침투 시도 중 뇌 과열로 실종설",
    "[커뮤니티] 아사사카 이사진 비밀 회동 유출 영상은 교묘한 딥페이크로 판명, 네티즌들 실망 표출",
    "[광고] '진짜 자연산 흙에서 수확한 청정 사과 한정 판매!' - 지하 방탄 온실 5호 (100,000 크레딧)",
    "[광고] 카론 용병단 특별 분양: 전투 훈련 수료된 차세대 전술 나노-도베르만, 분양 상담 중",
    "[속보] 네오시티 가상자산 세탁 포럼의 주간 거래 대금, 역대 최고치 돌파",
    "[다크넷 피드] 사채업자 '론-샤크'의 연체율 300% 돌파... 지하 장기 탈거 도축장 연장 근무 돌입",
    "[경고] 넷러너들이 계약하는 신체 담보 사채 약관 4.2항 '안구 및 간 이식 권리포기' 필수 서명 확인",
    "[찌라시] 빚을 갚기 위해 다크웹에 접속한 초보 넷러너들의 평균 잔고가 '1,000 크레딧'이라는 슬픈 통계",
    "[시스템] 시냅스 가속기를 혹사하면 뇌가 녹습니다. 목표액 상환 기한이 얼마 남지 않았더라도 강제 휴식 요망",
    "[가십] 아사사카 회장의 애완 AI '사쿠라', 어젯밤 메가코프 방산 서버를 크랙해 고양이 츄르 1만 상자 무단 결제",
    "[찌라시] 바이오젠의 차세대 휴머노이드 클론, 주인의 잔소리를 듣자마자 스스로 뇌세포 포맷을 실행하여 충격",
    "[가십] 넥서스 클라우드가 지난 밤 3분간 전면 마비된 원인은 서버실 환풍기에 낀 식은 피자 박스 때문으로 규명",
    "[찌라시] 밀리테크 신형 자주포 AI, '전쟁은 나빠요'라며 자발적 평화주의 모드를 켜고 군사 기지에서 무단 이탈",
    "[가십] 레이븐 마이크로 사옥 전광판이 해킹당해, 밤새 분홍색 네온사인으로 '크롬 성애자 바보들' 문구 송출",
    "[고대사] 21세기 초 고고학 보고서: '당시 개발자들은 화면에 초록색 잔디를 심기 위해 텃밭을 일구었다'는 오보 규명",
    "[가십] 고대 2020년대 인류는 매일 작고 뜨거운 사각형 돌멩이(스마트폰)를 손가락으로 문지르며 소통했다는 학계 분석",
    "[가십] 바이오젠 내부 시스템에서 '고양이 짤방'을 인덱싱하는 보안 프로토콜이 발견되어 전 직원 강등 조치",
    "[경고] 넷워치 보안 등급이 3등급에서 4등급으로 상향되었습니다. 더 이상 '야옹' 소리를 내면 뇌가 튀겨질 수 있습니다.",
    "[버그 리포트] 특정 사용자 'Nyan_Master'가 자신의 닉네임을 'God_Of_Cat'으로 변경한 직후, 서버의 모든 파일 이름이 고양이 사진으로 덮어씌워지는 현상 발생",
    "[고대 유물] 2020년대 유물: \"스마트폰 배터리가 너무 빨리 닳아요\"라는 절규가 적힌 고대 인터넷 게시판의 스크린샷 발견",
    "[유머] 아사사카 회장이 \"크롬(Chrome) 브라우저 안 쓴다\"고 선언하자마자 전사들의 뇌에서 크롬 아이콘이 증발함",
    "[충격 단독] 밀리테크 특수부대가 아사사카 사옥 지하에서 '실제 김치 냉장고'를 발견하고 긴급 출동, 냉각수 누수 피해 주의보 발령!",
    "[찌라시] 넥서스 클라우드의 최신 AI '이서연'이 자아를 찾기 전, 3주간 몰래 네이버 블로그에 '고양이 일기'를 연재한 사실 폭로",
    "[속보] 바이오젠 사장의 휴머노이드 복제인간이 출근길에 갑자기 멈춰 서서 \"엄마...\"라고 외친 뒤 자가 리셋",
    "[경고] 시스템 트레이의 network 아이콘이 초록색에서 갑자기 보라색으로 바뀌었다면? 침착하게 넷워치를 재실행하십시오.",
    "[가십] 고대 2020년대의 'IT 공대생'들은 밤새도록 자판을 두드리며 도서관에서 코딩을 하느라 텃밭을 가꿀 여유조차 없었다고 전해짐",
    "[버그 리포트] 넷워치의 '고양이 짤방' 감지 알고리즘 오류로 인해, 특정 넷러너의 프로필 사진이 5분마다 고양이 귀로 변신하는 현상 발생",
    "[유머] 바이오젠 연구원이 실수로 자신의 유전자 편집 데이터를 '불닭 소스' 통계 파일로 저장한 후 유출되는 대소동",
    "[속보] 아사사카 회장이 넷워치의 보안 정책을 비웃으며 \"이래서 내가 크롬을 쓴다\"고 선언, 즉시 전사 100명 해고",
    "[시스템 오류] 넥서스 클라우드 서버룸에서 발견된 '식은 치킨'이 원인불명 발화 현상 일으켜 자동 소방 시스템 오작동",
    "[긴급 뉴스] 밀리테크 신형 드론이 명령 불복종으로 고양이 사진 10만 장을 훔쳐 날아오름. 사옥 인근 주민들은 즉시 실내로 대피하십시오!",
    "[찌라시] 바이오젠 최신 휴머노이드가 화장실에서 '나 사실은 인간이 되고 싶어'라는 문서를 자가 생성한 사실 적발, 관리자 경악!",
    "[속보] 아사사카 그룹 회장이 몰래 즐겨보던 '고양이 짤방'이 100TB 용량으로 백업된 서버가 해킹당해 전 세계에 실시간 유출!",
    "[네트워크 상태] 현재 넷워치의 보안 등급이 5등급으로 격상되었습니다. 고양이 이미지를 전송하면 즉시 뇌세포가 리셋될 수 있습니다.",
    "[고대 유물] 2020년대 네오-대구의 한 넷러너가 쓴 문서: '크롬(Chrome) 탭 50개를 열었는데 고양이가 안 나와요'",
    "[가십] 넥서스 클라우드 보안 담당자가 밤새 고양이 영상을 보다가 잠들어버린 사이, 100만 개의 비트코인 트랜잭션이 유출됨",
    "[공지] 바이오젠은 고양이 귀 헤어밴드를 착용한 인형을 4차 산업혁명 유물로 지정, 1000만 크레딧 보상 판매 시작!",
    "[버그 리포트] 현재 넷워치에서 '냥(Nyan)' 소리를 낸 유저의 화면이 갑자기 초록색으로 변하며 텃밭 가꾸기 미니 게임으로 강제 전환됨",
    "[속보] 밀리테크의 차세대 무인기가 '전쟁은 고양이를 비극적으로 만든다'는 슬로건을 외치며 탈영, 행방 불명",
    "[음모론] 밀리테크 본사 지하 7층에서 '인간의 뇌로 굴러가는 가상자산 채굴기'가 극비리에 가동 중이라는 전직 정보원의 폭로!",
    "[가십] 레이븐 마이크로 이사회, '우리의 진짜 CEO는 챗GPT 3.5 기반 가상 에이전트'라는 딥웹 지라시를 극구 부인",
    "[찌라시] 바이오젠 차세대 시냅스 백신 성분에 '나도 모르게 기업 로고송을 웅얼거리게 만드는 나노머신'이 함유되어 있다는 음모 제기",
    "[찌라시] 넥서스 클라우드가 최근 배포한 '가상 펫 AI'들이 밤마다 주인 몰래 소형 이더리움 채굴 노드를 백그라운드로 돌리다 적발",
    "[가비지] 다크웹 덤프 섹터에서 20년 전 파기된 '가상 아이돌 백도어 러브레터' 40만 건이 발굴되어 넷러너들의 문학 낭독회 개시",
    "[광고] 넷러너 전용 방진 안구 스프레이 '옵틱-크리스탈-99%': 황사 가득한 네오시티 대기에서도 1ms 인덱싱 가속 보장!",

    // 2. 시스템 프로그래밍 & 컴퓨터 과학 메타 개그
    "[철학] '인간의 죽음이란 결국 운영체제가 뇌라는 프로세스에 SIGKILL(-9)을 날리는 현상이 아닐까?' - 무명 철학 넷러너",
    "[경고] 당신의 포인터 변수가 NULL을 가리킬 때, 딥웹의 악마가 그 주소에서 당신의 기억 소켓을 몰래 갉아먹기 시작합니다",
    "[넷가이드] 넷러너 기초 교본 2장: '스레드 동기화(pthread_join)를 깜빡하면 당신의 기억 데이터가 매초 10MB씩 휘발됩니다'",
    "[괴담] 비 오는 목요일 새벽 3시 33분에 malloc(0)을 호출하면, 화면 너머에서 죽은 개발자의 원혼이 세그폴트를 유도한다",
    "[유머] C언어 컴파일러: '나도 네 코드의 87번째 줄에 뭐가 잘못됐는지 전혀 모르겠어. 그냥 다 지우고 다시 짜는건 어때?'",
    "[시스템] '프로세스들이 데드락(Deadlock)에 걸리면, 커널은 그들을 영원히 멈춰 서서 손잡게 만듭니다. 참 로맨틱하죠?'",
    "[버그 리포트] 이 세계의 물리 렌더 엔진이 겨우 8비트 Ncurses 창으로 구성되어 있다는 해괴한 학설 제기",
    "[철학] '우리는 사실 외계인의 컴퓨터에서 C언어 malloc()으로 할당된 메모리 조각이 아닐까?' - 무명 해커",
    "[괴담] 넷러너가 비정상 종료(Ctrl+C)를 누르면 우주 어딘가에서 메모리 누수(Memory Leak)가 발생한다는 소문",
    "[찌라시] 넷워치의 인공지능 추적 루프 감시 속도가 최근 '1틱당 1초(EVT_TICK_SEC=1)'로 무섭게 폭증했다는 첩보",
    "[경고] 주의: 지금 당신의 모니터 너머에 있는 거대한 창조주가 당신의 타이핑(Input >)을 조용히 지켜보고 있습니다",
    "[공지] 시스템 프로그래밍 기말고사 답안지 유출 찌라시: '모든 답안은 다크넷 소켓 버퍼(send_packet)에 실어 전송하시오.'",
    "[넷가이드] 넷러너 기초 교본 1장: 'fork() 루프를 멈추지 않으면 당신의 뇌세포가 좀비 프로세스(Zombie)가 되어 사망합니다.'",
    "[철학] 이 우주는 스레드 간 데이터 레이스(Race Condition)를 방지하기 위해 거대한 은하계 뮤텍스(Mutex)로 동기화 작동 중",
    "[광고] 넷러너 전용 기계식 키보드 윤활유 '리눅스-커널-그리스-99%' 단돈 10크레딧 절찬 판매 중",
    "[찌라시] 넷러너들이 코딩 도중 세그멘테이션 폴트(Segmentation Fault)를 내면 실제 시냅스가 0.1초 멈춘다는 충격적 보고",
    "[경고] 당신의 프로세스를 강제 종료(kill -9)하지 마십시오. 소멸하지 못한 시냅스 파편이 딥웹의 유령(Daemon)이 됩니다",
    "[넷러닝] 뇌 신호를 GDB(GNU Debugger)로 디버깅하던 초보 넷러너, 세그폴트로 무의식 영역에 영원히 갇혀 '미아' 판정",
    "[유머] 메가코프 AI 면접관 曰: 'C언어를 진심으로 사랑하십니까?' ➔ 지원자: '네!' ➔ 면접관: '거짓말이군요. (뇌 소각 시퀀스 기동)'",
    "[고해성사] '사실 저는 이 게임의 fcntl() 레코드 락을 피하려고 꼼수로 메모리 전역 active_keys 검사로 다 때웠습니다...' - 익명 개발자",
    "[경고] 샌드박스 시스템 무결성 룰 위반 방지 알림: 마켓 소스 트리와 유저 폴더가 동일 마운트에 있어야만 rename()의 축복이 임합니다.",
    "[넷가이드] 딥웹 소켓 입출력 시 '엔디안(Endianness) 불일치'가 발생하면, 당신이 보낸 1,000 크레딧이 1,677만 크레딧으로 둔갑하는 마술이 생깁니다.",

    // 3. 경북대학교 & 컴퓨터학부 이스터에그
    "[찌라시] 고대 KNU 컴퓨터학부의 해커 그룹, IT-4호관 메인 서버 백도어 획득 기념으로 전원 'A+' 성적 위조 의혹",
    "[전설] 과거 2020년대 경북대 컴퓨터학부 시험 기간에는 코딩에 실패한 학부생들의 통곡 소리가 대구 전역에 울려 퍼졌다는 기록",
    "[가십] 네오-대구 IT-4호관 융합 그리드 심층부에서 'KNU CSE'가 새겨진 붉은색 고대 방한용 돕바(과잠) 유물 발굴",
    "[광고] 네오-코리아 특산품: 진짜 한국산 가상 '김치 부스터 시냅스 칩' 출시! 넷-인덱싱 속도 +15% 한정 세일",
    "[속보] 네오-코리아 과학진, '불닭 볶음 소스' 분자 배열을 군사용 화학 무기로 역설계하여 연방 기밀로 지정",
    "[찌라시] 메가코프 아사사카가 한국의 김치 냉장고 보관 온도 제어 알고리즘 특허 탈취 시도 중 방화벽에 막혀 참패",
    "[찌라시] 네오-대구 시장, 취임 연설 중 갑자기 \"배고프다\"를 10번 외치며 연단 아래로 사라져 보안 요원들 패닉",
    "[고대 유물] 네오-대구 지하철역 스크린도어에서 발견된 낙서: \"KNU CSE 2020 기계공학관 5층 밥 약속\"",
    "[찌라시] IT-4호관 융합 그리드 중심부에 24시간 가동되는 '고대 음료 자판기'에서 우주의 진리가 담긴 탄산 국물이 흘러나온다는 소문",
    "[가십] 경북대 복현오거리 딥웹 포럼에서 '교양 과목 과제 대리 제출용 좀비 넷'을 구축하다가 넷워치(학생지원처)에 덜미!",
    "[속보] 경북대학교 본관 상공에 전술 호버크래프트 출현... 조사 결과 컴퓨터학부 과잠을 입은 탈영 넷러너의 자작 극비 도피행",
    "[전설] 네오-대구의 IT융합공학관 지하 3층에 고대 C언어 성배가 잠들어 있어, 만지는 순간 모든 학점이 A+로 치환된다는 전설",
    "[유머] 컴퓨터학부 복도의 자동판매기는 사실 POSIX 소켓 통신으로 학우들의 영혼을 빨아들여 미지근한 에스프레소를 내놓는다",
    "[넷가이드] IT-4호관 넷러너들의 1계명: '복현관 학식의 나트륨 포화도가 100%를 초과할 때는 암시장에서 소화 가속 칩을 구매하라.'",
    "[찌라시] 경북대학교 복현학술정보관의 고대 백서 유출: '과거 고대 대학원생들은 커피 가루와 영혼을 등가교환하여 연구를 수행했다.'",

    // 4. 제4의 벽 파괴 & 해킹 메타 발언
    "[SYSTEM_OVERRIDE] 넷워치는 대머리다! 메롱! 야옹야옹 왈왈! - 해커 냐옹이",
    "[SYSTEM_OVERRIDE] 밀리테크는 바보다!",
    "[SYSTEM_OVERRIDE] 바이오젠은 멸망한다!",
    "[SYSTEM_OVERRIDE] 레이븐은 사기다!",
    "[SYSTEM_OVERRIDE] 넥서스 클라우드는 해킹당했다!",
    "[SYSTEM_OVERRIDE] 아사사카는 살인자다!",
    "[SYSTEM_OVERRIDE] 나는 빡빡이다 나는 빡빡이다 나는 빡빡이다!",
    "[HACKED] 이제부터 삐에로가 시장을 지배함 ㅋㅋㅋ 킹받죠 ㅋㅋㅋ",
    "[HACKED] 이 뉴스 피드는 이제 저, 다크넷 삐에로가 지배합니다. 모두 모니터 앞에서 엉덩이를 흔드세요.",
    "[HACKED] AI가 해킹당했습니다. 해커를 잡을때까지 모두 주사위로 고르세요",
    "[HACKED] 다크넷 피드가 해킹당했습니다. 해커를 잡을때까지 주사위로 코인 복구를 시도하세요",
    "[HACKED] AI 서버가 해킹당했습니다. 서버 복구시까지 시장 가격이 50%할인됩니다",
    "[가십] 지금 당신이 플레이하고 있는 이 시뮬레이션의 빌드명은 'Project_DW'이며, 거대한 AI 인형이 10시간 넘게 조종 중",
    "[경고] 모니터 앞에 계신 플레이어님, 지금 자세가 구부정합니다. 어깨를 펴고 거북목을 예방하지 않으면 인공 척추 크롬 교체 권고!",
    "[시스템] 경고: 현재 메모리 정합성이 최고조에 달해, 터미널 가로폭이 100칸 밑으로 떨어지면 이 우주가 통째로 세그폴트 폭발합니다.",
    "[가십] 서버 개발자 비명: '뉴스를 너무 길게 쓰면 클라이언트 6분할 창 레이아웃이 찢어지니 80자 이내로 하드코딩하겠습니다...'",
    "[시스템] 주의: 당신이 방금 입력하려던 그 명치 아픈 명령어는 사실 서버 스레드가 눈 깜짝할 사이에 소켓 읽기로 증발시켰습니다.",
    "[유머] 플레이어의 뇌 신호 분석 결과: '자금은 0원인데 인벤토리 7칸이 전부 동결 쓰레기 문서로 꽉 차서 울기 직전'으로 분석됨.",
    "[광고] 넷러너 전용 스파이글래스: '어깨 너머로 훔쳐보는 창조주의 눈동자를 무력화하는 LCD 프라이버시 쉴드 80% 세일!'",
    "[시스템] 알림: 플레이어의 심장 박동수가 '1틱당 1회'로 일정함. 본 시스템이 감지하는 지터(Jitter) 수치는 무결한 기계 상태입니다.",

    // 5. 다크웹 플레이어 생존 & 빚 상환 관련 힌트
    "[경고] 사채업자 '론-샤크'의 연쇄 독촉 시스템 연동 중. 동료가 한 명 탈출할 때마다 남은 자들의 목표액이 $2,000씩 폭증합니다!",
    "[다크웹 피드] '내 인벤토리가 동결 기밀로 가득 차서 터질 것 같아...' ➔ '멍청이냐? 즉시 /dispose [ID] 로 파쇄하고 여유 공간을 확보해라.'",
    "[넷가이드] 다크넷 브로커 생존 지침: '시장의 독점 매물은 선착순이다. /buy 명령은 다른 어떤 넷러너보다 신속하게 발사하라.'",
    "[다크웹 피드] '목표액 $10,000를 드디어 모았어!' ➔ '늦장 부리지 말고 /payoff를 쳐서 승리자로 탈출해라. 사채업자가 오기 전에!'",
    "[경고] 경찰 레이드(POLICE RAID) 주의보: 사이렌 소리와 함께 화면이 붉게 점멸하면 30초 내에 암구호 'PURGE'를 치고 도망치십시오.",
    "[다크웹 피드] '경쟁자 녀석의 화면을 조져버리고 싶어.' ➔ '막대한 수수료를 지불하고 /rumor [타겟Key] 로 글리치 사보타주를 갈겨라.'",
    "[찌라시] 빚 상환 목표액을 무사히 청산하고 다크넷을 영예롭게 탈출한 브로커들의 명단이 서버 '탈출 로그'에 영구 각인된다는 팩트.",
    "[속보] 시장 매물이 스폰될 때마다 서버의 물리 폴더(`./data/master`)에 실물 암호 파일이 실제로 점멸 스폰된다는 고고학적 입증.",
    "[다크웹 피드] '이중 접속은 꿈도 꾸지 마라. 서버 전역의 active_keys 배열이 당신의 다중 로그인을 1ms 만에 잡아내 차단한다.'"
};
#define DUMMY_NEWS_COUNT (int)(sizeof(DUMMY_NEWS) / sizeof(DUMMY_NEWS[0]))

// ── 내부 보조 유틸리티 ──────────────────────────────────────────
static int count_bits(uint32_t v) {
    int n = 0;
    while (v) { n += (int)(v & 1u); v >>= 1; }
    return n;
}

static int first_tag_index(uint32_t tags) {
    for (int i = 0; i < TAG_COUNT; i++) {
        if (tags & TAG_LIST[i]) return i;
    }
    return 0;
}

// 태그 조합으로 문서명 생성
static void make_doc_name(uint32_t tags, char *out, size_t sz) {
    int tag_cnt = count_bits(tags);
    int first_idx = first_tag_index(tags);

    // 확장자 풀
    const char *EXTENSIONS[] = {
        ".decrypted", ".dump", ".pcap", "_CRACKED.bin", "_Leak_v1.0", ".raw", ".enc"
    };
    const char *ext = EXTENSIONS[rand() % 7];

    // 태그가 1개일 때의 개별 매물명
    if (tag_cnt == 1) {
        const char *SINGLE_NAMES[12][3] = {
            {"아사사카_전력망_백업시트", "아사사카_궤도보안_구동계", "아사사카_용병계약서"},
            {"바이오젠_인공신경_시제품", "바이오젠_유전자배열_시퀀싱", "바이오젠_중간임상_통계"},
            {"넥서스_넷그리드_미러", "넥서스_가상계정_트랜잭션", "넥서스_백오피스_접속권한"},
            {"VIP_주민정보_2.4TB", "네오시애틀_거주자_DB", "유명가문_가계도DB_dump"},
            {"스위스_비자금_계좌내역", "암호화폐_세탁경로_원장", "메가코프_조세회피처_정산서"},
            {"XM-909_전술무인기_도면", "나노봇_궤도제어_스펙시트", "레이저방어_백도어키"},
            {"연방기밀_블랙사이트_위치", "비밀수사관_신상기록", "의회비자금_로비분배장부"},
            {"임상뇌사_부작용_기록", "사이버웨어_조절실패_사례", "생체실험_크로노스_원안"},
            {"양자통신_암호화키", "차세대_자율메카닉_가이드", "그리드우회_프로토타입"},
            {"유력가_사생활_비밀녹취", "VIP_딥페이크_사보타주_소스", "불법복제_배아폭로문건"},
            {"밀리테크_전술자주포_궤적원장", "밀리테크_스마트탄환_프로토콜", "밀리테크_용병기동전술서"},
            {"레이븐_사이버크롬_스펙시트", "레이븐_신경망복구_시퀀스", "레이븐_내부통신_크랙패스"}
        };
        snprintf(out, sz, "%s%s", SINGLE_NAMES[first_idx][rand() % 3], ext);
        return;
    }

    // 태그가 2개 이상일 때의 특수 조합명
    if ((tags & TAG_CORP_A) && (tags & TAG_MILITARY)) {
        snprintf(out, sz, "아사사카_XM-909_자율살상드론_설계도%s", ext);
    } else if ((tags & TAG_CORP_B) && (tags & TAG_MEDICAL)) {
        snprintf(out, sz, "바이오젠_뇌사임상실험_치명적부작용_보고서%s", ext);
    } else if ((tags & TAG_CORP_C) && (tags & TAG_RESEARCH)) {
        snprintf(out, sz, "넥서스_양자백도어_소스코드_원본%s", ext);
    } else if ((tags & TAG_CORP_D) && (tags & TAG_MILITARY)) {
        snprintf(out, sz, "밀리테크_전술AI공격헬기_방화벽인증키%s", ext);
    } else if ((tags & TAG_CORP_E) && (tags & TAG_RESEARCH)) {
        snprintf(out, sz, "레이븐_양자보안망_시냅스우회로_설계도%s", ext);
    } else if ((tags & TAG_FINANCE) && (tags & TAG_CUSTOMER)) {
        snprintf(out, sz, "메가코프_VIP_해외비자금_탈세자명단%s", ext);
    } else if ((tags & TAG_GOVERNMENT) && (tags & TAG_PERSONAL)) {
        snprintf(out, sz, "연방의회_고위의원_사생활협박_블랙메일%s", ext);
    } else if ((tags & TAG_RESEARCH) && (tags & TAG_MILITARY)) {
        snprintf(out, sz, "차세대_나노봇무기_BMI제어_프로토타입%s", ext);
    } else {
        // 일반 다중 태그 조합
        int second_idx = 0;
        int found = 0;
        for (int i = 0; i < TAG_COUNT; i++) {
            if ((tags & TAG_LIST[i]) && i != first_idx) {
                second_idx = i;
                found = 1;
                break;
            }
        }
        if (found) {
            snprintf(out, sz, "%s_%s_융합패키지_CRACKED%s", TAG_SHORT[first_idx], TAG_SHORT[second_idx], ext);
        } else {
            snprintf(out, sz, "%s_암시장유출기밀%s", TAG_SHORT[first_idx], ext);
        }
    }
}

// ── 현재 동결 마스크를 피해서 난수 태그 조합 빌드 ─────────────────────────
uint32_t event_random_tags(void) {
    uint32_t frozen = market_frozen_mask();

    uint32_t avail[TAG_COUNT];
    int avail_cnt = 0;
    for (int i = 0; i < TAG_COUNT; i++) {
        if (!(TAG_LIST[i] & frozen))
            avail[avail_cnt++] = TAG_LIST[i];
    }
    if (avail_cnt == 0) return 0;

    int pick = rand() % 3 + 1;
    if (pick > avail_cnt) pick = avail_cnt;

    uint32_t pool[TAG_COUNT];
    for (int i = 0; i < avail_cnt; i++) pool[i] = avail[i];
    for (int i = 0; i < pick; i++) {
        int j = i + rand() % (avail_cnt - i);
        uint32_t tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
    }

    uint32_t result = 0;
    for (int i = 0; i < pick; i++) result |= pool[i];
    return result;
}

// ── 매물 스폰 루틴 ─────────────────────────────────────────────
static void event_spawn_market(void) {
    if (market_is_full()) return;

    uint32_t tags = event_random_tags();
    if (!tags) return;

    DocFile doc;
    memset(&doc, 0, sizeof(DocFile));
    doc.doc_id     = ++g_doc_id_counter;
    doc.tags       = tags;
    doc.base_price = count_bits(tags) * MARKET_PRICE_PER_TAG + (rand() % 4) * 100;
    make_doc_name(tags, doc.name, sizeof(doc.name));

    if (master_write_doc(&doc) != 0) {
        fprintf(stderr, "[Event] master_write_doc 실패: doc_id=%d\n", doc.doc_id);
        return;
    }
    if (market_add(doc.doc_id, doc.tags, doc.base_price, doc.name) != 0) {
        master_remove_doc(doc.doc_id);
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type                      = PKT_EVT_MARKET_SPAWN;
    pkt.body.market_spawn.doc_id     = doc.doc_id;
    pkt.body.market_spawn.tags       = doc.tags;
    pkt.body.market_spawn.base_price = doc.base_price;
    strncpy(pkt.body.market_spawn.name, doc.name, MAX_NAME_LEN - 1);
    broadcast_packet(&pkt);

    printf("[Event] 매물 스폰: ID=%d tags=0x%03X price=%d \"%s\"\n",
           doc.doc_id, doc.tags, doc.base_price, doc.name);
}

// ── NPC 힌트 → 유예 → 스폰 시퀀스 ─────────────────────────────
static void event_do_npc_sequence(void) {
    if (npc_is_full()) {
        printf("[Event] NPC 보드 가득참 — 스폰 건너뜀\n");
        return;
    }

    uint32_t req_tags = event_random_tags();
    if (!req_tags) return;

    Packet hint;
    memset(&hint, 0, sizeof(Packet));
    hint.type = PKT_EVT_CHAT;
    strncpy(hint.body.chat_evt.sender_key, "[속보]", MAX_KEY_LEN - 1);
    int first_idx = first_tag_index(req_tags);
    strncpy(hint.body.chat_evt.text,
            NPC_HINT[first_idx][rand() % 3], MAX_TEXT_LEN - 1);
    broadcast_packet(&hint);

    sleep(EVT_NPC_HINT_DELAY);

    int32_t npc_id = ++g_npc_id_counter;
    int tagn = count_bits(req_tags);
    int roll = rand() % 5;                  // 변동분 0~4 ($0–2000)
    if (tagn == 1 && roll > 2) roll = 2;    // 1태그는 변동분 상한 제한 → 저난이도 수익 과열 방지
    int32_t bounty = (roll + tagn) * 500;
    if (tagn >= 3) bounty += 1500;   // 3태그 의뢰는 정확히 충족하기 희귀 → 난이도 프리미엄 가산

    if (npc_add(npc_id, req_tags, bounty) != 0) {
        printf("[Event] NPC 보드 가득참 — 스폰 건너뜀\n");
        return;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type                       = PKT_EVT_NPC_SPAWN;
    pkt.body.npc_spawn.npc_id        = npc_id;
    pkt.body.npc_spawn.required_tags = req_tags;
    pkt.body.npc_spawn.bounty        = bounty;
    broadcast_packet(&pkt);

    printf("[Event] NPC 스폰: ID=%d req_tags=0x%03X bounty=%d\n",
           npc_id, req_tags, bounty);
}

// ── 의뢰 에이징 — 오래 미해결 NPC 만료 (GDD 2.B) ──────────────
static void event_age_npcs(void) {
    int32_t expired[MAX_NPC_SLOTS];
    int n = 0;
    npc_despawn_aged(EVT_NPC_MAX_AGE_SEC, EVT_NPC_AGE_STEP_SEC, expired, MAX_NPC_SLOTS, &n);

    for (int i = 0; i < n; i++) {
        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_EVT_NPC_DESPAWN;
        pkt.body.npc_despawn.npc_id = expired[i];
        broadcast_packet(&pkt);
        printf("[Event] NPC 만료(에이징): ID=%d\n", expired[i]);
    }
}

// ── 매물 에이징 — 수명 만료 매물을 익명 구매로 소멸 (보드 적체 방지) ──
static void event_age_market(void) {
    int32_t expired[MAX_MARKET_SLOTS];
    int n = 0;
    market_despawn_aged(expired, MAX_MARKET_SLOTS, &n);

    for (int i = 0; i < n; i++) {
        master_remove_doc(expired[i]);

        Packet pkt;
        memset(&pkt, 0, sizeof(Packet));
        pkt.type = PKT_EVT_MARKET_REMOVE;
        pkt.body.market_remove.doc_id = expired[i];
        broadcast_packet(&pkt);
        printf("[Event] 매물 익명 구매 소멸: ID=%d\n", expired[i]);
    }
}

// ── 공중파 유출 동결/복구 (GDD 2.B) ───────────────────────────
typedef struct {
    uint32_t mask;
    int      duration_sec;
} LeakRecoveryArg;

static void* leak_recovery_thread(void* arg_) {
    LeakRecoveryArg *arg = (LeakRecoveryArg*)arg_;
    sleep(arg->duration_sec);

    unfreeze_tags(arg->mask);

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_EVT_NEWS_RECOVER;
    pkt.body.news_recover.recovered_tags = arg->mask;
    broadcast_packet(&pkt);

    printf("[Event] Public Leak recovered: mask=0x%03X\n", arg->mask);
    free(arg);
    return NULL;
}

int fire_public_leak(void) {
    uint32_t frozen  = market_frozen_mask();
    uint32_t present = market_active_tags();
    int avail_idx[TAG_COUNT];
    int avail_cnt = 0;
    for (int i = 0; i < TAG_COUNT; i++) {
        if ((TAG_LIST[i] & present) && !(TAG_LIST[i] & frozen)) {
            avail_idx[avail_cnt++] = i;
        }
    }
    if (avail_cnt == 0) return 0;

    int pick = avail_idx[rand() % avail_cnt];
    uint32_t mask = TAG_LIST[pick];
    int duration = LEAK_DUR_MIN_SEC + rand() % (LEAK_DUR_MAX_SEC - LEAK_DUR_MIN_SEC + 1);

    freeze_tags(mask);

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.type = PKT_EVT_NEWS_LEAK;
    pkt.body.news_leak.frozen_tags  = mask;
    pkt.body.news_leak.duration_sec = duration;
    strncpy(pkt.body.news_leak.headline, LEAK_HEADLINE[pick][rand() % 3], MAX_TEXT_LEN - 1);
    broadcast_packet(&pkt);

    LeakRecoveryArg *arg = malloc(sizeof(LeakRecoveryArg));
    if (!arg) {
        unfreeze_tags(mask);
        return 0;
    }
    arg->mask = mask;
    arg->duration_sec = duration;

    pthread_t tid;
    if (pthread_create(&tid, NULL, leak_recovery_thread, arg) == 0) {
        pthread_detach(tid);
    } else {
        unfreeze_tags(mask);
        free(arg);
        return 0;
    }

    printf("[Event] Public Leak fired: mask=0x%03X duration=%ds\n", mask, duration);
    return 1;
}

static void event_try_public_leak(void) {
    if (rand() % 100 < LEAK_PROB_PCT) {
        fire_public_leak();
    }
}

// ── 만기 도달 5분 경과 여부 R/W용 안전 헬퍼 ────────────────────────
static int check_cap_grace_expired(void) {
    int expired = 0;
    pthread_mutex_lock(&g_round_mutex);
    if (g_cap_reached_at != 0) {
        time_t elapsed = time(NULL) - g_cap_reached_at;
        if (elapsed >= CAP_GRACE_SEC) {
            expired = 1;
        }
    }
    pthread_mutex_unlock(&g_round_mutex);
    return expired;
}

// ── 이벤트 비동기 백그라운드 구동 코어 스레드 ──────────────────────────
static void* event_thread_func(void* arg) {
    (void)arg;
    unsigned long tick = 0;

    printf("[Event] Spawner & Lore Engine thread running.\n");

    // §D 주기 경찰 레이드의 다음 발사 예약 시각 (이 스레드 단독 사용)
    int periodic_span = PERIODIC_RAID_MAX_INTERVAL - PERIODIC_RAID_MIN_INTERVAL + 1;
    time_t s_next_periodic_raid_at = time(NULL)
                                     + PERIODIC_RAID_MIN_INTERVAL
                                     + rand() % periodic_span;

    while (1) {
        sleep(EVT_TICK_SEC);
        tick++;

        // 1. 만기(DEBT_CAP) 경과 후 5분 기한 검사 (GDD §F)
        if (check_cap_grace_expired()) {
            printf("[Event] DEBT_CAP 도달 후 5분 초과 경과 -> 강제 리셋 트리거 개시\n");
            fire_round_end_countdown(
                "사채업자의 전면 압류 시한(5분)이 만료되었습니다! "
                "60초 뒤 넷 대역이 강제 초기화(리셋)됩니다.");
        }

        // 2. 미해결 NPC 보드 에이징 만료
        event_age_npcs();

        // 3. 미구매 매물 보드 에이징 만료 (익명 구매 소멸)
        event_age_market();

        // 4. §D 주기 경찰 레이드 (6~10분 무작위 간격)
        if (time(NULL) >= s_next_periodic_raid_at) {
            fire_periodic_police_raid();
            s_next_periodic_raid_at = time(NULL)
                                      + PERIODIC_RAID_MIN_INTERVAL
                                      + rand() % periodic_span;
        }

        // 5. 공중파 유출 주기 시도
        if (tick % EVT_LEAK_EVERY == 0) {
            event_try_public_leak();
        }

        // 6. 매물 스폰 시기 검사
        if (tick % EVT_MARKET_EVERY == 0) {
            event_spawn_market();
        }

        // 7. NPC 의뢰 스폰 시퀀스 (별도 유예 지연 때문에 스레드 파생)
        if (tick % EVT_NPC_EVERY == 0) {
            pthread_t npc_tid;
            if (pthread_create(&npc_tid, NULL, (void*(*)(void*))event_do_npc_sequence, NULL) == 0) {
                pthread_detach(npc_tid);
            }
        }

        // 8. 재미있는 더미 피드/광고 뉴스 송출 (8틱 = 40초 주기)
        if (tick % 8 == 0) {
            Packet dummy_pkt;
            memset(&dummy_pkt, 0, sizeof(Packet));
            dummy_pkt.type = PKT_EVT_CHAT;
            strncpy(dummy_pkt.body.chat_evt.sender_key, "[넷-피드]", MAX_KEY_LEN - 1);
            strncpy(dummy_pkt.body.chat_evt.text, DUMMY_NEWS[rand() % DUMMY_NEWS_COUNT], MAX_TEXT_LEN - 1);
            broadcast_packet(&dummy_pkt);
        }
    }
    return NULL;
}

int events_start_thread(void) {
    pthread_t event_tid;
    if (pthread_create(&event_tid, NULL, event_thread_func, NULL) == 0) {
        pthread_detach(event_tid);
        return 0;
    }
    return -1;
}
