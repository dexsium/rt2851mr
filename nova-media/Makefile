# Makefile для компиляции nova-vdec / nova-rtk-rpc / nova-decoder-diagnostics
# кросс-тулчейном ARM (arm-linux-gnueabihf-gcc), той же архитектуры, что и
# реальное устройство (32-бит ARMv7, Cortex-A55 в aarch32-режиме — см.
# NOVA_RTD288O_32BIT_VS_64BIT в предыдущих отчётах). Не подменяет ARM32
# сборкой под ARM64 (прямое требование §10 ТЗ).

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -Inova-vdec -Inova-rtk-rpc

all: nova_vdec_diag

nova_vdec.o: nova-vdec/nova_vdec.c nova-vdec/nova_vdec.h
	$(CC) $(CFLAGS) -c nova-vdec/nova_vdec.c -o nova_vdec.o

nova_rpc.o: nova-rtk-rpc/nova_rpc.c nova-rtk-rpc/nova_rpc.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_rpc.c -o nova_rpc.o

nova_frame.o: nova-frame/nova_frame.c nova-frame/nova_frame.h
	$(CC) $(CFLAGS) -Inova-frame -c nova-frame/nova_frame.c -o nova_frame.o

nova_video_rpc.o: nova-rtk-rpc/nova_video_rpc.c nova-rtk-rpc/nova_video_rpc.h nova-rtk-rpc/nova_rpc_protocol.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_video_rpc.c -o nova_video_rpc.o

nova_vo_rpc.o: nova-rtk-rpc/nova_vo_rpc.c nova-rtk-rpc/nova_vo_rpc.h nova-rtk-rpc/nova_rpc_protocol.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_vo_rpc.c -o nova_vo_rpc.o

nova_vdec_diag.o: nova-decoder-diagnostics/nova_vdec_diag.c nova-vdec/nova_vdec.h nova-rtk-rpc/nova_rpc.h
	$(CC) $(CFLAGS) -c nova-decoder-diagnostics/nova_vdec_diag.c -o nova_vdec_diag.o

nova_vdec_diag: nova_vdec.o nova_rpc.o nova_video_rpc.o nova_vo_rpc.o nova_frame.o nova_vdec_diag.o
	$(CC) $(CFLAGS) -o nova_vdec_diag nova_vdec.o nova_rpc.o nova_video_rpc.o nova_vo_rpc.o nova_frame.o nova_vdec_diag.o

# test_ioctl_abi собирается ТЕМ ЖЕ компилятором, что указан в CC (может быть
# хостовым gcc в CI job "abi-mock-test" — см. nova-media-build.yml), не
# обязательно кросс-тулчейном: макросы _IO/_IOW дают одинаковый результат на
# всех архитектурах, использующих общий asm-generic/ioctl.h (ARM и x86_64—
# да; sparc/alpha/powerpc исторически нет, но это не наша цель).
test_ioctl_abi: tests/test_ioctl_abi.c
	$(CC) $(CFLAGS) -o test_ioctl_abi tests/test_ioctl_abi.c

test_rpc_protocol: tests/test_rpc_protocol.c nova-rtk-rpc/nova_rpc_protocol.h
	$(CC) $(CFLAGS) -o test_rpc_protocol tests/test_rpc_protocol.c

clean:
	rm -f *.o nova_vdec_diag test_ioctl_abi test_rpc_protocol

.PHONY: all clean
