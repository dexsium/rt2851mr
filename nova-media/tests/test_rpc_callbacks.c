/* Уровень B (mock): проверяет диспетчер без реального устройства —
 * собирает валидные/невалидные сообщения вручную и кормит их
 * nova_rpc_callbacks_dispatch(). Явно помечено как mock (эмуляция
 * сообщений, не реальный AVCPU). */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_callbacks.h"
#include "../nova-rtk-rpc/nova_rpc_protocol.h"

/* См. идентичный комментарий в test_rpc_concurrency.c/test_rpc_lifecycle.c —
 * найден реальным многочасовым зависанием CI в прошлых этапах, не
 * гипотетический. Добавлен сюда в этом этапе на случай, если исправление
 * self-unregister (ниже) окажется неполным — превращает скрытый deadlock
 * в быстрый явный провал, а не в зависший job. */
static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: test exceeded 15s wall-clock budget, aborting (likely deadlock)\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(97);
}

static void install_watchdog(unsigned seconds)
{
    struct sigaction sa;
    sa.sa_handler = watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(seconds);
}

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

/* --- ТЗ "RPC Transport Complete Closure" §4.1: self-unregister — callback
 * удаляет САМ СЕБЯ. Наивная реализация unregister() ("ждать busy==0")
 * здесь зависла бы навечно (busy может стать 0 только ПОСЛЕ возврата из
 * этого самого handler) — исправлено в nova_rpc_callbacks.c через
 * сравнение busy_thread с pthread_self(). */
static nova_rpc_callback_ctx_t *g_self_unreg_ctx;
static int g_self_unreg_handle = -1;
static int g_self_unreg_calls;

