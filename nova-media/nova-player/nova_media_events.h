/*
 * nova_media_events — события плеера Nova Media Engine (ТЗ "Realtek
 * VDEC/VO & GStreamer", §5). Независимы от GStreamer в публичном
 * заголовке (сам GStreamer не выставляется наружу из nova_player.h) —
 * внутри nova_player.c события формируются из GstBus-сообщений.
 */
#ifndef NOVA_MEDIA_EVENTS_H
#define NOVA_MEDIA_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nova_media_event_type {
    NOVA_MEDIA_EVENT_OPENED = 0,
    NOVA_MEDIA_EVENT_BUFFERING,     /* data = процент (0-100) в buffering_percent */
    NOVA_MEDIA_EVENT_PLAYING,
    NOVA_MEDIA_EVENT_PAUSED,
    NOVA_MEDIA_EVENT_POSITION_CHANGED, /* data = position_ms */
    NOVA_MEDIA_EVENT_SEEK_COMPLETED,
    NOVA_MEDIA_EVENT_END_OF_STREAM,
    NOVA_MEDIA_EVENT_ERROR,          /* data = error_message (не NULL), error_code */
    NOVA_MEDIA_EVENT_DECODER_CHANGED /* data = decoder_name */
} nova_media_event_type_t;

typedef struct nova_media_event {
    nova_media_event_type_t type;
    int64_t position_ms;       /* валидно для POSITION_CHANGED */
    int buffering_percent;     /* валидно для BUFFERING, 0..100 */
    int error_code;            /* валидно для ERROR — соответствует errno-подобным кодам nova_player */
    const char *message;       /* валидно для ERROR/DECODER_CHANGED — указатель живёт ТОЛЬКО на
                                 * время вызова callback'а, копировать при необходимости сохранить */
} nova_media_event_t;

/*
 * ВАЖНО (модель потоков, ТЗ §5 "события должны быть безопасны при
 * конкурентном закрытии плеера"): callback вызывается из ВНУТРЕННЕГО
 * потока плеера (который крутит GMainLoop для GstBus) — НЕ из потока,
 * вызвавшего nova_player_create()/_open()/и т.п. `nova_player_destroy()`
 * синхронно останавливает этот внутренний поток и ДОЖИДАЕТСЯ его
 * завершения (join) ДО того, как освобождает память плеера — поэтому
 * callback гарантированно НЕ может быть вызван после возврата из
 * `nova_player_destroy()` (ТЗ, матрица теста №23 "Callback после
 * destroy — не допускается"). Сам callback не должен вызывать
 * nova_player_destroy() на ТОМ ЖЕ плеере из своего собственного потока —
 * это саморазрушение потока, в котором исполняется callback,
 * задокументированный запрет, не проверяемый автоматически.
 */
typedef void (*nova_media_event_cb)(const nova_media_event_t *event, void *user_data);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_MEDIA_EVENTS_H */
