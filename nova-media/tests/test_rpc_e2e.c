/*
 * Сквозной (end-to-end) mock-тест ПРОИЗВОДСТВЕННОГО кода
 * (nova_rpc_transport_open/call/dispatch_reply) — не упрощённой
 * параллельной реализации. Вместо реальных /dev/rpc* используются 12
 * FIFO (mkfifo), пути которых передаются транспорту через переменные
 * окружения NOVA_RPC_DEV_* (см. nova_rpc_transport.c).
 *
 * ИЗМЕНЕНО в этом этапе ТЗ (§5 "не использовать намеренно
 * незавершаемые потоки как компромисс тестирования"): все читающие
 * потоки используют `poll()` с коротким тайм-аутом вместо
 * безусловно-блокирующего `read()`, чтобы периодически проверять флаг
 * `stop` и корректно завершаться — все три вспомогательных потока
 * `pthread_join()`'ятся в конце `main()`, ни один не остаётся висеть.
 *
 * ЧЕСТНАЯ ОГОВОРКА: `nova_rpc_transport_register()` (ioctl
 * RPC_IOCTHANDLER) НЕ покрыт этим e2e-тестом — FIFO не поддерживают
 * ioctl (вернули бы ENOTTY).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h> /* atomic_int для stop-флагов — найдено TSan: volatile int
                          не гарантирует атомарность, только отсутствие
                          оптимизации компилятором (CI run 43648589773). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_protocol.h"
#include "../nova-rtk-rpc/nova_rpc_transport.h"

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

static char g_dir[256];
static int g_passed = 0, g_failed = 0;

#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

#define POLL_STEP_MS 50

static void mkpath(char *out, const char *name)
{
    snprintf(out, 256, "%s/%s", g_dir, name);
}

/* read(), но с поллингом шагами POLL_STEP_MS, чтобы периодически можно
 * было проверить `stop`. Возвращает то же, что read(), либо 0 при stop. */
static ssize_t poll_read(int fd, void *buf, size_t cap, const atomic_int *stop)
{
    while (!*stop) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, POLL_STEP_MS);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            /* ТЗ "Realtek VDEC/VO & GStreamer" п.3.3: read() сам может
             * вернуть -1/EINTR (прерван сигналом) — раньше это честно НЕ
             * ретраилось и ошибочно трактовалось read_exact() как жёсткий
             * провал. POSIX требует повторить именно read(), не весь
             * poll()-цикл заново. */
            ssize_t r = read(fd, buf, cap);
            if (r < 0 && errno == EINTR)
                continue;
            return r;
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) {
            /* НАЙДЕННЫЙ реальный баг (ТЗ п.3.3, сценарий EBADF): если fd
             * закрыт/невалиден, poll() даёт POLLNVAL, НЕ POLLIN — раньше
             * ни одна ветка этого не ловила, и цикл крутился БЕСКОНЕЧНО
             * (pr>0, но ни один if не сработал — while просто повторяется).
             * POLLHUP без POLLIN — пир закрылся и данных больше нет,
             * тоже не повод продолжать опрашивать вечно. */
            errno = (pfd.revents & POLLNVAL) ? EBADF : EIO;
            return -1;
        }
        if (pr < 0 && errno != EINTR)
            return -1;
    }
    return 0;
}

struct echo_server_args {
    char w_path[256];
    char r_path[256];
    uint32_t fake_result;
    atomic_int *stop;
};

static void *echo_server_thread(void *arg)
{
    struct echo_server_args *a = (struct echo_server_args *)arg;
    int wfd = open(a->w_path, O_RDONLY | O_NONBLOCK);
    int rfd = open(a->r_path, O_WRONLY);
    uint8_t req[256];
    int answered = 0;

    if (wfd < 0 || rfd < 0) {
        fprintf(stderr, "echo_server: open failed\n");
        return NULL;
    }

    while (!*a->stop) {
        ssize_t n = poll_read(wfd, req, sizeof(req), a->stop);
        nova_rpc_struct_t hdr;

        if (n < (ssize_t)NOVA_RPC_STRUCT_WIRE_SIZE)
            continue;

        if (answered)
            continue; /* имитирует "AVCPU принял, но не ответил" (тайм-аут) */

        nova_rpc_xdr_decode_struct(req, (size_t)n, &hdr);

        {
            uint8_t reply[NOVA_RPC_STRUCT_WIRE_SIZE + 8];
            nova_rpc_struct_t reply_hdr;

            reply_hdr.program_id = NOVA_RPC_REPLYID;
            reply_hdr.version_id = NOVA_RPC_REPLYID;
            reply_hdr.procedure_id = 0;
            reply_hdr.task_id = 0;
            reply_hdr.sys_pid = 0;
            reply_hdr.parameter_size = 8;
            reply_hdr.context = hdr.context; /* буквально req_context из SVCFunc.c:92-93 */

            nova_rpc_xdr_encode_struct(reply, sizeof(reply), &reply_hdr);
            nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE, hdr.task_id);
            nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE + 4, a->fake_result);

            write(rfd, reply, sizeof(reply));
        }
        answered = 1;
    }

    close(wfd);
    close(rfd);
    return NULL;
}

struct dispatch_loop_args {
    nova_rpc_transport_t *t;
    int rfd;
    atomic_int *stop;
};

