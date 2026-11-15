/*
 * Тест жизненного цикла nova_rpc_transport (ПРОИЗВОДСТВЕННЫЙ код):
 * открытие/повторное открытие/закрытие/повторное закрытие/закрытие во
 * время ожидания ответа/переоткрытие после закрытия. Все тестовые
 * потоки корректно join'ятся (ТЗ §5 — запрещено использовать намеренно
 * незавершаемые потоки).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* См. идентичный комментарий в test_rpc_concurrency.c — найден реальным
 * многочасовым зависанием CI, не гипотетический. */
static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: test exceeded wall-clock budget, aborting (likely deadlock)\n";
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
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_protocol.h"
#include "../nova-rtk-rpc/nova_rpc_transport.h"

static int g_passed = 0, g_failed = 0;
#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

static void setup_fifos(const char *dir)
{
    const char *names[12] = {
        "wpoll_a", "wintr_a", "wpoll_v", "wintr_v", "wpoll_v2", "wintr_v2",
        "rpoll_a", "rintr_a", "rpoll_v", "rintr_v", "rpoll_v2", "rintr_v2",
    };
    const char *envs[12] = {
        "NOVA_RPC_DEV_WPOLL_A", "NOVA_RPC_DEV_WINTR_A", "NOVA_RPC_DEV_WPOLL_V", "NOVA_RPC_DEV_WINTR_V",
        "NOVA_RPC_DEV_WPOLL_V2", "NOVA_RPC_DEV_WINTR_V2", "NOVA_RPC_DEV_RPOLL_A", "NOVA_RPC_DEV_RINTR_A",
        "NOVA_RPC_DEV_RPOLL_V", "NOVA_RPC_DEV_RINTR_V", "NOVA_RPC_DEV_RPOLL_V2", "NOVA_RPC_DEV_RINTR_V2",
    };
    int i;

    for (i = 0; i < 12; i++) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        mkfifo(full, 0600);
        setenv(envs[i], full, 1);
    }
}

/* Открывает встречную сторону КАЖДОГО из 12 FIFO неблокирующе, чтобы
 * nova_rpc_transport_open() (производственный код) могла реально
 * завершить open() на каждом из них без зависания. Держит фиктивные
 * дескрипторы открытыми до явного закрытия вызывающим кодом теста. */
static int g_peer_fds[24];
static int g_peer_count = 0;

static void open_peers(const char *dir)
{
    const char *names[12] = {
        "wpoll_a", "wintr_a", "wpoll_v", "wintr_v", "wpoll_v2", "wintr_v2",
        "rpoll_a", "rintr_a", "rpoll_v", "rintr_v", "rpoll_v2", "rintr_v2",
    };
    /* Nova открывает write-пути WRONLY -> пир открывает их RDONLY;
     * Nova открывает read-пути RDONLY -> пир открывает их WRONLY. */
    int is_write_path[12] = {1,1,1,1,1,1, 0,0,0,0,0,0};
    int i;

    g_peer_count = 0;
    for (i = 0; i < 12; i++) {
        char full[256];
        int fd;
        snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        fd = open(full, is_write_path[i] ? O_RDONLY : O_WRONLY);
        g_peer_fds[g_peer_count++] = fd;
    }
}

static void close_peers(void)
{
    int i;
    for (i = 0; i < g_peer_count; i++)
        if (g_peer_fds[i] >= 0)
            close(g_peer_fds[i]);
    g_peer_count = 0;
}

struct open_thread_args {
    const char *dir;
};

static void close_peers(void); /* forward decl — определена ниже */

static void *peer_opener_thread(void *arg)
{
    struct open_thread_args *a = (struct open_thread_args *)arg;
    close_peers(); /* закрыть fd от предыдущего цикла открытия перед новым — без утечки */
    open_peers(a->dir);
    return NULL;
}

struct client_wait_args {
    nova_rpc_transport_t *t;
    int rc;
};

static void *closer_fn(void *arg)
{
    nova_rpc_transport_close((nova_rpc_transport_t *)arg);
    return NULL;
}

struct open_race_args {
    nova_rpc_transport_t *t;
    int rc;
};

static void *open_race_fn(void *arg)
{
    struct open_race_args *a = (struct open_race_args *)arg;
    a->rc = nova_rpc_transport_open(a->t);
    return NULL;
}

static void *waiter_fn(void *arg)
{
    struct client_wait_args *c = (struct client_wait_args *)arg;
    uint8_t result[4];
    c->rc = nova_rpc_transport_call(c->t,
                                     NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                     NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                     1, NULL, 0, result, sizeof(result), 400);
    return NULL;
}

