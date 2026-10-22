/*
 * Harmless injection probe for riverenv=backboardd:DYLD_INSERT_LIBRARIES=...
 * (kernel patch 0182). Loaded into backboardd by dyld, it reports through
 * RiverGPU's LOG selector (the USB log) that it is running, then reports
 * which graphics libraries the process loads later (OpenGLES, QuartzCore,
 * IOSurface, Metal), which shows whether CoreAnimation's GL path is in use.
 * It changes nothing in the process.
 *   tools/build-river-inject-probe.sh OUT
 */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { LOG = 6 };

static io_connect_t conn;

static void
say(const char *fmt, ...)
{
	char line[200];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (conn != MACH_PORT_NULL && n > 0) {
		IOConnectCallMethod(conn, LOG, NULL, 0, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1),
		    NULL, NULL, NULL, NULL);
	}
}

static const char *const watched[] = {
	"OpenGLES", "QuartzCore", "IOSurface", "IOAccelerator", "Metal", "GLEngine", "libGL",
};

static void
image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)slide;
	Dl_info info;
	if (!dladdr(mh, &info) || info.dli_fname == NULL) {
		return;
	}
	for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); i++) {
		if (strstr(info.dli_fname, watched[i]) != NULL) {
			say("probe: image %s", info.dli_fname);
			return;
		}
	}
}

__attribute__((constructor))
static void
probe_init(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RiverGPU"));
	if (svc == IO_OBJECT_NULL) {
		return;         /* nothing to report to; stay out of the way */
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		conn = MACH_PORT_NULL;
		return;
	}
	const char *env = getenv("DYLD_INSERT_LIBRARIES");
	say("probe: loaded in %s pid %d uid %d, DYLD_INSERT_LIBRARIES=%s, %u images so far",
	    getprogname(), getpid(), getuid(), env ? env : "(unset)", _dyld_image_count());
	/* Called now for every image already loaded, then for each new one. */
	_dyld_register_func_for_add_image(image_added);
}
