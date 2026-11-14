/*
 * Сквозной (end-to-end) mock-тест ПРОИЗВОДСТВЕННОГО кода
 * (nova_rpc_transport_open/call/dispatch_reply) — не упрощённой
 * параллельной реализации. Вместо реальных /dev/rpc* используются 12
 * FIFO (mkfifo), пути которых передаются транспорту через переменные
 * окружения NOVA_RPC_DEV_* (см. nova_rpc_transport.c) — на реальном
 * устройстве эти переменные не заданы, и используются настоящие пути.
 *
 * ЧЕСТНАЯ ОГОВОРКА: `nova_rpc_transport_register()` (ioctl
 * RPC_IOCTHANDLER) НЕ покрыт этим e2e-тестом — FIFO не поддерживают
 * ioctl (вернули бы ENOTTY), а эмуляция настоящего символьного
 * устройства с ioctl-семантикой вне рамок этой сессии. Покрыта
 * ОСНОВНАЯ, самая важная для этого ТЗ часть — реальный путь
 * отправки+приёма ответа (`nova_rpc_transport_call`).
 *
 * Схема теста:
 *   mock-поток A: открывает "серверную" сторону video-poll write-FIFO на
 *     чтение, декодирует запрос, формирует ответ (REPLYID/REPLYID,
 *     context=эхо, payload=[task_id][4-байтовый результат]) и пишет его
 *     в "серверную" сторону video-poll read-FIFO.
 *   mock-поток B (аналог RPCProxy_loop): блокирующе читает СОБСТВЕННЫЙ
 *     read-fd транспорта и прогоняет каждое сообщение через
 *     nova_rpc_transport_dispatch_reply() — именно так ответ долетает до
 *     ожидающего nova_rpc_transport_call().
 *   главный поток: nova_rpc_transport_call(..., BLOCK_MODE, ...) и
 *     проверяет, что получил ОЖИДАЕМЫЙ результат, а не тайм-аут.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_protocol.h"
#include "../nova-rtk-rpc/nova_rpc_transport.h"

static char g_dir[256];
static int g_passed = 0, g_failed = 0;

#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

static void mkpath(char *out, const char *name)
{
    snprintf(out, 256, "%s/%s", g_dir, name);
}

struct echo_server_args {
    char w_path[256]; /* "серверная" сторона — сторона теста того, что Nova пишет */
    char r_path[256]; /* "серверная" сторона — сторона теста того, что Nova читает */
    uint32_t fake_result;
    volatile int *stop;
};

/*
 * Отвечает ТОЛЬКО на первый полученный запрос (сценарии 1-2), затем
 * продолжает читать (и отбрасывать) дальнейшие запросы без ответа —
 * имитирует "AVCPU принял команду, но не ответил" для честной проверки
 * тайм-аута (сценарий 11). Держит `wfd` открытым до конца теста: если
 * закрыть его после первого ответа, второй write() производственного
 * кода в тот же FIFO получил бы EPIPE (нет читателя), а не настоящий
 * тайм-аут — это была реальная ошибка первой версии этого теста,
 * исправленная по результатам первого прогона в CI.
 */
