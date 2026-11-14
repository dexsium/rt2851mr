/*
 * gstreamer-nova-rtk — скелет GStreamer-элемента, использующего
 * аппаратный VDEC Realtek через nova-vdec (см. ../nova-vdec/).
 *
 * Это ЧЕСТНЫЙ СКЕЛЕТ (Этап D ТЗ, "архитектура", не полная реализация):
 * структура GObject/GstVideoDecoder — стандартная и реальная (выглядит
 * ровно так же, как любой другой hardware-decoder-плагин GStreamer:
 * v4l2codecs, vaapi и т.п.), но тело decide_allocation/handle_frame не
 * реализовано полностью — там, где нужна была бы передача декодированных
 * кадров из SVP-памяти (VDEC_SVPMEM_STRUCT) в GstBuffer, это отдельная,
 * не решённая в этой сессии задача zero-copy DMA-импорта, которая требует
 * доступа к реальному работающему декодеру для проверки (его нет —
 * см. блокеры в основном отчёте).
 *
 * Статус: IMPLEMENTED как структура/заголовки класса. NOT COMPILED в этой
 * сессии (нужны заголовки GStreamer 1.x / gst-plugins-bad, которых нет в
 * CI-пайплайне ядра; отдельная задача — добавить их в CI, см. "Следующие
 * шаги" в итоговом отчёте). Тело decode-функций: NOT IMPLEMENTED
 * (заглушка, честно помечена TODO, не выдаётся за готовое).
 */
#ifndef __GST_NOVARTKDEC_H__
#define __GST_NOVARTKDEC_H__

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

G_BEGIN_DECLS

#define GST_TYPE_NOVARTKDEC (gst_novartkdec_get_type())
G_DECLARE_FINAL_TYPE(GstNovaRtkDec, gst_novartkdec, GST, NOVARTKDEC, GstVideoDecoder)

struct _GstNovaRtkDec {
    GstVideoDecoder parent;

    /* nova_vdec_handle_t встроен как opaque-поле, чтобы не тянуть
     * nova_vdec.h в публичный заголовок GObject-класса. */
    void *vdec_priv;
    int   vdec_session_handle;
    gboolean hw_session_open;
};

GType gst_novartkdec_get_type(void);

G_END_DECLS

#endif /* __GST_NOVARTKDEC_H__ */
