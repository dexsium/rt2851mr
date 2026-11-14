#include "nova_rpc_transport.h"
#include "nova_rpc_protocol.h" /* NOVA_RPC_REPLYID = 99, RPCstruct.h:19 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Буквально rpcapi.h/RPCstruct.h: magic 'k', ioctl 3 = register. */
#define NOVA_RPC_IOC_MAGIC    'k'
#define NOVA_RPC_IOCTHANDLER  _IO(NOVA_RPC_IOC_MAGIC, 3)
#define NOVA_RPC_IOSTOP       _IO(NOVA_RPC_IOC_MAGIC, 4)

/* Переносимая замена glibc gettid() (появилась только в glibc>=2.30) —
 * буквально то же, что делает rpcstubapi.c:262 (`return (int)gettid();`),
 * но без зависимости от конкретной версии glibc. */
static long nova_gettid_fallback(void)
{
    return syscall(SYS_gettid);
}

/* §4.4 ТЗ: не переносить SIGUSR2/SIGRTMIN+19 "автоматически" без
 * доказанной совместимости с целевой средой — её у нас нет (нет живого
 * ядра Nova на устройстве для проверки). Поэтому пробуждение потока
 * здесь реализовано через pthread_cond_t на каждый pending-слот, не
 * через сигнал — это НЕ повторяет оригинальный механизм ackTask()/tkill
 * побитово (отличие задокументировано в .h), но полностью избегает
 * связанных с сигналами гонок/EINTR/конфликтов с другими подсистемами
 * Nova OS, которые §4.4/§8 ТЗ прямо просят не допускать. Поле taskID
 * в заголовке всё равно заполняется (gettid()) — для совместимости
 * формата сообщения с оригинальным ABI, даже если Nova не использует
 * его для своего внутреннего пробуждения. */
/* g_cond/g_cond_mutex глобальными больше не являются — перенесены в
 * nova_rpc_transport_t (pending_mutex/pending_cond), см. комментарий в
 * .h про найденную ThreadSanitizer гонку данных. */

static const char *nova_env_or(const char *env_name, const char *fallback)
{
    const char *v = getenv(env_name);
    return (v && v[0]) ? v : fallback;
}

int nova_rpc_transport_signal_number(void)
{
    /* Оставлено для совместимости интерфейса с предыдущей версией этого
     * файла; сам транспорт больше не использует сигналы (см. выше). */
    return 0;
}

int nova_rpc_transport_open(nova_rpc_transport_t *t)
{
    if (!t)
        return -EINVAL;

    memset(t, 0, sizeof(*t));

    /* CONFIRMED пути: rpcstubapi.c:76-85 (write), rpcapi.c:34-45 (read).
     * NOVA_RPC_DEV_* позволяют переопределить для e2e-mock-тестов (CI) —
     * без переменных окружения поведение идентично оригинальным путям. */
    t->fd_poll_audio  = open(nova_env_or("NOVA_RPC_DEV_WPOLL_A", "/dev/rpc0"), O_WRONLY);
    t->fd_intr_audio  = open(nova_env_or("NOVA_RPC_DEV_WINTR_A", "/dev/rpc1"), O_WRONLY);
    t->fd_poll_video  = open(nova_env_or("NOVA_RPC_DEV_WPOLL_V", "/dev/rpc4"), O_WRONLY);
    t->fd_intr_video  = open(nova_env_or("NOVA_RPC_DEV_WINTR_V", "/dev/rpc5"), O_WRONLY);
    t->fd_poll_video2 = open(nova_env_or("NOVA_RPC_DEV_WPOLL_V2", "/dev/rpc8"), O_WRONLY);
    t->fd_intr_video2 = open(nova_env_or("NOVA_RPC_DEV_WINTR_V2", "/dev/rpc9"), O_WRONLY);

    t->rfd_poll_audio  = open(nova_env_or("NOVA_RPC_DEV_RPOLL_A", "/dev/rpc2"), O_RDONLY);
    t->rfd_intr_audio  = open(nova_env_or("NOVA_RPC_DEV_RINTR_A", "/dev/rpc3"), O_RDONLY);
    t->rfd_poll_video  = open(nova_env_or("NOVA_RPC_DEV_RPOLL_V", "/dev/rpc6"), O_RDONLY);
    t->rfd_intr_video  = open(nova_env_or("NOVA_RPC_DEV_RINTR_V", "/dev/rpc7"), O_RDONLY);
    t->rfd_poll_video2 = open(nova_env_or("NOVA_RPC_DEV_RPOLL_V2", "/dev/rpc10"), O_RDONLY);
    t->rfd_intr_video2 = open(nova_env_or("NOVA_RPC_DEV_RINTR_V2", "/dev/rpc11"), O_RDONLY);

    if (t->fd_poll_audio < 0 || t->fd_intr_audio < 0 ||
        t->fd_poll_video < 0 || t->fd_intr_video < 0 ||
        t->fd_poll_video2 < 0 || t->fd_intr_video2 < 0 ||
        t->rfd_poll_audio < 0 || t->rfd_intr_audio < 0 ||
        t->rfd_poll_video < 0 || t->rfd_intr_video < 0 ||
        t->rfd_poll_video2 < 0 || t->rfd_intr_video2 < 0) {
        int err = -errno;
        nova_rpc_transport_close(t);
        return err;
    }

    pthread_mutex_init(&t->pending_mutex, NULL);
    pthread_cond_init(&t->pending_cond, NULL);
    t->opened = 1;
    return 0;
}