static void *echo_server_thread(void *arg)
{
    struct echo_server_args *a = (struct echo_server_args *)arg;
    int wfd = open(a->w_path, O_RDONLY);
    int rfd = open(a->r_path, O_WRONLY);
    uint8_t req[256];
    int answered = 0;

    if (wfd < 0 || rfd < 0) {
        fprintf(stderr, "echo_server: open failed\n");
        return NULL;
    }

    while (!*a->stop) {
        ssize_t n = read(wfd, req, sizeof(req));
        nova_rpc_struct_t hdr;

        if (n < (ssize_t)NOVA_RPC_STRUCT_WIRE_SIZE)
            continue;

        if (answered)
            continue; /* сценарий 11: получено, но намеренно не отвечаем */

        nova_rpc_xdr_decode_struct(req, (size_t)n, &hdr);

        {
            uint8_t reply[NOVA_RPC_STRUCT_WIRE_SIZE + 8];
            nova_rpc_struct_t reply_hdr;

            reply_hdr.program_id = NOVA_RPC_REPLYID;
            reply_hdr.version_id = NOVA_RPC_REPLYID;
            reply_hdr.procedure_id = 0;
            reply_hdr.task_id = 0;
            reply_hdr.sys_pid = 0;
            reply_hdr.parameter_size = 8; /* 4 (эхо task_id запроса) + 4 (результат) */
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
    int rfd; /* собственный read-fd транспорта (не серверная сторона!) */
    volatile int *stop;
};

static void *dispatch_loop_thread(void *arg)
{
    struct dispatch_loop_args *a = (struct dispatch_loop_args *)arg;
    uint8_t buf[256];

    while (!*a->stop) {
        ssize_t n = read(a->rfd, buf, sizeof(buf));
        if (n > 0)
            nova_rpc_transport_dispatch_reply(a->t, buf, (size_t)n);
    }
    return NULL;
}

/* Открывает "второй конец" для фифо, который production-код сам не
 * использует в этом тесте (все, кроме video-poll) — просто чтобы
 * nova_rpc_transport_open() не заблокировался навечно на других 10 fifo. */
static void *drain_thread(void *arg)
{
    char **paths = (char **)arg;
    int fds[10];
    int i;

    for (i = 0; i < 5; i++) fds[i] = open(paths[i], O_RDONLY);       /* остальные write-FIFO */
    for (i = 5; i < 10; i++) fds[i] = open(paths[i], O_WRONLY);      /* остальные read-FIFO */
    /* держим открытыми до конца процесса — закрываются при выходе */
    (void)fds;
    return NULL;
}

int main(void)
{
    nova_rpc_transport_t t;
    pthread_t srv, disp, drain;
    volatile int stop = 0;
    struct echo_server_args srv_args;
    struct dispatch_loop_args disp_args;
    int ret;
    uint8_t result[4];

    snprintf(g_dir, sizeof(g_dir), "/tmp/nova_rpc_e2e_%d", (int)getpid());
    mkdir(g_dir, 0700);

    {
        const char *names[12] = {
            "wpoll_a", "wintr_a", "wpoll_v", "wintr_v", "wpoll_v2", "wintr_v2",
            "rpoll_a", "rintr_a", "rpoll_v", "rintr_v", "rpoll_v2", "rintr_v2",
        };
        char paths[12][256];
        const char *envs[12] = {
            "NOVA_RPC_DEV_WPOLL_A", "NOVA_RPC_DEV_WINTR_A", "NOVA_RPC_DEV_WPOLL_V", "NOVA_RPC_DEV_WINTR_V",
            "NOVA_RPC_DEV_WPOLL_V2", "NOVA_RPC_DEV_WINTR_V2", "NOVA_RPC_DEV_RPOLL_A", "NOVA_RPC_DEV_RINTR_A",
            "NOVA_RPC_DEV_RPOLL_V", "NOVA_RPC_DEV_RINTR_V", "NOVA_RPC_DEV_RPOLL_V2", "NOVA_RPC_DEV_RINTR_V2",
        };
        int i;

        for (i = 0; i < 12; i++) {
            mkpath(paths[i], names[i]);
            mkfifo(paths[i], 0600);
            setenv(envs[i], paths[i], 1);
        }

        strcpy(srv_args.w_path, paths[2]);  /* wpoll_v */
        strcpy(srv_args.r_path, paths[8]);  /* rpoll_v */
        srv_args.fake_result = 0xCAFEF00D;
        srv_args.stop = &stop;

        {
            /* static — обязательно: drain_thread читает этот массив из
             * отдельного потока, который может пережить возврат из этого
             * блока; обычный локальный массив указателей здесь привёл бы
             * к use-after-scope (поймано ASan в CI, см. отчёт). */
            static char *drain_paths[10];
            static char storage[10][256];
            int idx = 0, j;
            for (j = 0; j < 12; j++) {
                if (j == 2 || j == 8) continue; /* video-poll обрабатываются отдельно */
                strcpy(storage[idx], paths[j]);
                drain_paths[idx] = storage[idx];
                idx++;
            }
            pthread_create(&drain, NULL, drain_thread, drain_paths);
        }
    }

    pthread_create(&srv, NULL, echo_server_thread, &srv_args);

    ret = nova_rpc_transport_open(&t);
    CHECK("nova_rpc_transport_open() succeeds against mock FIFOs", ret == 0);
    if (ret != 0) {
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

    /* Сценарий 11: тайм-аут — второй вызов, сервер не отвечает (поток
     * echo_server уже завершился после первого ответа). */
    {
        uint8_t result2[4];
        int ret2 = nova_rpc_transport_call(&t,
                                            NOVA_RPC_T_BLOCK_MODE | NOVA_RPC_T_USE_POLL_BUF | NOVA_RPC_T_SEND_VIDEO_CPU,
                                            NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                            NOVA_RPC_VIDEO_ToAgent_Run, NULL, 0,
                                            result2, sizeof(result2), 300);
        CHECK("Сценарий 11: тайм-аут честно возвращает -ETIMEDOUT, не висит и не лжёт", ret2 == -ETIMEDOUT);
    }

    /* Сценарий 12: отсутствие RPC-устройства — отдельный транспорт без open(). */
    {
        nova_rpc_transport_t t2;
        memset(&t2, 0, sizeof(t2));
        int ret3 = nova_rpc_transport_call(&t2, NOVA_RPC_T_BLOCK_MODE, 100, 0, 10,
                                            NULL, 0, NULL, 0, 100);
        CHECK("Сценарий 12: вызов на неоткрытом транспорте честно возвращает -ENODEV", ret3 == -ENODEV);
    }

    /* Сценарий 7: неверный размер XDR-данных (переполнение) отклоняется. */
    {
        int ret4 = nova_rpc_transport_call(&t, NOVA_RPC_T_NONBLOCK_MODE | NOVA_RPC_T_SEND_VIDEO_CPU,
                                            NOVA_RPC_PROGRAM_VIDEO_SYSTEM, NOVA_RPC_VERSION_VIDEO,
                                            1, NULL, (size_t)(32u * 1024 * 1024), NULL, 0, 0);
        CHECK("Сценарий 7: слишком большой args_size отклонён -EMSGSIZE, не передан как есть", ret4 == -EMSGSIZE);
    }

    stop = 1;
    /* echo_server/dispatch_loop/drain держат блокирующие read() на FIFO
     * без новых данных — не join'им их (ни один из них проверит `stop`
     * до следующего пробуждения из блокирующего read(), так что join
     * мог бы зависнуть навечно); процесс завершается следующей строкой,
     * ядро закрывает все FIFO само. Не считается утечкой ресурсов теста. */

    nova_rpc_transport_close(&t);

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
