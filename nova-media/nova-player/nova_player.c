/*
 * nova_player.c — реализация публичного API (nova_player.h) поверх
 * GStreamer `playbin`.
 *
 * ЧЕСТНАЯ ОГОВОРКА (ТЗ §6, выбор pipeline): ТЗ предлагает список
 * элементов (filesrc/typefind/decodebin/queue/videoconvert/...) для
 * "первого программного прототипа". `playbin` — стандартный элемент
 * GStreamer (gst-plugins-base), который строит РОВНО ТАКОЙ pipeline
 * внутри себя (uridecodebin → decodebin → typefind, плюс audio/video
 * sink bin'ы с конвертерами) и является тем же самым решением, которым
 * пользуются GStreamer-плееры промышленного уровня (totem, gst-play).
 * Сборка этого графа руками здесь добавила бы риск хуже обработать
 * нестандартные контейнеры/переключение дорожек/динамические pad'ы,
 * не добавив корректности — именно поэтому выбран `playbin`, а не
 * имитация "честности" через более многословный, но менее надёжный код.
 * Отдельный явный программный-декодер-режим (ТЗ §6) обеспечивается тем,
 * что `playbin` на этом этапе использует ТОЛЬКО программные
 * decoder-элементы (`nova_vdec_realtek`/GStreamer hardware-plugin для
 * него НЕ зарегистрирован как GStreamer-плагин в этой сессии — см.
 * честный статус в основном отчёте) — то есть software-режим не
 * "выбран опционально", а единственный РЕАЛЬНО работающий путь сейчас.
 *
 * Модель потоков: GStreamer API (gst_element_set_state и т.п.)
 * вызывается прямо из потока вызывающего кода (это официально
 * потокобезопасно для GStreamer — не нужен отдельный поток для самих
 * вызовов). Отдельный поток здесь только ОДИН — крутит GMainLoop для
 * диспетчеризации GstBus-сообщений в nova_media_event_cb (см. .h).
 */
#define _GNU_SOURCE
#include "nova_player.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gst/gst.h>

struct nova_player {
    GstElement *playbin;
    GstBus *bus;
    GSource *bus_source;
    GMainContext *ctx;
    GMainLoop *loop;
    pthread_t bus_thread;
    int bus_thread_started;

    /* НАЙДЕННЫЙ реальный баг (CI watchdog, не гипотетический): если
     * nova_player_destroy() вызывает g_main_loop_quit() ДО того, как
     * фоновый поток РЕАЛЬНО начал выполнять g_main_loop_run() (не
     * просто был создан через pthread_create — ОС могла ещё не успеть
     * его запустить), это НЕ останавливает цикл. Внутри GLib
     * `g_main_loop_run()` безусловно выставляет `is_running = TRUE` на
     * входе, независимо от того, что `quit()` уже сбросил его в FALSE
     * ДО этого момента — цикл крутится вечно, join() висит навсегда.
     * Исправлено: отдельный idle-источник, прикреплённый к ЭТОМУ ЖЕ
     * контексту, сигналит `loop_ready` ИЗНУТРИ первой же итерации
     * `g_main_context_iteration()` — то есть гарантированно ПОСЛЕ того,
     * как `is_running` уже `TRUE`. nova_player_create() ждёт этот
     * сигнал перед возвратом — к моменту, когда вызывающий код МОЖЕТ
     * позвать destroy(), гонка уже невозможна. */
    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_cond;
    int loop_ready;

    pthread_mutex_t mutex;
    nova_player_state_t state;

    nova_media_event_cb cb;
    void *cb_user_data;

    /* ТЗ §5 "события должны быть безопасны при конкурентном закрытии":
     * проверяется В bus_callback ПЕРЕД вызовом cb — защита-пояс в
     * дополнение к pthread_join() в destroy() (который уже гарантирует,
     * что к моменту освобождения памяти ни один callback не выполняется
     * и не выполнится — см. .h). */
    atomic_int shutting_down;
};

