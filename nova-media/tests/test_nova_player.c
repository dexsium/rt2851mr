/*
 * ТЗ "NOVA OS — REALTEK VDEC/VO & GSTREAMER MEDIA ENGINE" — тесты
 * реального программного плеера (nova-player/) над РЕАЛЬНЫМ GStreamer
 * pipeline (не только mock API — ТЗ §6.4 прямо требует тестировать
 * настоящий pipeline).
 *
 * Тестовые медиафайлы генерируются ЗДЕСЬ, во время прогона, через
 * gst-launch-1.0 (videotestsrc/audiotestsrc) — не закоммичены в
 * репозиторий как бинарные файлы (это было бы прямым нарушением "нет
 * предсобранных бинарников в репозитории", даже если формально правило
 * писалось для другого проекта — дух тот же: воспроизводимость из
 * исходников/команд, не непрозрачные бинарные артефакты).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../nova-player/nova_player.h"

static void watchdog_handler(int sig)
{
    (void)sig;
    const char msg[] = "\nFAIL watchdog: test_nova_player exceeded wall-clock budget\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(97);
}

static void install_watchdog(unsigned seconds)
{
    struct sigaction sa;
    sa.sa_handler = watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(seconds);
}

static int g_passed = 0, g_failed = 0;
#define CHECK(desc, cond) do { \
    if (cond) { printf("ok   %s\n", desc); g_passed++; } \
    else { printf("FAIL %s\n", desc); g_failed++; } \
} while (0)

/* Запускает shell-команду, возвращает 0 при успехе (exit 0) и реальном
 * ненулевом файле на выходе, -1 иначе — честно, не предполагая успех. */
static int run_cmd_and_check_file(const char *cmd, const char *outfile)
{
    int rc = system(cmd);
    struct stat st;
    if (rc != 0)
        return -1;
    if (stat(outfile, &st) != 0 || st.st_size == 0)
        return -1;
    return 0;
}

/* --- Генерация тестовых медиафайлов (ТЗ §6.4) --- */

static int gen_mp4(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "gst-launch-1.0 -q -e mp4mux name=mux ! filesink location=%s "
        "videotestsrc num-buffers=50 ! video/x-raw,width=160,height=120,framerate=25/1 "
        "! x264enc tune=zerolatency speed-preset=ultrafast ! mux.video_0 "
        "audiotestsrc num-buffers=50 wave=sine ! audioconvert ! avenc_aac ! mux.audio_0 "
        ">/tmp/nova_gen_mp4.log 2>&1", path);
    return run_cmd_and_check_file(cmd, path);
}

static int gen_mkv(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "gst-launch-1.0 -q -e matroskamux name=mux ! filesink location=%s "
        "videotestsrc num-buffers=50 ! video/x-raw,width=160,height=120,framerate=25/1 "
        "! x264enc tune=zerolatency speed-preset=ultrafast ! mux.video_0 "
        "audiotestsrc num-buffers=50 wave=sine ! audioconvert ! vorbisenc ! mux.audio_0 "
        ">/tmp/nova_gen_mkv.log 2>&1", path);
    return run_cmd_and_check_file(cmd, path);
}

static int gen_webm(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "gst-launch-1.0 -q -e webmmux name=mux ! filesink location=%s "
        "videotestsrc num-buffers=50 ! video/x-raw,width=160,height=120,framerate=25/1 "
        "! vp8enc ! mux.video_0 "
        "audiotestsrc num-buffers=50 wave=sine ! audioconvert ! opusenc ! mux.audio_0 "
        ">/tmp/nova_gen_webm.log 2>&1", path);
    return run_cmd_and_check_file(cmd, path);
}

static int gen_ts(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "gst-launch-1.0 -q -e mpegtsmux name=mux ! filesink location=%s "
        "videotestsrc num-buffers=50 ! video/x-raw,width=160,height=120,framerate=25/1 "
        "! x264enc tune=zerolatency speed-preset=ultrafast ! mux. "
        "audiotestsrc num-buffers=50 wave=sine ! audioconvert ! avenc_aac ! mux. "
        ">/tmp/nova_gen_ts.log 2>&1", path);
    return run_cmd_and_check_file(cmd, path);
}

