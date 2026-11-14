/*
 * nova-rtk-rpc — минимальная userspace-библиотека RPC-транспорта ARM↔AVCPU
 * для Realtek RTD2851M (платформа rtd288o).
 *
 * Это НЕ обратная разработка закрытого протокола: все константы ниже взяты
 * буквально из открытого GPL-заголовка
 *   kernel/drivers/rtk_kdriver/common/IPC/include/rpcapi.h
 * репозитория TCLOpenSource/rt2851mr (форк dexsium/rt2851mr, commit
 * 80a2688976ac8f67b73ebfd5da5e55f3f09a472f), и из device-узлов, которые
 * создаёт kernel/drivers/rtk_kdriver/RPC/RPCDriver.c (devfs_mk_cdev /
 * device_create для "rpc%d", "rpc100", "rpcdebug"; platform_device.name =
 * "Venus_RPC" — то самое имя, что подтверждено на живом устройстве в
 * NOVA_RTD288O_BSP_FULL_AUDIT.md).
 *
 * Эта библиотека НЕ заменяет закрытые librtk_media_hw_rpc.so /
 * librtk_rhal_rpccore.so — она реализует тот же минимальный протокол
 * напрямую поверх открытого ioctl ядра, без Binder/HIDL/Android.
 *
 * Статус: IMPLEMENTED. Компиляция проверяется в CI (см.
 * .github/workflows/nova-media-build.yml) — NOT VERIFIED на реальном
 * устройстве (нет способа загрузить собственное ядро на этом ТВ,
 * см. NOVA_AVCPU_BOOTSTRAP_FEASIBILITY.md §7).
 */
#ifndef NOVA_RPC_H
#define NOVA_RPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Буквально из rpcapi.h — не придуманные значения */
#define NOVA_RPC_NONBLOCK_MODE   0x00
#define NOVA_RPC_BLOCK_MODE      0x01
#define NOVA_RPC_USE_POLL_BUF    0x00
#define NOVA_RPC_USE_INTR_BUF    0x02
#define NOVA_RPC_SEND_VIDEO_CPU  0x00
#define NOVA_RPC_SEND_AUDIO_CPU  0x04
#define NOVA_RPC_SEND_VIDEO_CPU2 0x08

/*
 * Диапазоны programID — задокументированы буквально в
 * kernel/drivers/rtk_kdriver/common/IPC/readme.txt:
 *   RPC lib core : 0   - 100
 *   Video team   : 100 - 200
 *   Audio team   : 200 - 300
 *   System team  : 300 - 400
 */
#define NOVA_RPC_PROGRAM_RANGE_LIB_BASE    0
#define NOVA_RPC_PROGRAM_RANGE_VIDEO_BASE  100
#define NOVA_RPC_PROGRAM_RANGE_AUDIO_BASE  200
#define NOVA_RPC_PROGRAM_RANGE_SYSTEM_BASE 300

typedef struct nova_rpc_handle {
    int fd;              /* дескриптор /dev/rpc100 (общий control-узел) */
    unsigned long program_id;
    int registered;
} nova_rpc_handle_t;

/* Открывает общий RPC control-узел ("/dev/rpc100", создаётся самим
 * драйвером ядра — device_create(..., 100, ..., "rpc100") в RPCDriver.c).
 * Возвращает 0 и заполняет *out при успехе, -errno при ошибке. */
int nova_rpc_open(nova_rpc_handle_t *out);

/* Аналог registerRPC(programID) из rpcapi.h: ioctl(fd, RPC_IOCTHANDLER, programID).
 * magic 'k', номер 3 — буквально из rpcapi.h, не предположение. */
int nova_rpc_register(nova_rpc_handle_t *h, unsigned long program_id);

/* Аналог stopRPC(): ioctl(fd, RPC_IOSTOP). magic 'k', номер 4. */
int nova_rpc_stop(nova_rpc_handle_t *h);

/* Чтение кольцевого буфера ответов — обёртка над обычным read(2) поверх
 * того же узла, аналог readRingBuf() из rpcapi.h. */
int nova_rpc_read(nova_rpc_handle_t *h, void *buf, unsigned int size);

void nova_rpc_close(nova_rpc_handle_t *h);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_H */
