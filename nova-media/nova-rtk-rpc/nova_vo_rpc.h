#ifndef NOVA_VO_RPC_H
#define NOVA_VO_RPC_H

#include "nova_rpc_protocol.h"
#include "nova_rpc_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOVA_VO_RPC_DEFAULT_TIMEOUT_MS 5000

typedef struct nova_vo_rpc_session {
    nova_rpc_transport_t transport;
    int last_reply_status;
    uint8_t last_reply_raw[64];
    size_t last_reply_len;
} nova_vo_rpc_session_t;

int nova_vo_rpc_open(nova_vo_rpc_session_t *s);

int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port); /* 3390 */
int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id);     /* 3260 */
int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id); /* 3340 */
int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed); /* 3290 */
int nova_vo_rpc_get_vo_info(nova_vo_rpc_session_t *s, long instance_id); /* 3940 */

void nova_vo_rpc_close(nova_vo_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VO_RPC_H */
