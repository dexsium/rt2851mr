/*
 * nova-frame — описание декодированного кадра для Nova OS.
 *
 * Спроектирован по результатам реального аудита (см.
 * NOVA_VDEC_FRAME_OUTPUT_IMPLEMENTATION.md §4-9), не по шаблону V4L2/DRM
 * "по умолчанию". Ключевые находки, повлиявшие на эту структуру:
 *
 * 1. VDEC_IOC_FRAMEINFO (/dev/rtkvdec) даёт только СЧЁТЧИКИ
 *    (frame_matched/frame_dropped/frame_displaying/frame_freerun) —
 *    не физический адрес и не метаданные конкретного кадра. Прямого
 *    способа получить "хендл одного готового кадра" через этот ioctl
 *    в открытом коде НЕ найдено (см. §4 отчёта, "не найден отдельный
 *    dequeue-интерфейс").
 * 2. Закрытая библиотека `librtk_rhal_rpccore.so` (анализ `readelf
 *    --dyn-syms`, см. §5 отчёта) экспортирует `rhal_RegisterDecFrameInfoCallback`
 *    и RPC-сервис `VIDEO_RPC_DEC_ToSystem_Deliver_FrameInfo_0_svc` — то
 *    есть доставка информации о кадре в реальном стеке происходит ЧЕРЕЗ
 *    CALLBACK, вызываемый AVCPU через RPC, а не через poll/ioctl.
 * 3. `rhal_vrpc_VO_ConnectVDec` (тот же анализ) — отдельная RPC-команда,
 *    которая подключает выход декодера НАПРЯМУЮ к видеовыходу (VO/GDMA/OSD)
 *    на стороне AVCPU. Это означает: для простого вывода декодированного
 *    видео на экран декодированный пиксельный буфер МОЖЕТ НЕ ПЕРЕСЕКАТЬ
 *    ARM-адресное пространство вообще — путь VDEC→VO может быть целиком
 *    внутренним для AVCPU/аппаратуры.
 * 4. `VDEC_SVPMEM_STRUCT`/`rtkvdec_SVP_memory_init()` (см. §6 отчёта) —
 *    память кадров выделяется фиксированными блоками по 4 МБ из
 *    carveout-региона `CARVEDOUT_VDEC_RINGBUF`, адресация физическая.
 *    Обычные (незащищённые) кадры доступны через `VDEC_IOC_ALLOCSVPM`
 *    (без суффикса P) — защита (`VDEC_IOC_ALLOCSVPMP`,
 *    `rtkvdec_svp_enable_cpb_protection()`) включается отдельно, не
 *    обязательна для простого воспроизведения.
 *
 * Поэтому `nova_frame_t` — это дескриптор БУФЕРА памяти (для случаев,
 * когда кадр нужен в ARM-адресном пространстве: захват/thumbnail/запись),
 * а не предположение, что каждый кадр воспроизведения обязательно проходит
 * через ARM. Прямой путь воспроизведения (VDEC→VO) описан отдельно в
 * nova_frame_display_path_t ниже.
 *
 * Статус: IMPLEMENTED (структуры и API), NOT TESTED (нет способа получить
 * реальный кадр на этом устройстве, см. блокеры в основном отчёте).
 */
#ifndef NOVA_FRAME_H
#define NOVA_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nova_frame_pixfmt {
    NOVA_PIXFMT_UNKNOWN = 0,
    NOVA_PIXFMT_NV12,   /* подтверждённый декларацией формат для большинства
                            Realtek video-output путей (YUV 4:2:0 semi-planar) */
    NOVA_PIXFMT_NV21,
    NOVA_PIXFMT_YUV420P
} nova_frame_pixfmt_t;

typedef enum nova_frame_colorspace {
    NOVA_COLORSPACE_UNKNOWN = 0,
    NOVA_COLORSPACE_BT601,
    NOVA_COLORSPACE_BT709,
    NOVA_COLORSPACE_BT2020
} nova_frame_colorspace_t;

typedef enum nova_frame_range {
    NOVA_RANGE_UNKNOWN = 0,
    NOVA_RANGE_LIMITED,
    NOVA_RANGE_FULL
} nova_frame_range_t;

