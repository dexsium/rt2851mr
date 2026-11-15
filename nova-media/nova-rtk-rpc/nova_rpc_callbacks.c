#include "nova_rpc_callbacks.h"

#include <errno.h>
#include <string.h>

void nova_rpc_callbacks_init(nova_rpc_callback_ctx_t *ctx)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->busy_cond, NULL);
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
    /* ИСПРАВЛЕНО: раньше комментарий здесь утверждал, что гонка с
     * dispatch() исключена просто общим мьютексом — это было верно,
     * пока dispatch() держал мьютекс на всё время вызова handler, но
     * именно это создавало риск deadlock (см. .h). Теперь dispatch()
     * отпускает мьютекс перед вызовом handler и помечает запись
     * busy=1, поэтому здесь мы обязаны ЖДАТЬ, пока выполняющийся
     * прямо сейчас handler этой записи не завершится — иначе
     * стирание entries[handle] (в частности user_data) ПОКА handler
     * его использует — use-after-free. */
    while (ctx->entries[handle].active && ctx->entries[handle].busy)
        pthread_cond_wait(&ctx->busy_cond, &ctx->mutex);
    memset(&ctx->entries[handle], 0, sizeof(ctx->entries[handle]));
    pthread_mutex_unlock(&ctx->mutex);
}

int nova_rpc_callbacks_dispatch(nova_rpc_callback_ctx_t *ctx,
                                 const uint8_t *buf, size_t len)
{
    nova_rpc_struct_t hdr;
    int i, match = -1;
    nova_rpc_handler_fn handler = NULL;
    void *user_data = NULL;
    const uint8_t *payload;
    size_t payload_size;

    if (!ctx || !buf)
        return -EINVAL;

    if (nova_rpc_xdr_decode_struct(buf, len, &hdr) == 0)
        return -EBADMSG;

    pthread_mutex_lock(&ctx->mutex);
    for (i = 0; i < NOVA_RPC_CALLBACKS_MAX_HANDLERS; i++) {
        if (ctx->entries[i].active &&
            ctx->entries[i].program_id == hdr.program_id &&
            ctx->entries[i].version_id == hdr.version_id) {
            /* Копируем всё нужное и помечаем busy, ПОКА держим mutex —
             * сам вызов handler() делаем ПОСЛЕ unlock (см. .h/.c
             * комментарий про исправленный deadlock-риск). */
            handler = ctx->entries[i].handler;
            user_data = ctx->entries[i].user_data;
            ctx->entries[i].busy++;
            match = i;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    if (match < 0)
        return -ENOMSG;

    payload = buf + NOVA_RPC_STRUCT_WIRE_SIZE;
    payload_size = (len > NOVA_RPC_STRUCT_WIRE_SIZE)
                       ? (len - NOVA_RPC_STRUCT_WIRE_SIZE) : 0;
    if (payload_size > hdr.parameter_size)
        payload_size = hdr.parameter_size; /* не читать за заявленный размер */

    /* Вызов происходит БЕЗ удержания ctx->mutex — handler может
     * безопасно сам вызвать dispatch()/register()/unregister() на
     * этом же ctx (рекурсивный dispatch, повторная регистрация из
     * callback — оба явно требуются к проверке ТЗ §8), без deadlock. */
    handler(&hdr, payload, payload_size, user_data);

    pthread_mutex_lock(&ctx->mutex);
    ctx->entries[match].busy--;
    if (ctx->entries[match].busy == 0)
        pthread_cond_broadcast(&ctx->busy_cond);
    pthread_mutex_unlock(&ctx->mutex);

    return 0;
}

void nova_rpc_callbacks_destroy(nova_rpc_callback_ctx_t *ctx)
{
    if (!ctx)
        return;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->busy_cond);
    memset(ctx, 0, sizeof(*ctx));
}
