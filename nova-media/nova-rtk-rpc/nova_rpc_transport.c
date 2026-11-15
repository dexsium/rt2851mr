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

/* ТЗ §5.4/§6: сериализует close() с самим собой (concurrent close) и с
 * dispatch_reply() (reply во время закрытия) — объявлен здесь (а не
 * рядом с close(), как было сначала), т.к. dispatch_reply() тоже его
 * использует и стоит в файле раньше close(). См. подробный комментарий
 * прямо перед nova_rpc_transport_close(). */
static pthread_mutex_t g_close_guard = PTHREAD_MUTEX_INITIALIZER;

int nova_rpc_transport_signal_number(void)
{
    /* Оставлено для совместимости интерфейса с предыдущей версией этого
     * файла; сам транспорт больше не использует сигналы (см. выше). */
    return 0;
}

int nova_rpc_transport_open(nova_rpc_transport_t *t)
{
    int i;

    if (!t)
        return -EINVAL;

    /* ТЗ §5.1: повторный open() на уже открытом транспорте не должен
     * перезаписывать структуру/утекать fd. `state` ещё не было
     * инициализировано memset(0) при первом вызове, но
     * NOVA_RPC_T_STATE_UNINITIALIZED == 0, так что первый вызов на
     * свежем (например, стековом, заполненном нулями) объекте всё
     * равно пройдёт этот if как false — безопасно. На уже-открытом
     * транспорте (state == OPEN) НЕ трогаем существующие дескрипторы
     * и mutex/cond. */
    if (t->state == NOVA_RPC_T_STATE_OPEN || t->state == NOVA_RPC_T_STATE_OPENING)
        return -EALREADY;

    memset(t, 0, sizeof(*t));
    t->state = NOVA_RPC_T_STATE_OPENING;

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
        t->state = NOVA_RPC_T_STATE_UNINITIALIZED;
        nova_rpc_transport_close(t);
        return err;
    }

    pthread_mutex_init(&t->pending_mutex, NULL);
    pthread_cond_init(&t->pending_cond, NULL);
    for (i = 0; i < 6; i++)
        pthread_mutex_init(&t->send_mutex[i], NULL);
    t->opened = 1;
    t->waiters = 0;
    t->state = NOVA_RPC_T_STATE_OPEN;
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
             * по значению, зафиксированному на момент своей отправки.
             * Циклически по модулю 2^26 (NOVA_RPC_T_GEN_MASK) — см.
             * комментарий в .h про найденный и исправленный баг
             * "постоянный ESTALE после переполнения". */
            t->pending[i].generation = (t->pending[i].generation + 1) & NOVA_RPC_T_GEN_MASK;

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

/* Индекс в t->send_mutex[6], соответствующий тому же fd, что выбрал бы
 * nova_rpc_pick_fd() — один mutex на fd, сериализует только конкурентов
 * на ОДНОМ fd, не мешая параллельным вызовам на разные CPU/буферы. */