static void *dispatch_loop_thread(void *arg)
{
    struct dispatch_loop_args *a = (struct dispatch_loop_args *)arg;
    uint8_t buf[256];

    while (!*a->stop) {
        ssize_t n = poll_read(a->rfd, buf, sizeof(buf), a->stop);
        if (n > 0)
            nova_rpc_transport_dispatch_reply(a->t, buf, (size_t)n);
    }
    return NULL;
}

struct drain_args {
    char **paths; /* 10 путей: первые 5 — write-тип (открыть RDONLY), следующие 5 — read-тип (открыть WRONLY) */
    atomic_int *stop;
};

static void *drain_thread(void *arg)
{
    struct drain_args *a = (struct drain_args *)arg;
    int fds[10];
    int i;

    for (i = 0; i < 5; i++)
        fds[i] = open(a->paths[i], O_RDONLY | O_NONBLOCK);
    for (i = 5; i < 10; i++)
        fds[i] = open(a->paths[i], O_WRONLY);

    /* Просто держим открытыми и периодически проверяем stop — ничего не
     * делаем с данными (они предназначены для audio/video2/intr каналов,
     * не используемых этим e2e-тестом). */
    while (!*a->stop)
        usleep(POLL_STEP_MS * 1000);

    for (i = 0; i < 10; i++)
        if (fds[i] >= 0)
            close(fds[i]);

    return NULL;
}

int main(void)
{
    nova_rpc_transport_t t;
    pthread_t srv, disp, drain;
    atomic_int stop = 0;
    struct echo_server_args srv_args;
    struct dispatch_loop_args disp_args;
    struct drain_args drain_args_v;
    char *drain_paths[10];
    static char storage[10][256];
    char paths[12][256];
    int ret;
    uint8_t result[4];

    install_watchdog(30);
    snprintf(g_dir, sizeof(g_dir), "/tmp/nova_rpc_e2e_%d", (int)getpid());
    mkdir(g_dir, 0700);

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
        int i, idx;

        for (i = 0; i < 12; i++) {
            mkpath(paths[i], names[i]);
            mkfifo(paths[i], 0600);
            setenv(envs[i], paths[i], 1);
        }

        idx = 0;
        for (i = 0; i < 12; i++) {
            if (i == 2 || i == 8)
                continue; /* video-poll обрабатываются отдельно (echo_server/dispatch_loop) */
            strcpy(storage[idx], paths[i]);
            drain_paths[idx] = storage[idx];
            idx++;
        }
    }

    strcpy(srv_args.w_path, paths[2]);
    strcpy(srv_args.r_path, paths[8]);
    srv_args.fake_result = 0xCAFEF00D;
    srv_args.stop = &stop;

    drain_args_v.paths = drain_paths;
    drain_args_v.stop = &stop;
    pthread_create(&drain, NULL, drain_thread, &drain_args_v);
    pthread_create(&srv, NULL, echo_server_thread, &srv_args);

    ret = nova_rpc_transport_open(&t);
    CHECK("nova_rpc_transport_open() succeeds against mock FIFOs", ret == 0);
    if (ret != 0) {
        stop = 1;
        pthread_join(srv, NULL);
        pthread_join(drain, NULL);
        printf("%d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }

    disp_args.t = &t;
    disp_args.rfd = t.rfd_poll_video;
    disp_args.stop = &stop;
    pthread_create(&disp, NULL, dispatch_loop_thread, &disp_args);

    ret = nova_rpc_transport_call(&t,
                                   NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                   NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                   NOVA_RPC_VIDEO_ToAgent_Create, NULL, 0,
                                   result, sizeof(result), 3000);

    CHECK("Сценарий 1: успешный запрос и корректный ответ (нет тайм-аута)", ret == 4);
    CHECK("Сценарий 2: ответ с целочисленным результатом совпадает с ожидаемым",
          ret == 4 && nova_rpc_xdr_get_u32(result) == 0xCAFEF00D);

    {
        uint8_t result2[4];
        int ret2 = nova_rpc_transport_call(&t,
                                            NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                            NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                            NOVA_RPC_VIDEO_ToAgent_Run, NULL, 0,
                                            result2, sizeof(result2), 500);
        CHECK("Сценарий 11: тайм-аут честно возвращает -ETIMEDOUT, не висит и не лжёт", ret2 == -ETIMEDOUT);
    }

    {
        nova_rpc_transport_t t2;
        memset(&t2, 0, sizeof(t2));
        int ret3 = nova_rpc_transport_call(&t2, NOVA_RPC_T_BLOCK_MODE, 100, 0, 10,
                                            NULL, 0, NULL, 0, 100);
        CHECK("Сценарий 12: вызов на неоткрытом транспорте честно возвращает -ENODEV", ret3 == -ENODEV);
    }

    {
        int ret4 = nova_rpc_transport_call(&t, NOVA_RPC_T_NONBLOCK_MODE | NOVA_RPC_T_SEND_VIDEO_CPU,
                                            NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                            1, NULL, (size_t)(32u * 1024 * 1024), NULL, 0, 0);
        CHECK("Сценарий 7: слишком большой args_size отклонён -EMSGSIZE, не передан как есть", ret4 == -EMSGSIZE);
    }

    stop = 1;
    pthread_join(srv, NULL);
    pthread_join(disp, NULL);
    pthread_join(drain, NULL);

    nova_rpc_transport_close(&t);

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
