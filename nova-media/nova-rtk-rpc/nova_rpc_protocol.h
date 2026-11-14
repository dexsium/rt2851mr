/*
 * nova_rpc_protocol.h — таблица подтверждённых числовых идентификаторов
 * Realtek Video RPC протокола (программа "Video", ARM↔AVCPU).
 *
 * ВСЕ значения ниже скопированы буквально из открытых GPL-заголовков,
 * сгенерированных rpcgen-подобным инструментом из .x-описания интерфейса
 * (репозиторий TCLOpenSource/rt2851mr, форк dexsium/rt2851mr, commit
 * 80a2688976ac8f67b73ebfd5da5e55f3f09a472f):
 *
 *   ToAgent (ARM → AVCPU, команды управления):
 *     kernel/drivers/rtk_kdriver/common/IPC/generate/include/video/VideoRPC_System.h
 *
 *   ToSystem (AVCPU → ARM, callback/уведомления):
 *     kernel/drivers/rtk_kdriver/common/IPC/generate/include/video/VideoRPC_Agent.h
 *
 *   Структуры аргументов:
 *     .../generate/include/video/VideoRPC_System_data.h
 *     .../generate/include/video/VideoRPC_Agent_data.h
 *
 * Ни одно из чисел здесь не придумано по названию функции (прямое
 * требование ТЗ "не придумывай числовые значения команд и структуры по их
 * названиям") — каждое значение процитировано из конкретной строки файла,
 * это задокументировано в комментарии к каждой константе ниже.
 */
#ifndef NOVA_RPC_PROTOCOL_H
#define NOVA_RPC_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== Program/Version ID (CONFIRMED, не диапазон) =====================
 * VideoRPC_System.h:22-23 и VideoRPC_Agent.h:22-23 — точные #define, не
 * предположение по диапазону из readme.txt (та таблица описывает диапазон
 * ПРОЦЕДУРНЫХ id внутри отдела, не programID самой RPC-программы). */
#define NOVA_RPC_PROGRAM_VIDEO_SYSTEM 100  /* VideoRPC_System.h:22 — ToAgent (мы вызываем AVCPU) */
#define NOVA_RPC_PROGRAM_VIDEO_AGENT  300  /* VideoRPC_Agent.h:22 — ToSystem (AVCPU вызывает нас, мы — сервер) */
#define NOVA_RPC_VERSION_VIDEO        0    /* VideoRPC_System.h:23 / VideoRPC_Agent.h:23 */
#define NOVA_RPC_REPLYID             99    /* RPCstruct.h:19 — programID/versionID сентинел для ответов */

/* ===================== ToAgent: ARM → AVCPU (управление) =====================
 * VideoRPC_System.h, строки 28-109 (участок, реально процитированный в
 * этой сессии — файл длиннее, не весь продублирован здесь). */

#define NOVA_RPC_VIDEO_ToAgent_Create              10   /* VideoRPC_System.h:28 */
#define NOVA_RPC_VIDEO_ToAgent_Connect              20   /* VideoRPC_System.h:31 — связь декодер↔ring buffer, НЕ VO (см. ConnectVDec ниже) */
#define NOVA_RPC_VIDEO_ToAgent_InitRingBuffer        30   /* VideoRPC_System.h:34 */
#define NOVA_RPC_VIDEO_ToAgent_Run                   40   /* VideoRPC_System.h:37 — это и есть "Play" */
#define NOVA_RPC_VIDEO_ToAgent_Pause                 50   /* VideoRPC_System.h:40 */
#define NOVA_RPC_VIDEO_ToAgent_Stop                  60   /* VideoRPC_System.h:43 */
#define NOVA_RPC_VIDEO_ToAgent_Destroy                70   /* VideoRPC_System.h:46 */
#define NOVA_RPC_VIDEO_ToAgent_Flush                  80   /* VideoRPC_System.h:49 */
#define NOVA_RPC_VIDEO_ToAgent_SetRefClock             90   /* VideoRPC_System.h:55 */
#define NOVA_RPC_VIDEO_ToAgent_VideoCreate            100   /* VideoRPC_System.h:58 — совпадает по смыслу с RPCCMD_VIDEO_CREATE, реально вызываемым в VDEC_IOC_INIT (rtk_vdec_driver.c:1696); точное числовое тождество RPCCMD_VIDEO_CREATE==100 НЕ сверено построчно в этой сессии (сам #define RPCCMD_VIDEO_CREATE не процитирован) — NOT VERIFIED, помечено явно */
#define NOVA_RPC_VIDEO_ToAgent_DEC_Init             1020   /* VideoRPC_System.h:115 */
#define NOVA_RPC_VIDEO_ToAgent_DEC_ParseResolution  1120   /* VideoRPC_System.h:166 */
#define NOVA_RPC_VIDEO_ToAgent_DEC_SetDecodeMode    1125   /* VideoRPC_System.h:169 */

/* ===================== VO FILTER: ARM → AVCPU (видеовыход) ===================== */

#define NOVA_RPC_VO_FILTER_ToAgent_Display          3260   /* VideoRPC_System.h:436 */
#define NOVA_RPC_VO_FILTER_ToAgent_Capture          3270   /* VideoRPC_System.h:439 */
#define NOVA_RPC_VO_FILTER_ToAgent_SetSpeed         3290   /* VideoRPC_System.h:442 */
#define NOVA_RPC_VO_FILTER_ToAgent_Step             3300   /* VideoRPC_System.h:448 */
#define NOVA_RPC_VO_FILTER_ToAgent_ClearVideo       3340   /* VideoRPC_System.h:463 */
#define NOVA_RPC_VO_FILTER_ToAgent_FlushVideoOut    3350   /* VideoRPC_System.h:466 */
#define NOVA_RPC_VO_FILTER_ToAgent_ConfigDisplayDelay 3380 /* VideoRPC_System.h:475 */
/* Главная находка всей цепочки исследования — соединяет выход декодера
 * напрямую с видеовыходом внутри AVCPU: */
#define NOVA_RPC_VO_FILTER_ToAgent_ConnectVDec      3390   /* VideoRPC_System.h:478 */
#define NOVA_RPC_VO_FILTER_ToAgent_GetVoInfo        3940   /* VideoRPC_System.h:595 */

/* ===================== ToSystem: AVCPU → ARM (callback) =====================
 * VideoRPC_Agent.h. */

#define NOVA_RPC_VIDEO_ToSystem_EndOfStream            2   /* VideoRPC_Agent.h:31 */
#define NOVA_RPC_VIDEO_ToSystem_Deliver_FrameInfo      4   /* VideoRPC_Agent.h:37 (generic, аргумент просто long*) */
#define NOVA_RPC_VIDEO_DEC_ToSystem_FatalError          8   /* VideoRPC_Agent.h:49 */
#define NOVA_RPC_VIDEO_ToSystem_DecoderMessage         15   /* VideoRPC_Agent.h:64 */
#define NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_FrameInfo  18   /* VideoRPC_Agent.h:70-72, арг. VIDEO_RPC_DEC_FRAME_INFO */
#define NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_PicInfo    19   /* VideoRPC_Agent.h:73-75, арг. VIDEO_RPC_DEC_PIC_INFO */
#define NOVA_RPC_VIDEO_DEC_ToSystem_Deliver_MediaInfo  20   /* VideoRPC_Agent.h:76-78, арг. VIDEO_RPC_DEC_MEDIA_INFO */

#ifdef __cplusplus
}
#endif
#endif /* NOVA_RPC_PROTOCOL_H */