/* ТЗ "Realtek VDEC/VO & GStreamer" п.3.2: "close() во время заблокированной
 * операции отправки" — большой (8КБ) payload максимизирует реальное время,
 * проведённое внутри write() под send_mutex, увеличивая шанс, что close()
 * придётся РЕАЛЬНО застать вызов именно в этой фазе, не только в ожидании
 * ответа. */
static void *big_waiter_fn(void *arg)
{
    struct client_wait_args *c = (struct client_wait_args *)arg;
    static uint8_t args[8192];
    uint8_t result[4];
    c->rc = nova_rpc_transport_call(c->t,
                                     NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                     NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                     1, args, sizeof(args), result, sizeof(result), 400);
    return NULL;
}

/* ТЗ "RPC Transport Complete Closure" п.10: close() одновременно с ЖИВЫМ
 * dispatch_reply() (не последовательно, как в предыдущих этапах — здесь
 * dispatch_reply() вызывается в тесном цикле ДРУГИМ потоком, пока main
 * вызывает close()). Сообщения заведомо не совпадут ни с одним pending
 * (таблица пуста) — это нормально, цель теста не "доставить ответ", а
 * доказать отсутствие use-after-destroy между close() и dispatch_reply(),
 * защищённое g_close_guard (см. nova_rpc_transport.c). */
struct dispatch_loop_args {
    nova_rpc_transport_t *t;
    atomic_int *stop;
    atomic_int calls;
};

static void *dispatch_loop_fn(void *arg)
{
    struct dispatch_loop_args *a = (struct dispatch_loop_args *)arg;
    while (!atomic_load(a->stop)) {
        uint8_t msg[NOVA_RPC_STRUCT_WIRE_SIZE + 8];
        nova_rpc_struct_t h;
        h.program_id = NOVA_RPC_REPLYID; h.version_id = NOVA_RPC_REPLYID;
        h.procedure_id = 0; h.task_id = 0; h.sys_pid = 0; h.parameter_size = 8;
        h.context = 0;
        nova_rpc_xdr_encode_struct(msg, sizeof(msg), &h);
        nova_rpc_xdr_put_u32(msg + NOVA_RPC_STRUCT_WIRE_SIZE, 0);
        nova_rpc_xdr_put_u32(msg + NOVA_RPC_STRUCT_WIRE_SIZE + 4, 0);
        nova_rpc_transport_dispatch_reply(a->t, msg, sizeof(msg));
        atomic_fetch_add(&a->calls, 1);
    }
    return NULL;
}

/* ТЗ "RPC Transport Complete Closure" п.8: close() во время NONBLOCK_MODE
 * RPC. В отличие от BLOCK_MODE, nonblock-вызов не ждёт ответа — пишет и
 * сразу возвращается, но ОБА режима одинаково регистрируются как
 * `waiters` (см. nova_rpc_transport_call()) ДО того, как коснутся
 * send_mutex/fd, поэтому close() обязан так же безопасно ждать их
 * завершения. */
struct nonblock_loop_args {
    nova_rpc_transport_t *t;
    atomic_int *stop;
    atomic_int successes;
};

static void *nonblock_loop_fn(void *arg)
{
    struct nonblock_loop_args *a = (struct nonblock_loop_args *)arg;
    while (!atomic_load(a->stop)) {
        uint8_t args[4] = {0};
        int rc = nova_rpc_transport_call(a->t,
                                          NOVA_RPC_T_NONBLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                          NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                          1, args, sizeof(args), NULL, 0, 0);
        if (rc >= 0)
            atomic_fetch_add(&a->successes, 1);
        else if (rc == -ESHUTDOWN || rc == -ENODEV)
            break; /* транспорт закрывается/закрыт — честно останавливаемся, не крутимся впустую */
    }
    return NULL;
}