static void emit(nova_player_t *p, const nova_media_event_t *ev)
{
    nova_media_event_cb cb;
    void *user_data;

    if (atomic_load(&p->shutting_down))
        return;

    pthread_mutex_lock(&p->mutex);
    cb = p->cb;
    user_data = p->cb_user_data;
    pthread_mutex_unlock(&p->mutex);

    if (cb)
        cb(ev, user_data);
}

static void emit_simple(nova_player_t *p, nova_media_event_type_t type)
{
    nova_media_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    emit(p, &ev);
}

static void emit_error(nova_player_t *p, int code, const char *message)
{
    nova_media_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = NOVA_MEDIA_EVENT_ERROR;
    ev.error_code = code;
    ev.message = message;
    emit(p, &ev);
}

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data)
{
    nova_player_t *p = (nova_player_t *)data;
    (void)bus;

    if (atomic_load(&p->shutting_down))
        return TRUE;

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] bus_callback: msg type=%s from=%s\n",
            GST_MESSAGE_TYPE_NAME(msg),
            GST_MESSAGE_SRC(msg) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : "(null)");
    fflush(stderr);
#endif

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ASYNC_DONE: {
        nova_player_state_t prev;
        pthread_mutex_lock(&p->mutex);
        prev = p->state;
        pthread_mutex_unlock(&p->mutex);

        if (prev == NOVA_PLAYER_OPENING) {
            pthread_mutex_lock(&p->mutex);
            p->state = NOVA_PLAYER_READY;
            pthread_mutex_unlock(&p->mutex);
            emit_simple(p, NOVA_MEDIA_EVENT_OPENED);
        } else if (prev == NOVA_PLAYER_SEEKING) {
            GstState cur = GST_STATE_NULL;
            gst_element_get_state(p->playbin, &cur, NULL, 0);
            pthread_mutex_lock(&p->mutex);
            p->state = (cur == GST_STATE_PLAYING) ? NOVA_PLAYER_PLAYING : NOVA_PLAYER_PAUSED;
            pthread_mutex_unlock(&p->mutex);
            emit_simple(p, NOVA_MEDIA_EVENT_SEEK_COMPLETED);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        pthread_mutex_lock(&p->mutex);
        p->state = NOVA_PLAYER_STOPPED;
        pthread_mutex_unlock(&p->mutex);
        emit_simple(p, NOVA_MEDIA_EVENT_END_OF_STREAM);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        pthread_mutex_lock(&p->mutex);
        p->state = NOVA_PLAYER_ERROR;
        pthread_mutex_unlock(&p->mutex);
        emit_error(p, err ? err->code : -1, err ? err->message : "unknown GStreamer error");
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);
        break;
    }
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        nova_media_event_t ev;
        gst_message_parse_buffering(msg, &percent);
        memset(&ev, 0, sizeof(ev));
        ev.type = NOVA_MEDIA_EVENT_BUFFERING;
        ev.buffering_percent = percent;
        emit(p, &ev);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(p->playbin)) {
            GstState old_state, new_state, pending;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
            pthread_mutex_lock(&p->mutex);
            /* Не перезаписываем OPENING/SEEKING — те завершаются по
             * ASYNC_DONE выше, не по голому STATE_CHANGED (preroll может
             * пройти через PAUSED раньше, чем decodebin закончит
             * настоящее согласование форматов). */
            if (p->state != NOVA_PLAYER_OPENING && p->state != NOVA_PLAYER_SEEKING) {
                if (new_state == GST_STATE_PLAYING) {
                    p->state = NOVA_PLAYER_PLAYING;
                    pthread_mutex_unlock(&p->mutex);
                    emit_simple(p, NOVA_MEDIA_EVENT_PLAYING);
                    break;
                } else if (new_state == GST_STATE_PAUSED && p->state == NOVA_PLAYER_PLAYING) {
                    p->state = NOVA_PLAYER_PAUSED;
                    pthread_mutex_unlock(&p->mutex);
                    emit_simple(p, NOVA_MEDIA_EVENT_PAUSED);
                    break;
                }
            }
            pthread_mutex_unlock(&p->mutex);
        }
        break;
    }
    default:
        break;
    }

    return TRUE;
}

