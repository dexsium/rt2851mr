/*
 * nova_rpc_transport — реальный, двусторонний транспорт Realtek Video RPC.
 *
 * Reply path (CONFIRMED буквально, новое в этой сессии):
 *   `kernel/drivers/rtk_kdriver/common/IPC/src/SVCFunc.c:58-93` (`Reply_Handler`):
 *     1. читает `rpc->parameterSize` байт из ring buffer (`readRingBuf`);
 *     2. первые 4 байта — `u_long result_taskID` (XDR-декодированы);
 *     3. ОСТАВШИЕСЯ байты **копируются напрямую** в `(char *)rpc->context`
 *        (`memcpy((char *)rpc->context, buf, parameterSize - 4)`) —
 *        `context` в оригинале буквально используется как адрес памяти
 *        вызывающего процесса, не просто как "cookie";
 *     4. `ackTask(result_taskID)` = `tkill(taskid, SIGRPC)`
 *        (`rpcapi.c:283-287`) — пробуждает ИМЕННО тот поток.
 *   `ReplyHandler_register()` (`SVCFunc.c:159-163`) регистрирует этот
 *   обработчик для `programID=REPLYID=99, versionID=REPLYID=99`.
 *
 * ВАЖНОЕ ОТЛИЧИЕ Nova ОТ ОРИГИНАЛА (документировано, не скрыто): в
 * оригинале `context` — это НЕПРОВЕРЯЕМЫЙ raw-указатель: если ответ
 * придёт после тайм-аута (буфер вызывающего уже мог быть
 * освобождён/переиспользован), `Reply_Handler` всё равно сделает
 * `memcpy` в уже недействительный адрес — use-after-free в самом
 * протоколе Realtek, не найдено никакой защиты от этого в прочитанном
 * коде. Nova НЕ повторяет эту уязвимость: `context` здесь — индекс в
 * таблице `nova_rpc_pending_t` (не сырой указатель), и каждая запись
 * имеет явное состояние (`PENDING/COMPLETED/EXPIRED`) — поздний или
 * повторный ответ на уже истёкший/завершённый запрос безопасно
 * отбрасывается (см. `nova_rpc_transport_dispatch_reply()`), а не
 * разрушает память. Это прямое выполнение требования ТЗ §8 "защита от
 * use-after-free"/"защита от повторного применения устаревшего
 * контекста" — ценой намеренного, задокументированного отклонения от
 * побитового повторения оригинальной (уязвимой) реализации context.
 * Формат заголовка на проводе (28 байт, порядок полей) НЕ изменён —
 * отклонение только в том, ЧТО значит 32-битное значение в поле
 * `context` для НАШЕГО собственного процесса, а не в wire-формате.
 */
#ifndef NOVA_RPC_TRANSPORT_H
#define NOVA_RPC_TRANSPORT_H

#include "nova_rpc_xdr.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOVA_RPC_T_NONBLOCK_MODE   0x00
#define NOVA_RPC_T_BLOCK_MODE      0x01
#define NOVA_RPC_T_USE_POLL_BUF    0x00
#define NOVA_RPC_T_USE_INTR_BUF    0x02
#define NOVA_RPC_T_SEND_VIDEO_CPU   0x00
#define NOVA_RPC_T_SEND_AUDIO_CPU   0x04
#define NOVA_RPC_T_SEND_VIDEO_CPU2  0x08

#define NOVA_RPC_T_MAX_PENDING 64
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <assert.h>
_Static_assert(NOVA_RPC_T_MAX_PENDING == 64,
               "NOVA_RPC_T_MAX_PENDING must stay 64 to fit the 6-bit slot index packed into context");
#endif

typedef enum nova_rpc_pending_state {
    NOVA_RPC_PENDING_FREE = 0,
    NOVA_RPC_PENDING_WAITING,
    NOVA_RPC_PENDING_COMPLETED,
    /* Больше не используется текущей реализацией (см. .c): после
     * добавления generation-защиты от ABA тайм-аут переводит слот прямо
     * в FREE, не в EXPIRED — сохранено в enum только для обратной
     * совместимости кода/тестов, которые могли ссылаться на это имя. */
    NOVA_RPC_PENDING_EXPIRED
} nova_rpc_pending_state_t;

typedef struct nova_rpc_pending {
    uint32_t task_id;
    uint8_t *result_buf;
    size_t result_cap;
    size_t result_len;
    nova_rpc_pending_state_t state;
    /* Защита от ABA (найдена и устранена в этом этапе ТЗ, раздел
     * "Concurrency Audit" итогового отчёта): если бы тайм-аут сразу
     * освобождал слот (FREE) для переиспользования, опоздавший ответ на
     * СТАРЫЙ запрос мог бы по тому же индексу слота попасть НОВОМУ
     * запросу, который уже успел переиспользовать этот индекс —
     * повреждение результата чужого вызова. `generation` увеличивается
     * при каждом новом выделении слота; `context` на проводе кодирует
     * ОБА значения (индекс + generation), и dispatch_reply сверяет оба,
     * не только индекс. */
    uint32_t generation;
} nova_rpc_pending_t;

