/*
 * rivergpu-test: exercise the RiverGPU IOKit interface from user space.
 *
 * Built for iOS arm64 by tools/build-rivergpu-test.sh, installed by the
 * kernel from the boot image (river_files.c, /usr/local/bin/rivergpu-test)
 * and run as PID 1 with the boot-arg riverinit=/usr/local/bin/rivergpu-test.
 * It prints to /dev/console (USB log + screen) and never exits, because PID 1
 * exiting panics the kernel. Selectors: see iokit/Kernel/arm/RiverGPU.cpp.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { GET_PARAM, BO_NEW, BO_FREE, SUBMIT, WAIT, CPU_PREP };
#define CP_MEM_WRITE	61

static io_connect_t conn;

static uint32_t
pm4_parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^
	    (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}
static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000 | cnt | (pm4_parity(cnt) << 15) | ((op & 0x7f) << 16) |
	    (pm4_parity(op) << 23);
}

struct bo {
	uint32_t handle;
	uint64_t gpuva, size;
	volatile uint32_t *cpu;
};

static kern_return_t
bo_new(uint64_t size, struct bo *b)
{
	uint64_t in = size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, BO_NEW, &in, 1, out, &n);
	if (kr) return kr;
	b->handle = (uint32_t)out[0];
	b->gpuva = out[1];
	b->size = out[2];
	mach_vm_address_t addr = 0;
	mach_vm_size_t msize = 0;
	kr = IOConnectMapMemory64(conn, b->handle, mach_task_self(), &addr, &msize, kIOMapAnywhere);
	b->cpu = (volatile uint32_t *)(uintptr_t)addr;
	return kr;
}

struct submit_req {
	uint32_t ncmd, nbos;
	struct { uint32_t handle, offset, size_dw; } cmd[2];
	uint32_t bos[1];
};

static kern_return_t
submit(const struct submit_req *r, size_t len, uint32_t *fence)
{
	uint64_t out = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallMethod(conn, SUBMIT, NULL, 0, r, len, &out, &n, NULL, NULL);
	*fence = (uint32_t)out;
	return kr;
}

static kern_return_t
call2(uint32_t sel, uint64_t a, uint64_t b)
{
	uint64_t in[2] = { a, b };
	return IOConnectCallScalarMethod(conn, sel, in, 2, NULL, NULL);
}

static double
ms_since(uint64_t t0)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

int
main(void)
{
	int fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("rivergpu-test: PID %d waiting for RiverGPU\n", getpid());

	io_service_t svc = 0;
	for (int i = 0; i < 60 && !svc; i++) {
		svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RiverGPU"));
		if (!svc) sleep(1);
	}
	if (!svc) goto idle;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	printf("rivergpu-test: IOServiceOpen -> %#x\n", kr);
	if (kr) goto idle;

	static const char *names[] = { "chip-id", "gpu-id", "gmem-size", "gmem-base", "va-start", "va-size", "fence" };
	for (uint32_t p = 0; p < 7; p++) {
		uint64_t in = p, out = 0;
		uint32_t n = 1;
		kr = IOConnectCallScalarMethod(conn, GET_PARAM, &in, 1, &out, &n);
		printf("rivergpu-test: param %s = %#llx (%#x)\n", names[p], out, kr);
	}

	struct bo tgt, cmd;
	kern_return_t k1 = bo_new(64 * 1024, &tgt), k2 = bo_new(16 * 1024, &cmd);
	printf("rivergpu-test: BOs target %u gpu %#llx cpu %p (%#x), cmd %u gpu %#llx cpu %p (%#x)\n",
	    tgt.handle, tgt.gpuva, (void *)tgt.cpu, k1, cmd.handle, cmd.gpuva, (void *)cmd.cpu, k2);
	if (k1 || k2 || !tgt.cpu || !cmd.cpu) goto idle;

	/* One stream of 256 CP_MEM_WRITEs, submitted as two command buffers. */
	enum { N = 256 };
	for (uint32_t i = 0; i < N; i++) {
		uint64_t dst = tgt.gpuva + i * 4;
		cmd.cpu[i * 4 + 0] = pkt7(CP_MEM_WRITE, 3);
		cmd.cpu[i * 4 + 1] = (uint32_t)dst;
		cmd.cpu[i * 4 + 2] = (uint32_t)(dst >> 32);
		cmd.cpu[i * 4 + 3] = 0x75000000u | i;
		tgt.cpu[i] = 0xdeadbeef;
	}
	struct submit_req r = { .ncmd = 2, .nbos = 1 };
	r.cmd[0].handle = cmd.handle; r.cmd[0].offset = 0; r.cmd[0].size_dw = N / 2 * 4;
	r.cmd[1].handle = cmd.handle; r.cmd[1].offset = N / 2 * 16; r.cmd[1].size_dw = N / 2 * 4;
	r.bos[0] = tgt.handle;
	uint32_t fence = 0;
	uint64_t t0 = mach_absolute_time();
	kr = submit(&r, sizeof(r), &fence);
	kern_return_t kw = kr ? kr : call2(WAIT, fence, 1000);
	kern_return_t kp = call2(CPU_PREP, tgt.handle, 1000);
	double ms = ms_since(t0);
	unsigned good = 0;
	for (uint32_t i = 0; i < N; i++) good += tgt.cpu[i] == (0x75000000u | i);
	printf("rivergpu-test: submit %#x fence %u wait %#x prep %#x in %.3f ms: %u/%u words written by the GPU\n",
	    kr, fence, kw, kp, ms, good, N);

	/* Round-trip latency: 500 x (submit one command buffer, wait). */
	struct submit_req one = { .ncmd = 1, .nbos = 1 };
	one.cmd[0].handle = cmd.handle; one.cmd[0].offset = 0; one.cmd[0].size_dw = 4;
	one.bos[0] = tgt.handle;
	size_t one_len = 8 + 12 + 4;
	unsigned ok = 0;
	t0 = mach_absolute_time();
	for (int i = 0; i < 500; i++) {
		if (!submit(&one, one_len, &fence) && !call2(WAIT, fence, 1000)) ok++;
	}
	ms = ms_since(t0);
	printf("rivergpu-test: 500 submit+wait round trips: %u ok, %.1f us each\n", ok, ms * 1000 / 500);

	/* The kernel must refuse bad requests. */
	struct submit_req bad = one;
	bad.cmd[0].handle = 999;
	kern_return_t kb1 = submit(&bad, one_len, &fence);
	bad = one;
	bad.cmd[0].offset = (uint32_t)cmd.size;
	kern_return_t kb2 = submit(&bad, one_len, &fence);
	kern_return_t kb3 = call2(CPU_PREP, 12345, 10);
	printf("rivergpu-test: refused bad handle %#x, out-of-range offset %#x, bad prep %#x (expect non-zero)\n",
	    kb1, kb2, kb3);

	uint64_t h = cmd.handle;
	kern_return_t kf = IOConnectCallScalarMethod(conn, BO_FREE, &h, 1, NULL, NULL);
	printf("rivergpu-test: BO_FREE %#x; closing\n", kf);
	IOServiceClose(conn);
	printf("rivergpu-test: done\n");
idle:
	for (;;) {
		sleep(3600);
	}
}