static int nova_rpc_pick_send_mutex_index(int opt)
{
    int use_intr = (opt & NOVA_RPC_T_USE_INTR_BUF) != 0;

    if (opt & NOVA_RPC_T_SEND_AUDIO_CPU)
        return use_intr ? 1 : 0;
    if (opt & NOVA_RPC_T_SEND_VIDEO_CPU2)
        return use_intr ? 5 : 4;
    return use_intr ? 3 : 2;
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

    /* ТЗ §5.2: новые запросы запрещены, пока транспорт в CLOSING/CLOSED.
     * Проверяем под pending_mutex и, если проходим проверку, сразу же
     * регистрируемся как waiter — иначе между этой проверкой и
     * регистрацией close() мог бы успеть перейти в CLOSING и начать
     * ждать waiters==0, не увидев этот вызов. */
    pthread_mutex_lock(&t->pending_mutex);
    if (t->state != NOVA_RPC_T_STATE_OPEN) {
        pthread_mutex_unlock(&t->pending_mutex);
        return -ESHUTDOWN;
    }
    t->waiters++;
    pthread_mutex_unlock(&t->pending_mutex);

    /* Защита от переполнения — прямое требование ТЗ §8/§7. */
    if (args_size > (16u * 1024 * 1024)) {
        pthread_mutex_lock(&t->pending_mutex);
        t->waiters--;
        pthread_cond_broadcast(&t->pending_cond);
        pthread_mutex_unlock(&t->pending_mutex);
        return -EMSGSIZE;
    }

    total = NOVA_RPC_STRUCT_WIRE_SIZE + args_size;
    buf = (uint8_t *)malloc(total);
    if (!buf) {
        pthread_mutex_lock(&t->pending_mutex);
        t->waiters--;
        pthread_cond_broadcast(&t->pending_cond);
        pthread_mutex_unlock(&t->pending_mutex);
        return -ENOMEM;
    }

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
            pthread_mutex_lock(&t->pending_mutex);
            t->waiters--;
            pthread_cond_broadcast(&t->pending_cond);
            pthread_mutex_unlock(&t->pending_mutex);
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

    /* ТЗ §7: сериализация отправки на уровне ОДНОГО fd — см. комментарий
     * про send_mutex в .h. Один write() всего буфера целиком, как в
     * оригинале (rpcstubapi.c: один writeRingBuf() на весь пакет). */
    fd = nova_rpc_pick_fd(t, opt);
    {
        int mi = nova_rpc_pick_send_mutex_index(opt);
        pthread_mutex_lock(&t->send_mutex[mi]);
        n = write(fd, buf, total);
        pthread_mutex_unlock(&t->send_mutex[mi]);
    }
    free(buf);

    if (n < 0) {
        ret = -errno;
        goto fail_cleanup;
    }
    if ((size_t)n != total) {
        ret = -EIO;
        goto fail_cleanup;
    }

    if (!(opt & NOVA_RPC_T_BLOCK_MODE)) {
        pthread_mutex_lock(&t->pending_mutex);
        t->waiters--;
        pthread_cond_broadcast(&t->pending_cond);
        pthread_mutex_unlock(&t->pending_mutex);
        return (int)n;
    }

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
        /* ТЗ §5.2/§6: close() может перевести state в CLOSING и
         * разбудить pending_cond (broadcast) в ЛЮБОЙ момент этого
         * ожидания — проверяем это в ТОМ ЖЕ предикате цикла, чтобы
         * выйти сразу, а не ждать до timeout_ms, и чтобы close() не
         * зависло навечно, ожидая waiters==0 (ТЗ §6, сценарий "ответ
         * приходит во время закрытия" и "close одновременно с call"). */
        while (t->pending[slot].state == NOVA_RPC_PENDING_WAITING &&
               t->state == NOVA_RPC_T_STATE_OPEN) {
            int rc = pthread_cond_timedwait(&t->pending_cond, &t->pending_mutex, &deadline);
            if (rc == ETIMEDOUT)
                break;
        }
    }

    if (t->pending[slot].state == NOVA_RPC_PENDING_COMPLETED) {
        ret = (int)t->pending[slot].result_len;
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
    } else if (t->state != NOVA_RPC_T_STATE_OPEN) {
        /* Транспорт закрывается прямо сейчас — ответ, даже если он
         * ещё придёт позже, уже не будет сопоставлен ни с чем (см.
         * close() ниже: слот освобождается ДО ожидания waiters==0),
         * поэтому возвращаем именно -ESHUTDOWN, а не -ETIMEDOUT —
         * честное, документированное ТЗ §5.2 различие причин отказа. */
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
        ret = -ESHUTDOWN;
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
    t->waiters--;
    /* Будим close(), если он сейчас ждёт waiters==0 (см. там же) — тот
     * же pending_cond, не отдельная переменная, чтобы не плодить лишние
     * мьютексы/cond на один и тот же предикат. */
    pthread_cond_broadcast(&t->pending_cond);
    pthread_mutex_unlock(&t->pending_mutex);

    return ret;

fail_cleanup:
    pthread_mutex_lock(&t->pending_mutex);
    if (slot >= 0)
        t->pending[slot].state = NOVA_RPC_PENDING_FREE;
    t->waiters--;
    pthread_cond_broadcast(&t->pending_cond);
    pthread_mutex_unlock(&t->pending_mutex);
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

    /* ТЗ §6, сценарий "ответ приходит во время закрытия": без этого
     * g_close_guard() dispatch_reply() мог бы попытаться взять
     * pending_mutex ПОСЛЕ того, как close() его уже разрушил —
     * use-after-destroy. g_close_guard (тот же, что в close(), см.
     * там) делает невозможным одновременное выполнение dispatch_reply()
     * и разрушающей части close() — т.е. либо dispatch_reply() успевает
     * полностью отработать (и его результат будет честно отброшен
     * внутри как -ESTALE/доставлен, если слот всё ещё WAITING), либо
     * close() уже полностью завершился и t->opened==0 (проверка ниже). */
    pthread_mutex_lock(&g_close_guard);
    if (!t->opened) {
        pthread_mutex_unlock(&g_close_guard);
        return -ENODEV;
    }

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
        pthread_mutex_unlock(&g_close_guard);
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
    pthread_mutex_unlock(&g_close_guard);

    return 0;
}

void nova_rpc_transport_close(nova_rpc_transport_t *t)
{
    int i;

    if (!t)
        return;

    pthread_mutex_lock(&g_close_guard);

    if (t->opened) {
        /* ТЗ §5.2, исправление реальной находки прошлого этапа (6-минутное
         * зависание/UB, CI run 43634985194): раньше close() сразу
         * разрушал pending_mutex/pending_cond, даже если другой поток
         * был внутри pthread_cond_timedwait() на НИХ ЖЕ — use-after-destroy.
         * Теперь: 1) помечаем CLOSING и будим всех ожидающих; 2) ждём,
         * пока каждый waiter сам не увидит CLOSING и не выйдет (см.
         * nova_rpc_transport_call() — цикл ожидания проверяет t->state);
         * 3) только после waiters==0 трогаем mutex/cond/fd. */
        pthread_mutex_lock(&t->pending_mutex);
        t->state = NOVA_RPC_T_STATE_CLOSING;
        pthread_cond_broadcast(&t->pending_cond);
        while (t->waiters > 0)
            pthread_cond_wait(&t->pending_cond, &t->pending_mutex);
        pthread_mutex_unlock(&t->pending_mutex);
    }

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
        for (i = 0; i < 6; i++)
            pthread_mutex_destroy(&t->send_mutex[i]);
    }

    memset(t, 0, sizeof(*t));
    t->fd_poll_audio = t->fd_intr_audio = -1;
    t->fd_poll_video = t->fd_intr_video = -1;
    t->fd_poll_video2 = t->fd_intr_video2 = -1;
    t->rfd_poll_audio = t->rfd_intr_audio = -1;
    t->rfd_poll_video = t->rfd_intr_video = -1;
    t->rfd_poll_video2 = t->rfd_intr_video2 = -1;
    t->state = NOVA_RPC_T_STATE_CLOSED;

    pthread_mutex_unlock(&g_close_guard);
}
