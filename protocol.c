#include "protocol.h"
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stddef.h>

// 태그 이름 호출 함수
const char* get_tag_name(uint32_t tag_bit) {
    switch(tag_bit) {
        case TAG_CORP_A: return "기업A";
        case TAG_CORP_B: return "기업B";
        case TAG_CORP_C: return "기업C";
        case TAG_CUSTOMER: return "고객정보";
        case TAG_FINANCE: return "금융";
        case TAG_MILITARY: return "군사무기";
        case TAG_GOVERNMENT: return "정부기관";
        case TAG_MEDICAL: return "의료";
        case TAG_RESEARCH: return "연구개발";
        case TAG_PERSONAL: return "사적정보";
        default: return "알수없음";
    }
}

// 엔디안 변환 함수
static void swap_packet_endian(Packet *pkt) {
    switch(pkt->type) {
        case PKT_REQ_BUY:
            pkt->body.buy.doc_id = ntohl(pkt->body.buy.doc_id);
            break;
        case PKT_REQ_SELL:
            pkt->body.sell.npc_id = ntohl(pkt->body.sell.npc_id);
            pkt->body.sell.count  = ntohl(pkt->body.sell.count);
            for(int i = 0; i < MAX_INVEN_SIZE; i++) {
                pkt->body.sell.doc_ids[i] = ntohl(pkt->body.sell.doc_ids[i]);
            }
            break;
        case PKT_REQ_DISPOSE:
            pkt->body.dispose.doc_id = ntohl(pkt->body.dispose.doc_id);
            break;

        case PKT_RES_LOGIN_OK:
            pkt->body.login_ok.assigned_session_id = ntohl(pkt->body.login_ok.assigned_session_id);
            pkt->body.login_ok.money = ntohl(pkt->body.login_ok.money);
            pkt->body.login_ok.goal_money = ntohl(pkt->body.login_ok.goal_money);
            break;
        case PKT_RES_INVEN_INFO:
            pkt->body.inven_info.count = ntohl(pkt->body.inven_info.count);
            pkt->body.inven_info.money = ntohl(pkt->body.inven_info.money);
            for(int i = 0; i < MAX_INVEN_SIZE; i++) {
                pkt->body.inven_info.items[i].doc_id = ntohl(pkt->body.inven_info.items[i].doc_id);
                pkt->body.inven_info.items[i].tags = ntohl(pkt->body.inven_info.items[i].tags);
                pkt->body.inven_info.items[i].is_frozen = ntohl(pkt->body.inven_info.items[i].is_frozen);
            }
            break;
        case PKT_RES_BUY_OK:
            pkt->body.buy_ok.doc_id = ntohl(pkt->body.buy_ok.doc_id);
            pkt->body.buy_ok.remaining_money = ntohl(pkt->body.buy_ok.remaining_money);
            pkt->body.buy_ok.tags = ntohl(pkt->body.buy_ok.tags);
            break;
        case PKT_RES_SELL_OK:
            pkt->body.sell_ok.bounty = ntohl(pkt->body.sell_ok.bounty);
            pkt->body.sell_ok.new_money = ntohl(pkt->body.sell_ok.new_money);
            break;
        case PKT_RES_ERROR:
            pkt->body.error.error_code = ntohl(pkt->body.error.error_code);
            break;

        case PKT_EVT_MARKET_SPAWN:
            pkt->body.market_spawn.doc_id = ntohl(pkt->body.market_spawn.doc_id);
            pkt->body.market_spawn.tags = ntohl(pkt->body.market_spawn.tags);
            pkt->body.market_spawn.base_price = ntohl(pkt->body.market_spawn.base_price);
            break;
        case PKT_EVT_MARKET_REMOVE:
            pkt->body.market_remove.doc_id = ntohl(pkt->body.market_remove.doc_id);
            break;
        case PKT_EVT_NPC_SPAWN:
            pkt->body.npc_spawn.npc_id = ntohl(pkt->body.npc_spawn.npc_id);
            pkt->body.npc_spawn.required_tags = ntohl(pkt->body.npc_spawn.required_tags);
            pkt->body.npc_spawn.bounty = ntohl(pkt->body.npc_spawn.bounty);
            break;
        case PKT_EVT_NPC_DESPAWN:
            pkt->body.npc_despawn.npc_id = ntohl(pkt->body.npc_despawn.npc_id);
            break;
        case PKT_EVT_NEWS_LEAK:
            pkt->body.news_leak.frozen_tags = ntohl(pkt->body.news_leak.frozen_tags);
            pkt->body.news_leak.duration_sec = ntohl(pkt->body.news_leak.duration_sec);
            break;
        case PKT_EVT_NEWS_RECOVER:
            pkt->body.news_recover.recovered_tags = ntohl(pkt->body.news_recover.recovered_tags);
            break;
        case PKT_EVT_POLICE_RAID:
            pkt->body.police_raid.time_limit_sec = ntohl(pkt->body.police_raid.time_limit_sec);
            break;
        default:
            break;
    }
}