int nova_rpc_transport_register(nova_rpc_transport_t *t, uint32_t program_id)
{
    /* Буквально rpcapi.c:162-188 — register на ВСЕХ read-fd, не на одном
     * общем control-устройстве. */
    int fds[6];
    int i;

    if (!t || !t->opened)
        return -ENODEV;

    fds[0] = t->rfd_intr_audio;
    fds[1] = t->rfd_intr_video;
    fds[2] = t->rfd_intr_video2;
    fds[3] = t->rfd_poll_audio;
    fds[4] = t->rfd_poll_video;
    fds[5] = t->rfd_poll_video2;

    for (i = 0; i < 6; i++) {
        if (ioctl(fds[i], NOVA_RPC_IOCTHANDLER, (unsigned long)program_id) < 0)
            return -errno;
    }
    return 0;
}

/* Возвращает индекс слота (>=0) или -ENOSPC. *out_context получает
 * упакованный context (generation<<6|slot) для записи в заголовок —
 * см. комментарий в .h про защиту от ABA. */
static int nova_rpc_pending_alloc(nova_rpc_transport_t *t, uint32_t task_id,
                                   uint8_t *result_buf, size_t result_cap,
                                   uint32_t *out_context)
{
    int i;

    pthread_mutex_lock(&t->pending_mutex);
    for (i = 0; i < NOVA_RPC_T_MAX_PENDING; i++) {
        if (t->pending[i].state == NOVA_RPC_PENDING_FREE) {
            t->pending[i].task_id = task_id;
            t->pending[i].result_buf = result_buf;
            t->pending[i].result_cap = result_cap;
            t->pending[i].result_len = 0;
            t->pending[i].state = NOVA_RPC_PENDING_WAITING;
            /* generation увеличивается при каждом НОВОМ выделении (не при
             * освобождении) — так старый и новый владелец слота различимы
             * по значению, зафиксированному на момент своей отправки. */
            t->pending[i].generation++;

            *out_context = ((uint32_t)t->pending[i].generation << NOVA_RPC_T_SLOT_BITS) |
                           ((uint32_t)i & NOVA_RPC_T_SLOT_MASK);

            pthread_mutex_unlock(&t->pending_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&t->pending_mutex);
    return -ENOSPC;
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

int nova_rpc_transport_call(nova_rpc_transport_t *t, int opt,
                             uint32_t program_id, uint32_t version_id,
                             uint32_t procedure_id,
                             const uint8_t *args, size_t args_size,
                             uint8_t *result_buf, size_t result_cap,
                             int timeout_ms)
{
    nova_rpc_struct_t hdr;
    uint8_t *buf;
    size_t total;
    int fd, slot = -1;
    ssize_t n;
    int ret;

    if (!t || !t->opened)
        return -ENODEV;

    /* Защита от переполнения — прямое требование ТЗ §8/§7. */
    if (args_size > (16u * 1024 * 1024))
        return -EMSGSIZE;

    total = NOVA_RPC_STRUCT_WIRE_SIZE + args_size;
    buf = (uint8_t *)malloc(total);
    if (!buf)
        return -ENOMEM;

    hdr.program_id = program_id;
    hdr.version_id = version_id;
    hdr.procedure_id = procedure_id;
    hdr.task_id = (uint32_t)nova_gettid_fallback();
    hdr.sys_pid = 0;
    hdr.parameter_size = (uint32_t)args_size;

    if (opt & NOVA_RPC_T_BLOCK_MODE) {
        uint32_t ctx = 0;
        slot = nova_rpc_pending_alloc(t, hdr.task_id, result_buf, result_cap, &ctx);
        if (slot < 0) {
            free(buf);
            return slot;
        }
        /* context = (generation<<6)|индекс в pending[], не сырой указатель
         * — см. комментарий в .h про сознательное отличие от оригинала и
         * про защиту от ABA. */
        hdr.context = ctx;
    } else {
        hdr.context = 0;
    }

    nova_rpc_xdr_encode_struct(buf, total, &hdr);
    if (args && args_size > 0)
        memcpy(buf + NOVA_RPC_STRUCT_WIRE_SIZE, args, args_size);

    fd = nova_rpc_pick_fd(t, opt);
    n = write(fd, buf, total);
    free(buf);

    if (n < 0) {
        ret = -errno;
        goto fail_cleanup;
    }
    if ((size_t)n != total) {
        ret = -EIO;
        goto fail_cleanup;
    }

    if (!(opt & NOVA_RPC_T_BLOCK_MODE))
        return (int)n;

    /* Ждём пробуждения этого конкретного слота. ИСПРАВЛЕНО (найдено
     * ThreadSanitizer, не гипотетически — см. комментарий в .h): проверка
     * предиката и ожидание теперь выполняются под ТЕМ ЖЕ `pending_mutex`,
     * которым `dispatch_reply()` защищает запись `state` — раньше тут
     * использовался отдельный глобальный мьютекс, что было настоящей
     * гонкой данных (CI run 43648070639, TSan). */
    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&t->pending_mutex);
        while (t->pending[slot].state == NOVA_RPC_PENDING_WAITING) {
            int rc = pthread_cond_timedwait(&t->pending_cond, &t->pending_mutex, &deadline);
            if (rc == ETIMEDOUT)
                break;
        }
    }

    if (t->pending[slot].state == NOVA_RPC_PENDING_COMPLETED) {
        ret = (int)t->pending[slot].result_len;
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
    } else {
        /* Тайм-аут: слот освобождается НЕМЕДЛЕННО (не остаётся EXPIRED
         * навечно, если ответ так и не придёт — иначе исчерпание всех 64
         * слотов было бы постоянным после первого же потерянного ответа,
         * что реально нашёл тест на конкурентность в этом этапе ТЗ).
         * Безопасность от "запоздавшего ответа попал новому владельцу
         * того же слота" обеспечивает НЕ состояние EXPIRED, а generation
         * в context (см. .h/dispatch_reply) — опоздавший ответ несёт
         * СТАРОЕ значение generation и будет отвергнут, даже если слот
         * уже переиспользован другим вызовом. */
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
        ret = -ETIMEDOUT;
    }
    pthread_mutex_unlock(&t->pending_mutex);

    return ret;

fail_cleanup:
    if (slot >= 0) {
        pthread_mutex_lock(&t->pending_mutex);
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
        pthread_mutex_unlock(&t->pending_mutex);
    }
    return ret;
}

int nova_rpc_transport_dispatch_reply(nova_rpc_transport_t *t,
                                       const uint8_t *buf, size_t len)
{
    nova_rpc_struct_t hdr;
    const uint8_t *payload;
    size_t payload_size;
    int slot;
    uint32_t reply_generation;

    if (!t || !buf)
        return -EINVAL;

    if (nova_rpc_xdr_decode_struct(buf, len, &hdr) == 0)
        return -EBADMSG;

    if (hdr.program_id != NOVA_RPC_REPLYID || hdr.version_id != NOVA_RPC_REPLYID)
        return -ENOMSG; /* не ответ — пусть обработает nova_rpc_callbacks */

    slot = (int)(hdr.context & NOVA_RPC_T_SLOT_MASK);
    reply_generation = hdr.context >> NOVA_RPC_T_SLOT_BITS;

    payload = buf + NOVA_RPC_STRUCT_WIRE_SIZE;
    payload_size = (len > NOVA_RPC_STRUCT_WIRE_SIZE) ? (len - NOVA_RPC_STRUCT_WIRE_SIZE) : 0;
    if (payload_size > hdr.parameter_size)
        payload_size = hdr.parameter_size;

    /* Буквально SVCFunc.c:78-89: первые 4 байта payload — result_taskID
     * (XDR u_long), снимаются ПЕРЕД memcpy в буфер результата. Пропущено
     * в первой версии этого файла — поймано сквозным (e2e) mock-тестом
     * (test_rpc_e2e.c, "Сценарий 2"), не угадано постфактум. */
    if (payload_size < 4)
        return -EBADMSG;
    payload += 4;
    payload_size -= 4;

    if (slot < 0 || slot >= NOVA_RPC_T_MAX_PENDING)
        return -ENOENT; /* повреждённый/неизвестный context — безопасно игнорируем */

    pthread_mutex_lock(&t->pending_mutex);
    if (t->pending[slot].state != NOVA_RPC_PENDING_WAITING ||
        t->pending[slot].generation != reply_generation) {
        /* Слот свободен/занят другим вызовом (другая generation) —
         * ответ относится либо к уже завершённому запросу, либо к
         * ЧУЖОМУ запросу, переиспользовавшему этот же индекс слота
         * после тайм-аута (ABA) — в обоих случаях безопасно отбрасываем,
         * НЕ трогая result_buf. Это и есть защита от use-after-free и
         * от путаницы результатов разных вызовов (ТЗ §3, §4), которой
         * нет в оригинальном протоколе Realtek. */
        pthread_mutex_unlock(&t->pending_mutex);
        return -ESTALE;
    }

    {
        size_t to_copy = payload_size;
        if (to_copy > t->pending[slot].result_cap)
            to_copy = t->pending[slot].result_cap;
        if (t->pending[slot].result_buf && to_copy > 0)
            memcpy(t->pending[slot].result_buf, payload, to_copy);
        t->pending[slot].result_len = to_copy;
        t->pending[slot].state = NOVA_RPC_PENDING_COMPLETED;
    }
    /* Broadcast под тем же pending_mutex, которым защищён предикат —
     * тот же мьютекс, что ждущий поток держит в nova_rpc_transport_call()
     * (см. комментарий там). Исключает гонку данных, найденную TSan. */
    pthread_cond_broadcast(&t->pending_cond);
    pthread_mutex_unlock(&t->pending_mutex);

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
    if (t->rfd_poll_audio >= 0) close(t->rfd_poll_audio);
    if (t->rfd_intr_audio >= 0) close(t->rfd_intr_audio);
    if (t->rfd_poll_video >= 0) close(t->rfd_poll_video);
    if (t->rfd_intr_video >= 0) close(t->rfd_intr_video);
    if (t->rfd_poll_video2 >= 0) close(t->rfd_poll_video2);
    if (t->rfd_intr_video2 >= 0) close(t->rfd_intr_video2);

    if (t->opened)
    {
        pthread_mutex_destroy(&t->pending_mutex);
        pthread_cond_destroy(&t->pending_cond);
    }

    memset(t, 0, sizeof(*t));
    t->fd_poll_audio = t->fd_intr_audio = -1;
    t->fd_poll_video = t->fd_intr_video = -1;
    t->fd_poll_video2 = t->fd_intr_video2 = -1;
    t->rfd_poll_audio = t->rfd_intr_audio = -1;
    t->rfd_poll_video = t->rfd_intr_video = -1;
    t->rfd_poll_video2 = t->rfd_intr_video2 = -1;
}
