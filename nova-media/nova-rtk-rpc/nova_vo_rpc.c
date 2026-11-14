/* Реализация nova_vo_rpc.h. Та же честная оговорка, что в nova_video_rpc.c:
 * отправка самого RPC-вызова (не только его ID/структуры) — NOT IMPLEMENTED
 * до восстановления формата заголовка сообщения. */
#include "nova_vo_rpc.h"

#include <errno.h>
#include <string.h>

static int nova_vo_rpc_call_TODO(nova_rpc_handle_t *rpc, int procedure_id,
                                  const void *arg, unsigned int arg_size)
{
    (void)rpc; (void)procedure_id; (void)arg; (void)arg_size;
    return -ENOSYS;
}

int nova_vo_rpc_open(nova_vo_rpc_session_t *s)
{
    int ret;

    if (!s)
        return -EINVAL;

    memset(s, 0, sizeof(*s));

    ret = nova_rpc_open(&s->rpc);
    if (ret != 0)
        return ret;

    return nova_rpc_register(&s->rpc, NOVA_RPC_PROGRAM_RANGE_VIDEO_BASE);
}

int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port)
{
    /* Буквально struct VIDEO_RPC_VO_FILTER_ConnectVDec { long instanceID; u_char nPort; }; */
    struct { long instance_id; unsigned char n_port; } arg;

    if (!s)
        return -EINVAL;

    arg.instance_id = instance_id;
    arg.n_port = port;

    return nova_vo_rpc_call_TODO(&s->rpc, NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec,
                                  &arg, sizeof(arg));
}

int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id)
{
    if (!s)
        return -EINVAL;

    return nova_vo_rpc_call_TODO(&s->rpc, NOVA_RPC_VO_FILTER_ToAgent_Display,
                                  &instance_id, sizeof(instance_id));
}

int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id)
{
    if (!s)
        return -EINVAL;

    return nova_vo_rpc_call_TODO(&s->rpc, NOVA_RPC_VO_FILTER_ToAgent_ClearVideo,
                                  &instance_id, sizeof(instance_id));
}

int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed)
{
    struct { long instance_id; int speed; } arg;

    if (!s)
        return -EINVAL;

    arg.instance_id = instance_id;
    arg.speed = speed;

    return nova_vo_rpc_call_TODO(&s->rpc, NOVA_RPC_VO_FILTER_ToAgent_SetSpeed,
                                  &arg, sizeof(arg));
}

void nova_vo_rpc_close(nova_vo_rpc_session_t *s)
{
    if (!s)
        return;

    nova_rpc_close(&s->rpc);
}