// 문자열 강제 오버플로우 방지 (널 터미네이션)
static void enforce_null_termination(Packet *pkt) {
    switch(pkt->type) {
        case PKT_REQ_LOGIN:
            pkt->body.login.key[MAX_KEY_LEN - 1] = '\0';
            break;
        case PKT_REQ_CHAT:
            pkt->body.chat.text[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_REQ_MINIGAME_SUBMIT:
            pkt->body.minigame.passcode[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_REQ_RUMOR:
            pkt->body.rumor.target_key[MAX_KEY_LEN - 1] = '\0';
            break;
        case PKT_RES_BUY_OK:
            pkt->body.buy_ok.name[MAX_NAME_LEN - 1] = '\0';
            break;
        case PKT_RES_ERROR:
            pkt->body.error.reason[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_EVT_CHAT:
            pkt->body.chat_evt.sender_key[MAX_KEY_LEN - 1] = '\0';
            pkt->body.chat_evt.text[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_EVT_MARKET_SPAWN:
            pkt->body.market_spawn.name[MAX_NAME_LEN - 1] = '\0';
            break;
        case PKT_EVT_NEWS_LEAK:
            pkt->body.news_leak.headline[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_EVT_POLICE_RAID:
            pkt->body.police_raid.passcode[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_EVT_GAME_OVER:
        case PKT_EVT_VICTORY:
            pkt->body.endgame.message[MAX_TEXT_LEN - 1] = '\0';
            break;
        case PKT_RES_INVEN_INFO:
            for(int i = 0; i < MAX_INVEN_SIZE; i++) {
                pkt->body.inven_info.items[i].name[MAX_NAME_LEN - 1] = '\0';
            }
            break;
        default:
            break;
    }
}

// 수신 함수
int packet_recv(int sockfd, Packet *pkt) {
    // 수신 전 패킷 구조체를 0으로 꽉 채워 쓰레기값 원천 차단
    memset(pkt, 0, sizeof(Packet)); 
    
    // 구조체 크기만큼 정확히 대기해서 읽음
    int ret = recv(sockfd, pkt, sizeof(Packet), MSG_WAITALL);
    if (ret <= 0) return ret;

    // 헤더 스왑
    pkt->type = ntohl(pkt->type);
    pkt->session_id = ntohl(pkt->session_id);

    // 바디 엔디안 자동 스왑
    swap_packet_endian(pkt);

    // 문자열 오버플로우 강제 방어
    enforce_null_termination(pkt);

    return ret;
}

// 송신 함수
int packet_send(int sockfd, const Packet *pkt) {
    // 송신 패킷은 원본을 훼손하면 안 되므로 복사본 생성
    Packet send_pkt;
    memcpy(&send_pkt, pkt, sizeof(Packet));

    // 보낼 때도 오버플로우 방지 및 남는 공간 쓰레기값 제거 (이미 원본이 0 초기화됐길 기대하지만 이중 방어)
    enforce_null_termination(&send_pkt);

    // 바디 스왑 (htonl이나 ntohl이나 로직은 동일하므로 swap 재활용)
    swap_packet_endian(&send_pkt);
    
    // 헤더 스왑
    send_pkt.type = htonl(send_pkt.type);
    send_pkt.session_id = htonl(send_pkt.session_id);

    return send(sockfd, &send_pkt, sizeof(Packet), 0);
}
