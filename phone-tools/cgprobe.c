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
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SHARED_CACHE_UNSLID 0x180000000ULL

static void report_bt(void);
static id (*original_sbf_image)(id, SEL, CGSize, CGFloat, long, id, id);
static void (*original_legibility_update)(id, SEL);
static _Thread_local int in_clock_legibility;

static void
probe_legibility_update(id self, SEL cmd)
{
	static int calls;
	int log_this = __sync_fetch_and_add(&calls, 1) < 24;
	if (log_this) {
		CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
		Ivar ivar = class_getInstanceVariable(object_getClass(self), "_lookasideLabel");
		id label = ivar ? object_getIvar(self, ivar) : NULL;
		CGRect lookaside = label ? ((CGRect (*)(id, SEL))objc_msgSend)(label, sel_registerName("bounds")) : CGRectZero;
		CGSize fit = label ? ((CGSize (*)(id, SEL, CGSize))objc_msgSend)(label, sel_registerName("sizeThatFits:"), CGSizeMake(1000, 1000)) : CGSizeZero;
		dprintf(2, "cgprobe: legibility bounds %.2fx%.2f lookaside %.2fx%.2f fit %.2fx%.2f\n",
		    bounds.size.width, bounds.size.height, lookaside.size.width, lookaside.size.height, fit.width, fit.height);
		if (bounds.size.width > 100 && bounds.size.height > 80 &&
		    lookaside.size.width == 0 && lookaside.size.height == 0 &&
		    fit.width > 0 && fit.height > 0) {
			((void (*)(id, SEL, CGRect))objc_msgSend)(label, sel_registerName("setBounds:"),
			    CGRectMake(0, 0, fit.width, fit.height));
			((void (*)(id, SEL, CGRect))objc_msgSend)(label, sel_registerName("setFrame:"),
			    CGRectMake(0, 0, fit.width, fit.height));
			dprintf(2, "cgprobe: clock lookaside frame set %.2fx%.2f\n", fit.width, fit.height);
		}
	}
	CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	int previous_clock = in_clock_legibility;
	if (bounds.size.width > 100 && bounds.size.height > 80) in_clock_legibility = 1;
	original_legibility_update(self, cmd);
	in_clock_legibility = previous_clock;
}

static id
probe_sbf_image(id self, SEL cmd, CGSize size, CGFloat scale, long type, id pool, id drawing)
{
	static int calls;
	int log_this = __sync_fetch_and_add(&calls, 1) < 8;
	id result = original_sbf_image(self, cmd, size, scale, type, pool, drawing);
	if (log_this || result == NULL || in_clock_legibility) {
		dprintf(2, "cgprobe: sbf_image size %.3fx%.3f scale %.3f type %ld pool %p drawing %p result %p\n",
		    size.width, size.height, scale, (long)type, pool, drawing, result);
		if (result == NULL) {
			report_bt();
		}
	}
	return result;
}

__attribute__((constructor)) static void
probe_loaded(void)
{
	os_log_error(OS_LOG_DEFAULT, "cgprobe: loaded pid %d", getpid());
	dprintf(2, "cgprobe: loaded pid %d\n", getpid());
	Class image = objc_getClass("UIImage");
	SEL selector = sel_registerName("sbf_imageFromContextWithSize:scale:type:pool:drawing:");
	Method method = image ? class_getClassMethod(image, selector) : NULL;
	if (method != NULL) {
		original_sbf_image = (void *)method_setImplementation(method, (IMP)probe_sbf_image);
	}
	dprintf(2, "cgprobe: sbf_image hook %s\n", original_sbf_image ? "installed" : "unavailable");
	Class legibility = objc_getClass("SBUILegibilityLabel");
	Method update = legibility ? class_getInstanceMethod(legibility, sel_registerName("_updateLegibilityView")) : NULL;
	if (update != NULL) {
		original_legibility_update = (void *)method_setImplementation(update, (IMP)probe_legibility_update);
	}
	dprintf(2, "cgprobe: legibility hook %s\n", original_legibility_update ? "installed" : "unavailable");
}

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
report_bt(void)
{
	void *frames[24];
	int n = backtrace(frames, 24);
	char line[1024];
	size_t len = 0;
	uint64_t slide = cache_slide();
	for (int i = 2; i < n && len < sizeof(line) - 40; i++) {
		uint64_t a = (uint64_t)frames[i];
		Dl_info di;
		const char *img = (dladdr(frames[i], &di) && di.dli_fname) ? strrchr(di.dli_fname, '/') : NULL;
		len += (size_t)snprintf(line + len, sizeof(line) - len, " %s:0x%llx", img ? img + 1 : "?",
		    (unsigned long long)(a - slide));
	}
	os_log_error(OS_LOG_DEFAULT, "cgprobe: bt (unslid, slide 0x%llx):%{public}s", (unsigned long long)slide, line);
	dprintf(2, "cgprobe: bt (unslid, slide 0x%llx):%s\n", (unsigned long long)slide, line);
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
	dprintf(2, "cgprobe: %s NULL: %zux%zu bpc %zu bpr %zu info 0x%x cs %s\n",
	    fn, w, h, bpc, bpr, info, csname);
	report_bt();
}

static void
report_invalid(const char *fn)
{
	static int reports;
	if (reports++ >= 12) {
		return;
	}
	os_log_error(OS_LOG_DEFAULT, "cgprobe: %{public}s called with NULL context", fn);
	dprintf(2, "cgprobe: %s called with NULL context\n", fn);
	report_bt();
}

static CGContextRef
probe_create(void *data, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs, uint32_t info)
{
	static int first;
	if (!first++) {
		dprintf(2, "cgprobe: first CGBitmapContextCreate %zux%zu\n", w, h);
	}
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
	static int first;
	if (!first++) {
		dprintf(2, "cgprobe: first CGBitmapContextCreateWithData %zux%zu\n", w, h);
	}
	CGContextRef c = CGBitmapContextCreateWithData(data, w, h, bpc, bpr, cs, info, cb, cbinfo);
	if (c == NULL) {
		report("CGBitmapContextCreateWithData", w, h, bpc, bpr, cs, info);
	}
	return c;
}

static void
probe_save_gstate(CGContextRef c)
{
	static int first;
	if (!first++) {
		dprintf(2, "cgprobe: first CGContextSaveGState %p\n", c);
	}
	if (c == NULL) {
		report_invalid("CGContextSaveGState");
	}
	CGContextSaveGState(c);
}

static CGImageRef
probe_create_image(CGContextRef c)
{
	static int first;
	if (!first++) {
		dprintf(2, "cgprobe: first CGBitmapContextCreateImage %p\n", c);
	}
	if (c == NULL) {
		report_invalid("CGBitmapContextCreateImage");
	}
	return CGBitmapContextCreateImage(c);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)probe_create, (const void *)CGBitmapContextCreate },
	{ (const void *)probe_create_with_data, (const void *)CGBitmapContextCreateWithData },
	{ (const void *)probe_save_gstate, (const void *)CGContextSaveGState },
	{ (const void *)probe_create_image, (const void *)CGBitmapContextCreateImage },
};
