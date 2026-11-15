/*
 * ТЗ "RPC Transport Complete Closure" ЧАСТЬ V / п.26 — Performance
 * Benchmark. ОТДЕЛЬНЫЙ инструмент, не смешан с функциональными тестами
 * (не participates в PASS/FAIL CI gating — печатает таблицу измерений).
 *
 * ЧЕСТНАЯ ОГОВОРКА (прямое требование ТЗ §11/§13): это измерения
 * mock-транспорта на FIFO в x86_64 CI-контейнере (GitHub Actions
 * ubuntu-latest), НЕ показатели реального Realtek RPC-драйвера на
 * телевизоре. Числа характеризуют собственный код транспорта (XDR,
 * мьютексы, диспетчеризацию) и накладные расходы CI-окружения, НЕ
 * задержку настоящего межпроцессорного RPC на AVCPU.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_protocol.h"
#include "../nova-rtk-rpc/nova_rpc_transport.h"

static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: benchmark exceeded wall-clock budget\n";
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

#define POLL_STEP_MS 20
#define MAX_N 64

static ssize_t poll_read(int fd, void *buf, size_t cap, const atomic_int *stop)
{
    while (!*stop) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, POLL_STEP_MS);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
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
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL))) {
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

static ssize_t read_exact(int fd, void *buf, size_t n, const atomic_int *stop)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = poll_read(fd, (uint8_t *)buf + got, n - got, stop);
        if (r < 0) return -1;
        /* ТЗ "Realtek VDEC/VO & GStreamer" п.3.3: EOF и stop-выход от
         * poll_read() неотличимы по значению (оба 0) — в обоих случаях
         * ждать больше нечего; раньше EOF-без-stop уходил в continue
         * (бесконечный busy-spin на fd, который и дальше будет давать
         * EOF). Всегда возвращаем собранное. */
        if (r == 0) return (ssize_t)got;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t read_one_message(int fd, uint8_t *buf, size_t cap, const atomic_int *stop)
{
    nova_rpc_struct_t hdr;
    ssize_t n;
    size_t total;

    n = read_exact(fd, buf, NOVA_RPC_STRUCT_WIRE_SIZE, stop);
    if (n != (ssize_t)NOVA_RPC_STRUCT_WIRE_SIZE) return n;
    nova_rpc_xdr_decode_struct(buf, NOVA_RPC_STRUCT_WIRE_SIZE, &hdr);
    total = NOVA_RPC_STRUCT_WIRE_SIZE + hdr.parameter_size;
    if (total > cap) return -1;
    n = read_exact(fd, buf + NOVA_RPC_STRUCT_WIRE_SIZE, hdr.parameter_size, stop);
    if (n != (ssize_t)hdr.parameter_size) return -1;
    return (ssize_t)total;
}

struct echo_args {
    int rfd, wfd, n;
    atomic_int *stop;
};

static void *echo_server(void *arg)
{
    struct echo_args *a = (struct echo_args *)arg;
    int got = 0;

    while (got < a->n && !*a->stop) {
        uint8_t req[64];
        nova_rpc_struct_t req_hdr;
        uint8_t reply[NOVA_RPC_STRUCT_WIRE_SIZE + 8];
        nova_rpc_struct_t reply_hdr;
        ssize_t n = read_one_message(a->rfd, req, sizeof(req), a->stop);
        if (n < (ssize_t)(NOVA_RPC_STRUCT_WIRE_SIZE + 4))
            continue;
        nova_rpc_xdr_decode_struct(req, (size_t)n, &req_hdr);

        reply_hdr.program_id = NOVA_RPC_REPLYID;
        reply_hdr.version_id = NOVA_RPC_REPLYID;
        reply_hdr.procedure_id = 0;
        reply_hdr.task_id = 0;
        reply_hdr.sys_pid = 0;
        reply_hdr.parameter_size = 8;
        reply_hdr.context = req_hdr.context;
        nova_rpc_xdr_encode_struct(reply, sizeof(reply), &reply_hdr);
        nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE, req_hdr.task_id);
        nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE + 4, 0);
        write(a->wfd, reply, sizeof(reply));
        got++;
    }
    return NULL;
}

struct dispatch_args {
    nova_rpc_transport_t *t;
    int rfd;
    atomic_int *stop;
};

