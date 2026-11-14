#include "nova_rpc_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Буквально rpcstubapi.c:44-49. На реальном устройстве (Android/bionic
 * ARM32) это SIGUSR2; под glibc (наша CI-сборка) — SIGRTMIN+19. Логика
 * условия воспроизведена, не только значение для одной платформы. */
#if defined(__BIONIC__) && !defined(__LP64__)
#define NOVA_SIGRPC SIGUSR2
#else
#define NOVA_SIGRPC (SIGRTMIN + 19)
#endif

/* Буквально rpcstubapi.c:76-85. */
static const char *const NOVA_DEV_POLL_AUDIO  = "/dev/rpc0";
static const char *const NOVA_DEV_INTR_AUDIO  = "/dev/rpc1";
static const char *const NOVA_DEV_POLL_VIDEO  = "/dev/rpc4";
static const char *const NOVA_DEV_INTR_VIDEO  = "/dev/rpc5";
static const char *const NOVA_DEV_POLL_VIDEO2 = "/dev/rpc8";
static const char *const NOVA_DEV_INTR_VIDEO2 = "/dev/rpc9";

static volatile sig_atomic_t g_nova_rpc_isack = 0;

static void nova_rpc_sig_handler(int sig)
{
    (void)sig;
    g_nova_rpc_isack = 1;
}

int nova_rpc_transport_signal_number(void)
{
    return NOVA_SIGRPC;
}

int nova_rpc_transport_open(nova_rpc_transport_t *t)
{
    struct sigaction sa;

    if (!t)
        return -EINVAL;

    memset(t, 0, sizeof(*t));

    t->fd_poll_audio  = open(NOVA_DEV_POLL_AUDIO, O_WRONLY);
    t->fd_intr_audio  = open(NOVA_DEV_INTR_AUDIO, O_WRONLY);
    t->fd_poll_video  = open(NOVA_DEV_POLL_VIDEO, O_WRONLY);
    t->fd_intr_video  = open(NOVA_DEV_INTR_VIDEO, O_WRONLY);
    t->fd_poll_video2 = open(NOVA_DEV_POLL_VIDEO2, O_WRONLY);
    t->fd_intr_video2 = open(NOVA_DEV_INTR_VIDEO2, O_WRONLY);

    /* Оригинал (openStubRPC(), rpcstubapi.c:159-196) делает exit(-1) при
     * любой неудаче открытия — мы честно возвращаем ошибку вызывающему
     * вместо того, чтобы завершать процесс, но сохраняем тот же факт:
     * если ХОТЯ БЫ ОДНО устройство не открылось, транспорт не готов. На
     * стоковом Android-ядре этих device-нод для Nova-процесса не будет
     * (они принадлежат другому control-path) — ожидаемая, не аварийная
     * ошибка, см. honest-комментарий в nova_vdec_diag.c. */
    if (t->fd_poll_audio < 0 || t->fd_intr_audio < 0 ||
        t->fd_poll_video < 0 || t->fd_intr_video < 0 ||
        t->fd_poll_video2 < 0 || t->fd_intr_video2 < 0) {
        int err = -errno;
        nova_rpc_transport_close(t);
        return err;
    }

    sa.sa_handler = nova_rpc_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(NOVA_SIGRPC, &sa, NULL) != 0) {
        int err = -errno;
        nova_rpc_transport_close(t);
        return err;
    }

    t->opened = 1;
    return 0;
}

static int nova_rpc_pick_fd(const nova_rpc_transport_t *t, int opt)
{
    int use_intr = (opt & NOVA_RPC_T_USE_INTR_BUF) != 0;

    if (opt & NOVA_RPC_T_SEND_AUDIO_CPU)
        return use_intr ? t->fd_intr_audio : t->fd_poll_audio;
    if (opt & NOVA_RPC_T_SEND_VIDEO_CPU2)
        return use_intr ? t->fd_intr_video2 : t->fd_poll_video2;

    return use_intr ? t->fd_intr_video : t->fd_poll_video;
}

