/* Уровень A: round-trip кодека + проверка самого байтового порядка
 * (big-endian, не native) — ловит регрессию, если кто-то случайно
 * заменит явный побайтовый код на memcpy/native int. */
#include <stdio.h>
#include <string.h>

#include "../nova-rtk-rpc/nova_rpc_xdr.h"

static int failures = 0;

#define CHECK(desc, cond) do { \
    if (!(cond)) { printf("FAIL %s\n", desc); failures++; } \
    else printf("ok   %s\n", desc); \
} while (0)

int main(void)
{
    uint8_t buf[4];
    nova_rpc_xdr_put_u32(buf, 0x01020304u);
    CHECK("put_u32 is big-endian", buf[0] == 0x01 && buf[1] == 0x02 &&
                                    buf[2] == 0x03 && buf[3] == 0x04);
    CHECK("get_u32 round-trip", nova_rpc_xdr_get_u32(buf) == 0x01020304u);

    {
        nova_rpc_struct_t s = {100, 0, 40, 777, 0, 12, 0};
        uint8_t wire[NOVA_RPC_STRUCT_WIRE_SIZE];
        nova_rpc_struct_t back;

        CHECK("encode_struct returns wire size",
              nova_rpc_xdr_encode_struct(wire, sizeof(wire), &s) == NOVA_RPC_STRUCT_WIRE_SIZE);
        CHECK("decode_struct returns wire size",
              nova_rpc_xdr_decode_struct(wire, sizeof(wire), &back) == NOVA_RPC_STRUCT_WIRE_SIZE);
        CHECK("round-trip program_id", back.program_id == s.program_id);
        CHECK("round-trip procedure_id", back.procedure_id == s.procedure_id);
        CHECK("round-trip task_id", back.task_id == s.task_id);
        CHECK("round-trip parameter_size", back.parameter_size == s.parameter_size);

        /* VIDEO_SYSTEM=100 должен лежать в первых 4 байтах как big-endian 0x00000064 */
        CHECK("program_id on wire is big-endian 100",
              wire[0] == 0x00 && wire[1] == 0x00 && wire[2] == 0x00 && wire[3] == 100);
    }

    /* --- ТЗ "RPC Transport Complete Closure" §25: golden vector — полный
     * байт-в-байт эталон всех 7 полей (не только round-trip через сам
     * кодек, который мог бы симметрично ошибаться в обе стороны одинаково
     * и не поймать регрессию). Значения выбраны так, чтобы у каждого
     * 4-байтного слова был уникальный, легко проверяемый байтовый
     * паттерн. */
    {
        nova_rpc_struct_t s = {
            0x00000064u, /* program_id = 100 (VIDEO_SYSTEM) */
            0x00000000u, /* version_id = 0 */
            0x00000064u, /* procedure_id = 100 (VideoCreate) */
            0x12345678u, /* task_id */
            0x00000000u, /* sys_pid */
            0x0000001Cu, /* parameter_size = 28 */
            0xAABBCCDDu  /* context */
        };
        uint8_t wire[NOVA_RPC_STRUCT_WIRE_SIZE];
        static const uint8_t expected[NOVA_RPC_STRUCT_WIRE_SIZE] = {
            0x00, 0x00, 0x00, 0x64, /* program_id */
            0x00, 0x00, 0x00, 0x00, /* version_id */
            0x00, 0x00, 0x00, 0x64, /* procedure_id */
            0x12, 0x34, 0x56, 0x78, /* task_id */
            0x00, 0x00, 0x00, 0x00, /* sys_pid */
            0x00, 0x00, 0x00, 0x1C, /* parameter_size */
            0xAA, 0xBB, 0xCC, 0xDD, /* context */
        };

        CHECK("golden vector: encode_struct wire size == 28",
              nova_rpc_xdr_encode_struct(wire, sizeof(wire), &s) == NOVA_RPC_STRUCT_WIRE_SIZE);
        CHECK("golden vector: байт-в-байт совпадение с эталоном (big-endian, 7x4 байт)",
              memcmp(wire, expected, sizeof(expected)) == 0);

        {
            nova_rpc_struct_t back;
            CHECK("golden vector: decode(эталонные байты) восстанавливает все 7 полей",
                  nova_rpc_xdr_decode_struct(expected, sizeof(expected), &back) == NOVA_RPC_STRUCT_WIRE_SIZE &&
                  back.program_id == s.program_id && back.version_id == s.version_id &&
                  back.procedure_id == s.procedure_id && back.task_id == s.task_id &&
                  back.sys_pid == s.sys_pid && back.parameter_size == s.parameter_size &&
                  back.context == s.context);
        }
    }

    CHECK("encode_struct rejects short buffer",
          nova_rpc_xdr_encode_struct((uint8_t[4]){0}, 4, &(nova_rpc_struct_t){0}) == 0);
    CHECK("decode_struct rejects short buffer",
          nova_rpc_xdr_decode_struct((const uint8_t[4]){0}, 4, &(nova_rpc_struct_t){0}) == 0);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all XDR tests passed\n");
    return 0;
}
