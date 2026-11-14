/* Та же честная проверка, что test_video_rpc.c, для VO FILTER команд. */
#include <stdio.h>

#include "../nova-rtk-rpc/nova_vo_rpc.h"

int main(void)
{
    nova_vo_rpc_session_t s;
    int ret;

    ret = nova_vo_rpc_open(&s);
    printf("nova_vo_rpc_open() on host without /dev/rpc* -> %d (expected < 0)\n", ret);
    if (ret == 0) {
        printf("FAIL: unexpected success without real device nodes\n");
        nova_vo_rpc_close(&s);
        return 1;
    }

    ret = nova_vo_rpc_connect_vdec(&s, 1, 0);
    if (ret >= 0) {
        printf("FAIL: connect_vdec() returned success (%d) without an open transport\n", ret);
        return 1;
    }
    printf("ok   nova_vo_rpc_connect_vdec() on closed session honestly fails (%d)\n", ret);

    printf("ok   NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec = %d\n", NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec);

    nova_vo_rpc_close(&s);
    return 0;
}
