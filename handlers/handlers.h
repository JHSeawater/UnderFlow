#ifndef HANDLERS_H
#define HANDLERS_H

#include <sys/types.h>
#include "protocol.h"

/*
 * handlers 모듈 — /buy /sell /dispose /inventory /rumor 명령 라우팅.
 *
 * server_main.c가 PKT_REQ_CHAT을 처리한 뒤 나머지는 이 dispatcher로 흘려보낸다.
 *
 *   client_handler() 루프:
 *     packet_recv(sock, &pkt);
 *     if (pkt.type == PKT_REQ_CHAT) { ... }       // 기존 A 영역
 *     else handlers_dispatch(sock, &pkt, my_key, my_offset);  // B 영역
 *
 *   - my_offset : 로그인 시 userdb_find/signup으로 받은 off_t. 매번 find 다시 안 함.
 */
void handlers_dispatch(int sock, Packet *pkt, const char *my_key, off_t my_offset);


/*
 * 게임 종료 트리거 — B의 /sell 핸들러가 승리자를 감지하면 호출.
 * A 영역에서 실제 브로드캐스트(PKT_EVT_VICTORY, PKT_EVT_GAME_OVER)를 구현해야 함.
 *
 * winner_sock : 승리한 클라이언트의 소켓 fd.
 *              나머지 접속자 전원에게는 GAME_OVER 브로드캐스트.
 */
extern void server_trigger_victory(int winner_sock);


/*
 * 패킷 브로드캐스트 — handlers가 매물 제거 / NPC 매각 알림을 전 유저에 푸쉬할 때 사용.
 * 구현은 server_main.c의 broadcast_packet에 이미 존재.
 */
extern void broadcast_packet(Packet *pkt);

#endif