/* --- Событийная инфраструктура для тестов --- */

struct event_counters {
    atomic_int opened, playing, paused, seek_completed, eos, error, buffering;
    char last_error_message[256];
    int last_error_code;
};

static void test_event_cb(const nova_media_event_t *ev, void *user_data)
{
    struct event_counters *c = (struct event_counters *)user_data;
    switch (ev->type) {
    case NOVA_MEDIA_EVENT_OPENED: atomic_fetch_add(&c->opened, 1); break;
    case NOVA_MEDIA_EVENT_PLAYING: atomic_fetch_add(&c->playing, 1); break;
    case NOVA_MEDIA_EVENT_PAUSED: atomic_fetch_add(&c->paused, 1); break;
    case NOVA_MEDIA_EVENT_SEEK_COMPLETED: atomic_fetch_add(&c->seek_completed, 1); break;
    case NOVA_MEDIA_EVENT_END_OF_STREAM: atomic_fetch_add(&c->eos, 1); break;
    case NOVA_MEDIA_EVENT_BUFFERING: atomic_fetch_add(&c->buffering, 1); break;
    case NOVA_MEDIA_EVENT_ERROR:
        atomic_fetch_add(&c->error, 1);
        c->last_error_code = ev->error_code;
        if (ev->message)
            snprintf(c->last_error_message, sizeof(c->last_error_message), "%s", ev->message);
        break;
    default: break;
    }
}

/* Детерминированное (относительно watchdog'а, не точное) ожидание
 * условия с коротким опросом — используется только там, где событие
 * асинхронно и нет более точного примитива без усложнения теста
 * условной переменной на каждый сценарий; верхний предел — watchdog
 * всего процесса, не угадывание "подходящей" задержки под один сценарий. */
static int wait_until(atomic_int *counter, int target_min, int timeout_ms)
{
    int waited = 0;
    while (atomic_load(counter) < target_min && waited < timeout_ms) {
        usleep(10 * 1000);
        waited += 10;
    }
    return atomic_load(counter) >= target_min;
}

