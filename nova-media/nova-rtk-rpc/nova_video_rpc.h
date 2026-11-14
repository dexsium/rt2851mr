/*
 * nova_video_rpc — DEC-команды Video RPC поверх РЕАЛЬНОГО двустороннего
 * транспорта (nova_rpc_transport_call — отправка + приём ответа, см.
 * nova_rpc_transport.h). program_id=100 (CONFIRMED VideoRPC_System.h:22).
 *
 * `last_reply_status`: 0 = ответ реально получен и скопирован в
 * `last_reply_raw`; -ETIMEDOUT = не получен за timeout; других
 * отрицательных errno — ошибка транспорта при отправке. НЕ являются
 * фиктивным успехом — если AVCPU недоступен (нет устройства/нет
 * прошивки), это будет либо ошибка open(), либо честный -ETIMEDOUT.
 */
#ifndef NOVA_VIDEO_RPC_H
#define NOVA_VIDEO_RPC_H

#include "nova_rpc_protocol.h"
#include "nova_rpc_transport.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOVA_VIDEO_RPC_DEFAULT_TIMEOUT_MS 5000

typedef struct nova_video_rpc_session {
    nova_rpc_transport_t transport;
    long instance_id;
    int last_reply_status;
    uint8_t last_reply_raw[64];
    size_t last_reply_len;
} nova_video_rpc_session_t;

int nova_video_rpc_open(nova_video_rpc_session_t *s);

int nova_video_rpc_create(nova_video_rpc_session_t *s);         /* 10 */
int nova_video_rpc_video_create(nova_video_rpc_session_t *s);   /* 100 */
int nova_video_rpc_dec_init(nova_video_rpc_session_t *s);       /* 1020 */
int nova_video_rpc_init_ring_buffer(nova_video_rpc_session_t *s,
                                     uint32_t phys_addr, uint32_t size); /* 30 */
int nova_video_rpc_connect(nova_video_rpc_session_t *s);        /* 20 */
int nova_video_rpc_run(nova_video_rpc_session_t *s);             /* 40 — "Play" */
int nova_video_rpc_pause(nova_video_rpc_session_t *s);           /* 50 */
int nova_video_rpc_stop(nova_video_rpc_session_t *s);             /* 60 */
int nova_video_rpc_flush(nova_video_rpc_session_t *s);             /* 80 */
int nova_video_rpc_destroy(nova_video_rpc_session_t *s);           /* 70 */

void nova_video_rpc_close(nova_video_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VIDEO_RPC_H */
