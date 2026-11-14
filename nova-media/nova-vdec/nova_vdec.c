/* Реализация nova_vdec.h. Номера ioctl скопированы буквально из
 * kernel/include/rtk_kdriver/rtk_vdec.h строки 214-237 (magic 'v'). */
#include "nova_vdec.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NOVA_VDEC_IOC_MAGIC 'v'
#define NOVA_VDEC_IOC_INIT       _IO  (NOVA_VDEC_IOC_MAGIC, 1)
#define NOVA_VDEC_IOC_DEINIT     _IO  (NOVA_VDEC_IOC_MAGIC, 2)
#define NOVA_VDEC_IOC_OPEN       _IOWR(NOVA_VDEC_IOC_MAGIC, 3, int)
#define NOVA_VDEC_IOC_CLOSE      _IOW (NOVA_VDEC_IOC_MAGIC, 4, int)
#define NOVA_VDEC_IOC_PLAY       _IOW (NOVA_VDEC_IOC_MAGIC, 5, int)
#define NOVA_VDEC_IOC_PAUSE      _IOW (NOVA_VDEC_IOC_MAGIC, 6, int)
#define NOVA_VDEC_IOC_FLUSH      _IOW (NOVA_VDEC_IOC_MAGIC, 7, int)
#define NOVA_VDEC_IOC_STOP       _IOW (NOVA_VDEC_IOC_MAGIC, 8, int)
#define NOVA_VDEC_IOC_SET        _IOW (NOVA_VDEC_IOC_MAGIC, 10, int)
#define NOVA_VDEC_IOC_FRAMEINFO  _IOW (NOVA_VDEC_IOC_MAGIC, 14, nova_vdec_frameinfo_t *)
#define NOVA_VDEC_IOC_ALLOCSVPM  _IOWR(NOVA_VDEC_IOC_MAGIC, 15, nova_vdec_svpmem_t *)
#define NOVA_VDEC_IOC_FREESVPM   _IOW (NOVA_VDEC_IOC_MAGIC, 16, nova_vdec_svpmem_t *)

int nova_vdec_init(nova_vdec_handle_t *h)
{
    if (!h)
        return -EINVAL;

    memset(h, 0, sizeof(*h));
    h->fd = open(NOVA_VDEC_DEVICE_NODE, O_RDWR);
    if (h->fd < 0)
        return -errno;

    if (ioctl(h->fd, NOVA_VDEC_IOC_INIT) < 0) {
        int err = -errno;
        close(h->fd);
        h->fd = -1;
        return err;
    }

    return 0;
}

int nova_vdec_open_session(nova_vdec_handle_t *h, int *ret_handle)
{
    int arg;

    if (!h || h->fd < 0 || !ret_handle)
        return -EINVAL;

    arg = 0;
    if (ioctl(h->fd, NOVA_VDEC_IOC_OPEN, &arg) < 0)
        return -errno;

    h->opened = 1;
    *ret_handle = arg;
    return 0;
}

int nova_vdec_close_session(nova_vdec_handle_t *h, int handle)
{
    if (!h || h->fd < 0)
        return -EINVAL;

    if (ioctl(h->fd, NOVA_VDEC_IOC_CLOSE, &handle) < 0)
        return -errno;

    h->opened = 0;
    return 0;
}

static int nova_vdec_ctl(nova_vdec_handle_t *h, unsigned long req, int handle)
{
    if (!h || h->fd < 0)
        return -EINVAL;

    if (ioctl(h->fd, req, &handle) < 0)
        return -errno;

    return 0;
}

int nova_vdec_play(nova_vdec_handle_t *h, int handle)
{
    return nova_vdec_ctl(h, NOVA_VDEC_IOC_PLAY, handle);
}

int nova_vdec_pause(nova_vdec_handle_t *h, int handle)
{
    return nova_vdec_ctl(h, NOVA_VDEC_IOC_PAUSE, handle);
}

int nova_vdec_flush(nova_vdec_handle_t *h, int handle)
{
    return nova_vdec_ctl(h, NOVA_VDEC_IOC_FLUSH, handle);
}

int nova_vdec_stop(nova_vdec_handle_t *h, int handle)
{
    return nova_vdec_ctl(h, NOVA_VDEC_IOC_STOP, handle);
}

int nova_vdec_set(nova_vdec_handle_t *h, const nova_vdec_set_cmd_t *cmd)
{
    if (!h || h->fd < 0 || !cmd)
        return -EINVAL;

    /* VDEC_IOC_SET в оригинальном коде принимает int (указатель на
     * VDEC_SET_CMD_T передаётся как int-совместимое значение на 32-бит
     * ARM userspace, см. rtk_vdec_driver.c:2317) — здесь передаём адрес
     * структуры напрямую, что соответствует тому же вызову. */
    if (ioctl(h->fd, NOVA_VDEC_IOC_SET, (unsigned long)cmd) < 0)
        return -errno;

    return 0;
}

int nova_vdec_get_frameinfo(nova_vdec_handle_t *h, nova_vdec_frameinfo_t *out)
{
    if (!h || h->fd < 0 || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    if (ioctl(h->fd, NOVA_VDEC_IOC_FRAMEINFO, out) < 0)
        return -errno;

    return 0;
}

int nova_vdec_alloc_svpmem(nova_vdec_handle_t *h, nova_vdec_svpmem_t *mem)
{
    if (!h || h->fd < 0 || !mem)
        return -EINVAL;

    if (ioctl(h->fd, NOVA_VDEC_IOC_ALLOCSVPM, mem) < 0)
        return -errno;

    return 0;
}

int nova_vdec_free_svpmem(nova_vdec_handle_t *h, nova_vdec_svpmem_t *mem)
{
    if (!h || h->fd < 0 || !mem)
        return -EINVAL;

    if (ioctl(h->fd, NOVA_VDEC_IOC_FREESVPM, mem) < 0)
        return -errno;

    return 0;
}

void nova_vdec_deinit(nova_vdec_handle_t *h)
{
    if (!h || h->fd < 0)
        return;

    ioctl(h->fd, NOVA_VDEC_IOC_DEINIT);
    close(h->fd);
    h->fd = -1;
    h->opened = 0;
}
