/*
 * nova_rpc_callbacks — безопасный диспетчер входящих RPC-сообщений
 * (callback'и AVCPU→ARM и ответы на наши вызовы), спроектированный по
 * образцу подтверждённой архитектуры `DispatchRPCRequest()`/`RPCProxy_loop()`
 * (`kernel/drivers/rtk_kdriver/common/IPC/src/RPCProxy.c:33-110`):
 *
 * - `DispatchRPCRequest()` декодирует заголовок `RPC_STRUCT`, затем
 *   обходит linked-list регистраций `{programID, versionID, handler}` и
 *   вызывает совпавший `handler` (RPCProxy.c:33-47).
 * - `RPCProxy_loop()` — отдельный поток на каждый CPU-target, читает
 *   заголовок через `readRingBuf()`, затем зовёт `DispatchRPCRequest()`
 *   (RPCProxy.c:58-110).
 *
 * ЧЕСТНАЯ ОГОВОРКА: сам `readRingBuf()` (как именно поток реально читает
 * входящие байты — какое устройство, `read()` или что-то ещё) не был
 * найден построчно в этой сессии (его тело не процитировано — только
 * прототип в rpcapi.h). Поэтому этот диспетчер принимает готовый буфер
 * байт извне (`nova_rpc_callbacks_dispatch(ctx, buf, len)`), а не сам
 * решает, откуда его читать — честно оставляет это вызывающему коду
 * (production-реализация должна будет открыть нужное устройство и
 * передавать сюда прочитанные байты, когда это будет восстановлено).
 *
 * Требование §7 ТЗ "callback не должен обращаться к уже освобождённой
 * сессии": каждая регистрация хранит opaque `user_data`, и отзыв
 * регистрации (`nova_rpc_callbacks_unregister`) синхронизирован мьютексом
 * с диспетчером — после `unregister()` диспетчер гарантированно не
 * вызовет этот handler ни для одного уже стоящего в очереди сообщения
 * (использует тот же мьютекс, что и в оригинале, `pMutex`, см.
 * `RPCProxy_loop()` — `pthread_mutex_lock(pData->pMutex)`).
 *
 * Статус: IMPLEMENTED, COMPILED, MOCK_TESTED (см. tests/test_rpc_callbacks.c).
 */
#ifndef NOVA_RPC_CALLBACKS_H
#define NOVA_RPC_CALLBACKS_H

#include "nova_rpc_xdr.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOVA_RPC_CALLBACKS_MAX_HANDLERS 16

typedef void (*nova_rpc_handler_fn)(const nova_rpc_struct_t *hdr,
                                     const uint8_t *payload, size_t payload_size,
                                     void *user_data);

typedef struct nova_rpc_callback_entry {
    uint32_t program_id;
    uint32_t version_id;
    nova_rpc_handler_fn handler;
    void *user_data;
    int active;
} nova_rpc_callback_entry_t;

typedef struct nova_rpc_callback_ctx {
    nova_rpc_callback_entry_t entries[NOVA_RPC_CALLBACKS_MAX_HANDLERS];
    pthread_mutex_t mutex;
} nova_rpc_callback_ctx_t;

void nova_rpc_callbacks_init(nova_rpc_callback_ctx_t *ctx);

/* Возвращает дескриптор регистрации (>=0) или -ENOSPC, если таблица
 * заполнена (NOVA_RPC_CALLBACKS_MAX_HANDLERS — честный, документированный
 * лимит, а не "неограниченный список" как в оригинальной linked-list —
 * прямое требование ТЗ "ограничение размеров"/предсказуемая память). */
int nova_rpc_callbacks_register(nova_rpc_callback_ctx_t *ctx,
                                 uint32_t program_id, uint32_t version_id,
                                 nova_rpc_handler_fn handler, void *user_data);

/* После возврата из этой функции диспетчер гарантированно не вызовет
 * обработчик с этим дескриптором ни для одного последующего сообщения —
 * синхронизировано тем же мьютексом, что держит dispatch(). */
void nova_rpc_callbacks_unregister(nova_rpc_callback_ctx_t *ctx, int handle);

/*
 * Разбирает заголовок RPC_STRUCT из buf, ищет совпадающую регистрацию по
 * (programID, versionID) и вызывает её handler с оставшимся payload.
 * Если buf короче заголовка, или совпадение не найдено — возвращает
 * отрицательный код ошибки, НЕ вызывает ничего (прямое требование ТЗ
 * "как обрабатываются неизвестные callback" — безопасно игнорируются с
 * диагностикой, не падение).
 *
 * Возвращает 0 при успешной доставке, -ENOMSG если ни один обработчик не
 * совпал, -EBADMSG если buf короче NOVA_RPC_STRUCT_WIRE_SIZE.
 */
int nova_rpc_callbacks_dispatch(nova_rpc_callback_ctx_t *ctx,
                                 const uint8_t *buf, size_t len);

void nova_rpc_callbacks_destroy(nova_rpc_callback_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_CALLBACKS_H */