int nova_rpc_transport_send(nova_rpc_transport_t *t, int opt,
                             uint32_t program_id, uint32_t version_id,
                             uint32_t procedure_id, uint32_t task_id,
                             uint32_t context,
                             const uint8_t *args, size_t args_size)
{
    nova_rpc_struct_t hdr;
    uint8_t *buf;
    size_t total;
    int fd;
    ssize_t n;

    if (!t || !t->opened)
        return -ENODEV;

    /* Проверка переполнения — прямое требование ТЗ, в оригинале её нет
     * (ClientStub.c доверяет вызывающему коду). */
    if (args_size > (16u * 1024 * 1024))
        return -EMSGSIZE;

    total = NOVA_RPC_STRUCT_WIRE_SIZE + args_size;
    buf = (uint8_t *)malloc(total);
    if (!buf)
        return -ENOMEM;

    hdr.program_id = program_id;
    hdr.version_id = version_id;
    hdr.procedure_id = procedure_id;
    hdr.task_id = task_id;
    hdr.sys_pid = 0; /* не используется на этой платформе, см. комментарий в .h */
    hdr.parameter_size = (uint32_t)args_size;
    hdr.context = context;

    nova_rpc_xdr_encode_struct(buf, total, &hdr);
    if (args && args_size > 0)
        memcpy(buf + NOVA_RPC_STRUCT_WIRE_SIZE, args, args_size);

    fd = nova_rpc_pick_fd(t, opt);
    if (fd < 0) {
        free(buf);
        return -ENODEV;
    }

    if (opt & NOVA_RPC_T_BLOCK_MODE)
        g_nova_rpc_isack = 0;

    n = write(fd, buf, total);
    free(buf);

    if (n < 0)
        return -errno;
    if ((size_t)n != total)
        return -EIO; /* частичная запись — честная ошибка, не "успех" */

    return (int)n;
}

int nova_rpc_transport_wait_reply(int timeout_ms)
{
    /* Буквально __timedwaitReply(), rpcstubapi.c:371-411: sigtimedwait()
     * на явно заблокированном NOVA_SIGRPC, шаг 30 мс (WAKEUP_TIMEOUT_MS),
     * максимум TIMEOUT_SEC*1000/30 попыток в оригинале (там — 100с по
     * умолчанию); здесь timeout_ms задаёт общий бюджет явно, вызывающим
     * кодом, вместо вшитой константы. sigsuspend() (безтаймаутный вариант
     * оригинала, rpcstubapi.c:335-363) не воспроизведён отдельно —
     * sigtimedwait с step-циклом даёт тот же результат и тестируем. */
    sigset_t wait_mask, orig_mask;
    struct timespec step;
    int waited_ms = 0;
    const int poll_step_ms = 30;

    sigemptyset(&wait_mask);
    sigaddset(&wait_mask, NOVA_SIGRPC);
    sigprocmask(SIG_BLOCK, &wait_mask, &orig_mask);

    step.tv_sec = 0;
    step.tv_nsec = (long)poll_step_ms * 1000 * 1000;

    g_nova_rpc_isack = 0;

    while (!g_nova_rpc_isack) {
        int sig = sigtimedwait(&wait_mask, NULL, &step);

        if (sig == NOVA_SIGRPC) {
            g_nova_rpc_isack = 1;
            break;
        }
        if (sig < 0 && errno != EAGAIN && errno != EINTR) {
            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            return -errno;
        }

        waited_ms += poll_step_ms;
        if (timeout_ms > 0 && waited_ms >= timeout_ms) {
            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            return -ETIMEDOUT;
        }
    }

    sigprocmask(SIG_SETMASK, &orig_mask, NULL);
    return 0;
}

void nova_rpc_transport_close(nova_rpc_transport_t *t)
{
    if (!t)
        return;

    if (t->fd_poll_audio >= 0) close(t->fd_poll_audio);
    if (t->fd_intr_audio >= 0) close(t->fd_intr_audio);
    if (t->fd_poll_video >= 0) close(t->fd_poll_video);
    if (t->fd_intr_video >= 0) close(t->fd_intr_video);
    if (t->fd_poll_video2 >= 0) close(t->fd_poll_video2);
    if (t->fd_intr_video2 >= 0) close(t->fd_intr_video2);

    memset(t, 0, sizeof(*t));
    t->fd_poll_audio = t->fd_intr_audio = -1;
    t->fd_poll_video = t->fd_intr_video = -1;
    t->fd_poll_video2 = t->fd_intr_video2 = -1;
}