/* context = (generation << NOVA_RPC_T_SLOT_BITS) | slot_index.
 * NOVA_RPC_T_MAX_PENDING=64=2^6, поэтому 6 бит индекса достаточно;
 * оставшиеся 26 бит — generation (переполнение раз в 2^26 выделений
 * одного слота — не достижимо в пределах теста или разумной работы
 * процесса). */
#define NOVA_RPC_T_SLOT_BITS 6
#define NOVA_RPC_T_SLOT_MASK ((1u << NOVA_RPC_T_SLOT_BITS) - 1)

typedef struct nova_rpc_transport {
    /* write-side (отправка), CONFIRMED rpcstubapi.c:76-85 */
    int fd_poll_video, fd_intr_video;
    int fd_poll_audio, fd_intr_audio;
    int fd_poll_video2, fd_intr_video2;
    /* read-side (приём/callback), CONFIRMED rpcapi.c:34-45 */
    int rfd_poll_video, rfd_intr_video;
    int rfd_poll_audio, rfd_intr_audio;
    int rfd_poll_video2, rfd_intr_video2;

    int opened;

    nova_rpc_pending_t pending[NOVA_RPC_T_MAX_PENDING];
    pthread_mutex_t pending_mutex;
    /* ВАЖНО (найдено ThreadSanitizer, не гипотетически): условная
     * переменная ожидания ответа ОБЯЗАНА использовать ТОТ ЖЕ мьютекс, что
     * защищает проверяемый предикат (`pending[i].state`), иначе чтение
     * `state` в цикле ожидания происходит без удержания `pending_mutex` —
     * настоящая гонка данных (TSan: `nova_rpc_transport.c:244` читает
     * `state` без `pending_mutex`, пока `dispatch_reply()` пишет его под
     * `pending_mutex`, CI run 43648070639). Раньше здесь использовался
     * общий ГЛОБАЛЬНЫЙ `pthread_cond_t` с ОТДЕЛЬНЫМ глобальным мьютексом
     * — исправлено: теперь cond var принадлежит самому транспорту и
     * всегда используется вместе именно с `pending_mutex`. */
    pthread_cond_t pending_cond;
} nova_rpc_transport_t;

/* Позволяет override путей device-нод для mock/e2e-тестов (CI без
 * реального /dev/rpcN) — если переменная окружения не задана, используется
 * подтверждённый реальный путь без изменений. Не влияет на поведение на
 * реальном устройстве. */
int nova_rpc_transport_open(nova_rpc_transport_t *t);

/* registerRPC(programID) — CONFIRMED rpcapi.c:162-188: ioctl(fd,
 * RPC_IOCTHANDLER, programID) на ВСЕХ открытых read-fd (не на одном
 * общем /dev/rpc100, как предполагалось в более ранней версии этой
 * библиотеки — это явное исправление по итогам построчного чтения
 * rpcapi.c в этой сессии). */
int nova_rpc_transport_register(nova_rpc_transport_t *t, uint32_t program_id);

/*
 * Отправляет RPC-вызов и, если NOVA_RPC_T_BLOCK_MODE, блокирует вызывающий
 * поток до ответа (или таймаута). `result_buf`/`result_cap` — куда
 * скопировать тело ответа (НЕ включая 4-байтовый taskID — он снимается
 * этим слоем). Возвращает:
 *   >=0            — число байт реального ответа, скопированных в result_buf
 *   -ETIMEDOUT     — ответ не пришёл за timeout_ms
 *   -errno         — ошибка транспорта (open/write)
 * Если `opt` не содержит BLOCK_MODE, result_buf/result_cap игнорируются,
 * функция возвращает >=0 сразу после успешной записи (честно — не ответ).
 */
int nova_rpc_transport_call(nova_rpc_transport_t *t, int opt,
                             uint32_t program_id, uint32_t version_id,
                             uint32_t procedure_id,
                             const uint8_t *args, size_t args_size,
                             uint8_t *result_buf, size_t result_cap,
                             int timeout_ms);

/*
 * Вызывается читающим потоком (см. tests/test_rpc_e2e.c за примером) на
 * каждое сообщение, прочитанное с read-fd. Если заголовок — REPLYID/REPLYID
 * (99/99), ищет pending-запрос по индексу в `context`, безопасно
 * копирует результат (или отбрасывает, если запрос уже
 * COMPLETED/EXPIRED/неизвестен — защита от повторного/запоздавшего
 * ответа) и поднимает сигнал ожидающему потоку. Возвращает 0, если
 * сообщение было обработано как ответ, -ENOMSG если это не REPLYID
 * (вызывающий код должен передать его в nova_rpc_callbacks вместо этого).
 */
int nova_rpc_transport_dispatch_reply(nova_rpc_transport_t *t,
                                       const uint8_t *buf, size_t len);

int nova_rpc_transport_signal_number(void);

void nova_rpc_transport_close(nova_rpc_transport_t *t);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_TRANSPORT_H */
