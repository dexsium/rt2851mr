/* Уровень B (mock): проверяет диспетчер без реального устройства —
 * собирает валидные/невалидные сообщения вручную и кормит их
 * nova_rpc_callbacks_dispatch(). Явно помечено как mock (эмуляция
 * сообщений, не реальный AVCPU). */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* --- Для теста рекурсивного dispatch()/register() из handler (ТЗ §8) --- */
static nova_rpc_callback_ctx_t *g_recursive_ctx;
static int g_recursive_calls;
static int g_reregistered_calls;

static void recursive_inner_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                                     size_t payload_size, void *user_data)
{
    (void)hdr; (void)payload; (void)payload_size; (void)user_data;
    g_recursive_calls++;
}

/* Вызывается при dispatch() сообщения (program_id=1) — сам, находясь
 * ВНУТРИ dispatch(), зовёт dispatch() ПОВТОРНО на том же ctx для
 * ДРУГОГО сообщения (program_id=2). Если бы dispatch() всё ещё держал
 * ctx->mutex во время вызова handler (старая версия) — это был бы
 * гарантированный deadlock на невозвратном pthread_mutex. */
static void recursive_outer_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                                     size_t payload_size, void *user_data)
{
    uint8_t inner[NOVA_RPC_STRUCT_WIRE_SIZE];
    (void)hdr; (void)payload; (void)payload_size; (void)user_data;

    {
        nova_rpc_struct_t h = {2, 0, 0, 1, 0, 0, 0};
        nova_rpc_xdr_encode_struct(inner, sizeof(inner), &h);
    }
    nova_rpc_callbacks_dispatch(g_recursive_ctx, inner, sizeof(inner));
}

/* Вызывается при dispatch() сообщения (program_id=3) — сам, находясь
 * ВНУТРИ dispatch(), зовёт nova_rpc_callbacks_register() на том же ctx
 * (повторная регистрация ИЗ callback, ТЗ §8). Старая версия (register()
 * берёт тот же ctx->mutex, который dispatch() держал бы во время вызова
 * handler) — тоже гарантированный deadlock. */
static void reregistering_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                                   size_t payload_size, void *user_data)
{
    nova_rpc_callback_ctx_t *ctx = (nova_rpc_callback_ctx_t *)user_data;
    (void)hdr; (void)payload; (void)payload_size;

    nova_rpc_callbacks_register(ctx, 4, 0, recursive_inner_handler, NULL);
    g_reregistered_calls++;
}

/* --- Стресс-тест (ТЗ §8: concurrent register/dispatch/unregister) --- */
static nova_rpc_callback_ctx_t *g_stress_ctx;
static atomic_int g_stress_stop;

static void stress_noop_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                                  size_t payload_size, void *user_data)
{
    (void)hdr; (void)payload; (void)payload_size; (void)user_data;
}

/* Непрерывно регистрирует и тут же отзывает обработчик — проверяет
 * register()/unregister() под конкурентной нагрузкой от dispatch(). */
static void *stress_churn_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stress_stop)) {
        int h = nova_rpc_callbacks_register(g_stress_ctx, 100, 0, stress_noop_handler, NULL);
        if (h >= 0)
            nova_rpc_callbacks_unregister(g_stress_ctx, h);
    }
    return NULL;
}

/* Непрерывно диспетчеризует сообщения, которые могут как совпасть с
 * churn-потоком (program_id=100), так и не совпасть (-ENOMSG) — оба
 * исхода допустимы, тест смотрит только на отсутствие краша/deadlock
 * (под ASan/TSan это ловит use-after-free/гонки). */
