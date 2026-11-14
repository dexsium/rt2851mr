/*
 * nova_rpc_xdr — минимальный XDR-кодек, байт-в-байт совместимый с
 * форматом, который реально использует Realtek-прошивка.
 *
 * CONFIRMED (не предположение): `kernel/drivers/rtk_kdriver/common/IPC/src/xdr/xdr_mem.c`
 * (vendored копия glibc sunrpc `xdr_mem.c`) — функции `xdrmem_putlong`/
 * `xdrmem_getlong` используют `htonl()`/`ntohl()` (строки 131/111 в
 * скачанной копии): каждое 32-битное поле (`long`/`u_long`/`int`/`u_int`)
 * кодируется как 4 байта в сетевом (big-endian) порядке — независимо от
 * того, что сам ARM32 — little-endian. Это прямой ответ на требование
 * ТЗ "не используй напрямую ABI ARM32, если XDR требует фиксированного
 * сетевого представления" — не используем, кодируем явно big-endian.
 *
 * `struct RPC_STRUCT` (CONFIRMED, `RPCstruct.h:96-112`, ветка без
 * `CONFIG_ARM64` — наш случай): 7 полей по 4 байта каждое (`u_long`
 * = 4 байта на ARM32) в порядке programID, versionID, procedureID,
 * taskID, sysPID (поле есть, т.к. `RTD_1195` определён безусловно в этом
 * же заголовке, `RPCstruct.h:32`), parameterSize, context — итого 28
 * байт заголовка.
 *
 * Статус: IMPLEMENTED, COMPILED (см. CI). Побайтовая совместимость с
 * реальной прошивкой — NOT VERIFIED (нет способа сравнить с живым
 * обменом без работающего Nova-ядра на устройстве), но КОДЕК сам по себе
 * не угадан — он воспроизводит документированное поведение
 * `xdrmem_putlong`/`getlong` из того же открытого дерева исходников.
 */
#ifndef NOVA_RPC_XDR_H
#define NOVA_RPC_XDR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Размер struct RPC_STRUCT на проводе (28 байт, 7 x u32 big-endian). */
#define NOVA_RPC_STRUCT_WIRE_SIZE 28

/* Статическая проверка (ТЗ §7): независимо от ABI компилятора/архитектуры
 * (ARM32 с 4-байтным `long`, ARM64/x86_64 с 8-байтным `long`), проводное
 * представление RPC_STRUCT фиксировано — 7 полей по 4 байта. Этот размер
 * не зависит от sizeof(long) на целевой платформе: кодек работает с
 * uint32_t, не с native `long`, именно чтобы исключить эту зависимость
 * (прямое требование ТЗ "не используй напрямую ABI ARM32"/"отсутствие
 * зависимости от размера long"). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <assert.h>
_Static_assert(NOVA_RPC_STRUCT_WIRE_SIZE == 7 * 4,
               "NOVA_RPC_STRUCT_WIRE_SIZE must stay 7 big-endian u32 fields (28 bytes)");
#endif

typedef struct nova_rpc_struct {
    uint32_t program_id;
    uint32_t version_id;
    uint32_t procedure_id;
    uint32_t task_id;
    uint32_t sys_pid;       /* RTD_1195 — присутствует безусловно, см. комментарий выше */
    uint32_t parameter_size;
    uint32_t context;
} nova_rpc_struct_t;

/* Кодирует одно 32-битное поле в big-endian (= xdrmem_putlong). */
void nova_rpc_xdr_put_u32(uint8_t *buf, uint32_t value);

/* Декодирует одно 32-битное поле из big-endian (= xdrmem_getlong). */
uint32_t nova_rpc_xdr_get_u32(const uint8_t *buf);

/* Сериализует RPC_STRUCT в NOVA_RPC_STRUCT_WIRE_SIZE байт по адресу buf.
 * buf должен иметь минимум NOVA_RPC_STRUCT_WIRE_SIZE свободных байт.
 * Возвращает число записанных байт (всегда NOVA_RPC_STRUCT_WIRE_SIZE) или
 * 0 при ошибке аргументов. */
size_t nova_rpc_xdr_encode_struct(uint8_t *buf, size_t buf_size,
                                   const nova_rpc_struct_t *s);

/* Разбирает заголовок из буфера. Возвращает NOVA_RPC_STRUCT_WIRE_SIZE при
 * успехе, 0 если buf_size меньше требуемого. */
size_t nova_rpc_xdr_decode_struct(const uint8_t *buf, size_t buf_size,
                                   nova_rpc_struct_t *out);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_XDR_H */
