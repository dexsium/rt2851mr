/*
 * Уровень A/B: на CI-хосте нет /dev/rpc0,1,4,5,8,9 (это устройства
 * реального ТВ) — поэтому этот тест ПРОВЕРЯЕТ ЧЕСТНОСТЬ транспорта:
 * nova_rpc_transport_open() должен вернуть реальную ошибку (-ENOENT и
 * т.п.), а не фиктивный успех (прямое требование ТЗ "не подменяй
 * реальные операции заглушками, возвращающими успех"). Это не тест
 * аппаратной отправки — HARDWARE_TESTED здесь не присваивается.
 */
#include <errno.h>
#include <stdio.h>

#include "../nova-rtk-rpc/nova_rpc_transport.h"

int main(void)
{
    nova_rpc_transport_t t;
    int ret = nova_rpc_transport_open(&t);

    if (ret == 0) {
        printf("UNEXPECTED: transport opened successfully on a host without "
               "/dev/rpc* -- this would mean a fake success, failing the test\n");
        nova_rpc_transport_close(&t);
        return 1;
    }

    printf("ok   nova_rpc_transport_open() honestly failed with %d on a host "
           "without /dev/rpc* (no fake success)\n", ret);

    /* signal_number() не требует открытого транспорта. */
    printf("ok   transport signal number = %d\n", nova_rpc_transport_signal_number());

    /* wait_reply с нулевым таймаутом не должно висеть вечно -- проверяем
     * отрицательный timeout_ms=50 (короткий бюджет) завершается с
     * -ETIMEDOUT, а не зависает (требование "отсутствие бесконечного
     * ожидания"). */
    ret = nova_rpc_transport_wait_reply(50);
    if (ret != -ETIMEDOUT) {
        printf("FAIL wait_reply(50) expected -ETIMEDOUT, got %d\n", ret);
        return 1;
    }
    printf("ok   wait_reply times out instead of blocking forever\n");

    return 0;
}