static void *stress_dispatch_thread(void *arg)
{
    uint8_t msg[NOVA_RPC_STRUCT_WIRE_SIZE];
    nova_rpc_struct_t h = {100, 0, 0, 1, 0, 0, 0};
    (void)arg;
    nova_rpc_xdr_encode_struct(msg, sizeof(msg), &h);
    while (!atomic_load(&g_stress_stop))
        nova_rpc_callbacks_dispatch(g_stress_ctx, msg, sizeof(msg));
    return NULL;
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

    /* --- ТЗ §8: рекурсивный dispatch() и повторная регистрация ИЗ
     * callback — раньше (dispatch() вызывал handler ПОД ctx->mutex)
     * оба сценария были бы гарантированным deadlock-ом на невозвратном
     * pthread_mutex. Сейчас handler вызывается ВНЕ mutex (см. .c) —
     * оба вызова должны пройти без зависания (сам факт завершения
     * теста — и есть проверка отсутствия deadlock; под TSan — отсутствие
     * предупреждений о гонках на ctx->mutex). */
    {
        nova_rpc_callback_ctx_t ctx2;
        uint8_t outer_msg[NOVA_RPC_STRUCT_WIRE_SIZE];
        uint8_t inner_msg[NOVA_RPC_STRUCT_WIRE_SIZE];

        g_recursive_ctx = &ctx2;
        g_recursive_calls = 0;
        g_reregistered_calls = 0;
        nova_rpc_callbacks_init(&ctx2);

        /* "outer" вызывает dispatch() ПОВТОРНО на том же ctx2 изнутри
         * своего собственного handler — рекурсивный dispatch (ТЗ §8). */
        nova_rpc_callbacks_register(&ctx2, 1, 0, recursive_outer_handler, NULL);
        /* "inner" — то, что вызовет рекурсивный dispatch внутри outer. */
        nova_rpc_callbacks_register(&ctx2, 2, 0, recursive_inner_handler, NULL);

        build_message(outer_msg, sizeof(outer_msg), 1, 0, 0, 0);
        if (nova_rpc_callbacks_dispatch(&ctx2, outer_msg, sizeof(outer_msg)) != 0 ||
            g_recursive_calls != 1) {
            printf("FAIL recursive dispatch from within handler deadlocked or did not run\n");
            failures++;
        } else {
            printf("ok   recursive dispatch() from within a handler completes without deadlock\n");
        }

        /* "reregister" handler вызывает nova_rpc_callbacks_register() на
         * том же ctx2 изнутри себя — повторная регистрация из callback. */
        build_message(inner_msg, sizeof(inner_msg), 3, 0, 0, 0);
        nova_rpc_callbacks_register(&ctx2, 3, 0, reregistering_handler, &ctx2);
        if (nova_rpc_callbacks_dispatch(&ctx2, inner_msg, sizeof(inner_msg)) != 0 ||
            g_reregistered_calls != 1) {
            printf("FAIL re-registration from within a handler deadlocked or did not run\n");
            failures++;
        } else {
            printf("ok   register() from within a handler completes without deadlock\n");
        }

        nova_rpc_callbacks_destroy(&ctx2);
    }

    /* --- Стресс-тест (ТЗ §8: "многопоточный stress-test", "одновременная
     * регистрация и dispatch"): N потоков постоянно register/dispatch/
     * unregister на одном ctx, пока M других потоков непрерывно
     * dispatch()-ят сообщения — под ASan/TSan это ловит и use-after-free
     * (unregister во время выполнения handler), и гонки данных. */
    {
        nova_rpc_callback_ctx_t ctx3;
        pthread_t churners[4], dispatchers[4];
        int i;

        nova_rpc_callbacks_init(&ctx3);
        g_stress_ctx = &ctx3;
        atomic_store(&g_stress_stop, 0);

        for (i = 0; i < 4; i++)
            pthread_create(&churners[i], NULL, stress_churn_thread, NULL);
        for (i = 0; i < 4; i++)
            pthread_create(&dispatchers[i], NULL, stress_dispatch_thread, NULL);

        usleep(200 * 1000); /* дать потокам поработать под нагрузкой */
        atomic_store(&g_stress_stop, 1);

        for (i = 0; i < 4; i++)
            pthread_join(churners[i], NULL);
        for (i = 0; i < 4; i++)
            pthread_join(dispatchers[i], NULL);

        nova_rpc_callbacks_destroy(&ctx3);
        printf("ok   callback stress-test (concurrent register/dispatch/unregister) "
               "completed without crash/deadlock (%d joins)\n", 8);
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all mock callback tests passed\n");
    return 0;
}
