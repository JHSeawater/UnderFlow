#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_TEXT_LEN 256
#define MAX_KEY_LEN 32
#define MAX_INVEN_SIZE 5 // GDD 2-C: 인벤토리 최대 한도

// 1. 패킷 타입 정의 (어떤 목적의 데이터인지)
typedef enum {
    // 클라이언트 -> 서버 요청 (Request)
    PKT_REQ_LOGIN = 100,
    PKT_REQ_CHAT,
    PKT_REQ_BUY,
    PKT_REQ_SELL,
    PKT_REQ_DISPOSE,
    PKT_REQ_INVEN,
    PKT_REQ_RUMOR,
    PKT_REQ_MINIGAME_SUBMIT, // 경찰 레이드 암호 제출

    // 서버 -> 클라이언트 응답 (Response)
    PKT_RES_LOGIN_SUCCESS = 200,
    PKT_RES_LOGIN_FAIL,
    PKT_RES_INVEN_INFO,      // 인벤토리 조회 결과
    PKT_RES_ERROR,           // 범용 에러 메시지 (돈 부족, 없는 아이템 등)

    // 서버 -> 클라이언트 브로드캐스트 이벤트 (Event)
    PKT_EVT_CHAT = 300,      // 다른 사람의 채팅 수신
    PKT_EVT_MARKET_SPAWN,    // 새로운 기밀 문서 상점 스폰
    PKT_EVT_NPC_SPAWN,       // 새로운 NPC 의뢰 스폰
    PKT_EVT_NEWS_LEAK,       // 공중파 유출 (속보 및 동결)
    PKT_EVT_RUMOR_GLITCH,    // 누군가 사보타주 발동 (화면 깨짐 연출 지시)
    PKT_EVT_POLICE_RAID,     // 경찰 레이드 발동 (미니게임 시작 지시)
    PKT_EVT_GAME_OVER,       // 게임 오버 / 승리 처리
} PacketType;

// 2. 단일 통합 패킷 구조체
// 모든 통신은 sizeof(Packet) 크기만큼 고정으로 주고받습니다.
#pragma pack(push, 1) // 패킷 크기를 컴파일러가 임의로 조정하지 못하게 1바이트 단위로 압축
typedef struct {
    // 정수형 데이터 (반드시 전송 전 htonl(), 수신 후 ntohl() 거쳐야 함)
    int32_t type;              // PacketType 열거형 값 (엔디안 변환 필수)
    int32_t user_id;           // 송신자 또는 수신자의 식별 번호
    
    // GDD 2-A: 다중 속성 문서 조합 판매를 위한 배열 처리
    int32_t item_count;               // 패킷에 포함된 아이템의 개수
    int32_t item_ids[MAX_INVEN_SIZE]; // 거래/조회하려는 기밀 문서의 고유 ID 배열
    
    int32_t target_npc_id;     // /sell 시 타겟이 되는 NPC ID
    int32_t value;             // 가격, 자본금, 혹은 동결 타이머 시간 등 숫자 데이터
    
    // 문자열 데이터 (엔디안 변환 필요 없음)
    char user_key[MAX_KEY_LEN]; // 접속 시 사용하는 Key
    char tags[MAX_TEXT_LEN];    // 아이템의 속성 태그 문자열 (예: "[군사]+[보안]")
    char text[MAX_TEXT_LEN];    // 채팅 내용, 속보 메시지, 에러 사유 등
} Packet;
#pragma pack(pop) // 압축 해제

#endif // PROTOCOL_H
