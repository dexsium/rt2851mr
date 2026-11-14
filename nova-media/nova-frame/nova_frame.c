#include "nova_frame.h"

#include <string.h>

void nova_frame_init_from_svpmem(nova_frame_t *frame,
                                  uint32_t phys_addr, uint32_t size,
                                  pid_t owner_pid)
{
    if (!frame)
        return;

    memset(frame, 0, sizeof(*frame));
    frame->handle = (nova_frame_handle_t)phys_addr; /* opaque для внешнего кода */
    frame->phys_addr = phys_addr;
    frame->size = size;
    frame->owner_pid = owner_pid;
    frame->dmabuf_fd = -1; /* DMA-BUF недоступен — см. комментарий в заголовке */
    frame->state = NOVA_FRAME_FREE;
    frame->pixel_format = NOVA_PIXFMT_UNKNOWN;
    frame->colorspace = NOVA_COLORSPACE_UNKNOWN;
    frame->range = NOVA_RANGE_UNKNOWN;
}

void nova_frame_release(nova_frame_t *frame)
{
    if (!frame)
        return;

    if (frame->state == NOVA_FRAME_DECODING ||
        frame->state == NOVA_FRAME_DISPLAYING) {
        /* Вызывающий нарушил контракт §9 ТЗ ("не допускать повторного
         * использования кадра до завершения отображения") — не падаем,
         * но не меняем состояние и не зовём callback, чтобы не скрыть
         * ошибку молча в релизной сборке. */
        return;
    }

    frame->state = NOVA_FRAME_RELEASED;

    if (frame->release_cb)
        frame->release_cb(frame, frame->user_data);
}
