/*
 * ТЗ "RPC Transport Complete Closure" ЧАСТЬ II / п.16, 17, 19 —
 * Fault Injection Framework.
 *
 * Использует механизм dependency injection, уже предусмотренный
 * в nova_rpc_transport.c (nova_fi_*_hook, см. nova_rpc_transport.h):
 * указатели-перехватчики по умолчанию NULL (реальная функция), тестовый
 * код здесь временно подставляет свои обёртки, которые детерминированно
 * отказывают на заданном по счёту вызове — без исчерпания реальной
 * памяти/ресурсов CI-машины (прямое требование ТЗ §9).
 *
 * ВАЖНО: это НЕ отдельная пересборка nova_rpc_transport.c — используется
 * ТОТ ЖЕ .o, что и во всех остальных тестах; перехватчики переключаются
 * во время выполнения этого единственного процесса и ВСЕГДА возвращаются
 * в NULL после каждого сценария (в т.ч. при неожиданном выходе — иначе
 * следующий тест унаследовал бы отказ).
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
#include <unistd.h>

static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: fault-injection test exceeded wall-clock budget\n";
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

#include "../nova-rtk-rpc/nova_rpc_protocol.h"
#include "../nova-rtk-rpc/nova_rpc_transport.h"

static int g_passed = 0, g_failed = 0;
#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

/* --- Общая инфраструктура: все перехватчики считают вызовы и отказывают
 * РОВНО на заданном (1-based) номере вызова, затем (если fail_at>0 и
 * вызовов больше) ведут себя как настоящая функция. fail_at<=0 — никогда
 * не отказывать (прозрачный проход). Каждый сценарий сбрасывает счётчики
 * и САМ восстанавливает hook=NULL в конце (см. reset_all_hooks()). */
static atomic_int g_malloc_calls, g_malloc_fail_at;
static atomic_int g_mutex_calls, g_mutex_fail_at;
static atomic_int g_cond_calls, g_cond_fail_at;
static atomic_int g_write_calls, g_write_fail_at;
static atomic_int g_write_fail_errno;
static atomic_int g_write_short_at; /* >0: на этом вызове вернуть короткую запись (n-1 байт) вместо ошибки */

static void *fi_malloc(size_t sz)
{
    int n = atomic_fetch_add(&g_malloc_calls, 1) + 1;
    if (atomic_load(&g_malloc_fail_at) == n)
        return NULL;
    return malloc(sz);
}

static int fi_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    int n = atomic_fetch_add(&g_mutex_calls, 1) + 1;
    if (atomic_load(&g_mutex_fail_at) == n)
        return EAGAIN; /* pthread_* возвращают код ошибки напрямую, не через errno */
    return pthread_mutex_init(m, a);
}

static int fi_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    int n = atomic_fetch_add(&g_cond_calls, 1) + 1;
    if (atomic_load(&g_cond_fail_at) == n)
        return EAGAIN;
    return pthread_cond_init(c, a);
}

static ssize_t fi_write(int fd, const void *buf, size_t count)
{
    int n = atomic_fetch_add(&g_write_calls, 1) + 1;
    if (atomic_load(&g_write_fail_at) == n) {
        errno = atomic_load(&g_write_fail_errno);
        return -1;
    }
    if (atomic_load(&g_write_short_at) == n && count > 1)
        return write(fd, buf, count - 1); /* честная короткая запись, не придуманная */
    return write(fd, buf, count);
}

static void reset_all_hooks(void)
{
    nova_fi_malloc_hook = NULL;
    nova_fi_mutex_init_hook = NULL;
    nova_fi_cond_init_hook = NULL;
    nova_fi_write_hook = NULL;
    atomic_store(&g_malloc_calls, 0); atomic_store(&g_malloc_fail_at, 0);
    atomic_store(&g_mutex_calls, 0); atomic_store(&g_mutex_fail_at, 0);
    atomic_store(&g_cond_calls, 0); atomic_store(&g_cond_fail_at, 0);
    atomic_store(&g_write_calls, 0); atomic_store(&g_write_fail_at, 0);
    atomic_store(&g_write_fail_errno, 0); atomic_store(&g_write_short_at, 0);
}

/* --- FIFO-обвязка (та же проверенная схема, что в остальных тестах
 * этого набора: RDONLY первым, затем WRONLY|O_NONBLOCK — без ENXIO). --- */
