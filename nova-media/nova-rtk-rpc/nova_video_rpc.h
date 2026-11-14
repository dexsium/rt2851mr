/*
 * nova_video_rpc — DEC-команды Realtek Video RPC поверх РЕАЛЬНОГО
 * транспорта (nova_rpc_transport), не заглушки. program_id=100
 * (NOVA_RPC_PROGRAM_VIDEO_SYSTEM, CONFIRMED VideoRPC_System.h:22),
 * version=0.
 *
 * Отправка (nova_rpc_transport_send) теперь реальна (см.
 * nova_rpc_transport.h). Приёмная сторона (чтение ответа/распаковка
 * RPCRES_LONG) честно НЕ реализована — у нас нет подтверждённого способа
 * прочитать входящий ring buffer (см. честную оговорку в
 * nova_rpc_callbacks.h), поэтому каждая функция здесь отправляет запрос
 * по-настоящему, но результат (`-ENOTSUP` в `reply_status`) помечен явно
 * как не прочитанный, а не выдуман успешным — прямое требование ТЗ
 * "если точная семантика неизвестна, возвращай -ENOTSUP".
 */
#ifndef NOVA_VIDEO_RPC_H
#define NOVA_VIDEO_RPC_H

#include "nova_rpc_protocol.h"
#include "nova_rpc_transport.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Буквально VideoRPC_Agent_data.h:250-297 (см. предыдущий отчёт). */
typedef struct nova_video_rpc_frame_info {
    long instance_id;
    unsigned int pts_high;
    unsigned int pts_low;
    unsigned char frame_type;
} nova_video_rpc_frame_info_t;

typedef struct nova_video_rpc_media_info {
    long instance_id;
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate;
    unsigned int level;
    unsigned int profile;
} nova_video_rpc_media_info_t;

typedef struct nova_video_rpc_session {
    nova_rpc_transport_t transport;
    long instance_id;
    uint32_t next_task_id;   /* инкрементальный, роль taskID — см. .c */
    /* reply_status != 0, пока reply не прочитан по-настоящему (честно
     * ENOTSUP) — см. комментарий в заголовке файла. */
    int last_reply_status;
} nova_video_rpc_session_t;

int nova_video_rpc_open(nova_video_rpc_session_t *s);

/* Каждая функция реально отправляет RPC-вызов (program=100, конкретный
 * procedureID из nova_rpc_protocol.h) через nova_rpc_transport_send().
 * Возвращает >=0 (число отправленных байт) при успешной ОТПРАВКЕ,
 * отрицательный errno при ошибке транспорта. Успешная отправка НЕ
 * означает успешное выполнение на AVCPU — s->last_reply_status остаётся
 * -ENOTSUP, пока приёмная сторона не реализована (см. выше). */
int nova_video_rpc_create(nova_video_rpc_session_t *s);         /* 10 */
int nova_video_rpc_video_create(nova_video_rpc_session_t *s);   /* 100 */
int nova_video_rpc_dec_init(nova_video_rpc_session_t *s);       /* 1020 */
int nova_video_rpc_init_ring_buffer(nova_video_rpc_session_t *s,
                                     uint32_t phys_addr, uint32_t size); /* 30 */
int nova_video_rpc_connect(nova_video_rpc_session_t *s);        /* 20 */
int nova_video_rpc_run(nova_video_rpc_session_t *s);             /* 40 — "Play" */
int nova_video_rpc_pause(nova_video_rpc_session_t *s);           /* 50 */
int nova_video_rpc_stop(nova_video_rpc_session_t *s);             /* 60 */
int nova_video_rpc_flush(nova_video_rpc_session_t *s);             /* 80 */
int nova_video_rpc_destroy(nova_video_rpc_session_t *s);           /* 70 */

void nova_video_rpc_close(nova_video_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VIDEO_RPC_H */