static void *dispatch_thread(void *arg)
{
    struct dispatch_args *a = (struct dispatch_args *)arg;
    uint8_t buf[256];
    while (!*a->stop) {
        ssize_t n = read_one_message(a->rfd, buf, sizeof(buf), a->stop);
        if (n > 0)
            nova_rpc_transport_dispatch_reply(a->t, buf, (size_t)n);
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

struct client_args {
    nova_rpc_transport_t *t;
    double latency_ms;
    int rc;
};

static void *client_thread(void *arg)
{
    struct client_args *c = (struct client_args *)arg;
    uint8_t args[4] = {0}, result[4];
    double t0 = now_ms();
    c->rc = nova_rpc_transport_call(c->t,
                                     NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                     NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                     42, args, sizeof(args), result, sizeof(result), 5000);
    c->latency_ms = now_ms() - t0;
    return NULL;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* Запускает N конкурентных клиентов, возвращает отсортированные
 * задержки (миллисекунды) в lat_out (должен вмещать N элементов) и
 * общую wall-clock длительность раунда (для RPS) в total_ms_out. */
static void run_round(nova_rpc_transport_t *t, int n, int wfd, int rfd,
                       double *lat_out, double *total_ms_out)
{
    pthread_t clients[MAX_N];
    struct client_args cargs[MAX_N];
    pthread_t srv;
    struct echo_args srv_args;
    atomic_int stop = 0;
    double t0;
    int i;

    srv_args.rfd = wfd; srv_args.wfd = rfd; srv_args.n = n; srv_args.stop = &stop;
    pthread_create(&srv, NULL, echo_server, &srv_args);

    t0 = now_ms();
    for (i = 0; i < n; i++) {
        cargs[i].t = t;
        pthread_create(&clients[i], NULL, client_thread, &cargs[i]);
    }
    for (i = 0; i < n; i++)
        pthread_join(clients[i], NULL);
    *total_ms_out = now_ms() - t0;

    stop = 1;
    pthread_join(srv, NULL);

    for (i = 0; i < n; i++)
        lat_out[i] = cargs[i].latency_ms;
    qsort(lat_out, (size_t)n, sizeof(double), cmp_double);
}

static double percentile(const double *sorted, int n, double p)
{
    double idx = p * (n - 1);
    int lo = (int)idx;
    int hi = lo + 1 < n ? lo + 1 : lo;
    double frac = idx - lo;
    return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

int main(void)
{
    char dir[256], w_path[256], r_path[256];
    nova_rpc_transport_t t;
    pthread_t disp;
    struct dispatch_args disp_args;
    atomic_int stop = 0;
    int ret, server_wfd, server_rfd;
    int sizes[5] = {1, 2, 8, 32, 64};
    int si;
    struct rusage ru_before, ru_after;
    double open_start, open_ms, close_start, close_ms;

    setvbuf(stdout, NULL, _IOLBF, 0);
    install_watchdog(60);

    snprintf(dir, sizeof(dir), "/tmp/nova_rpc_bench_%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(w_path, sizeof(w_path), "%s/wpoll_v", dir);
    snprintf(r_path, sizeof(r_path), "%s/rpoll_v", dir);
    mkfifo(w_path, 0600);
    mkfifo(r_path, 0600);

    {
        const char *rest[10] = { "wpoll_a", "wintr_a", "wintr_v", "wpoll_v2", "wintr_v2",
                                   "rpoll_a", "rintr_a", "rintr_v", "rpoll_v2", "rintr_v2" };
        const char *rest_env[10] = { "NOVA_RPC_DEV_WPOLL_A", "NOVA_RPC_DEV_WINTR_A", "NOVA_RPC_DEV_WINTR_V",
                                       "NOVA_RPC_DEV_WPOLL_V2", "NOVA_RPC_DEV_WINTR_V2",
                                       "NOVA_RPC_DEV_RPOLL_A", "NOVA_RPC_DEV_RINTR_A", "NOVA_RPC_DEV_RINTR_V",
                                       "NOVA_RPC_DEV_RPOLL_V2", "NOVA_RPC_DEV_RINTR_V2" };
        static char rest_paths[10][256];
        int i;
        setenv("NOVA_RPC_DEV_WPOLL_V", w_path, 1);
        setenv("NOVA_RPC_DEV_RPOLL_V", r_path, 1);
        for (i = 0; i < 10; i++) {
            char full[256];
            snprintf(full, sizeof(full), "%s/%s", dir, rest[i]);
            mkfifo(full, 0600);
            strcpy(rest_paths[i], full);
            setenv(rest_env[i], full, 1);
        }
        for (i = 0; i < 10; i++) {
            int fd_r = open(rest_paths[i], O_RDONLY | O_NONBLOCK);
            int fd_w = open(rest_paths[i], O_WRONLY | O_NONBLOCK);
            (void)fd_r; (void)fd_w;
        }
    }

    server_wfd = open(w_path, O_RDONLY | O_NONBLOCK);
    { int tmp_reader = open(r_path, O_RDONLY | O_NONBLOCK); (void)tmp_reader; }
    server_rfd = open(r_path, O_WRONLY | O_NONBLOCK);

    getrusage(RUSAGE_SELF, &ru_before);
    open_start = now_ms();
    ret = nova_rpc_transport_open(&t);
    open_ms = now_ms() - open_start;
    if (ret != 0) {
        printf("BENCH ABORTED: nova_rpc_transport_open() failed with %d\n", ret);
        return 1;
    }

    disp_args.t = &t; disp_args.rfd = t.rfd_poll_video; disp_args.stop = &stop;
    pthread_create(&disp, NULL, dispatch_thread, &disp_args);

    printf("=== Nova RPC Transport — Performance Benchmark ===\n");
    printf("Среда: GitHub Actions ubuntu-latest, x86_64, gcc (CROSS_COMPILE=), "
           "mock-транспорт на FIFO (НЕ реальный Realtek /dev/rtk_rpc, НЕ показатель "
           "производительности на телевизоре).\n");
    printf("Методика: round-trip задержка BLOCK_MODE-вызова "
           "nova_rpc_transport_call() от момента вызова до возврата, N "
           "конкурентных клиентских потоков на раунд, echo-mock-сервер "
           "отвечает сразу по получении. clock_gettime(CLOCK_MONOTONIC).\n\n");
    printf("open() транспорта: %.3f мс\n\n", open_ms);

    printf("%-6s %10s %10s %10s %10s %10s %12s\n",
           "N", "avg(ms)", "median", "p95", "p99", "max", "RPS");
    for (si = 0; si < 5; si++) {
        int n = sizes[si];
        double lat[MAX_N];
        double total_ms, sum = 0;
        int i;

        run_round(&t, n, server_wfd, server_rfd, lat, &total_ms);
        for (i = 0; i < n; i++) sum += lat[i];

        printf("%-6d %10.3f %10.3f %10.3f %10.3f %10.3f %12.1f\n",
               n, sum / n, percentile(lat, n, 0.50),
               percentile(lat, n, 0.95), percentile(lat, n, 0.99),
               lat[n - 1], (double)n / (total_ms / 1000.0));
    }

    getrusage(RUSAGE_SELF, &ru_after);
    printf("\nCPU-время процесса (getrusage, user+sys), за весь прогон: %.3f с\n",
           (double)(ru_after.ru_utime.tv_sec - ru_before.ru_utime.tv_sec) +
           (double)(ru_after.ru_utime.tv_usec - ru_before.ru_utime.tv_usec) / 1e6 +
           (double)(ru_after.ru_stime.tv_sec - ru_before.ru_stime.tv_sec) +
           (double)(ru_after.ru_stime.tv_usec - ru_before.ru_stime.tv_usec) / 1e6);
    printf("Пиковая резидентная память процесса (ru_maxrss): %ld КБ\n", ru_after.ru_maxrss);
    printf("Открытых fd на транспорт: 12 (6 write + 6 read, фиксировано структурой — "
           "не зависит от нагрузки)\n");
    printf("Потоков на транспорт (вне тестовой обвязки): 0 собственных — транспорт не "
           "создаёт потоки сам, вызывающий код сам управляет диспетчером "
           "(nova_rpc_transport_dispatch_reply) в своём потоке\n");

    stop = 1;
    pthread_join(disp, NULL);

    close_start = now_ms();
    nova_rpc_transport_close(&t);
    close_ms = now_ms() - close_start;
    printf("close() транспорта (без активных запросов): %.3f мс\n", close_ms);

    close(server_wfd);
    close(server_rfd);

    printf("\nОГОВОРКА: все числа выше — характеристики САМОГО транспорта "
           "(XDR, мьютексы, send_mutex, диспетчеризация) и mock FIFO в "
           "CI-контейнере. НЕ являются показателем задержки реального RPC к "
           "AVCPU на телевизоре — для этого нужен отдельный замер на "
           "аппаратном устройстве, который прямо запрещён ограничениями "
           "безопасности этого этапа (ТЗ §14).\n");

    return 0;
}
