/*
 * nova_vo_rpc.h — обёртка над RPC-командами Realtek VO FILTER (видеовыход).
 *
 * Структура VIDEO_RPC_VO_FILTER_ConnectVDec скопирована буквально из
 * kernel/drivers/rtk_kdriver/common/IPC/generate/include/video/VideoRPC_System_data.h:1063-1066:
 *     struct VIDEO_RPC_VO_FILTER_ConnectVDec { long instanceID; u_char nPort; };
 *
 * Это и есть прямой, структурный ответ на главный вопрос §5 ТЗ: команда,
 * соединяющая декодер с видеовыходом, принимает только instanceID (чей
 * декодер) и nPort (какой порт/плоскость вывода) — НЕ физический адрес
 * буфера кадра. Это согласуется с nova_video_rpc.h (VIDEO_RPC_DEC_FRAME_INFO
 * тоже без адреса буфера) и даёт согласованную картину: пиксельные данные
 * не передаются через ARM-видимую RPC-структуру ни на одном из двух
 * проверенных путей (ToSystem-уведомление о кадре, ToAgent-подключение к
 * VO) — вся передача, по всей видимости, остаётся внутри AVCPU/аппаратуры.
 *
 * Статус: IMPLEMENTED (структуры/ID), NOT IMPLEMENTED (отправка — та же
 * оговорка, что в nova_video_rpc.c), NOT TESTED.
 */
#ifndef NOVA_VO_RPC_H
#define NOVA_VO_RPC_H

#include "nova_rpc.h"
#include "nova_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nova_vo_rpc_session {
    nova_rpc_handle_t rpc;
} nova_vo_rpc_session_t;

int nova_vo_rpc_open(nova_vo_rpc_session_t *s);

/* VIDEO_RPC_VO_FILTER_ToAgent_ConnectVDec (3390). instance_id — хендл
 * decoder-сессии (nova_video_rpc_session_t.instance_id), port — номер
 * видеоплоскости/порта VO (точная нумерация портов — NOT VERIFIED, не
 * найдена построчно в этой сессии: нужен rtk_venusfb.c/VO-инициализация). */
int nova_vo_rpc_connect_vdec(nova_vo_rpc_session_t *s, long instance_id, unsigned char port);

int nova_vo_rpc_display(nova_vo_rpc_session_t *s, long instance_id);     /* 3260 */
int nova_vo_rpc_clear_video(nova_vo_rpc_session_t *s, long instance_id); /* 3340 */
int nova_vo_rpc_set_speed(nova_vo_rpc_session_t *s, long instance_id, int speed); /* 3290 */

void nova_vo_rpc_close(nova_vo_rpc_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VO_RPC_H */