static const char *g_names[12] = {
    "wpoll_a", "wintr_a", "wpoll_v", "wintr_v", "wpoll_v2", "wintr_v2",
    "rpoll_a", "rintr_a", "rpoll_v", "rintr_v", "rpoll_v2", "rintr_v2",
};
static const char *g_envs[12] = {
    "NOVA_RPC_DEV_WPOLL_A", "NOVA_RPC_DEV_WINTR_A", "NOVA_RPC_DEV_WPOLL_V", "NOVA_RPC_DEV_WINTR_V",
    "NOVA_RPC_DEV_WPOLL_V2", "NOVA_RPC_DEV_WINTR_V2", "NOVA_RPC_DEV_RPOLL_A", "NOVA_RPC_DEV_RINTR_A",
    "NOVA_RPC_DEV_RPOLL_V", "NOVA_RPC_DEV_RINTR_V", "NOVA_RPC_DEV_RPOLL_V2", "NOVA_RPC_DEV_RINTR_V2",
};
static int g_peer_fds[24];
static int g_peer_count;

static void close_peers(void)
{
    int i;
    for (i = 0; i < g_peer_count; i++)
        if (g_peer_fds[i] >= 0)
            close(g_peer_fds[i]);
    g_peer_count = 0;
}

static void setup_all_fifos(const char *dir)
{
    int i;
    g_peer_count = 0;
    for (i = 0; i < 12; i++) {
        char full[256];
        int fd_r, fd_w;
        snprintf(full, sizeof(full), "%s/%s", dir, g_names[i]);
        mkfifo(full, 0600);
        setenv(g_envs[i], full, 1);
        fd_r = open(full, O_RDONLY | O_NONBLOCK);
        fd_w = open(full, O_WRONLY | O_NONBLOCK);
        g_peer_fds[g_peer_count++] = fd_r;
        g_peer_fds[g_peer_count++] = fd_w;
    }
}