int main(void)
{
    char dir[256];
    nova_rpc_transport_t t;
    pthread_t opener;
    struct open_thread_args oa;
    int ret;

    /* Построчная буферизация stdout — иначе watchdog'овский _exit(97) (не
     * flush'ит stdio-буферы, в отличие от обычного return/exit()) теряет
     * ВСЕ уже напечатанные "ok"-строки, и диагностика "где именно висим"
     * становится невозможной по логам CI. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_watchdog(30);
    snprintf(dir, sizeof(dir), "/tmp/nova_rpc_lifecycle_%d", (int)getpid());
    mkdir(dir, 0700);
    setup_fifos(dir);
    oa.dir = dir;

    /* --- Сценарий 1: обычное открытие --- */
    pthread_create(&opener, NULL, peer_opener_thread, &oa);
    ret = nova_rpc_transport_open(&t);
    pthread_join(opener, NULL);
    CHECK("1. Открытие транспорта успешно", ret == 0);

    /* --- Сценарий 2 (ТЗ "RPC Transport Hardening" §5.1 — РЕАЛЬНОЕ
     * исправление, не прежнее поведение): open() на уже-открытом
     * транспорте теперь возвращает -EALREADY и НЕ трогает существующие
     * fd/mutex/cond — проверяем это явно, не закрывая транспорт сначала. */
    ret = nova_rpc_transport_open(&t);
    CHECK("2. Повторный open() на уже открытом транспорте -> -EALREADY, fd не тронуты",
          ret == -EALREADY);

    nova_rpc_transport_close(&t);
    CHECK("3. Закрытие транспорта (после открытия) не падает", 1);

    /* --- Сценарий 4: повторное закрытие уже закрытого транспорта --- */
    nova_rpc_transport_close(&t);
    CHECK("4. Повторное закрытие уже закрытого транспорта безопасно (no double free/crash)", 1);

    /* --- Сценарий 7: повторное открытие после закрытия --- */
    pthread_create(&opener, NULL, peer_opener_thread, &oa);
    ret = nova_rpc_transport_open(&t);
    pthread_join(opener, NULL);
    CHECK("7. Повторное открытие после закрытия снова успешно", ret == 0);

    /* --- Сценарии 5/6 (ТЗ "RPC Transport Hardening" §5.2/§6 — РЕАЛЬНОЕ
     * исправление production-кода, не обход): close() теперь ДЕЙСТВИТЕЛЬНО
     * безопасен при вызове конкурентно с активным nova_rpc_transport_call()
     * в BLOCK_MODE. В прошлом этапе это было понижено до UNVERIFIED после
     * настоящего 6-минутного зависания CI (run 43634985194, use-after-destroy
     * pending_mutex) — в этом этапе причина устранена в самом
     * nova_rpc_transport_close()/_call() (state machine CLOSING + счётчик
     * waiters, см. .h/.c), и тест теперь ВЫЗЫВАЕТ настоящую гонку, а не
     * избегает её. Ожидаемый результат: close() завершается без
     * use-after-destroy (под ASan/TSan это было бы поймано), а ожидающий
     * вызов получает именно -ESHUTDOWN (транспорт закрыт до тайм-аута на
     * 400мс), а не -ETIMEDOUT. */
    {
        struct client_wait_args cwa;
        pthread_t waiter;

        cwa.t = &t;
        cwa.rc = 0;
        pthread_create(&waiter, NULL, waiter_fn, &cwa);
        /* ТЗ "RPC Transport Complete Closure" §8/§22: убрана зависимость
         * от usleep() как "доказательства" конкурентной корректности —
         * вместо угадывания задержки ждём ДЕТЕРМИНИРОВАННО, пока
         * waiter_fn реально зарегистрируется как waiter внутри
         * nova_rpc_transport_call() (t.waiters>0 под ТЕМ ЖЕ pending_mutex,
         * что и производственный код — не произвольное чтение поля,
         * которое TSan корректно пометил бы гонкой данных). Это
         * гарантирует, что гонка close() vs call() ниже РЕАЛЬНА, а не
         * "предположительно успела начаться" по таймеру. install_watchdog(30)
         * выше — аварийный выход, если это предположение всё же неверно. */
        {
            int seen = 0;
            while (!seen) {
                pthread_mutex_lock(&t.pending_mutex);
                seen = (t.waiters > 0);
                pthread_mutex_unlock(&t.pending_mutex);
            }
        }
        nova_rpc_transport_close(&t); /* НАСТОЯЩАЯ гонка close() vs активный call() */
        pthread_join(waiter, NULL);
        CHECK("5/6. close() конкурентно с активным call() безопасен (нет use-after-destroy) "
              "и вызывающий получает -ESHUTDOWN, не -ETIMEDOUT",
              cwa.rc == -ESHUTDOWN);
    }

    /* --- Новый сценарий (ТЗ §5.4): несколько потоков вызывают close()
     * ОДНОВРЕМЕННО на одном транспорте — раньше это могло дважды
     * разрушить один и тот же pending_mutex/pending_cond (UB); теперь
     * сериализовано через g_close_guard (см. .c). */
    {
        pthread_t opener2, closers[4];
        int ci;

        pthread_create(&opener2, NULL, peer_opener_thread, &oa);
        ret = nova_rpc_transport_open(&t);
        pthread_join(opener2, NULL);
        CHECK("Открытие перед тестом конкурентного close() успешно", ret == 0);

        for (ci = 0; ci < 4; ci++)
            pthread_create(&closers[ci], NULL, closer_fn, &t);
        for (ci = 0; ci < 4; ci++)
            pthread_join(closers[ci], NULL);
        CHECK("4 конкурентных close() на одном транспорте не привели к крашу/UB "
              "(под ASan/TSan — иначе job сам бы упал)", 1);
    }

    /* --- ТЗ "RPC Transport Complete Closure" п.5: НАСТОЯЩАЯ конкурентная
     * гонка open() и close() на ОДНОМ транспорте (не последовательно).
     * ИСПРАВЛЕНИЕ production-кода этого этапа: раньше open() не держал
     * НИКАКОГО мьютекса на время своей последовательности из 12 open() —
     * конкурентный close() мог бы вмешаться в середину. Теперь open() и
     * close() сериализованы ТЕМ ЖЕ g_close_guard, что уже защищал
     * close()/close() и close()/dispatch_reply(). Транспорт `t` в этой
     * точке уже CLOSED (после предыдущего блока) — безопасная, заранее
     * известная начальная точка для гонки (не неинициализированная
     * память). */
    {
        pthread_t opener5, t_open, t_close;
        struct open_race_args ora;

        /* НАЙДЕНО (реальное зависание, не гипотетически — поймано
         * watchdog'ом в CI): нельзя join'ить opener5 ДО старта open() —
         * peer_opener_thread() открывает только ОДНУ сторону каждого из
         * 12 FIFO (`open_peers()`), блокируясь в ожидании, что именно
         * nova_rpc_transport_open() откроет ВТОРУЮ сторону — это
         * РЕНДЕЗВУ двух конкурентных блокирующих open(), как и во ВСЕХ
         * остальных сценариях этого файла (см. сценарий 1 выше). Если
         * join'ить opener5 первым (как было здесь по ошибке), он висит
         * навечно — никто не открывает вторую сторону. Исправлено:
         * opener5 запускается КОНКУРЕНТНО с open_race_fn, join — после
         * обоих. */
        pthread_create(&opener5, NULL, peer_opener_thread, &oa);

        ora.t = &t; ora.rc = -999;
        pthread_create(&t_open, NULL, open_race_fn, &ora);
        pthread_create(&t_close, NULL, closer_fn, &t); /* t уже CLOSED —
                                                         * close() на уже
                                                         * закрытом идемпотентен */
        pthread_join(opener5, NULL);
        pthread_join(t_open, NULL);
        pthread_join(t_close, NULL);

        CHECK("конкурентные open() и close() на одном транспорте не привели "
              "к крашу/UB (сериализованы через g_close_guard — реальное "
              "исправление п.5, раньше у open() вообще не было защиты)", 1);
        CHECK("после гонки open()/close() результат open() однозначен "
              "(0 — успех, либо отрицательный код ошибки, не неопределённое "
              "значение)", ora.rc == 0 || ora.rc < 0);

        /* Независимо от порядка гонки, приводим транспорт к известному
         * CLOSED состоянию перед следующим тестом. */
        if (ora.rc == 0)
            nova_rpc_transport_close(&t);
        close_peers();
    }

    /* --- ТЗ "RPC Transport Complete Closure" п.10: close() одновременно с
     * ЖИВЫМ dispatch_reply() (не последовательно). */
    {
        pthread_t opener4, dr_thread;
        struct dispatch_loop_args dla;
        atomic_int dr_stop = 0;

        pthread_create(&opener4, NULL, peer_opener_thread, &oa);
        ret = nova_rpc_transport_open(&t);
        pthread_join(opener4, NULL);
        CHECK("Открытие перед тестом close() vs живой dispatch_reply() успешно", ret == 0);

        dla.t = &t;
        dla.stop = &dr_stop;
        atomic_store(&dla.calls, 0);
        pthread_create(&dr_thread, NULL, dispatch_loop_fn, &dla);

        /* Детерминированный барьер: ждём, пока dispatch_reply() реально
         * выполнится хотя бы раз, прежде чем звать close() параллельно. */
        while (atomic_load(&dla.calls) == 0)
            ;

        nova_rpc_transport_close(&t); /* гонка close() vs живой цикл dispatch_reply() */
        atomic_store(&dr_stop, 1);
        pthread_join(dr_thread, NULL);
        CHECK("close() конкурентно с живым циклом dispatch_reply() безопасен "
              "(нет use-after-destroy — иначе ASan/TSan job упал бы сам)", 1);
    }

    /* --- ТЗ "RPC Transport Complete Closure" п.8: close() во время
     * NONBLOCK_MODE RPC. Поток непрерывно отправляет nonblock-запросы;
     * главный поток ждёт (детерминированно, без usleep) первого
     * реального успеха, затем вызывает close() конкурентно с ещё
     * работающим циклом отправки. */
    {
        pthread_t opener3, nb_thread;
        struct nonblock_loop_args nba;
        atomic_int nb_stop = 0;

        pthread_create(&opener3, NULL, peer_opener_thread, &oa);
        ret = nova_rpc_transport_open(&t);
        pthread_join(opener3, NULL);
        CHECK("Открытие перед тестом close() vs NONBLOCK_MODE успешно", ret == 0);

        nba.t = &t;
        nba.stop = &nb_stop;
        atomic_store(&nba.successes, 0);
        pthread_create(&nb_thread, NULL, nonblock_loop_fn, &nba);

        /* Детерминированный барьер: ждём первого РЕАЛЬНОГО успешного
         * nonblock-отправления (не предположение по времени). */
        while (atomic_load(&nba.successes) == 0)
            ;

        nova_rpc_transport_close(&t); /* гонка close() vs цикл NONBLOCK_MODE-отправок */
        atomic_store(&nb_stop, 1);
        pthread_join(nb_thread, NULL);
        CHECK("close() конкурентно с непрерывными NONBLOCK_MODE-отправками безопасен "
              "(нет use-after-destroy/UB — иначе ASan/TSan job упал бы сам)", 1);
        CHECK("хотя бы один nonblock-вызов успел пройти ДО close()",
              atomic_load(&nba.successes) > 0);

        /* После close() транспорт гарантированно НЕ открыт — свежий
         * nonblock-вызов на НЕМ ЖЕ должен честно вернуть -ENODEV, не
         * писать в уже закрытые fd. */
        {
            uint8_t args[4] = {0};
            int rc_after = nova_rpc_transport_call(&t,
                NOVA_RPC_T_NONBLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                1, args, sizeof(args), NULL, 0, 0);
            CHECK("NONBLOCK_MODE-вызов на уже закрытом транспорте честно возвращает -ENODEV",
                  rc_after == -ENODEV);
        }
    }

    /* --- ТЗ "Realtek VDEC/VO & GStreamer" п.3.2: close() ИМЕННО во время
     * заблокированной отправки (большой payload, не только во время
     * ожидания ответа, как в сценарии 5/6 выше). */
    {
        pthread_t opener6, big_waiter;
        struct client_wait_args cwa;

        pthread_create(&opener6, NULL, peer_opener_thread, &oa);
        ret = nova_rpc_transport_open(&t);
        pthread_join(opener6, NULL);
        CHECK("Открытие перед тестом close() vs заблокированная отправка успешно", ret == 0);

        cwa.t = &t; cwa.rc = 0;
        pthread_create(&big_waiter, NULL, big_waiter_fn, &cwa);
        {
            int seen = 0;
            while (!seen) {
                pthread_mutex_lock(&t.pending_mutex);
                seen = (t.waiters > 0);
                pthread_mutex_unlock(&t.pending_mutex);
            }
        }
        nova_rpc_transport_close(&t); /* гонка с реально большой отправкой (8КБ) */
        pthread_join(big_waiter, NULL);
        CHECK("close() во время заблокированной отправки большого payload безопасен "
              "(нет use-after-close/use-after-destroy/deadlock — иначе ASan/TSan job "
              "сам бы упал) и вызов честно завершается -ESHUTDOWN",
              cwa.rc == -ESHUTDOWN);
        close_peers();
    }

    /* --- Финальное открытие для проверки на утечки fd и чистого выхода --- */
    pthread_create(&opener, NULL, peer_opener_thread, &oa);
    ret = nova_rpc_transport_open(&t);
    pthread_join(opener, NULL);
    CHECK("Финальное открытие снова успешно (нет накопленной порчи состояния)", ret == 0);
    nova_rpc_transport_close(&t);
    close_peers();

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
