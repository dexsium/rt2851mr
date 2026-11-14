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

    CHECK("encode_struct rejects short buffer",
          nova_rpc_xdr_encode_struct((uint8_t[4]){0}, 4, &(nova_rpc_struct_t){0}) == 0);
    CHECK("decode_struct rejects short buffer",
          nova_rpc_xdr_decode_struct((const uint8_t[4]){0}, 4, &(nova_rpc_struct_t){0}) == 0);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all XDR tests passed\n");
    return 0;
}