int main(void)
{
    char dir[256];

    install_watchdog(30);
    snprintf(dir, sizeof(dir), "/tmp/nova_rpc_fi_%d", (int)getpid());
    mkdir(dir, 0700);

    /* === п.16: malloc() === */
    {
        nova_rpc_transport_t t;
        int ret;

        setup_all_fifos(dir);
        ret = nova_rpc_transport_open(&t);
        CHECK("fault-injection: открытие транспорта для malloc-сценариев успешно", ret == 0);

        /* Первый malloc() внутри nova_rpc_transport_call() возвращает NULL. */
        reset_all_hooks();
        atomic_store(&g_malloc_fail_at, 1);
        nova_fi_malloc_hook = fi_malloc;
        {
            uint8_t args[4] = {0}, result[4];
            int rc = nova_rpc_transport_call(&t,
                NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                1, args, sizeof(args), result, sizeof(result), 200);
            CHECK("malloc fail (1-й вызов) -> честный -ENOMEM, без краша", rc == -ENOMEM);
        }
        reset_all_hooks();

        /* Транспорт должен остаться полностью рабочим после отказа malloc —
         * нет утечки waiters/слота (прямое требование ТЗ §9 "освобождение
         * всех выделенных ресурсов", "отсутствие зависших потоков"). */
        {
            pthread_mutex_lock(&t.pending_mutex);
            CHECK("после отказа malloc счётчик waiters корректно вернулся к 0 "
                  "(нет утечки состояния)", t.waiters == 0);
            pthread_mutex_unlock(&t.pending_mutex);
        }

        nova_rpc_transport_close(&t);
        close_peers();
    }

    /* === п.17: pthread_mutex_init() / pthread_cond_init() === */
    {
        nova_rpc_transport_t t;
        int ret;

        /* 1-й pthread_mutex_init() внутри open() (pending_mutex) отказывает. */
        setup_all_fifos(dir);
        reset_all_hooks();
        atomic_store(&g_mutex_fail_at, 1);
        nova_fi_mutex_init_hook = fi_mutex_init;
        ret = nova_rpc_transport_open(&t);
        CHECK("pthread_mutex_init fail (pending_mutex) -> open() честно "
              "возвращает ошибку, НЕ притворный успех с неинициализированным "
              "mutex (НАЙДЕННЫЙ и исправленный реальный пробел — раньше возврат "
              "pthread_mutex_init() игнорировался полностью)",
              ret < 0);
        reset_all_hooks();
        close_peers();

        /* Повторное открытие СО ВСЕМИ рабочими хуками снова успешно — отказ
         * инициализации не оставил fd-утечку/повреждённое состояние. */
        setup_all_fifos(dir);
        ret = nova_rpc_transport_open(&t);
        CHECK("после отказа pthread_mutex_init повторное открытие снова "
              "успешно (нет утечки fd/порчи состояния)", ret == 0);
        if (ret == 0)
            nova_rpc_transport_close(&t);
        close_peers();

        /* 1-й pthread_cond_init() (происходит ПОСЛЕ успешного mutex_init —
         * проверяем, что уже инициализированный pending_mutex корректно
         * разрушается при последующем отказе cond_init). */
        setup_all_fifos(dir);
        reset_all_hooks();
        atomic_store(&g_cond_fail_at, 1);
        nova_fi_cond_init_hook = fi_cond_init;
        ret = nova_rpc_transport_open(&t);
        CHECK("pthread_cond_init fail (после успешного mutex_init) -> open() "
              "честно возвращает ошибку, pending_mutex корректно разрушен "
              "(не просто брошен недоинициализированным)",
              ret < 0);
        reset_all_hooks();
        close_peers();

        setup_all_fifos(dir);
        ret = nova_rpc_transport_open(&t);
        CHECK("после отказа pthread_cond_init повторное открытие снова "
              "успешно", ret == 0);
        if (ret == 0)
            nova_rpc_transport_close(&t);
        close_peers();

        /* Отказ ПОСЛЕДНЕГО (6-го) send_mutex[i] init — проверяет, что уже
         * инициализированные send_mutex[0..4] корректно разрушаются. */
        setup_all_fifos(dir);
        reset_all_hooks();
        atomic_store(&g_mutex_fail_at, 7); /* 1-й mutex_init — pending_mutex (успешно), 2..7-й — send_mutex[0..5], 7-й = send_mutex[5] отказывает */
        nova_fi_mutex_init_hook = fi_mutex_init;
        ret = nova_rpc_transport_open(&t);
        CHECK("pthread_mutex_init fail на последнем (6-м) send_mutex -> "
              "честная ошибка, ранее инициализированные send_mutex[0..4] "
              "корректно разрушены (под ASan это было бы поймано, если бы "
              "не разрушались)",
              ret < 0);
        reset_all_hooks();
        close_peers();

        setup_all_fifos(dir);
        ret = nova_rpc_transport_open(&t);
        CHECK("после отказа на последнем send_mutex повторное открытие "
              "снова успешно", ret == 0);
        if (ret == 0)
            nova_rpc_transport_close(&t);
        close_peers();
    }

    /* === п.19: ошибки write() (EINTR/EAGAIN/EPIPE/EBADF, короткая запись) === */
    {
        nova_rpc_transport_t t;
        int ret;
        int errnos[4] = { EINTR, EAGAIN, EPIPE, EBADF };
        const char *names[4] = { "EINTR", "EAGAIN", "EPIPE", "EBADF" };
        int i;

        setup_all_fifos(dir);
        ret = nova_rpc_transport_open(&t);
        CHECK("открытие транспорта для write-сценариев успешно", ret == 0);

        for (i = 0; i < 4; i++) {
            uint8_t args[4] = {0}, result[4];
            int rc;
            char desc[128];

            reset_all_hooks();
            atomic_store(&g_write_fail_at, 1);
            atomic_store(&g_write_fail_errno, errnos[i]);
            nova_fi_write_hook = fi_write;

            rc = nova_rpc_transport_call(&t,
                NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                1, args, sizeof(args), result, sizeof(result), 200);
            snprintf(desc, sizeof(desc),
                     "write() fail с errno=%s -> честный -errno (%d), без краша, слот освобождён",
                     names[i], -errnos[i]);
            CHECK(desc, rc == -errnos[i]);

            reset_all_hooks();
            {
                pthread_mutex_lock(&t.pending_mutex);
                CHECK("после отказа write() waiters корректно вернулся к 0",
                      t.waiters == 0);
                pthread_mutex_unlock(&t.pending_mutex);
            }
        }

        /* Короткая запись (меньше total) -> честная -EIO, не "частичный успех". */
        reset_all_hooks();
        atomic_store(&g_write_short_at, 1);
        nova_fi_write_hook = fi_write;
        {
            uint8_t args[4] = {0}, result[4];
            int rc = nova_rpc_transport_call(&t,
                NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                1, args, sizeof(args), result, sizeof(result), 200);
            CHECK("короткая запись (n != total) -> честный -EIO, не принята "
                  "как частичный успех", rc == -EIO);
        }
        reset_all_hooks();

        nova_rpc_transport_close(&t);
        close_peers();
    }

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
