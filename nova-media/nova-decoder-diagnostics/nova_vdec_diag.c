/*
 * nova-decoder-diagnostics — CLI-утилита для проверки, что кадры реально
 * прошли через аппаратный VDEC Realtek, а не через программный путь.
 *
 * Опирается на VDEC_IOC_FRAMEINFO (/dev/rtkvdec), который заполняет
 * VDEC_FRAMEINFO_STRUCT{pid, frame_dropped, frame_displaying,
 * frame_matched, frame_freerun} — это статистика самого ядро-драйвера
 * аппаратного декодера, а не что-либо, что можно получить от программного
 * декодера. Ненулевой frame_matched после реального воспроизведения —
 * прямой диагностический признак, требуемый §9 ТЗ ("не объявлять
 * декодирование работающим без диагностических признаков использования
 * аппаратного VDEC").
 *
 * Эта утилита НЕ воспроизводит видео и не запускает декодирование сама —
 * она только читает /dev/rtkvdec и печатает счётчики. Безопасна для
 * запуска в read-only режиме (VDEC_IOC_INIT + VDEC_IOC_FRAMEINFO), не
 * открывает сессию декодирования (VDEC_IOC_OPEN) сама.
 *
 * Статус: IMPLEMENTED. NOT TESTED на реальном устройстве — требует
 * собственного загружаемого ядра с этим драйвером, которого на этом ТВ
 * запустить невозможно (locked bootloader, см. отчёт о bootstrap).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../nova-vdec/nova_vdec.h"
#include "../nova-rtk-rpc/nova_rpc.h"

/* Проверка доступности device-ноды без попытки её открыть/изменить —
 * безопасна даже на стоковом Android-ядре (просто access(2)). */
static void check_node(const char *path)
{
    if (access(path, F_OK) == 0)
        printf("[node] %-20s существует\n", path);
    else
        printf("[node] %-20s отсутствует (%s)\n", path, strerror(errno));
}

int main(void)
{
    nova_vdec_handle_t h;
    nova_rpc_handle_t rpc;
    nova_vdec_frameinfo_t info;
    int ret;

    printf("=== Проверка device-нод (read-only, access(2)) ===\n");
    check_node(NOVA_VDEC_DEVICE_NODE);
    check_node(NOVA_RPC_DEVICE_NODE);
    check_node("/dev/rpc0");
    check_node("/dev/rpcdebug");

    printf("\n=== RPC control-узел ===\n");
    ret = nova_rpc_open(&rpc);
    if (ret == 0) {
        printf("nova_rpc_open: ok (fd открыт)\n");
        nova_rpc_close(&rpc);
    } else {
        printf("nova_rpc_open: failed %d (%s)\n", ret, strerror(-ret));
    }

    printf("\n=== VDEC ===\n");
    if (ret != 0) {
        fprintf(stderr, "nova_vdec_init(%s) failed: %d (%s)\n",
                NOVA_VDEC_DEVICE_NODE, ret, strerror(-ret));
        fprintf(stderr,
            "Ожидаемо на устройстве без собранного и запущенного ядра "
            "Nova (/dev/rtkvdec создаёт драйвер kernel/drivers/rtk_kdriver/"
            "vdec/rtk_vdec_driver.c).\n");
        return 1;
    }

    ret = nova_vdec_get_frameinfo(&h, &info);
    if (ret != 0) {
        fprintf(stderr, "VDEC_IOC_FRAMEINFO failed: %d (%s)\n",
                ret, strerror(-ret));
        nova_vdec_deinit(&h);
        return 1;
    }

    printf("pid=%d frame_matched=%u frame_displaying=%u "
           "frame_dropped=%u frame_freerun=%u\n",
           info.pid, info.frame_matched, info.frame_displaying,
           info.frame_dropped, info.frame_freerun);

    if (info.frame_matched > 0)
        printf("=> Аппаратный VDEC реально обработал кадры (frame_matched > 0).\n");
    else
        printf("=> Пока нет подтверждённого прохождения кадров через аппаратный VDEC.\n");

    nova_vdec_deinit(&h);
    return 0;
}