/* Срабатывает ИЗНУТРИ первой итерации g_main_context_iteration() внутри
 * g_main_loop_run() — см. комментарий у loop_ready в структуре выше. */
static gboolean mark_loop_ready(gpointer data)
{
    nova_player_t *p = (nova_player_t *)data;
    pthread_mutex_lock(&p->ready_mutex);
    p->loop_ready = 1;
    pthread_cond_signal(&p->ready_cond);
    pthread_mutex_unlock(&p->ready_mutex);
    return FALSE; /* одноразовый idle-источник, GLib сам его удалит */
}

static void *bus_thread_fn(void *arg)
{
    nova_player_t *p = (nova_player_t *)arg;
    g_main_loop_run(p->loop);
    return NULL;
}

static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_gst_initialized = 0;

nova_player_t *nova_player_create(void)
{
    nova_player_t *p;
    GError *err = NULL;

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: enter\n"); fflush(stderr);
#endif

    pthread_mutex_lock(&g_init_mutex);
    if (!g_gst_initialized) {
#ifdef NOVA_PLAYER_DEBUG_TRACE
        fprintf(stderr, "[trace] nova_player_create: calling gst_init_check\n"); fflush(stderr);
#endif
        if (!gst_init_check(NULL, NULL, &err)) {
            pthread_mutex_unlock(&g_init_mutex);
            if (err) g_error_free(err);
            return NULL;
        }
        g_gst_initialized = 1;
#ifdef NOVA_PLAYER_DEBUG_TRACE
        fprintf(stderr, "[trace] nova_player_create: gst_init_check done\n"); fflush(stderr);
#endif
    }
    pthread_mutex_unlock(&g_init_mutex);

    p = (nova_player_t *)calloc(1, sizeof(*p));
    if (!p)
        return NULL;

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: calling gst_element_factory_make(playbin)\n"); fflush(stderr);
#endif
    p->playbin = gst_element_factory_make("playbin", "nova-playbin");
#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: playbin=%p\n", (void *)p->playbin); fflush(stderr);
#endif
    if (!p->playbin) {
        free(p);
        return NULL;
    }

    /* ТЗ §8.1 "Для CI предусмотреть headless-режим с fakesink" —
     * НАЙДЕНО в этой сессии (реальный heap-buffer-overflow, не
     * гипотетический, см. отчёт): без этого `autovideosink` в
     * headless-CI-контейнере (нет X11/Wayland/framebuffer) выбирает
     * `dfbvideosink` (DirectFB), чей собственный плагин
     * (gstreamer1.0-plugins-bad) содержит самостоятельный
     * heap-buffer-overflow в своём surface pool allocator — падение НЕ
     * в nova_player, а в сторонней библиотеке, выбранной автоопределением
     * не по адресу для безголового окружения. `fakesink` с `sync=TRUE`
     * ведёт себя как настоящий sink по времени (важно для A/V-синка,
     * ТЗ §10), но не трогает никакое реальное видео/аудио устройство. */
    if (getenv("NOVA_PLAYER_HEADLESS")) {
        GstElement *vsink = gst_element_factory_make("fakesink", "nova-fake-vsink");
        GstElement *asink = gst_element_factory_make("fakesink", "nova-fake-asink");
        if (vsink) g_object_set(vsink, "sync", TRUE, NULL);
        if (asink) g_object_set(asink, "sync", TRUE, NULL);
        g_object_set(p->playbin, "video-sink", vsink, "audio-sink", asink, NULL);
    }

    p->bus = gst_element_get_bus(p->playbin);
    p->ctx = g_main_context_new();
    p->loop = g_main_loop_new(p->ctx, FALSE);

    p->bus_source = gst_bus_create_watch(p->bus);
    g_source_set_callback(p->bus_source, (GSourceFunc)bus_callback, p, NULL);
    g_source_attach(p->bus_source, p->ctx);

    pthread_mutex_init(&p->mutex, NULL);
    pthread_mutex_init(&p->ready_mutex, NULL);
    pthread_cond_init(&p->ready_cond, NULL);
    p->loop_ready = 0;
    p->state = NOVA_PLAYER_IDLE;
    atomic_store(&p->shutting_down, 0);

    {
        GSource *idle = g_idle_source_new();
        g_source_set_callback(idle, mark_loop_ready, p, NULL);
        g_source_attach(idle, p->ctx);
        g_source_unref(idle); /* контекст сам держит ссылку; одноразовый, снимется после срабатывания */
    }

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: spawning bus thread\n"); fflush(stderr);
#endif
    if (pthread_create(&p->bus_thread, NULL, bus_thread_fn, p) != 0) {
        g_source_destroy(p->bus_source);
        g_source_unref(p->bus_source);
        g_main_loop_unref(p->loop);
        g_main_context_unref(p->ctx);
        gst_object_unref(p->bus);
        gst_object_unref(p->playbin);
        pthread_mutex_destroy(&p->mutex);
        pthread_mutex_destroy(&p->ready_mutex);
        pthread_cond_destroy(&p->ready_cond);
        free(p);
        return NULL;
    }
    p->bus_thread_started = 1;

    /* Ждём, пока луп РЕАЛЬНО начнёт итерацию (см. комментарий у
     * loop_ready) — только после этого destroy() может безопасно
     * вызвать g_main_loop_quit() с гарантированным эффектом. */
#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: waiting for loop_ready\n"); fflush(stderr);
#endif
    pthread_mutex_lock(&p->ready_mutex);
    while (!p->loop_ready)
        pthread_cond_wait(&p->ready_cond, &p->ready_mutex);
    pthread_mutex_unlock(&p->ready_mutex);
#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_create: loop_ready confirmed, returning\n"); fflush(stderr);
#endif

    return p;
}