static void self_unregistering_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                                        size_t payload_size, void *user_data)
{
    (void)hdr; (void)payload; (void)payload_size; (void)user_data;
    g_self_unreg_calls++;
    nova_rpc_callbacks_unregister(g_self_unreg_ctx, g_self_unreg_handle);
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

/* --- ТЗ "RPC Transport Complete Closure" п.15: lifetime/ownership
 * user_data — ДЕТЕРМИНИРОВАННО (не по времени) доказываем, что
 * unregister() из ДРУГОГО потока не может вернуться раньше, чем
 * handler закончит использовать user_data. Логика: handler, начав
 * выполняться, зависает на спине до явного разрешения; главный поток
 * ждёт (детерминированно) начала выполнения handler, затем запускает
 * unregister() в отдельном потоке и ТОЛЬКО ПОСЛЕ ЭТОГО разрешает
 * handler продолжить. unregister()-поток сам, сразу после своего
 * возврата (без каких-либо задержек), проверяет флаг "handler уже
 * закончил" — если реализация корректна, это ВСЕГДА true (порядок
 * гарантирован busy/busy_cond, не угадан по времени). */
static atomic_int g_lifetime_running;
static atomic_int g_lifetime_release;
static atomic_int g_lifetime_finished;
static int g_lifetime_user_data_ok;

static void lifetime_handler(const nova_rpc_struct_t *hdr, const uint8_t *payload,
                              size_t payload_size, void *user_data)
{
    int *val = (int *)user_data;
    (void)hdr; (void)payload; (void)payload_size;
    atomic_store(&g_lifetime_running, 1);
    while (!atomic_load(&g_lifetime_release))
        ; /* ждём явного разрешения от главного потока (см. main) */
    g_lifetime_user_data_ok = (*val == 123);
    atomic_store(&g_lifetime_finished, 1);
}

struct dispatch_once_args {
    nova_rpc_callback_ctx_t *ctx;
    const uint8_t *buf;
    size_t len;
};

static void *dispatch_once_thread(void *arg)
{
    struct dispatch_once_args *a = (struct dispatch_once_args *)arg;
    nova_rpc_callbacks_dispatch(a->ctx, a->buf, a->len);
    return NULL;
}

struct unreg_thread_args {
    nova_rpc_callback_ctx_t *ctx;
    int handle;
    int finished_before_return;
};

static void *unregister_thread_fn(void *arg)
{
    struct unreg_thread_args *a = (struct unreg_thread_args *)arg;
    nova_rpc_callbacks_unregister(a->ctx, a->handle);
    /* Проверяем СРАЗУ по возврату, без единой лишней инструкции между
     * ними — это и есть сам инвариант, не предположение. */
    a->finished_before_return = atomic_load(&g_lifetime_finished);
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

    install_watchdog(15);
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

    /* --- ТЗ "RPC Transport Complete Closure" §4.1: self-unregister. --- */
    {
        nova_rpc_callback_ctx_t ctx_self;
        uint8_t self_msg[NOVA_RPC_STRUCT_WIRE_SIZE];

        nova_rpc_callbacks_init(&ctx_self);
        g_self_unreg_ctx = &ctx_self;
        g_self_unreg_calls = 0;
        g_self_unreg_handle = nova_rpc_callbacks_register(&ctx_self, 7, 0,
                                                            self_unregistering_handler, NULL);

        build_message(self_msg, sizeof(self_msg), 7, 0, 0, 0);
        if (nova_rpc_callbacks_dispatch(&ctx_self, self_msg, sizeof(self_msg)) != 0 ||
            g_self_unreg_calls != 1) {
            printf("FAIL self-unregister deadlocked or did not run\n");
            failures++;
        } else {
            printf("ok   callback self-unregister (вызывает unregister() на самого себя) "
                   "завершается без deadlock\n");
        }

        /* После self-unregister повторный dispatch на тот же program_id
         * не должен найти обработчик (он реально удалён, не просто
         * "забыл" вызваться). */
        if (nova_rpc_callbacks_dispatch(&ctx_self, self_msg, sizeof(self_msg)) != -ENOMSG) {
            printf("FAIL self-unregistered handler still matches after self-unregister\n");
            failures++;
        } else {
            printf("ok   после self-unregister обработчик действительно удалён (-ENOMSG)\n");
        }

        nova_rpc_callbacks_destroy(&ctx_self);
    }

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

    /* --- ТЗ "RPC Transport Complete Closure" п.15: lifetime/ownership
     * user_data (детерминированный тест порядка, см. комментарий выше). */
    {
        nova_rpc_callback_ctx_t ctx_life;
        int *data = (int *)malloc(sizeof(int));
        int handle_life;
        pthread_t t_dispatch, t_unreg;
        uint8_t life_msg[NOVA_RPC_STRUCT_WIRE_SIZE];
        struct dispatch_once_args doa;
        struct unreg_thread_args uargs;

        *data = 123;
        nova_rpc_callbacks_init(&ctx_life);
        atomic_store(&g_lifetime_running, 0);
        atomic_store(&g_lifetime_release, 0);
        atomic_store(&g_lifetime_finished, 0);
        g_lifetime_user_data_ok = 0;

        handle_life = nova_rpc_callbacks_register(&ctx_life, 55, 0, lifetime_handler, data);
        build_message(life_msg, sizeof(life_msg), 55, 0, 0, 0);

        doa.ctx = &ctx_life; doa.buf = life_msg; doa.len = sizeof(life_msg);
        pthread_create(&t_dispatch, NULL, dispatch_once_thread, &doa);

        /* Детерминированный барьер: ждём, пока handler РЕАЛЬНО начнёт
         * выполняться (и сам зависнет на своей собственной спине). */
        while (!atomic_load(&g_lifetime_running))
            ;

        uargs.ctx = &ctx_life; uargs.handle = handle_life; uargs.finished_before_return = -1;
        pthread_create(&t_unreg, NULL, unregister_thread_fn, &uargs);

        /* Только ТЕПЕРЬ разрешаем handler продолжить и завершиться —
         * unregister()-поток в этот момент либо уже начал ждать busy_cond,
         * либо как раз пытается это сделать; в любом случае он не может
         * корректно вернуться раньше этого момента. */
        atomic_store(&g_lifetime_release, 1);

        pthread_join(t_dispatch, NULL);
        pthread_join(t_unreg, NULL);
        free(data);
        nova_rpc_callbacks_destroy(&ctx_life);

        if (!g_lifetime_user_data_ok) {
            printf("FAIL lifetime: user_data не был валиден внутри handler\n");
            failures++;
        } else {
            printf("ok   lifetime: user_data оставался валиден внутри handler всё время выполнения\n");
        }
        if (uargs.finished_before_return != 1) {
            printf("FAIL lifetime: unregister() вернулся РАНЬШЕ, чем handler закончил "
                   "использовать user_data (нарушение инварианта порядка)\n");
            failures++;
        } else {
            printf("ok   lifetime: unregister() из другого потока не вернулся раньше, чем "
                   "handler закончил использовать user_data (инвариант порядка, не угадан "
                   "по времени)\n");
        }
    }

    /* --- Стресс-тест (ТЗ §4.3/§8: "минимум 8 потоков", "не ограничиваться
     * 200мс", "несколько повторений"): N потоков постоянно register/
     * dispatch/unregister на одном ctx, пока M других потоков непрерывно
     * dispatch()-ят сообщения — под ASan/TSan это ловит и use-after-free
     * (unregister во время выполнения handler), и гонки данных.
     * В этом этапе (ТЗ "RPC Transport Complete Closure" §4.3) увеличено:
     * 8+8=16 потоков (было 4+4=8), 1000мс на прогон (было 200мс), 3
     * независимых повторения (было 1) — каждое со свежим ctx. */
    {
        int rep;
        for (rep = 0; rep < 3; rep++) {
            nova_rpc_callback_ctx_t ctx3;
            pthread_t churners[8], dispatchers[8];
            int i;

            nova_rpc_callbacks_init(&ctx3);
            g_stress_ctx = &ctx3;
            atomic_store(&g_stress_stop, 0);

            for (i = 0; i < 8; i++)
                pthread_create(&churners[i], NULL, stress_churn_thread, NULL);
            for (i = 0; i < 8; i++)
                pthread_create(&dispatchers[i], NULL, stress_dispatch_thread, NULL);

            usleep(1000 * 1000); /* дать потокам поработать под нагрузкой */
            atomic_store(&g_stress_stop, 1);

            for (i = 0; i < 8; i++)
                pthread_join(churners[i], NULL);
            for (i = 0; i < 8; i++)
                pthread_join(dispatchers[i], NULL);

            nova_rpc_callbacks_destroy(&ctx3);
        }
        printf("ok   callback stress-test: 3 повторения по 16 потоков (8 churn + 8 dispatch), "
               "1с каждое — без краша/deadlock\n");
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all mock callback tests passed\n");
    return 0;
}
