/*
 * Реализация nova_video_rpc.h.
 *
 * ЧЕСТНАЯ ОГОВОРКА (важно, не прятать): rpcapi.h (откуда взят nova_rpc.h)
 * даёт только registerRPC()/stopRPC()/readRingBuf() — приём ответов через
 * общий кольцевой буфер. Он НЕ даёт отдельной "send RPC call(programID,
 * procedureID, args)" функции для userspace. Кернел-сторона
 * (`send_rpc_command()` в rtk_vdec_driver.c) — это функция ЯДРА, не
 * экспортируемая в userspace напрямую. Поэтому точный формат пакета,
 * который userspace должен записать в /dev/rpc100, чтобы он дошёл до
 * AVCPU как конкретный procedureID с конкретными аргументами, **не был
 * восстановлен построчно в этой сессии** — у нас есть только XDR-функции
 * (`xdr_VIDEO_RPC_...`, см. generate/src/.../*_xdr.c), которые умеют
 * СЕРИАЛИЗОВАТЬ структуру в байты, но не сам протокол кадра сообщения
 * (заголовок с programID/procedureID/длиной, куда именно его писать).
 *
 * Поэтому ниже КАЖДАЯ функция честно реализована как обёртка вокруг
 * nova_rpc_call_TODO() — единственной точки, которую нужно дописать,
 * когда формат заголовка сообщения будет восстановлен (см. "следующие
 * шаги" в основном отчёте). Это НЕ эмуляция и не притворство, что
 * команда отправлена — функция явно возвращает -ENOSYS, пока не
 * реализована по-настоящему.
 *
 * Статус: IMPLEMENTED (структура кода, ID, аргументы), NOT IMPLEMENTED
 * (сама отправка), NOT TESTED.
 */
#include "nova_video_rpc.h"

#include <errno.h>
#include <string.h>

/* Единственное место, которое нужно дописать настоящей реализацией после
 * восстановления формата заголовка RPC-сообщения userspace->AVCPU (см.
 * комментарий выше и §17 основного отчёта). */
static int nova_rpc_call_TODO(nova_rpc_handle_t *rpc, int procedure_id,
                               const void *arg, unsigned int arg_size,
                               void *reply, unsigned int reply_size)
{
    (void)rpc; (void)procedure_id; (void)arg; (void)arg_size;
    (void)reply; (void)reply_size;
    return -ENOSYS;
}

int nova_video_rpc_open(nova_video_rpc_session_t *s)
{
    int ret;

    if (!s)
        return -EINVAL;

    memset(s, 0, sizeof(*s));

    ret = nova_rpc_open(&s->rpc);
    if (ret != 0)
        return ret;

    /* Диапазон Video team, см. common/IPC/readme.txt и
     * NOVA_RPC_PROGRAM_RANGE_VIDEO_BASE в nova_rpc.h. */
    ret = nova_rpc_register(&s->rpc, NOVA_RPC_PROGRAM_RANGE_VIDEO_BASE);
    if (ret != 0) {
        nova_rpc_close(&s->rpc);
        return ret;
    }

    return 0;
}

int nova_video_rpc_create(nova_video_rpc_session_t *s)
{
    long reply_instance_id = 0;
    int ret;

    if (!s)
        return -EINVAL;

    ret = nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Create,
                              NULL, 0, &reply_instance_id, sizeof(reply_instance_id));
    if (ret != 0)
        return ret;

    s->instance_id = reply_instance_id;
    return 0;
}

int nova_video_rpc_dec_init(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_DEC_Init,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_init_ring_buffer(nova_video_rpc_session_t *s,
                                     uint32_t phys_addr, uint32_t size)
{
    struct { long instance_id; uint32_t phys_addr; uint32_t size; } arg;

    if (!s)
        return -EINVAL;

    arg.instance_id = s->instance_id;
    arg.phys_addr = phys_addr;
    arg.size = size;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_InitRingBuffer,
                               &arg, sizeof(arg), NULL, 0);
}

int nova_video_rpc_connect(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Connect,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_run(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Run,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_pause(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Pause,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_stop(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Stop,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_flush(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Flush,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

int nova_video_rpc_destroy(nova_video_rpc_session_t *s)
{
    if (!s)
        return -EINVAL;

    return nova_rpc_call_TODO(&s->rpc, NOVA_RPC_VIDEO_ToAgent_Destroy,
                               &s->instance_id, sizeof(s->instance_id), NULL, 0);
}

void nova_video_rpc_close(nova_video_rpc_session_t *s)
{
    if (!s)
        return;

    nova_rpc_close(&s->rpc);
}
