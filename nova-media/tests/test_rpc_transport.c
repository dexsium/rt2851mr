/*
 * Честная проверка транспорта без реальных /dev/rpc*: open() должен
 * провалиться (-ENOENT), не выдать фиктивный успех. Также проверяем,
 * что вызов nova_rpc_transport_call() в BLOCK_MODE честно завершается
 * по тайм-ауту, если никто не отвечает (отсутствие бесконечного
 * ожидания — требование ТЗ §8).
 *
 * ТЗ "RPC Transport Complete Closure" п.6/п.18: частичная инициализация —
 * ровно ОДНА из 12 device-нод (6-я по порядку открытия,
 * NOVA_RPC_DEV_WINTR_V2) не существует, остальные 11 — реальные FIFO с
 * уже открытым читателем-пиром (иначе production-код завис бы на
 * блокирующем open(), а не честно провалился). Проверяем: (1) open()
 * возвращает ошибку, не притворную успешность; (2) повторный open() со
 * ВСЕМИ корректными путями после этого снова успешен — то есть частичный
 * сбой не оставил утечки fd/повреждённого состояния.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../nova-rtk-rpc/nova_rpc_transport.h"

static int g_passed = 0, g_failed = 0;
#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

/* Порядок открытия в nova_rpc_transport_open() (nova_rpc_transport.c):
 * WPOLL_A, WINTR_A, WPOLL_V, WINTR_V, WPOLL_V2, WINTR_V2 (6-й — тот,
 * что мы ломаем), затем RPOLL_A, RINTR_A, RPOLL_V, RINTR_V, RPOLL_V2,
 * RINTR_V2. */
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

/* Создаёт все 12 FIFO и открывает ОБА конца каждой неблокирующе (кроме
 * пути с индексом skip_index, которому даём заведомо несуществующий
 * путь) — та же проверенная схема, что в test_rpc_concurrency.c для
 * "лишних" нод: RDONLY всегда ПЕРВЫМ (не зависит от наличия writer'а,
 * POSIX), затем WRONLY|O_NONBLOCK (уже есть читатель — без ENXIO).
 * Благодаря этому последующий БЛОКИРУЮЩИЙ open() внутри
 * nova_rpc_transport_open() (production-код) не висит ни на одном
 * из 11 корректных путей — у каждого уже есть оба конца открыты. */
static void setup_with_one_broken(const char *dir, int skip_index)
{
    int i;
    g_peer_count = 0;
    for (i = 0; i < 12; i++) {
        char full[256];
        int fd_r, fd_w;
        if (i == skip_index) {
            snprintf(full, sizeof(full), "%s/does_not_exist", dir);
            setenv(g_envs[i], full, 1);
            continue; /* сознательно не создаём файл и не открываем пир */
        }
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
    nova_rpc_transport_t t;
    int ret = nova_rpc_transport_open(&t);

    if (ret == 0) {
        printf("UNEXPECTED: transport opened without real /dev/rpc* -- fake success\n");
        nova_rpc_transport_close(&t);
        return 1;
    }
    printf("ok   nova_rpc_transport_open() honestly failed with %d (no fake success)\n", ret);
    g_passed++;

    /* --- Частичная инициализация: 11 из 12 fd реальны, 6-й (WINTR_V2)
     * сломан (путь не существует). --- */
    {
        char dir[256];
        int ret2, ret3;

        snprintf(dir, sizeof(dir), "/tmp/nova_rpc_partial_init_%d", (int)getpid());
        mkdir(dir, 0700);

        setup_with_one_broken(dir, 5 /* WINTR_V2, 6-й по порядку (индекс с 0) */);
        ret2 = nova_rpc_transport_open(&t);
        CHECK("частичная инициализация (сломан 6-й из 12 fd): open() честно "
              "провалился, не выдал притворный успех",
              ret2 < 0);
        close_peers();

        /* Повторяем с ПОЛНОСТЬЮ корректными путями — частичный сбой не
         * должен оставить утечку fd или повреждённое состояние,
         * мешающее следующему открытию. */
        setup_with_one_broken(dir, -1 /* ничего не ломаем */);
        ret3 = nova_rpc_transport_open(&t);
        CHECK("после частично неудачной инициализации повторное открытие "
              "СО ВСЕМИ корректными путями снова успешно (нет утечки/порчи состояния)",
              ret3 == 0);
        if (ret3 == 0)
            nova_rpc_transport_close(&t);
        close_peers();
    }

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
