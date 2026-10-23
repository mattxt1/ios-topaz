/*
 * cgprobe: inserted into one process (riverenv=NAME:DYLD_INSERT_LIBRARIES=
 * /usr/local/lib/libcgprobe.dylib). When CGBitmapContextCreate[WithData]
 * returns NULL, log the request and the caller's return addresses as
 * unslid shared-cache addresses (symbolicate on the Mac with ipsw dyld a2s).
 * Read with: oslog NAME | grep cgprobe
 */
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SHARED_CACHE_UNSLID 0x180000000ULL

static uint64_t
cache_slide(void)
{
	static uint64_t slide = ~0ULL;
	if (slide == ~0ULL) {
		uint64_t base = 0;
		slide = syscall(294 /* shared_region_check_np */, &base) == 0 ? base - SHARED_CACHE_UNSLID : 0;
	}
	return slide;
}

static void
report(const char *fn, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs, uint32_t info)
{
	static int reports;
	if (reports++ >= 40) {
		return;
	}
	char csname[96] = "null";
	if (cs != NULL) {
		CFStringRef n = CGColorSpaceCopyName(cs);
		if (n != NULL) {
			CFStringGetCString(n, csname, sizeof(csname), kCFStringEncodingUTF8);
			CFRelease(n);
		} else {
			snprintf(csname, sizeof(csname), "model %d comps %zu", CGColorSpaceGetModel(cs),
			    CGColorSpaceGetNumberOfComponents(cs));
		}
	}
	os_log_error(OS_LOG_DEFAULT, "cgprobe: %{public}s NULL: %zux%zu bpc %zu bpr %zu info 0x%x cs %{public}s",
	    fn, w, h, bpc, bpr, info, csname);
	void *frames[24];
	int n = backtrace(frames, 24);
	char line[1024];
	size_t len = 0;
	uint64_t slide = cache_slide();
	for (int i = 1; i < n && len < sizeof(line) - 40; i++) {
		uint64_t a = (uint64_t)frames[i];
		Dl_info di;
		const char *img = (dladdr(frames[i], &di) && di.dli_fname) ? strrchr(di.dli_fname, '/') : NULL;
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s:0x%llx", img ? img + 1 : "?",
		    (unsigned long long)(a - slide));
	}
	os_log_error(OS_LOG_DEFAULT, "cgprobe: bt (unslid, slide 0x%llx):%{public}s", (unsigned long long)slide, line);
}

static CGContextRef
probe_create(void *data, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs, uint32_t info)
{
	CGContextRef c = CGBitmapContextCreate(data, w, h, bpc, bpr, cs, info);
	if (c == NULL) {
		report("CGBitmapContextCreate", w, h, bpc, bpr, cs, info);
	}
	return c;
}

static CGContextRef
probe_create_with_data(void *data, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs,
    uint32_t info, CGBitmapContextReleaseDataCallback cb, void *cbinfo)
{
	CGContextRef c = CGBitmapContextCreateWithData(data, w, h, bpc, bpr, cs, info, cb, cbinfo);
	if (c == NULL) {
		report("CGBitmapContextCreateWithData", w, h, bpc, bpr, cs, info);
	}
	return c;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)probe_create, (const void *)CGBitmapContextCreate },
	{ (const void *)probe_create_with_data, (const void *)CGBitmapContextCreateWithData },
};
