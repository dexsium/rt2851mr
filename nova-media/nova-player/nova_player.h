/*
 * nova_player — публичный API плеера Nova Media Engine (ТЗ "Realtek
 * VDEC/VO & GStreamer" §5). Реализация — ../nova_player.c, поверх
 * GStreamer `playbin` (см. честную оговорку в nova_player.c про выбор
 * playbin вместо ручной сборки filesrc/typefind/decodebin/...: playbin
 * УЖЕ строит ровно такой pipeline внутри себя, вручную собранный граф не
 * добавил бы корректности, только риск хуже обработать нестандартные
 * медиафайлы).
 *
 * Модель владения: `nova_player_t` — opaque-указатель, управляется ТОЛЬКО
 * через функции ниже. `nova_player_destroy()` — единственный законный
 * способ освободить плеер; после вызова указатель недействителен.
 *
 * Модель потоков: все функции `nova_player_*` (кроме callback'ов, см.
 * nova_media_events.h) можно вызывать из ЛЮБОГО одного потока за раз —
 * плеер НЕ является thread-safe для конкурентных вызовов его
 * публичного API из разных потоков одновременно (это сознательное,
 * документированное ограничение, не найденный дефект — конкурентный
 * доступ к ОДНОМУ nova_player_t должен сериализоваться вызывающим
 * кодом). Внутренний GMainLoop-поток для GstBus — отдельный, его
 * взаимодействие с публичным API безопасно (см. nova_media_events.h).
 */
#ifndef NOVA_PLAYER_H
#define NOVA_PLAYER_H

#include <stdint.h>

#include "nova_media_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nova_player_state {
    NOVA_PLAYER_IDLE = 0,
    NOVA_PLAYER_OPENING,
    NOVA_PLAYER_READY,
    NOVA_PLAYER_PLAYING,
    NOVA_PLAYER_PAUSED,
    NOVA_PLAYER_SEEKING,
    NOVA_PLAYER_STOPPED,
    NOVA_PLAYER_ERROR
} nova_player_state_t;

typedef struct nova_player nova_player_t;

/* Создаёт плеер в состоянии NOVA_PLAYER_IDLE. Возвращает NULL при
 * ошибке инициализации GStreamer (например, gst_init() не смог найти
 * нужные плагины — проверяется лениво при первом create(), не в
 * библиотечном конструкторе). */
nova_player_t *nova_player_create(void);

/* Регистрирует callback для событий (см. nova_media_events.h). Можно
 * вызвать в любой момент жизни плеера; NULL снимает регистрацию.
 * user_data передаётся в callback без интерпретации. */
void nova_player_set_event_callback(nova_player_t *p, nova_media_event_cb cb, void *user_data);

/*
 * Открывает медиафайл по пути (локальный путь, не URI — внутри
 * преобразуется в file://, путь должен быть абсолютным или относительным
 * ТЕКУЩЕЙ рабочей директории процесса). Асинхронно: переводит плеер в
 * NOVA_PLAYER_OPENING и возвращает немедленно (0), либо честно
 * возвращает отрицательный код ошибки СРАЗУ при тривиальных причинах
 * (`p==NULL` -> -EINVAL, путь NULL -> -EINVAL). Реальный результат
 * открытия (успех/ошибка/формат не определён) приходит как событие
 * NOVA_MEDIA_EVENT_OPENED либо NOVA_MEDIA_EVENT_ERROR.
 */
int nova_player_open(nova_player_t *p, const char *path);

/* Переводит в PLAYING. Допустимо из READY/PAUSED/STOPPED (с позиции 0
 * для STOPPED). Возвращает 0 при успешной попытке перехода, иначе
 * отрицательный код (-EINVAL на NULL/не открытый плеер). */
int nova_player_play(nova_player_t *p);

int nova_player_pause(nova_player_t *p);

/* Синоним nova_player_play() из состояния PAUSED — оставлен отдельной
 * функцией по требованию ТЗ §5 к точному API, поведение идентично play(). */
int nova_player_resume(nova_player_t *p);

/* Перемотка на position_ms от начала потока. Асинхронно — завершение
 * сигнализируется событием NOVA_MEDIA_EVENT_SEEK_COMPLETED. */
int nova_player_seek(nova_player_t *p, int64_t position_ms);

/* Останавливает воспроизведение, сбрасывает позицию в 0. После stop()
 * повторный play() начинает заново (READY-подобное поведение). */
int nova_player_stop(nova_player_t *p);

/* Возвращает текущую позицию в миллисекундах, либо -1 если не
 * определена (например, до первого PLAYING). */
int64_t nova_player_get_position(nova_player_t *p);

/* Возвращает длительность в миллисекундах, либо -1 если не определена
 * (ещё не открыт/формат не позволяет определить до буферизации). */
int64_t nova_player_get_duration(nova_player_t *p);

nova_player_state_t nova_player_get_state(nova_player_t *p);

/*
 * Синхронно останавливает внутренний поток (join), освобождает
 * GStreamer pipeline и память плеера. ПОСЛЕ ВОЗВРАТА из этой функции
 * callback, зарегистрированный через nova_player_set_event_callback(),
 * ГАРАНТИРОВАННО не будет вызван (ТЗ, матрица теста №23). `p` становится
 * недействителен.
 */
void nova_player_destroy(nova_player_t *p);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_PLAYER_H */
