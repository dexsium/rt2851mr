/*
 * Тест конкурентности ПРОИЗВОДСТВЕННОГО кода nova_rpc_transport_call()/
 * dispatch_reply() — реальные потоки, реальный транспорт (FIFO-mock),
 * без упрощённой параллельной реализации.
 *
 * Схема: каждый поток-"клиент" отправляет свой запрос с УНИКАЛЬНЫМ
 * 4-байтовым значением в args и получает его же обратно как результат
 * (сервер-эхо просто копирует payload обратно) — если бы context/слоты
 * где-то перепутались между потоками, конкретный поток получил бы ЧУЖОЕ
 * значение, что эта проверка ловит напрямую, не по общим счётчикам.
 *
 * Сервер-эхо собирает ровно N запросов, затем отвечает в заданном
 * порядке (forward/reverse/random) — чтобы отдельно проверить сценарии
 * "ответы в обратном порядке"/"в случайном порядке" (ТЗ §4).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h> /* atomic_int для stop-флагов — найдено TSan, см. test_rpc_e2e.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Аппаратный предохранитель от зависания (найден реально, не гипотетически:
 * на CI run 43635651464 этот тест/соседний job провисел в "in progress"
 * более двух часов до принудительной отмены — точная причина не
 * локализована построчно в этой сессии при статическом анализе, см.
 * честную запись в итоговом отчёте). SIGALRM гарантированно завершает
 * процесс ненулевым кодом вместо бесконечного/многочасового зависания —
 * превращает скрытый deadlock в быстрый, явный провал теста, а не в
 * растраченные часы CI. Не маскирует проблему успешным выходом.
 */
static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: test exceeded 60s wall-clock budget, aborting (likely deadlock, see NOVA_RPC_REPLY_PATH_AUDIT.md)\n";
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

#define POLL_STEP_MS 20
#define MAX_N 65

static ssize_t poll_read(int fd, void *buf, size_t cap, const atomic_int *stop)
{
    while (!*stop) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, POLL_STEP_MS);
        if (pr > 0 && (pfd.revents & POLLIN))
            return read(fd, buf, cap);
        if (pr < 0 && errno != EINTR)
            return -1;
    }
    return 0;
}

/*
 * РЕАЛЬНЫЙ баг, найденный именно этим тестом (не гипотеза): с несколькими
 * конкурентными писателями в один и тот же FIFO один вызов read() с
 * буфером БОЛЬШЕ размера одного сообщения может захватить байты ДВУХ (или
 * более) сообщений сразу, что ломает разбор совершенно непредсказуемо —
 * именно это вызывало FAIL во всех сценариях 2/8/32/64 на предыдущем
 * прогоне CI (run 43647629010, до этого исправления). Решение —
 * буквально то же самое, что делает оригинальный `RPCProxy_loop()`
 * (`RPCProxy.c:88-97`, предыдущие этапы): читать РОВНО `sizeof(RPC_STRUCT)`
 * байт заголовка, затем РОВНО `parameterSize` байт полезной нагрузки —
 * двумя отдельными точными чтениями, не одним произвольного размера.
 */
