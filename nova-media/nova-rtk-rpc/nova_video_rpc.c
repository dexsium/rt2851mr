#include "nova_video_rpc.h"

#include <errno.h>
#include <string.h>

static int nova_video_rpc_call(nova_video_rpc_session_t *s, uint32_t procedure_id,
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
                                   NOVA_VIDEO_RPC_DEFAULT_TIMEOUT_MS);

    s->last_reply_status = (ret >= 0) ? 0 : ret;
    s->last_reply_len = (ret > 0) ? (size_t)ret : 0;
    return ret;
}

int nova_video_rpc_open(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    memset(s, 0, sizeof(*s));
    s->last_reply_status = -ENOTSUP; /* пока ни один вызов не делался */

    return nova_rpc_transport_open(&s->transport);
}

int nova_video_rpc_create(nova_video_rpc_session_t *s)
{
    int ret = nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Create, NULL, 0);
    if (ret > 0 && s->last_reply_len >= 4)
        s->instance_id = (long)nova_rpc_xdr_get_u32(s->last_reply_raw);
    return ret;
}

int nova_video_rpc_video_create(nova_video_rpc_session_t *s)
{
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_VideoCreate, NULL, 0);
}

int nova_video_rpc_dec_init(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_DEC_Init, arg, sizeof(arg));
}

int nova_video_rpc_init_ring_buffer(nova_video_rpc_session_t *s,
                                     uint32_t phys_addr, uint32_t size)
{
    uint8_t arg[12];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg + 0, (uint32_t)s->instance_id);
    nova_rpc_xdr_put_u32(arg + 4, phys_addr);
    nova_rpc_xdr_put_u32(arg + 8, size);

    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_InitRingBuffer, arg, sizeof(arg));
}

int nova_video_rpc_connect(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Connect, arg, sizeof(arg));
}

int nova_video_rpc_run(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Run, arg, sizeof(arg));
}

int nova_video_rpc_pause(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Pause, arg, sizeof(arg));
}

int nova_video_rpc_stop(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Stop, arg, sizeof(arg));
}

int nova_video_rpc_flush(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Flush, arg, sizeof(arg));
}

int nova_video_rpc_destroy(nova_video_rpc_session_t *s)
{
    uint8_t arg[4];

    if (!s)
        return -EINVAL;

    nova_rpc_xdr_put_u32(arg, (uint32_t)s->instance_id);
    return nova_video_rpc_call(s, NOVA_RPC_VIDEO_ToAgent_Destroy, arg, sizeof(arg));
}

void nova_video_rpc_close(nova_video_rpc_session_t *s)
{
    if (!s)
        return;

    nova_rpc_transport_close(&s->transport);
}
