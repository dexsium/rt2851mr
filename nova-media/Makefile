# Makefile для компиляции nova-vdec / nova-rtk-rpc / nova-decoder-diagnostics
# кросс-тулчейном ARM (arm-linux-gnueabihf-gcc), той же архитектуры, что и
# реальное устройство (32-бит ARMv7, Cortex-A55 в aarch32-режиме). Не
# подменяет ARM32 сборкой под ARM64.

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -Inova-vdec -Inova-rtk-rpc
LDLIBS = -lpthread

all: nova_vdec_diag

nova_vdec.o: nova-vdec/nova_vdec.c nova-vdec/nova_vdec.h
	$(CC) $(CFLAGS) -c nova-vdec/nova_vdec.c -o nova_vdec.o

nova_rpc.o: nova-rtk-rpc/nova_rpc.c nova-rtk-rpc/nova_rpc.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_rpc.c -o nova_rpc.o

nova_rpc_xdr.o: nova-rtk-rpc/nova_rpc_xdr.c nova-rtk-rpc/nova_rpc_xdr.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_rpc_xdr.c -o nova_rpc_xdr.o

nova_rpc_transport.o: nova-rtk-rpc/nova_rpc_transport.c nova-rtk-rpc/nova_rpc_transport.h nova_rpc_xdr.o
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_rpc_transport.c -o nova_rpc_transport.o

nova_rpc_callbacks.o: nova-rtk-rpc/nova_rpc_callbacks.c nova-rtk-rpc/nova_rpc_callbacks.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_rpc_callbacks.c -o nova_rpc_callbacks.o

nova_frame.o: nova-frame/nova_frame.c nova-frame/nova_frame.h
	$(CC) $(CFLAGS) -Inova-frame -c nova-frame/nova_frame.c -o nova_frame.o

nova_video_rpc.o: nova-rtk-rpc/nova_video_rpc.c nova-rtk-rpc/nova_video_rpc.h nova-rtk-rpc/nova_rpc_protocol.h nova-rtk-rpc/nova_rpc_transport.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_video_rpc.c -o nova_video_rpc.o

nova_vo_rpc.o: nova-rtk-rpc/nova_vo_rpc.c nova-rtk-rpc/nova_vo_rpc.h nova-rtk-rpc/nova_rpc_protocol.h nova-rtk-rpc/nova_rpc_transport.h
	$(CC) $(CFLAGS) -c nova-rtk-rpc/nova_vo_rpc.c -o nova_vo_rpc.o

nova_vdec_diag.o: nova-decoder-diagnostics/nova_vdec_diag.c nova-vdec/nova_vdec.h nova-rtk-rpc/nova_rpc.h
	$(CC) $(CFLAGS) -c nova-decoder-diagnostics/nova_vdec_diag.c -o nova_vdec_diag.o

NOVA_OBJS = nova_vdec.o nova_rpc.o nova_rpc_xdr.o nova_rpc_transport.o nova_rpc_callbacks.o \
            nova_video_rpc.o nova_vo_rpc.o nova_frame.o nova_vdec_diag.o

nova_vdec_diag: $(NOVA_OBJS)
	$(CC) $(CFLAGS) -o nova_vdec_diag $(NOVA_OBJS) $(LDLIBS)

# Хостовые mock/ABI-тесты (могут собираться CROSS_COMPILE= пустым в CI —
# _IO*-макросы и XDR big-endian-кодировка дают одинаковый результат на
# ARM и x86_64).
test_ioctl_abi: tests/test_ioctl_abi.c
	$(CC) $(CFLAGS) -o test_ioctl_abi tests/test_ioctl_abi.c

test_rpc_protocol: tests/test_rpc_protocol.c nova-rtk-rpc/nova_rpc_protocol.h
	$(CC) $(CFLAGS) -o test_rpc_protocol tests/test_rpc_protocol.c

test_rpc_xdr: tests/test_rpc_xdr.c nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_xdr tests/test_rpc_xdr.c nova_rpc_xdr.o

test_rpc_callbacks: tests/test_rpc_callbacks.c nova_rpc_callbacks.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_callbacks tests/test_rpc_callbacks.c nova_rpc_callbacks.o nova_rpc_xdr.o $(LDLIBS)

test_rpc_transport: tests/test_rpc_transport.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_transport tests/test_rpc_transport.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_video_rpc: tests/test_video_rpc.c nova_video_rpc.o nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_video_rpc tests/test_video_rpc.c nova_video_rpc.o nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_vo_rpc: tests/test_vo_rpc.c nova_vo_rpc.o nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_vo_rpc tests/test_vo_rpc.c nova_vo_rpc.o nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_rpc_e2e: tests/test_rpc_e2e.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_e2e tests/test_rpc_e2e.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_rpc_concurrency: tests/test_rpc_concurrency.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_concurrency tests/test_rpc_concurrency.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_rpc_lifecycle: tests/test_rpc_lifecycle.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_lifecycle tests/test_rpc_lifecycle.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

test_rpc_fault_injection: tests/test_rpc_fault_injection.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o test_rpc_fault_injection tests/test_rpc_fault_injection.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

bench_rpc_transport: tests/bench_rpc_transport.c nova_rpc_transport.o nova_rpc_xdr.o
	$(CC) $(CFLAGS) -o bench_rpc_transport tests/bench_rpc_transport.c nova_rpc_transport.o nova_rpc_xdr.o $(LDLIBS)

MOCK_TESTS = test_ioctl_abi test_rpc_protocol test_rpc_xdr test_rpc_callbacks \
             test_rpc_transport test_video_rpc test_vo_rpc test_rpc_e2e \
             test_rpc_concurrency test_rpc_lifecycle test_rpc_fault_injection

mock-tests: $(MOCK_TESTS)

clean:
	rm -f *.o nova_vdec_diag $(MOCK_TESTS) bench_rpc_transport

.PHONY: all clean mock-tests
