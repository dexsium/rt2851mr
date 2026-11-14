#include "nova_rpc_xdr.h"

void nova_rpc_xdr_put_u32(uint8_t *buf, uint32_t value)
{
    /* Big-endian, как htonl() в xdrmem_putlong (xdr_mem.c:131). Пишем
     * явно по байтам, не полагаясь на native endianness компилятора. */
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)(value);
}

uint32_t nova_rpc_xdr_get_u32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

size_t nova_rpc_xdr_encode_struct(uint8_t *buf, size_t buf_size,
                                   const nova_rpc_struct_t *s)
{
    if (!buf || !s || buf_size < NOVA_RPC_STRUCT_WIRE_SIZE)
        return 0;

    nova_rpc_xdr_put_u32(buf + 0,  s->program_id);
    nova_rpc_xdr_put_u32(buf + 4,  s->version_id);
    nova_rpc_xdr_put_u32(buf + 8,  s->procedure_id);
    nova_rpc_xdr_put_u32(buf + 12, s->task_id);
    nova_rpc_xdr_put_u32(buf + 16, s->sys_pid);
    nova_rpc_xdr_put_u32(buf + 20, s->parameter_size);
    nova_rpc_xdr_put_u32(buf + 24, s->context);

    return NOVA_RPC_STRUCT_WIRE_SIZE;
}

size_t nova_rpc_xdr_decode_struct(const uint8_t *buf, size_t buf_size,
                                   nova_rpc_struct_t *out)
{
    if (!buf || !out || buf_size < NOVA_RPC_STRUCT_WIRE_SIZE)
        return 0;

    out->program_id      = nova_rpc_xdr_get_u32(buf + 0);
    out->version_id      = nova_rpc_xdr_get_u32(buf + 4);
    out->procedure_id     = nova_rpc_xdr_get_u32(buf + 8);
    out->task_id          = nova_rpc_xdr_get_u32(buf + 12);
    out->sys_pid           = nova_rpc_xdr_get_u32(buf + 16);
    out->parameter_size    = nova_rpc_xdr_get_u32(buf + 20);
    out->context           = nova_rpc_xdr_get_u32(buf + 24);

    return NOVA_RPC_STRUCT_WIRE_SIZE;
}
