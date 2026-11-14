/* Реализация nova_rpc.h. См. комментарии в заголовке о происхождении констант. */
#include "nova_rpc.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Буквально из rpcapi.h: magic 'k', ioctl 3 = THANDLER (register), 4 = STOP. */
#define NOVA_RPC_IOC_MAGIC    'k'
#define NOVA_RPC_IOCTHANDLER  _IO(NOVA_RPC_IOC_MAGIC, 3)
#define NOVA_RPC_IOSTOP       _IO(NOVA_RPC_IOC_MAGIC, 4)

int nova_rpc_open(nova_rpc_handle_t *out)
{
    if (!out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->fd = open(NOVA_RPC_DEVICE_NODE, O_RDWR);
    if (out->fd < 0)
        return -errno;

    return 0;
}

int nova_rpc_register(nova_rpc_handle_t *h, unsigned long program_id)
{
    int ret;

    if (!h || h->fd < 0)
        return -EINVAL;

    ret = ioctl(h->fd, NOVA_RPC_IOCTHANDLER, program_id);
    if (ret < 0)
        return -errno;

    h->program_id = program_id;
    h->registered = 1;
    return 0;
}

int nova_rpc_stop(nova_rpc_handle_t *h)
{
    int ret;

    if (!h || h->fd < 0)
        return -EINVAL;

    ret = ioctl(h->fd, NOVA_RPC_IOSTOP);
    if (ret < 0)
        return -errno;

    h->registered = 0;
    return 0;
}

int nova_rpc_read(nova_rpc_handle_t *h, void *buf, unsigned int size)
{
    ssize_t n;

    if (!h || h->fd < 0 || !buf)
        return -EINVAL;

    n = read(h->fd, buf, size);
    if (n < 0)
        return -errno;

    return (int)n;
}

void nova_rpc_close(nova_rpc_handle_t *h)
{
    if (!h)
        return;

    if (h->registered)
        nova_rpc_stop(h);

    if (h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }
}