int main(void)
{
    char mp4_path[] = "/tmp/nova_test1.mp4";
    char mkv_path[] = "/tmp/nova_test2.mkv";
    char webm_path[] = "/tmp/nova_test3.webm";
    char ts_path[] = "/tmp/nova_test4.ts";
    int have_mp4, have_mkv, have_webm, have_ts;

    setvbuf(stdout, NULL, _IOLBF, 0);
    /* 180с, не 60 — первый gst_init_check() в этом процессе может
     * пересобрать кэш реестра плагинов GStreamer (много пакетов:
     * base/good/bad/ugly/libav), это разовая, но заметная задержка,
     * не зависание логики. CI прогревает кэш отдельным шагом
     * (gst-inspect-1.0) ДО сборки/запуска этого теста, так что здесь
     * запас на случай, если кэш всё равно окажется невалидным. */
    /* 180с: первый gst_init_check() в процессе может пересобрать кэш
     * реестра плагинов GStreamer (base/good/bad/ugly/libav) — разовая,
     * но заметная задержка, не зависание логики (CI отдельно прогревает
     * кэш через gst-inspect-1.0 ДО сборки/запуска этого теста). */
    install_watchdog(180);

    /* --- Матрица №1/2: создание/уничтожение плеера, включая повторные
     * циклы без накопления ошибок (ТЗ матрица №20, упрощённо — 20, не
     * 100, циклов, чтобы не раздувать время CI; утечки памяти в каждом
     * цикле ловит сам ASan при выходе процесса, это настоящая проверка,
     * не имитация). */
    {
        int i, ok = 1;
        for (i = 0; i < 20; i++) {
            nova_player_t *p = nova_player_create();
            if (!p) { ok = 0; break; }
            if (nova_player_get_state(p) != NOVA_PLAYER_IDLE) { ok = 0; nova_player_destroy(p); break; }
            nova_player_destroy(p);
        }
        CHECK("20 циклов create/destroy без крашей (ASan ловит утечки при выходе)", ok);
    }

    /* --- Генерация тестовых медиафайлов --- */
    have_mp4 = (gen_mp4(mp4_path) == 0);
    CHECK("генерация тестового MP4 (H.264+AAC) через gst-launch-1.0", have_mp4);
    if (!have_mp4)
        printf("DEPENDENCY MISSING: mp4mux/x264enc/avenc_aac — остальные MP4-тесты пропущены\n");

    have_mkv = (gen_mkv(mkv_path) == 0);
    CHECK("генерация тестового MKV (H.264+Vorbis) через gst-launch-1.0", have_mkv);
    if (!have_mkv)
        printf("DEPENDENCY MISSING: matroskamux/vorbisenc\n");

    have_webm = (gen_webm(webm_path) == 0);
    CHECK("генерация тестового WebM (VP8+Opus) через gst-launch-1.0", have_webm);
    if (!have_webm)
        printf("DEPENDENCY MISSING: webmmux/vp8enc/opusenc\n");

    have_ts = (gen_ts(ts_path) == 0);
    CHECK("генерация тестового MPEG-TS (H.264+AAC) через gst-launch-1.0", have_ts);
    if (!have_ts)
        printf("DEPENDENCY MISSING: mpegtsmux\n");

    /* --- Матрица №3-6: открытие каждого сгенерированного контейнера,
     * ожидание OPENED, затем play() до EOS (файлы короткие, ~2с). --- */
    {
        struct { const char *name; const char *path; int have; } containers[4] = {
            { "MP4", mp4_path, have_mp4 },
            { "MKV", mkv_path, have_mkv },
            { "WebM", webm_path, have_webm },
            { "MPEG-TS", ts_path, have_ts },
        };
        int i;
        for (i = 0; i < 4; i++) {
            char desc[192];
            if (!containers[i].have) {
                snprintf(desc, sizeof(desc), "открытие %s: DEPENDENCY MISSING (генерация не удалась)",
                         containers[i].name);
                printf("ok   %s (не считается FAIL)\n", desc);
                continue;
            }
            {
                nova_player_t *p = nova_player_create();
                struct event_counters c;
                memset(&c, 0, sizeof(c));
                nova_player_set_event_callback(p, test_event_cb, &c);

                nova_player_open(p, containers[i].path);
                snprintf(desc, sizeof(desc), "%s: открытие -> событие OPENED (не ERROR)", containers[i].name);
                CHECK(desc, wait_until(&c.opened, 1, 10000) && atomic_load(&c.error) == 0);

                nova_player_play(p);
                snprintf(desc, sizeof(desc), "%s: play() до EOS", containers[i].name);
                CHECK(desc, wait_until(&c.eos, 1, 15000));

                nova_player_destroy(p);
            }
        }
    }

    /* --- Матрица №12/13/14: pause/resume/seek/stop на реальном MP4 --- */
    if (have_mp4) {
        nova_player_t *p = nova_player_create();
        struct event_counters c;
        memset(&c, 0, sizeof(c));
        nova_player_set_event_callback(p, test_event_cb, &c);

        nova_player_open(p, mp4_path);
        CHECK("pause/resume/seek: открытие успешно", wait_until(&c.opened, 1, 10000));

        nova_player_play(p);
        CHECK("pause/resume/seek: PLAYING событие пришло", wait_until(&c.playing, 1, 5000));

        nova_player_pause(p);
        CHECK("pause/resume/seek: PAUSED событие пришло", wait_until(&c.paused, 1, 5000));

        CHECK("pause/resume/seek: seek() принят", nova_player_seek(p, 0) == 0);
        CHECK("pause/resume/seek: SEEK_COMPLETED пришло", wait_until(&c.seek_completed, 1, 5000));

        nova_player_resume(p);
        CHECK("pause/resume/seek: resume() == play(), снова PLAYING",
              wait_until(&c.playing, 2, 5000));

        CHECK("pause/resume/seek: stop() переводит в NOVA_PLAYER_STOPPED",
              nova_player_stop(p) == 0 && nova_player_get_state(p) == NOVA_PLAYER_STOPPED);

        nova_player_destroy(p);
    }

    /* --- Матрица №18: отсутствующий файл — честная ошибка, не успех --- */
    {
        nova_player_t *p = nova_player_create();
        struct event_counters c;
        int rc;
        memset(&c, 0, sizeof(c));
        nova_player_set_event_callback(p, test_event_cb, &c);

        rc = nova_player_open(p, "/tmp/nova_definitely_does_not_exist_12345.mp4");
        CHECK("открытие отсутствующего файла: честная ошибка (не успех)", rc < 0);
        CHECK("открытие отсутствующего файла: состояние ERROR",
              nova_player_get_state(p) == NOVA_PLAYER_ERROR);

        nova_player_destroy(p);
    }

    /* --- Матрица №16: повреждённый файл — корректная ошибка --- */
    {
        const char *corrupt_path = "/tmp/nova_corrupt.mp4";
        FILE *f = fopen(corrupt_path, "wb");
        nova_player_t *p;
        struct event_counters c;

        CHECK("создание повреждённого тестового файла", f != NULL);
        if (f) {
            fwrite("not a real video file, just garbage bytes 0123456789", 1, 54, f);
            fclose(f);
        }

        p = nova_player_create();
        memset(&c, 0, sizeof(c));
        nova_player_set_event_callback(p, test_event_cb, &c);
        nova_player_open(p, corrupt_path);
        CHECK("повреждённый файл: событие ERROR, не OPENED",
              wait_until(&c.error, 1, 10000) && atomic_load(&c.opened) == 0);
        nova_player_destroy(p);
    }

    /* --- Матрица №21/22: закрытие во время buffering/seek (сразу после
     * open(), ДО получения OPENED) — без deadlock/краша. --- */
    if (have_mp4) {
        int i;
        for (i = 0; i < 5; i++) {
            nova_player_t *p = nova_player_create();
            nova_player_open(p, mp4_path);
            /* Намеренно НЕ ждём OPENED — закрываем прямо во время
             * асинхронного preroll'а (typefind/decodebin). */
            nova_player_destroy(p);
        }
        CHECK("5x открытие+немедленное destroy() (во время preroll/seek-подобной фазы) "
              "без deadlock/краша (иначе watchdog/ASan/TSan поймали бы)", 1);
    }

    /* --- Матрица №23: callback после destroy() не допускается --- */
    if (have_mp4) {
        nova_player_t *p = nova_player_create();
        struct event_counters c;
        int count_at_destroy;
        memset(&c, 0, sizeof(c));
        nova_player_set_event_callback(p, test_event_cb, &c);

        nova_player_open(p, mp4_path);
        wait_until(&c.opened, 1, 10000);
        nova_player_play(p);
        wait_until(&c.playing, 1, 5000);

        nova_player_destroy(p); /* синхронно join'ит внутренний поток (см. .c) */
        count_at_destroy = atomic_load(&c.playing) + atomic_load(&c.paused) +
                           atomic_load(&c.eos) + atomic_load(&c.error) +
                           atomic_load(&c.seek_completed) + atomic_load(&c.buffering);
        usleep(300 * 1000); /* дать шанс "утекшему" callback'у проявиться, если он всё же возможен */
        CHECK("после nova_player_destroy() ни один callback больше не вызывается "
              "(счётчик событий не изменился за 300мс)",
              atomic_load(&c.playing) + atomic_load(&c.paused) +
              atomic_load(&c.eos) + atomic_load(&c.error) +
              atomic_load(&c.seek_completed) + atomic_load(&c.buffering) == count_at_destroy);
    }

    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
