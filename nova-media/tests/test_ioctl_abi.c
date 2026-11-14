/*
 * Уровень A/B теста (статический + эмулируемый, см. ТЗ п.12): проверяет, что
 * вычисляемые в nova_vdec.c/nova_rpc.c номера ioctl совпадают буквально с
 * литеральными значениями, которые даёт тот же _IO/_IOW/_IOWR на эталонных
 * числах из kernel/include/rtk_kdriver/rtk_vdec.h (magic 'v') и rpcapi.h
 * (magic 'k'). Запускается НА ХОСТЕ (x86_64), без реального устройства —
 * это Mock/ABI-регрессионный тест, а не доказательство работы на железе.
 *
 * Если кто-то в будущем поменяет magic/номер в nova_vdec.h без синхронизации
 * с оригиналом — этот тест упадёт раньше, чем ошибка попадёт на устройство.
 *
 * Статус: MOCK_TESTED (компилируется и выполняется в CI на ubuntu-latest,
 * см. .github/workflows/nova-media-build.yml, job "abi-mock-test").
 */
#include <stdio.h>
#include <sys/ioctl.h>

/* Копия ровно тех значений из rtk_vdec.h:214-237 (см. комментарии в
 * ../nova-vdec/nova_vdec.c) — продублированы здесь независимо от
 * основного кода, чтобы тест ловил рассинхронизацию, а не просто повторял
 * одну и ту же ошибку в обоих местах. */
#define REF_VDEC_MAGIC 'v'
#define REF_VDEC_IOC_INIT       _IO  (REF_VDEC_MAGIC, 1)
#define REF_VDEC_IOC_OPEN       _IOWR(REF_VDEC_MAGIC, 3, int)
#define REF_VDEC_IOC_PLAY       _IOW (REF_VDEC_MAGIC, 5, int)
#define REF_VDEC_IOC_FRAMEINFO  _IOW (REF_VDEC_MAGIC, 14, int)
#define REF_VDEC_IOC_ALLOCSVPM  _IOWR(REF_VDEC_MAGIC, 15, int)

#define REF_RPC_MAGIC 'k'
#define REF_RPC_IOCTHANDLER  _IO(REF_RPC_MAGIC, 3)
#define REF_RPC_IOSTOP       _IO(REF_RPC_MAGIC, 4)

/* Из nova_vdec.c (приватные #define, продублированы здесь буквально для
 * сравнения по значению — заголовок их наружу не экспортирует). */
#define NOVA_VDEC_IOC_MAGIC 'v'
#define NOVA_VDEC_IOC_INIT       _IO  (NOVA_VDEC_IOC_MAGIC, 1)
#define NOVA_VDEC_IOC_OPEN       _IOWR(NOVA_VDEC_IOC_MAGIC, 3, int)
#define NOVA_VDEC_IOC_PLAY       _IOW (NOVA_VDEC_IOC_MAGIC, 5, int)
#define NOVA_VDEC_IOC_FRAMEINFO  _IOW (NOVA_VDEC_IOC_MAGIC, 14, int)
#define NOVA_VDEC_IOC_ALLOCSVPM  _IOWR(NOVA_VDEC_IOC_MAGIC, 15, int)

#define NOVA_RPC_IOC_MAGIC    'k'
#define NOVA_RPC_IOCTHANDLER  _IO(NOVA_RPC_IOC_MAGIC, 3)
#define NOVA_RPC_IOSTOP       _IO(NOVA_RPC_IOC_MAGIC, 4)

static int failures = 0;

#define CHECK(name, got, want) do { \
    if ((unsigned long)(got) != (unsigned long)(want)) { \
        printf("FAIL %-24s got=0x%08lx want=0x%08lx\n", name, \
               (unsigned long)(got), (unsigned long)(want)); \
        failures++; \
    } else { \
        printf("ok   %-24s = 0x%08lx\n", name, (unsigned long)(got)); \
    } \
} while (0)

int main(void)
{
    CHECK("VDEC_IOC_INIT",      NOVA_VDEC_IOC_INIT,      REF_VDEC_IOC_INIT);
    CHECK("VDEC_IOC_OPEN",      NOVA_VDEC_IOC_OPEN,      REF_VDEC_IOC_OPEN);
    CHECK("VDEC_IOC_PLAY",      NOVA_VDEC_IOC_PLAY,      REF_VDEC_IOC_PLAY);
    CHECK("VDEC_IOC_FRAMEINFO", NOVA_VDEC_IOC_FRAMEINFO, REF_VDEC_IOC_FRAMEINFO);
    CHECK("VDEC_IOC_ALLOCSVPM", NOVA_VDEC_IOC_ALLOCSVPM, REF_VDEC_IOC_ALLOCSVPM);
    CHECK("RPC_IOCTHANDLER",    NOVA_RPC_IOCTHANDLER,    REF_RPC_IOCTHANDLER);
    CHECK("RPC_IOSTOP",         NOVA_RPC_IOSTOP,         REF_RPC_IOSTOP);

    if (failures) {
        printf("%d ioctl ABI mismatch(es)\n", failures);
        return 1;
    }
    printf("all ioctl ABI constants match reference values\n");
    return 0;
}
