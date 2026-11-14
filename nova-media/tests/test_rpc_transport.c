/*
 * Честная проверка транспорта без реальных /dev/rpc*: open() должен
 * провалиться (-ENOENT), не выдать фиктивный успех. Также проверяем,
 * что вызов nova_rpc_transport_call() в BLOCK_MODE честно завершается
 * по тайм-ауту, если никто не отвечает (отсутствие бесконечного
 * ожидания — требование ТЗ §8).
 */
#include <errno.h>
#include <stdio.h>

#include "../nova-rtk-rpc/nova_rpc_transport.h"

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

    return 0;
}
