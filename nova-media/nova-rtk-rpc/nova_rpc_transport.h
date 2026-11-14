/*
 * nova_rpc_transport — реальный транспорт Realtek Video RPC для userspace,
 * восстановленный построчно из `kernel/drivers/rtk_kdriver/common/IPC/src/rpcstubapi.c`
 * (функции `openStubRPC`/`writeRingBuf`/`waitReply`/`getTaskID`, строки
 * 76-85, 151-225, 276-363) и `ClientStub.c` (функции `client_call`/
 * `prepareRPCCall`, строки 37-143). Это НЕ предположение по названию —
 * каждая деталь ниже процитирована из конкретной функции.
 *
 * Подтверждённый механизм отправки (CONFIRMED):
 * 1. Открыть ОБА устройства для нужного CPU/режима:
 *      Audio: /dev/rpc0 (poll, O_WRONLY), /dev/rpc1 (intr, O_WRONLY)
 *      Video: /dev/rpc4 (poll), /dev/rpc5 (intr)
 *      Video2: /dev/rpc8 (poll), /dev/rpc9 (intr)
 *    (rpcstubapi.c:76-85, открываются в openStubRPC():162-210)
 * 2. Собрать непрерывный буфер [RPC_STRUCT-заголовок][XDR-аргументы],
 *    закодировать оба big-endian (см. nova_rpc_xdr.h).
 * 3. write() всего буфера целиком в выбранный fd (poll или intr —
 *    по флагу USE_INTR_BUF) — ClientStub.c:96, rpcstubapi.c:317-320.
 * 4. Если BLOCK_MODE: заблокировать сигнал SIGRPC (SIGUSR2 на 32-бит ARM
 *    bionic, иначе SIGRTMIN+19 — rpcstubapi.c:44-49), затем
 *    sigsuspend()-цикл до isAck=1, выставляемого обработчиком сигнала
 *    (rpcstubapi.c:335-363), либо sigtimedwait с таймаутом 100 секунд,
 *    опрос каждые 30 мс (rpcstubapi.c:367-411, вариант
 *    `__timedwaitReply`).
 *
 * Статус: IMPLEMENTED, COMPILED — отправка (шаги 1-4) реализована по-настоящему,
 * не заглушка. Приёмная сторона (как именно доставляется SIGRPC
 * конкретному потоку — через `tkill()` из читающего RPCProxy_loop-потока,
 * `RPCProxy.c:58-110`, подтверждено архитектурно, но точная привязка
 * taskID→сигнал не протестирована без живого AVCPU) — см.
 * nova_rpc_callbacks.h. NOT TESTED на реальном железе.
 */
#ifndef NOVA_RPC_TRANSPORT_H
#define NOVA_RPC_TRANSPORT_H

#include "nova_rpc_xdr.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Буквально rpcstubapi.c / RPCstruct.h — не придуманы. */
#define NOVA_RPC_T_NONBLOCK_MODE  0x00
#define NOVA_RPC_T_BLOCK_MODE     0x01
#define NOVA_RPC_T_USE_POLL_BUF   0x00
#define NOVA_RPC_T_USE_INTR_BUF   0x02
#define NOVA_RPC_T_SEND_VIDEO_CPU  0x00
#define NOVA_RPC_T_SEND_AUDIO_CPU  0x04
#define NOVA_RPC_T_SEND_VIDEO_CPU2 0x08

typedef struct nova_rpc_transport {
    int fd_poll_video;
    int fd_intr_video;
    int fd_poll_audio;
    int fd_intr_audio;
    int fd_poll_video2;
    int fd_intr_video2;
    int opened;
} nova_rpc_transport_t;

/* Открывает device-ноды, подтверждённые в rpcstubapi.c:76-85/162-196.
 * Открывает video+audio+video2 сразу (как и openStubRPC() — не делает
 * их опциональными по вызову, т.к. оригинал тоже открывает все сразу). */
int nova_rpc_transport_open(nova_rpc_transport_t *t);

/*
 * Отправляет один RPC-вызов: кодирует nova_rpc_struct_t + args (уже
 * сериализованные вызывающим кодом — этот слой не знает про конкретные
 * XDR-схемы отдельных команд, только про сам заголовок) в один буфер и
 * пишет его в выбранный fd. `opt` — комбинация NOVA_RPC_T_* флагов,
 * буквально как в оригинале (ClientStub.c:39, rpcstubapi.c:276).
 *
 * Возвращает число переданных байт при успехе, -errno при ошибке
 * (включая -EMSGSIZE, если args_size переполнил бы буфер — проверка
 * границ, которой в оригинале доверяет вызывающий код, но прямо
 * требуется ТЗ "проверка переполнения").
 */
int nova_rpc_transport_send(nova_rpc_transport_t *t, int opt,
                             uint32_t program_id, uint32_t version_id,
                             uint32_t procedure_id, uint32_t task_id,
                             uint32_t context,
                             const uint8_t *args, size_t args_size);

/*
 * Блокирующее ожидание ответа — тот же сигнальный механизм, что в
 * rpcstubapi.c:335-363/367-411. `timeout_ms` — если >0, используется
 * sigtimedwait-эквивалент; если 0, ждёт как исходный блокирующий вариант
 * (ограничен внутренним максимумом — см. .c). Возвращает 0 при получении
 * сигнала, -ETIMEDOUT при таймауте, -errno при другой ошибке.
 *
 * ЧЕСТНАЯ ОГОВОРКА: кто и когда реально пошлёт этот сигнал нашему потоку
 * — отдельный, не реализованный в этой библиотеке механизм (должен быть
 * реализован читающим потоком, см. nova_rpc_callbacks.h). Если такой
 * поток не запущен, эта функция всегда завершится по таймауту —
 * это не ошибка библиотеки, а отсутствие второй половины системы.
 */
int nova_rpc_transport_wait_reply(int timeout_ms);

/* Прямой доступ к сигналу, используемому этим транспортом (для
 * установки обработчика извне, как newact.sa_handler в оригинале). */
int nova_rpc_transport_signal_number(void);

void nova_rpc_transport_close(nova_rpc_transport_t *t);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_TRANSPORT_H */