typedef enum nova_frame_state {
    NOVA_FRAME_FREE = 0,      /* буфер свободен, можно переиспользовать */
    NOVA_FRAME_DECODING,      /* AVCPU пишет в буфер — НЕ трогать */
    NOVA_FRAME_READY,         /* кадр готов, ожидает отображения/потребления */
    NOVA_FRAME_DISPLAYING,    /* кадр сейчас на экране — НЕ освобождать */
    NOVA_FRAME_RELEASED       /* потребитель закончил, можно вернуть в пул */
} nova_frame_state_t;

/* Безопасный хендл буфера для обычных Nova-приложений. Физический адрес
 * (nova_frame_t.phys_addr) заполняется только для доверенного слоя
 * (media-service/display-service), которые реально его используют —
 * обычные приложения получают только этот opaque handle + при
 * необходимости DMA-BUF fd (если платформа это поддерживает — NOT
 * VERIFIED в этой сессии: открытый код SVP-аллокатора не экспортирует
 * dma-buf fd напрямую, см. §6 отчёта; требуется отдельная dma-buf
 * exporter-прослойка над carveout-памятью, которая пока не написана). */
typedef uint64_t nova_frame_handle_t;

typedef struct nova_frame {
    nova_frame_handle_t handle;

    uint32_t width;
    uint32_t height;
    uint32_t stride;              /* байт на строку, может быть > width*bpp */
    nova_frame_pixfmt_t pixel_format;
    nova_frame_colorspace_t colorspace;
    nova_frame_range_t range;
    int interlaced;               /* 0 = progressive, 1 = interlaced */

    /* Физический адрес буфера в SVP-памяти (из VDEC_SVPMEM_STRUCT.addr).
     * ВНИМАНИЕ (прямое требование §9 ТЗ): не передавать это поле обычным
     * Nova-приложениям без необходимости — доступно только доверенному
     * коду (media-service), который сам вызывал nova_vdec_alloc_svpmem(). */
    uint32_t phys_addr;
    uint32_t size;

    /* -1, если DMA-BUF недоступен (текущий статус — именно так, см. выше). */
    int dmabuf_fd;

    int64_t timestamp_us;
    uint64_t frame_number;

    nova_frame_state_t state;
    pid_t owner_pid;               /* из VDEC_SVPMEM_STRUCT.pid */

    /* Вызывается ровно один раз, когда потребитель закончил с кадром —
     * обязан перевести state в NOVA_FRAME_RELEASED и вызвать
     * nova_vdec_free_svpmem() (не делается автоматически, чтобы вызывающий
     * мог сначала завершить любой асинхронный вывод/DMA). */
    void (*release_cb)(struct nova_frame *self, void *user_data);
    void *user_data;
} nova_frame_t;

/* Инициализация дескриптора из уже выделенной SVP-памяти (см. nova_vdec.h
 * nova_vdec_alloc_svpmem()). Не выделяет память сама — только описывает её. */
void nova_frame_init_from_svpmem(nova_frame_t *frame,
                                  uint32_t phys_addr, uint32_t size,
                                  pid_t owner_pid);

/* Безопасно помечает кадр использованным и вызывает release_cb, если он
 * задан. Не освобождает SVP-память сама (это явно отдельный шаг, см.
 * предупреждение в §9 ТЗ "не допускать повторного использования кадра до
 * завершения его отображения" — вызывающий код должен сам гарантировать
 * порядок). */
void nova_frame_release(nova_frame_t *frame);

/*
 * Альтернативный, вероятно более реалистичный путь для обычного
 * воспроизведения (не захвата/обработки кадра на CPU) — прямое соединение
 * декодера с видеовыходом внутри AVCPU, без прохождения кадра через ARM.
 * Соответствует найденной RPC-команде `VO_ConnectVDec` (см. §5 отчёта).
 * НЕ реализовано как код в этой сессии — сама численная RPC-команда
 * (programID/procedureID/аргументы) не вычитана построчно из открытых
 * generate/-заголовков (см. "следующие шаги" в основном отчёте), только
 * подтверждено её существование по имени экспортируемого символа.
 */
typedef struct nova_frame_display_path {
    int vdec_session_handle;   /* из nova_vdec_open_session() */
    int vo_connected;          /* 1, если VO_ConnectVDec успешно выполнен — NOT IMPLEMENTED */
} nova_frame_display_path_t;

#ifdef __cplusplus
}
#endif
#endif /* NOVA_FRAME_H */