static ssize_t read_exact(int fd, void *buf, size_t n, const atomic_int *stop)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = poll_read(fd, (uint8_t *)buf + got, n - got, stop);
        if (r < 0)
            return -1;
        if (r == 0) {
            if (*stop)
                return (ssize_t)got;
            continue; /* poll_read() вернул 0 из-за stop-проверки внутри своего цикла — не EOF */
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Читает ровно одно RPC-сообщение (заголовок + его реальный payload) в
 * буфер `buf` (должен быть достаточного размера), возвращает общий размер
 * или -1/0 при ошибке/остановке. */
static ssize_t read_one_message(int fd, uint8_t *buf, size_t cap, const atomic_int *stop)
{
    nova_rpc_struct_t hdr;
    ssize_t n;
    size_t total;

    n = read_exact(fd, buf, NOVA_RPC_STRUCT_WIRE_SIZE, stop);
    if (n != (ssize_t)NOVA_RPC_STRUCT_WIRE_SIZE)
        return n;

    nova_rpc_xdr_decode_struct(buf, NOVA_RPC_STRUCT_WIRE_SIZE, &hdr);
    total = NOVA_RPC_STRUCT_WIRE_SIZE + hdr.parameter_size;
    if (total > cap)
        return -1; /* не должно происходить в этом тесте, но не читаем за границу буфера */

    n = read_exact(fd, buf + NOVA_RPC_STRUCT_WIRE_SIZE, hdr.parameter_size, stop);
    if (n != (ssize_t)hdr.parameter_size)
        return -1;

    return (ssize_t)total;
}

enum reply_order { ORDER_FORWARD, ORDER_REVERSE, ORDER_RANDOM };

struct captured_req {
    nova_rpc_struct_t hdr;
    uint32_t echo_value;
};

struct echo_n_args {
    int rfd_write_side; /* "серверная" сторона video-poll write-FIFO (RDONLY) */
    int wfd_read_side;  /* "серверная" сторона video-poll read-FIFO (WRONLY) */
    int n;
    enum reply_order order;
    atomic_int *stop;
};

static void *echo_n_server(void *arg)
{
    struct echo_n_args *a = (struct echo_n_args *)arg;
    struct captured_req reqs[MAX_N];
    int got = 0;
    int order[MAX_N];
    int i;

    while (got < a->n && !*a->stop) {
        uint8_t req[64];
        ssize_t n = read_one_message(a->rfd_write_side, req, sizeof(req), a->stop);
        if (n < (ssize_t)(NOVA_RPC_STRUCT_WIRE_SIZE + 4))
            continue;
        nova_rpc_xdr_decode_struct(req, (size_t)n, &reqs[got].hdr);
        reqs[got].echo_value = nova_rpc_xdr_get_u32(req + NOVA_RPC_STRUCT_WIRE_SIZE);
        got++;
    }

    for (i = 0; i < got; i++)
        order[i] = i;

    if (a->order == ORDER_REVERSE) {
        for (i = 0; i < got; i++)
            order[i] = got - 1 - i;
    } else if (a->order == ORDER_RANDOM) {
        for (i = got - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
    }

    for (i = 0; i < got; i++) {
        int idx = order[i];
        uint8_t reply[NOVA_RPC_STRUCT_WIRE_SIZE + 8];
        nova_rpc_struct_t reply_hdr;

        reply_hdr.program_id = NOVA_RPC_REPLYID;
        reply_hdr.version_id = NOVA_RPC_REPLYID;
        reply_hdr.procedure_id = 0;
        reply_hdr.task_id = 0;
        reply_hdr.sys_pid = 0;
        reply_hdr.parameter_size = 8;
        reply_hdr.context = reqs[idx].hdr.context;

        nova_rpc_xdr_encode_struct(reply, sizeof(reply), &reply_hdr);
        nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE, reqs[idx].hdr.task_id);
        nova_rpc_xdr_put_u32(reply + NOVA_RPC_STRUCT_WIRE_SIZE + 4, reqs[idx].echo_value);

        write(a->wfd_read_side, reply, sizeof(reply));
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

struct client_args {
    nova_rpc_transport_t *t;
    uint32_t value;
    int ok; /* 1, если результат совпал с отправленным значением */
    int rc; /* возврат nova_rpc_transport_call() */
};

static void *client_thread(void *arg)
{
    struct client_args *c = (struct client_args *)arg;
    uint8_t args[4], result[4];

    nova_rpc_xdr_put_u32(args, c->value);
    c->rc = nova_rpc_transport_call(c->t,
                                     NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                     NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                     42, args, sizeof(args), result, sizeof(result), 5000);
    c->ok = (c->rc == 4 && nova_rpc_xdr_get_u32(result) == c->value);
    return NULL;
}

/* Запускает N конкурентных клиентов против транспорта `t` с заданным
 * порядком ответов сервера. Возвращает число клиентов, получивших
 * СВОЁ (не чужое и не поддельное) значение обратно. */
static int run_n_concurrent(nova_rpc_transport_t *t, int n, enum reply_order order,
                             int wfd_write_side, int rfd_read_side)
{
    pthread_t clients[MAX_N];
    struct client_args cargs[MAX_N];
    pthread_t srv;
    struct echo_n_args srv_args;
    atomic_int stop = 0;
    int i, ok_count = 0;

    srv_args.rfd_write_side = wfd_write_side;
    srv_args.wfd_read_side = rfd_read_side;
    srv_args.n = n;
    srv_args.order = order;
    srv_args.stop = &stop;
    pthread_create(&srv, NULL, echo_n_server, &srv_args);

    for (i = 0; i < n; i++) {
        cargs[i].t = t;
        cargs[i].value = 0x1000u + (uint32_t)i;
        pthread_create(&clients[i], NULL, client_thread, &cargs[i]);
    }

    for (i = 0; i < n; i++)
        pthread_join(clients[i], NULL);

    stop = 1;
    pthread_join(srv, NULL);

    for (i = 0; i < n; i++)
        if (cargs[i].ok)
            ok_count++;

    return ok_count;
}

int main(void)
{
    char dir[256];
    char w_path[256], r_path[256];
    nova_rpc_transport_t t;
    pthread_t disp;
    struct dispatch_args disp_args;
    atomic_int stop = 0;
    int ret;
    int server_wfd, server_rfd;

    install_watchdog(60);
    srand(12345); /* детерминированный seed — тест воспроизводим */

    snprintf(dir, sizeof(dir), "/tmp/nova_rpc_conc_%d", (int)getpid());
    mkdir(dir, 0700);
    snprintf(w_path, sizeof(w_path), "%s/wpoll_v", dir);
    snprintf(r_path, sizeof(r_path), "%s/rpoll_v", dir);
    mkfifo(w_path, 0600);
    mkfifo(r_path, 0600);

    /* Остальные 10 нод транспорту всё равно нужны открытыми — создаём и
     * сразу открываем оба конца без дальнейшего использования. */
    {
        /* rest[i] и rest_env[i] — соответствующая пара имя/переменная,
         * явно выписаны по порядку (без формулы-сопоставления, чтобы не
         * рисковать рассинхронизацией индексов). */
        const char *rest[10]     = { "wpoll_a", "wintr_a", "wintr_v", "wpoll_v2", "wintr_v2",
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
        /* Открываем ОБА конца каждого "лишнего" FIFO неблокирующе, чтобы
         * будущий БЛОКИРУЮЩИЙ open() внутри nova_rpc_transport_open() не
         * завис навечно. ВАЖНО (реальный баг, найденный именно этим
         * тестом через настоящее многочасовое зависание CI — не
         * гипотеза): O_WRONLY|O_NONBLOCK на FIFO без читателя завершается
         * с ENXIO НЕМЕДЛЕННО (POSIX), а не блокируется и не ждёт — первая
         * версия этого кода открывала WRONLY ДО RDONLY для "read-типа"
         * путей, из-за чего writer-сторона никогда реально не
         * устанавливалась, и последующий БЛОКИРУЮЩИЙ open(path, O_RDONLY)
         * внутри production-кода (nova_rpc_transport_open) висел
         * бесконечно, ожидая writer'а, которого не было. Порядок здесь
         * исправлен: RDONLY открывается ПЕРВЫМ для каждого пути (это
         * всегда успевает немедленно независимо от наличия writer'а),
         * ТОЛЬКО ПОСЛЕ этого пробуем WRONLY (к этому моменту читатель уже
         * есть, поэтому ENXIO не возникает). */
        for (i = 0; i < 10; i++) {
            int fd_r = open(rest_paths[i], O_RDONLY | O_NONBLOCK);
            int fd_w = open(rest_paths[i], O_WRONLY | O_NONBLOCK);
            (void)fd_r;
            (void)fd_w;
        }
    }

    server_wfd = open(w_path, O_RDONLY | O_NONBLOCK);
    /* Та же ловушка, что была исправлена выше: открывать r_path WRONLY
     * раньше, чем у него появится хоть один читатель, привело бы к
     * немедленному ENXIO. Production-код сам станет читателем r_path
     * чуть позже (nova_rpc_transport_open), но ОН открывает её БЛОКИРУЮЩЕ
     * и ждёт именно writer'а — поэтому здесь сначала создаём временного
     * читателя (оставляем открытым, лишний читатель на FIFO безвреден),
     * и только потом открываем writer-сторону. */
    {
        int tmp_reader = open(r_path, O_RDONLY | O_NONBLOCK);
        (void)tmp_reader;
    }
    server_rfd = open(r_path, O_WRONLY | O_NONBLOCK);

    ret = nova_rpc_transport_open(&t);
    CHECK("transport open succeeds for concurrency test", ret == 0);
    if (ret != 0) {
        printf("%d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }

    disp_args.t = &t;
    disp_args.rfd = t.rfd_poll_video;
    disp_args.stop = &stop;
    pthread_create(&disp, NULL, dispatch_thread, &disp_args);

    CHECK("2 параллельных запроса: оба получили СВОЙ результат (forward)",
          run_n_concurrent(&t, 2, ORDER_FORWARD, server_wfd, server_rfd) == 2);
    CHECK("8 параллельных запросов: все получили СВОЙ результат (reverse)",
          run_n_concurrent(&t, 8, ORDER_REVERSE, server_wfd, server_rfd) == 8);
    CHECK("32 параллельных запроса: все получили СВОЙ результат (random)",
          run_n_concurrent(&t, 32, ORDER_RANDOM, server_wfd, server_rfd) == 32);
    CHECK("64 параллельных запроса (заполняют всю таблицу pending[64]): все получили СВОЙ результат",
          run_n_concurrent(&t, 64, ORDER_RANDOM, server_wfd, server_rfd) == 64);

    /* 65-й запрос при полностью занятой таблице: блокируем все 64 слота
     * НЕ дав серверу ответить (чтобы они оставались WAITING), затем
     * пробуем выделить 65-й — должен получить -ENOSPC от транспорта. */
    {
        pthread_t blockers[64];
        struct client_args bargs[64];
        int i;
        uint8_t args[4], result[4];
        int rc65;

        for (i = 0; i < 64; i++) {
            bargs[i].t = &t;
            bargs[i].value = 0x2000u + (uint32_t)i;
            /* таймаут подольше, чем время проверки 65-го — никто не
             * отвечает этим 64 вообще (сервер не запущен для них) */
        }
        /* Отправляем 64 BLOCK_MODE-вызова из 64 потоков, каждый займёт
         * один слот и будет ждать (никто не ответит) — используем
         * прямой вызов без обёртки client_thread с меньшим таймаутом,
         * чтобы тест не висел долго. */
        for (i = 0; i < 64; i++) {
            pthread_create(&blockers[i], NULL, client_thread, &bargs[i]);
        }
        /* небольшая пауза, чтобы все 64 успели дойти до nova_rpc_pending_alloc()
         * и реально занять слоты до попытки 65-го */
        usleep(300 * 1000);

        nova_rpc_xdr_put_u32(args, 0xFFFF);
        rc65 = nova_rpc_transport_call(&t,
                                        NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                        NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                        42, args, sizeof(args), result, sizeof(result), 100);
        CHECK("65-й конкурентный запрос при полностью занятой таблице отклонён -ENOSPC",
              rc65 == -ENOSPC);

        for (i = 0; i < 64; i++)
            pthread_join(blockers[i], NULL);
        /* эти 64 сами завершатся по тайм-ауту (никто не ответил) —
         * их client_args.rc будет -ETIMEDOUT, что ожидаемо и безопасно
         * освобождает все 64 слота обратно. */

        /* ЧЕСТНАЯ НАХОДКА: эти 64 запроса реально ушли в wpoll_v (никто
         * их не прочитал, т.к. echo_n_server в этом блоке не запускался)
         * — их байты остаются в ядерном буфере FIFO. Без явного дренажа
         * следующий echo_n_server прочитал бы ИХ первыми (в порядке
         * FIFO), спутав со свежими запросами финальной проверки — именно
         * так и провалился предыдущий прогон CI (run 43648070639).
         * Дренируем неблокирующим чтением, пока есть данные. */
        {
            /* server_wfd уже открыт O_NONBLOCK (см. открытие ниже в main) —
             * read() на пустой неблокирующей FIFO вернёт -1/EAGAIN, не
             * заблокируется. */
            uint8_t junk[256];
            while (read(server_wfd, junk, sizeof(junk)) > 0) { }
        }
    }

    /* Повторная отправка после освобождения слота — таблица снова пуста
     * после предыдущего блока (все 64 тайм-аута освободили свои слоты),
     * и FIFO очищена от непрочитанных старых запросов (см. выше). */
    CHECK("Повторный запрос после освобождения слотов снова проходит успешно",
          run_n_concurrent(&t, 2, ORDER_FORWARD, server_wfd, server_rfd) == 2);

    stop = 1;
    pthread_join(disp, NULL);
    close(server_wfd);
    close(server_rfd);
    nova_rpc_transport_close(&t);

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
