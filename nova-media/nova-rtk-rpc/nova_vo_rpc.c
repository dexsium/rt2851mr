#include "nova_vo_rpc.h"

#include <errno.h>
#include <string.h>

static int nova_vo_rpc_call(nova_vo_rpc_session_t *s, uint32_t procedure_id,
                              const uint8_t *args, size_t args_size)
{
    int ret;

    if (!s)
        return -EINVAL;

    memset(s->last_reply_raw, 0, sizeof(s->last_reply_raw));
    ret = nova_rpc_transport_call(&s->transport,
                                   NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF |
                                   NOVA_RPC_T_SEND_VIDEO_CPU,
                                   NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                   procedure_id, args, args_size,
                                   s->last_reply_raw, sizeof(s->last_reply_raw),
                                   NOVA_VO_RPC_DEFAULT_TIMEOUT_MS);

    s->last_reply_status = (ret >= 0) ? 0 : ret;
    s->last_reply_len = (ret > 0) ? (size_t)ret : 0;
    return ret;
}

int nova_vo_rpc_open(nova_vo_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    memset(s, 0, sizeof(*s));
    s->last_reply_status = -ENOTSUP;

    return nova_rpc_transport_open(&s->transport);
}

int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port)
{
    uint8_t arg[5];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    arg[4] = port;

    return nova_vo_rpc_call(s, NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec, arg, sizeof(arg));
}

int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    return nova_vo_rpc_call(s, NOVA_RPC_VO_FILTER_ToAgent_Display, arg, sizeof(arg));
}

int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    return nova_vo_rpc_call(s, NOVA_RPC_VO_FILTER_ToAgent_ClearVideo, arg, sizeof(arg));
}

int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed)
{
    uint8_t arg[8];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg + 0, (uint32_t)instance_id);
    nova_rpc_xdr_put_u32(arg + 4, (uint32_t)speed);

    return nova_vo_rpc_call(s, NOVA_RPC_VO_FILTER_ToAgent_SetSpeed, arg, sizeof(arg));
}

int nova_vo_rpc_get_vo_info(nova_vo_rpc_session_t *s, long instance_id)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)instance_id);
    return nova_vo_rpc_call(s, NOVA_RPC_VO_FILTER_ToAgent_GetVoInfo, arg, sizeof(arg));
}

void nova_vo_rpc_close(nova_vo_rpc_session_t *s)
{
    if (!s)
        return;

    nova_rpc_transport_close(&s->transport);
}
