/*
 * nova_video_rpc.h — обёртка Nova над Realtek Video RPC (ToAgent/ToSystem
 * команды декодера). Структуры скопированы буквально из
 * kernel/drivers/rtk_kdriver/common/IPC/generate/include/video/VideoRPC_Agent_data.h
 * (строки 250-297, см. nova_rpc_protocol.h для точных ссылок на каждую
 * константу).
 *
 * ВАЖНАЯ НАХОДКА (прямо отвечает на главный вопрос §5 ТЗ "может ли VDEC
 * передавать кадры в VO без копирования в ARM-память"): структура
 * VIDEO_RPC_DEC_FRAME_INFO, которой AVCPU уведомляет ARM о кадре, содержит
 * ТОЛЬКО instanceID, PTS (высокое/низкое слово) и frame_type — НЕТ ни
 * физического адреса, ни указателя на буфер. Это прямое структурное
 * подтверждение (не предположение по названию), что обычный путь
 * воспроизведения не передаёт пиксельные данные через эту RPC-цепочку
 * вообще — только метаданные/статистику.
 *
 * Статус: IMPLEMENTED. Отправка этих команд на AVCPU требует реально
 * работающего ядра Nova с загруженной прошивкой AVCPU — NOT TESTED на
 * этой сессии (блокеры не изменились, см. предыдущие отчёты).
 */
#ifndef NOVA_VIDEO_RPC_H
#define NOVA_VIDEO_RPC_H

#include "nova_rpc.h"
#include "nova_rpc_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Буквально VideoRPC_Agent_data.h:250-256. */
typedef struct nova_video_rpc_frame_info {
    long instance_id;
    unsigned int pts_high;
    unsigned int pts_low;
    unsigned char frame_type;
} nova_video_rpc_frame_info_t;

/* Буквально VideoRPC_Agent_data.h:258-279 (без переименования полей). */
typedef struct nova_video_rpc_pic_info {
    long instance_id;
    unsigned short frame_rate;
    unsigned short aspect_ratio;
    unsigned short hor_size;
    unsigned short ver_size;
    unsigned int bit_rate;
    unsigned char afd;
    unsigned char prog_seq;
    unsigned char prog_frame;
    unsigned short active_x;
    unsigned short active_y;
    unsigned short active_w;
    unsigned short active_h;
    unsigned short disp_hor_size;
    unsigned short disp_ver_size;
    unsigned char aspect_ratio_idc;
    unsigned int sar_width;
    unsigned int sar_height;
    unsigned char info_3d;
} nova_video_rpc_pic_info_t;

/* Буквально VideoRPC_Agent_data.h:281-297 — это и есть источник
 * width/height/frame_rate/profile/level для §13 ТЗ (детектирование
 * кодека/разрешения), не предположение. */
typedef struct nova_video_rpc_media_info {
    long instance_id;
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate;
    unsigned int aspect_ratio_n;
    unsigned int aspect_ratio_d;
    unsigned int level;
    unsigned int profile;
    unsigned int type_3d;
    unsigned int par_width;
    unsigned int par_height;
    unsigned int type_lr;
    unsigned int type_scan;
    unsigned int afd;
} nova_video_rpc_media_info_t;

typedef void (*nova_video_rpc_frame_info_cb)(const nova_video_rpc_frame_info_t *info, void *user_data);
typedef void (*nova_video_rpc_pic_info_cb)(const nova_video_rpc_pic_info_t *info, void *user_data);
typedef void (*nova_video_rpc_media_info_cb)(const nova_video_rpc_media_info_t *info, void *user_data);

typedef struct nova_video_rpc_session {
    nova_rpc_handle_t rpc;
    long instance_id;        /* заполняется после ToAgent_Create/VideoCreate */
    nova_video_rpc_frame_info_cb on_frame_info;
    nova_video_rpc_pic_info_cb on_pic_info;
    nova_video_rpc_media_info_cb on_media_info;
    void *user_data;
} nova_video_rpc_session_t;

/* Регистрирует программу Video RPC (registerRPC(NOVA_RPC_PROGRAM_RANGE_VIDEO_BASE),
 * см. nova_rpc.h) и подготавливает сессию. Не создаёт decoder instance сама —
 * это отдельный шаг (nova_video_rpc_create), который реально отправляет
 * NOVA_RPC_VIDEO_ToAgent_Create/VideoCreate. */
int nova_video_rpc_open(nova_video_rpc_session_t *s);

/* Отправляет NOVA_RPC_VIDEO_ToAgent_Create (10). Возвращает 0 при успехе,
 * instance_id сохраняется в s->instance_id (формат ответа — NOT VERIFIED,
 * см. основной отчёт §2). */
int nova_video_rpc_create(nova_video_rpc_session_t *s);

/* NOVA_RPC_VIDEO_ToAgent_DEC_Init (1020). */
int nova_video_rpc_dec_init(nova_video_rpc_session_t *s);

/* NOVA_RPC_VIDEO_ToAgent_InitRingBuffer (30). */
int nova_video_rpc_init_ring_buffer(nova_video_rpc_session_t *s,
                                     uint32_t phys_addr, uint32_t size);

/* NOVA_RPC_VIDEO_ToAgent_Connect (20) — соединяет decoder instance с
 * ring buffer/следующим фильтром конвейера. НЕ путать с
 * nova_vo_rpc_connect_vdec() (соединение с видеовыходом, другая, VO FILTER
 * команда 3390) — это разные RPC-программы/назначения, оба буквально
 * называются "Connect" в разных файлах, что и требовало ТЗ не путать. */
int nova_video_rpc_connect(nova_video_rpc_session_t *s);

/* NOVA_RPC_VIDEO_ToAgent_Run (40) — это и есть "Play". */
int nova_video_rpc_run(nova_video_rpc_session_t *s);
int nova_video_rpc_pause(nova_video_rpc_session_t *s);   /* 50 */
int nova_video_rpc_stop(nova_video_rpc_session_t *s);    /* 60 */
int nova_video_rpc_flush(nova_video_rpc_session_t *s);   /* 80 */
int nova_video_rpc_destroy(nova_video_rpc_session_t *s); /* 70 */

void nova_video_rpc_close(nova_video_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VIDEO_RPC_H */
