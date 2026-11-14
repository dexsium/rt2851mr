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

nova_vdec_diag.o: nova-decoder-diagnostics/nova_vdec_diag.c nova-vdec/nova_vdec.h
	$(CC) $(CFLAGS) -c nova-decoder-diagnostics/nova_vdec_diag.c -o nova_vdec_diag.o

nova_vdec_diag: nova_vdec.o nova_rpc.o nova_vdec_diag.o
	$(CC) $(CFLAGS) -o nova_vdec_diag nova_vdec.o nova_rpc.o nova_vdec_diag.o

clean:
	rm -f *.o nova_vdec_diag

.PHONY: all clean
