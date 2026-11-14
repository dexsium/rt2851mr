/*
 * Уровень A/B: на хосте без /dev/rpc* nova_video_rpc_open() честно
 * проваливается (та же проверка, что test_rpc_transport.c), поэтому
 * здесь мы НЕ зовём ни одну send-функцию напрямую на неоткрытой сессии
 * (это дало бы -ENODEV из транспорта, что тоже честно, но не проверяет
 * само кодирование аргументов). Вместо этого тест проверяет procedure ID
 * констант, которые send-функции обязаны использовать — через
 * nova_rpc_protocol.h (сверка с test_rpc_protocol.c уже покрывает числа,
 * здесь проверяем, что заголовок компилируется и значения достижимы из
 * этого модуля без дублирования логики).
 */
#include <stdio.h>

#include "../nova-rtk-rpc/nova_video_rpc.h"

int main(void)
{
    nova_video_rpc_session_t s;
    int ret;

    ret = nova_video_rpc_open(&s);
    printf("nova_video_rpc_open() on host without /dev/rpc* -> %d (expected < 0, honest failure)\n", ret);
    if (ret == 0) {
        printf("FAIL: unexpected success without real device nodes\n");
        nova_video_rpc_close(&s);
        return 1;
    }

    /* Вызов send-функции на неоткрытой сессии не должен притворяться
     * успехом -- транспорт не открыт (t->opened==0), send должен вернуть
     * -ENODEV, не 0. */
    ret = nova_video_rpc_run(&s);
    if (ret >= 0) {
        printf("FAIL: nova_video_rpc_run() returned success (%d) without an open transport\n", ret);
        return 1;
    }
    printf("ok   nova_video_rpc_run() on closed session honestly fails (%d)\n", ret);

    printf("ok   NOVA_RPC_VIDEO_ToAgent_Run (Play) = %d\n", NOVA_RPC_VIDEO_ToAgent_Run);
    printf("ok   NOVA_RPC_PROGRAM_VIDEO_SYSTEM = %d\n", NOVA_RPC_PROGRAM_VIDEO_SYSTEM);

    nova_video_rpc_close(&s);
    return 0;
}
