/* Уровень B (mock): проверяет диспетчер без реального устройства —
 * собирает валидные/невалидные сообщения вручную и кормит их
 * nova_rpc_callbacks_dispatch(). Явно помечено как mock (эмуляция
 * сообщений, не реальный AVCPU). */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../nova-rtk-rpc/nova_rpc_callbacks.h"
#include "../nova-rtk-rpc/nova_rpc_protocol.h"

static int g_frame_info_calls = 0;
static int g_last_payload_size = -1;
static void *g_last_user_data = NULL;

static void on_frame_info(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                           size_t payload_size, void *user_data)
{
    (void)hdr;
    (void)payload;
    g_frame_info_calls++;
    g_last_payload_size = (int)payload_size;
    g_last_user_data = user_data;
}

static void build_message(uint8_t *buf, size_t buf_size,
                           uint32_t program_id, uint32_t version_id,
                           uint32_t procedure_id, size_t payload_size)
{
    nova_rpc_struct_t hdr = {program_id, version_id, procedure_id, 1, 0, (uint32_t)payload_size, 0};
    nova_rpc_xdr_encode_struct(buf, buf_size, &hdr);
    memset(buf + NOVA_RPC_STRUCT_WIRE_SIZE, 0xAB, payload_size);
}

int main(void)
{
    nova_rpc_callback_ctx_t ctx;
    uint8_t msg[NOVA_RPC_STRUCT_WIRE_SIZE + 16];
    int handle;
    int failures = 0;
    int marker = 42;

    nova_rpc_callbacks_init(&ctx);

    handle = nova_rpc_callbacks_register(&ctx, NOVA_RPC_PROGRAM_VIDEO_AGENT,
                                          NOVA_RPC_VERSION_VIDEO, on_frame_info, &marker);
    if (handle < 0) { printf("FAIL register\n"); failures++; }
    else printf("ok   register returned handle %d\n", handle);

    build_message(msg, sizeof(msg), NOVA_RPC_PROGRAM_VIDEO_AGENT, NOVA_RPC_VERSION_VIDEO,
                  NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_FrameInfo, 9);
    if (nova_rpc_callbacks_dispatch(&ctx, msg, sizeof(msg)) != 0) {
        printf("FAIL dispatch matching message\n"); failures++;
    } else if (g_frame_info_calls != 1 || g_last_payload_size != 9 || g_last_user_data != &marker) {
        printf("FAIL handler not invoked correctly (calls=%d size=%d)\n",
               g_frame_info_calls, g_last_payload_size);
        failures++;
    } else {
        printf("ok   dispatch delivered to correct handler with correct payload size\n");
    }

    /* Неизвестная команда — не должна вызвать ничего и не должна упасть. */
    build_message(msg, sizeof(msg), 9999, 0, 1, 4);
    if (nova_rpc_callbacks_dispatch(&ctx, msg, sizeof(msg)) != -ENOMSG) {
        printf("FAIL unknown program should return -ENOMSG\n"); failures++;
    } else {
        printf("ok   unknown program safely ignored (-ENOMSG)\n");
    }

    /* Слишком короткое сообщение — не должна читать за границу буфера. */
    if (nova_rpc_callbacks_dispatch(&ctx, msg, 4) != -EBADMSG) {
        printf("FAIL short message should return -EBADMSG\n"); failures++;
    } else {
        printf("ok   short/incomplete message rejected (-EBADMSG)\n");
    }

    /* Отзыв регистрации — после него совпадающее сообщение больше не доставляется. */
    nova_rpc_callbacks_unregister(&ctx, handle);
    g_frame_info_calls = 0;
    build_message(msg, sizeof(msg), NOVA_RPC_PROGRAM_VIDEO_AGENT, NOVA_RPC_VERSION_VIDEO,
                  NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_FrameInfo, 9);
    if (nova_rpc_callbacks_dispatch(&ctx, msg, sizeof(msg)) != -ENOMSG || g_frame_info_calls != 0) {
        printf("FAIL unregistered handler must not be called (use-after-free risk)\n");
        failures++;
    } else {
        printf("ok   handler not called after unregister (no use-after-free)\n");
    }

    nova_rpc_callbacks_destroy(&ctx);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all mock callback tests passed\n");
    return 0;
}
