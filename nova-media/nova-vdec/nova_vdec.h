/*
 * nova-vdec — userspace API аппаратного видеодекодера Realtek (rtd288o)
 * для Nova OS, без Android Framework.
 *
 * Все ioctl-номера и структуры ниже скопированы буквально из открытого
 * GPL-заголовка kernel/include/rtk_kdriver/rtk_vdec.h (репозиторий
 * TCLOpenSource/rt2851mr, форк dexsium/rt2851mr, commit
 * 80a2688976ac8f67b73ebfd5da5e55f3f09a472f, строки 214-237 для ioctl,
 * 59-205 для структур) — это не придуманный интерфейс и не V4L2-обёртка,
 * а тот же самый ABI, который используют закрытые librtk_omx_vdec.so /
 * librtk_kadp_vdec.so для разговора с /dev/rtkvdec. Разница в том, что
 * здесь он вызывается напрямую из Linux userspace, без OMX/Binder/HIDL.
 *
 * Драйвер ядра (/dev/rtkvdec, misc-устройство, см. rtk_vdec_driver.c:3181,
 * device_create(rtkvdec_class, ..., "rtkvdec")) НЕ переписывается — он уже
 * существует в открытом дереве и не завязан на Android (platform_driver,
 * обычный file_operations.unlocked_ioctl). Переписывать его нет смысла и
 * противоречило бы "не делать лишнего, что уже есть в открытом коде".
 *
 * ВАЖНО (честно, по правилам ТЗ): этот код реализует интерфейс К
 * существующему драйверу. Он не обходит и не решает отдельную проблему —
 * сама прошивка трёх MIPS-сопроцессоров (AVCPU) должна быть загружена
 * ДО того, как любой из этих вызовов сможет реально декодировать кадр
 * (см. NOVA_4K_VIDEO_DECODER_FORENSIC_AUDIT.md, NOVA_AVCPU_BOOTSTRAP_FEASIBILITY.md).
 * Без AVCPU эти ioctl либо вернут ошибку, либо (не проверено) будут висеть —
 * это НЕ проверялось на реальном железе в этой сессии.
 *
 * Статус: IMPLEMENTED (не TESTED, не COMPILED до подтверждения CI).
 */
#ifndef NOVA_VDEC_H
#define NOVA_VDEC_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOVA_VDEC_DEVICE_NODE "/dev/rtkvdec"

/* Буквально rtk_vdec.h:149-154 (VDEC_FRAMEINFO_STRUCT) */
typedef struct nova_vdec_frameinfo {
    pid_t pid;
    unsigned int frame_dropped;
    unsigned int frame_displaying;
    unsigned int frame_matched;
    unsigned int frame_freerun;
} nova_vdec_frameinfo_t;

/* Буквально rtk_vdec.h:162-166 (VDEC_SVPMEM_STRUCT) — защищённая (SVP)
 * область памяти для декодированных кадров / Secure Video Path. */
typedef struct nova_vdec_svpmem {
    pid_t pid;
    unsigned int addr;
    unsigned int size;
} nova_vdec_svpmem_t;

/* Буквально rtk_vdec.h:113-120 (VDEC_SET_T) — команды VDEC_IOC_SET. */
typedef enum nova_vdec_set_type {
    NOVA_VDEC_SET_BS_RB = 0,   /* bitstream ring buffer */
    NOVA_VDEC_SET_CMD_RB,      /* inband command ring buffer */
    NOVA_VDEC_SET_CC_RB,       /* closed-caption ring buffer */
    NOVA_VDEC_SET_REFCLOCK,
    NOVA_VDEC_SET_DECODEMODE,
    NOVA_VDEC_SET_SPEED
} nova_vdec_set_type_t;

typedef struct nova_vdec_set_cmd {
    nova_vdec_set_type_t type;
    int data;
    int handle;
} nova_vdec_set_cmd_t;

typedef struct nova_vdec_handle {
    int fd;       /* дескриптор /dev/rtkvdec */
    int opened;   /* VDEC_IOC_OPEN выполнен */
} nova_vdec_handle_t;

/* open("/dev/rtkvdec") + VDEC_IOC_INIT (_IO('v',1)). */
int nova_vdec_init(nova_vdec_handle_t *h);

/* VDEC_IOC_OPEN (_IOWR('v',3,int)) — открывает декодирующую сессию,
 * handle возвращается через ret_handle. */
int nova_vdec_open_session(nova_vdec_handle_t *h, int *ret_handle);

int nova_vdec_close_session(nova_vdec_handle_t *h, int handle);

/* VDEC_IOC_PLAY/PAUSE/FLUSH/STOP — _IOW('v', 5..8, int), аргумент — handle
 * сессии, возвращённый nova_vdec_open_session(). */
int nova_vdec_play(nova_vdec_handle_t *h, int handle);
int nova_vdec_pause(nova_vdec_handle_t *h, int handle);
int nova_vdec_flush(nova_vdec_handle_t *h, int handle);
int nova_vdec_stop(nova_vdec_handle_t *h, int handle);

/* VDEC_IOC_SET (_IOW('v',10,int)) — настройка ring buffer'ов/режима. */
int nova_vdec_set(nova_vdec_handle_t *h, const nova_vdec_set_cmd_t *cmd);

/* VDEC_IOC_FRAMEINFO (_IOW('v',14,...)) — диагностика: сколько кадров
 * реально прошло через декодер (frame_matched), сколько отброшено.
 * Это и есть "диагностический признак использования аппаратного VDEC" из
 * §9 ТЗ: ненулевой frame_matched при неизменном frame_dropped/CPU-load —
 * прямое, а не косвенное доказательство аппаратного декодирования. */
int nova_vdec_get_frameinfo(nova_vdec_handle_t *h, nova_vdec_frameinfo_t *out);

/* VDEC_IOC_ALLOCSVPM/FREESVPM — выделение/освобождение SVP-памяти кадров. */
int nova_vdec_alloc_svpmem(nova_vdec_handle_t *h, nova_vdec_svpmem_t *mem);
int nova_vdec_free_svpmem(nova_vdec_handle_t *h, nova_vdec_svpmem_t *mem);

void nova_vdec_deinit(nova_vdec_handle_t *h);

#ifdef __cplusplus
}
#endif
#endif /* NOVA_VDEC_H */
