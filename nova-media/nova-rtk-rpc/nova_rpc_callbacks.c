#include "nova_rpc_callbacks.h"

#include <errno.h>
#include <string.h>

void nova_rpc_callbacks_init(nova_rpc_callback_ctx_t *ctx)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->mutex, NULL);
}

int nova_rpc_callbacks_register(nova_rpc_callback_ctx_t *ctx,
                                 uint32_t program_id, uint32_t version_id,
                                 nova_rpc_handler_fn handler, void *user_data)
{
    int i;

    if (!ctx || !handler)
        return -EINVAL;

    pthread_mutex_lock(&ctx->mutex);
    for (i = 0; i < NOVA_RPC_CALLBACKS_MAX_HANDLERS; i++) {
        if (!ctx->entries[i].active) {
            ctx->entries[i].program_id = program_id;
            ctx->entries[i].version_id = version_id;
            ctx->entries[i].handler = handler;
            ctx->entries[i].user_data = user_data;
            ctx->entries[i].active = 1;
            pthread_mutex_unlock(&ctx->mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    return -ENOSPC;
}

void nova_rpc_callbacks_unregister(nova_rpc_callback_ctx_t *ctx, int handle)
{
    if (!ctx || handle < 0 || handle >= NOVA_RPC_CALLBACKS_MAX_HANDLERS)
        return;

    pthread_mutex_lock(&ctx->mutex);
    /* Держим мьютекс на всё время очистки записи — dispatch() берёт тот
     * же мьютекс перед обходом таблицы, поэтому гонка "dispatch уже
     * выбрал handler, пока мы его стираем" исключена: либо dispatch
     * видит запись целиком активной (и безопасно вызывает handler, чей
     * user_data тогда ещё не освобождён вызывающим кодом по контракту
     * "unregister только после того, как handler больше не нужен"),
     * либо видит её уже неактивной. */
    memset(&ctx->entries[handle], 0, sizeof(ctx->entries[handle]));
    pthread_mutex_unlock(&ctx->mutex);
}

int nova_rpc_callbacks_dispatch(nova_rpc_callback_ctx_t *ctx,
                                 const uint8_t *buf, size_t len)
{
    nova_rpc_struct_t hdr;
    int i;
    int delivered = 0;

    if (!ctx || !buf)
        return -EINVAL;

    if (nova_rpc_xdr_decode_struct(buf, len, &hdr) == 0)
        return -EBADMSG;

    pthread_mutex_lock(&ctx->mutex);
    for (i = 0; i < NOVA_RPC_CALLBACKS_MAX_HANDLERS; i++) {
        if (ctx->entries[i].active &&
            ctx->entries[i].program_id == hdr.program_id &&
            ctx->entries[i].version_id == hdr.version_id) {
            const uint8_t *payload = buf + NOVA_RPC_STRUCT_WIRE_SIZE;
            size_t payload_size = (len > NOVA_RPC_STRUCT_WIRE_SIZE)
                                       ? (len - NOVA_RPC_STRUCT_WIRE_SIZE) : 0;

            if (payload_size > hdr.parameter_size)
                payload_size = hdr.parameter_size; /* не читать за заявленный размер */

            ctx->entries[i].handler(&hdr, payload, payload_size,
                                     ctx->entries[i].user_data);
            delivered = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    return delivered ? 0 : -ENOMSG;
}

void nova_rpc_callbacks_destroy(nova_rpc_callback_ctx_t *ctx)
{
    if (!ctx)
        return;

    pthread_mutex_destroy(&ctx->mutex);
    memset(ctx, 0, sizeof(*ctx));
}