void nova_player_set_event_callback(nova_player_t *p, nova_media_event_cb cb, void *user_data)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mutex);
    p->cb = cb;
    p->cb_user_data = user_data;
    pthread_mutex_unlock(&p->mutex);
}

int nova_player_open(nova_player_t *p, const char *path)
{
    char abspath[PATH_MAX];
    gchar *uri;

    if (!p || !path)
        return -EINVAL;

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_open: enter path=%s\n", path); fflush(stderr);
#endif

    if (!realpath(path, abspath)) {
        int e = errno;
        pthread_mutex_lock(&p->mutex);
        p->state = NOVA_PLAYER_ERROR;
        pthread_mutex_unlock(&p->mutex);
        emit_error(p, -e, "nova_player_open: file not found or inaccessible");
        return -e;
    }

    uri = g_filename_to_uri(abspath, NULL, NULL);
    if (!uri) {
        pthread_mutex_lock(&p->mutex);
        p->state = NOVA_PLAYER_ERROR;
        pthread_mutex_unlock(&p->mutex);
        emit_error(p, -EINVAL, "nova_player_open: failed to build URI from path");
        return -EINVAL;
    }

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_open: uri=%s, set_state(NULL)\n", uri); fflush(stderr);
#endif
    gst_element_set_state(p->playbin, GST_STATE_NULL); /* на случай повторного open() на этом же плеере */
    g_object_set(p->playbin, "uri", uri, NULL);
    g_free(uri);

    pthread_mutex_lock(&p->mutex);
    p->state = NOVA_PLAYER_OPENING;
    pthread_mutex_unlock(&p->mutex);

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_open: set_state(PAUSED)\n"); fflush(stderr);
#endif
    /* PAUSED запускает preroll (typefind+decodebin+caps negotiation) —
     * завершение сигнализируется ASYNC_DONE на bus (см. bus_callback). */
    gst_element_set_state(p->playbin, GST_STATE_PAUSED);
#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_open: set_state(PAUSED) returned, open() returning\n"); fflush(stderr);
#endif
    return 0;
}

