/*
 * Mock/регрессионный тест (Уровень A/B): сверяет числовые ID в
 * nova_rpc_protocol.h с литеральными значениями, процитированными из
 * VideoRPC_System.h/VideoRPC_Agent.h в этой сессии (см. комментарии в
 * самом nova_rpc_protocol.h со ссылками на конкретные строки). Не
 * обращается к устройству, не требует AVCPU.
 */
#include <stdio.h>

#include "../nova-rtk-rpc/nova_rpc_protocol.h"

static int failures = 0;

#define CHECK(name, got, want) do { \
    if ((long)(got) != (long)(want)) { \
        printf("FAIL %-40s got=%ld want=%ld\n", name, (long)(got), (long)(want)); \
        failures++; \
    } else { \
        printf("ok   %-40s = %ld\n", name, (long)(got)); \
    } \
} while (0)

int main(void)
{
    /* Эталон — литералы из VideoRPC_System.h (ToAgent). */
    CHECK("VIDEO_ToAgent_Create",            NOVA_RPC_VIDEO_ToAgent_Create,            10);
    CHECK("VIDEO_ToAgent_Connect",           NOVA_RPC_VIDEO_ToAgent_Connect,            20);
    CHECK("VIDEO_ToAgent_InitRingBuffer",    NOVA_RPC_VIDEO_ToAgent_InitRingBuffer,     30);
    CHECK("VIDEO_ToAgent_Run",               NOVA_RPC_VIDEO_ToAgent_Run,                40);
    CHECK("VIDEO_ToAgent_Pause",             NOVA_RPC_VIDEO_ToAgent_Pause,              50);
    CHECK("VIDEO_ToAgent_Stop",              NOVA_RPC_VIDEO_ToAgent_Stop,               60);
    CHECK("VIDEO_ToAgent_Destroy",           NOVA_RPC_VIDEO_ToAgent_Destroy,            70);
    CHECK("VIDEO_ToAgent_Flush",             NOVA_RPC_VIDEO_ToAgent_Flush,              80);
    CHECK("VIDEO_ToAgent_VideoCreate",       NOVA_RPC_VIDEO_ToAgent_VideoCreate,       100);
    CHECK("VIDEO_ToAgent_DEC_Init",          NOVA_RPC_VIDEO_ToAgent_DEC_Init,         1020);
    CHECK("VIDEO_ToAgent_DEC_ParseResolution",NOVA_RPC_VIDEO_ToAgent_DEC_ParseResolution,1120);

    /* VO FILTER. */
    CHECK("VO_FILTER_ToAgent_Display",       NOVA_RPC_VO_FILTER_ToAgent_Display,      3260);
    CHECK("VO_FILTER_ToAgent_SetSpeed",      NOVA_RPC_VO_FILTER_ToAgent_SetSpeed,      3290);
    CHECK("VO_FILTER_ToAgent_ClearVideo",    NOVA_RPC_VO_FILTER_ToAgent_ClearVideo,   3340);
    CHECK("VO_FILTER_ToAgent_ConnectVDec",   NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec,  3390);

    /* ToSystem (VideoRPC_Agent.h). */
    CHECK("VIDEO_ToSystem_EndOfStream",      NOVA_RPC_VIDEO_ToSystem_EndOfStream,        2);
    CHECK("VIDEO_DEC_ToSystem_FatalError",   NOVA_RPC_VIDEO_DEC_ToSystem_FatalError,     8);
    CHECK("VIDEO_DEC_ToSystem_Deliver_FrameInfo", NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_FrameInfo, 18);
    CHECK("VIDEO_DEC_ToSystem_Deliver_PicInfo",   NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_PicInfo,   19);
    CHECK("VIDEO_DEC_ToSystem_Deliver_MediaInfo", NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_MediaInfo, 20);

    if (failures) {
        printf("%d RPC protocol ID mismatch(es)\n", failures);
        return 1;
    }
    printf("all RPC protocol IDs match cited source values\n");
    return 0;
}
