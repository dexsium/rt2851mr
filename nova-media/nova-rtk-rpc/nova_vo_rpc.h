/*
 * nova_vo_rpc — VO FILTER команды поверх реального транспорта. Тот же
 * программный ID (100, VIDEO_SYSTEM), т.к. VO_FILTER_* процедуры — часть
 * того же сгенерированного VideoRPC_System.h (см. nova_rpc_protocol.h).
 * Та же честная оговорка про отсутствие приёма ответа, что в
 * nova_video_rpc.h.
 */
#ifndef NOVA_VO_RPC_H
#define NOVA_VO_RPC_H

#include "nova_rpc_protocol.h"
#include "nova_rpc_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nova_vo_rpc_session {
    nova_rpc_transport_t transport;
    uint32_t next_task_id;
} nova_vo_rpc_session_t;

int nova_vo_rpc_open(nova_vo_rpc_session_t *s);

/* VIDEO_RPC_VO_FILTER_ToAgent_ConnectVDec (3390). Аргумент — буквально
 * struct VIDEO_RPC_VO_FILTER_ConnectVDec {long instanceID; u_char nPort;},
 * см. предыдущий отчёт. */
int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port);
int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id);     /* 3260 */
int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id); /* 3340 */
int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed); /* 3290 */

void nova_vo_rpc_close(nova_vo_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VO_RPC_H */