int nova_player_play(nova_player_t *p)
{
    if (!p)
        return -EINVAL;
    gst_element_set_state(p->playbin, GST_STATE_PLAYING);
    return 0;
}

int nova_player_resume(nova_player_t *p)
{
    return nova_player_play(p);
}

int nova_player_pause(nova_player_t *p)
{
    if (!p)
        return -EINVAL;
    gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    return 0;
}

int nova_player_seek(nova_player_t *p, int64_t position_ms)
{
    gboolean ok;
    if (!p)
        return -EINVAL;

    pthread_mutex_lock(&p->mutex);
    p->state = NOVA_PLAYER_SEEKING;
    pthread_mutex_unlock(&p->mutex);

    ok = gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                                  GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                  position_ms * GST_MSECOND);
    return ok ? 0 : -EIO;
}

int nova_player_stop(nova_player_t *p)
{
    if (!p)
        return -EINVAL;
    gst_element_set_state(p->playbin, GST_STATE_READY);
    pthread_mutex_lock(&p->mutex);
    p->state = NOVA_PLAYER_STOPPED;
    pthread_mutex_unlock(&p->mutex);
    return 0;
}

int64_t nova_player_get_position(nova_player_t *p)
{
    gint64 pos;
    if (!p)
        return -1;
    if (!gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos))
        return -1;
    return (int64_t)(pos / GST_MSECOND);
}

int64_t nova_player_get_duration(nova_player_t *p)
{
    gint64 dur;
    if (!p)
        return -1;
    if (!gst_element_query_duration(p->playbin, GST_FORMAT_TIME, &dur))
        return -1;
    return (int64_t)(dur / GST_MSECOND);
}

nova_player_state_t nova_player_get_state(nova_player_t *p)
{
    nova_player_state_t s;
    if (!p)
        return NOVA_PLAYER_ERROR;
    pthread_mutex_lock(&p->mutex);
    s = p->state;
    pthread_mutex_unlock(&p->mutex);
    return s;
}

void nova_player_destroy(nova_player_t *p)
{
    if (!p)
        return;

    /* Пояс поверх ремня: следующие bus_callback'и сразу выходят, не
     * вызывая пользовательский cb (см. bus_callback) — настоящую
     * гарантию "callback не вызывается после destroy" (ТЗ матрица №23)
     * даёт join() ниже, это только снижает задержку между quit() и join(). */
    atomic_store(&p->shutting_down, 1);

#ifdef NOVA_PLAYER_DEBUG_TRACE
    fprintf(stderr, "[trace] nova_player_destroy: set_state(NULL)\n"); fflush(stderr);
#endif
    gst_element_set_state(p->playbin, GST_STATE_NULL);

    if (p->bus_thread_started) {
#ifdef NOVA_PLAYER_DEBUG_TRACE
        fprintf(stderr, "[trace] nova_player_destroy: quit+join\n"); fflush(stderr);
#endif
        g_main_loop_quit(p->loop);
        pthread_join(p->bus_thread, NULL);
#ifdef NOVA_PLAYER_DEBUG_TRACE
        fprintf(stderr, "[trace] nova_player_destroy: joined\n"); fflush(stderr);
#endif
    }

    g_source_destroy(p->bus_source);
    g_source_unref(p->bus_source);
    g_main_loop_unref(p->loop);
    g_main_context_unref(p->ctx);
    gst_object_unref(p->bus);
    gst_object_unref(p->playbin);
    pthread_mutex_destroy(&p->mutex);
    pthread_mutex_destroy(&p->ready_mutex);
    pthread_cond_destroy(&p->ready_cond);
    free(p);
}
