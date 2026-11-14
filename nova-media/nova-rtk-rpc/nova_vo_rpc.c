#include "nova_vo_rpc.h"

#include <errno.h>
#include <string.h>

static int nova_vo_rpc_send(nova_vo_rpc_session_t *s, uint32_t procedure_id,
                              const uint8_t *args, size_t args_size)
{
    uint32_t task_id;

    if (!s)
        return -EINVAL;

    task_id = ++s->next_task_id;

    return nova_rpc_transport_send(&s->transport,
                                    NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF |
                                    NOVA_RPC_T_SEND_VIDEO_CPU,
                                    NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                    procedure_id, task_id, /*context=*/0,
                                    args, args_size);
}

int nova_vo_rpc_open(nova_vo_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    memset(s, 0, sizeof(*s));
    return nova_rpc_transport_open(&s->transport);
}

int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port)
{
    uint8_t arg[5]; /* long instanceID (4B на ARM32) + u_char nPort — без паддинга в XDR */

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    arg[4] = port;

    return nova_vo_rpc_send(s, NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec, arg, sizeof(arg));
}

int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    return nova_vo_rpc_send(s, NOVA_RPC_VO_FILTER_ToAgent_Display, arg, sizeof(arg));
}

int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    return nova_vo_rpc_send(s, NOVA_RPC_VO_FILTER_ToAgent_ClearVideo, arg, sizeof(arg));
}

int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed)
{
    uint8_t arg[8];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg + 0, (uint32_t)instance_id);
    nova_rpc_xdr_put_u32(arg + 4, (uint32_t)speed);

    return nova_vo_rpc_send(s, NOVA_RPC_VO_FILTER_ToAgent_SetSpeed, arg, sizeof(arg));
}

void nova_vo_rpc_close(nova_vo_rpc_session_t *s)
{
    if (!s)
        return;

    nova_rpc_transport_close(&s->transport);
}
