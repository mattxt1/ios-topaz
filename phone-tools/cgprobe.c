/*
 * cgprobe: inserted into SpringBoard via riverenv2.
 * Logs diagnostics directly to /tmp/cgprobe.log.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <os/log.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SHARED_CACHE_UNSLID 0x180000000ULL

static void cglog(const char *fmt, ...)
{
	static int fd = -1;
	if (fd < 0) {
		fd = open("/tmp/cgprobe.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	}
	if (fd >= 0) {
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		if (n > 0) {
			write(fd, buf, (size_t)n);
		}
	}
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
	cglog("cgprobe: bt (slide 0x%llx):%s\n", (unsigned long long)slide, line);
}

static id (*original_sbf_image)(id, SEL, CGSize, CGFloat, long, id, id);
static void (*original_legibility_update)(id, SEL);
static _Thread_local int in_clock_legibility;

static void
probe_legibility_update(id self, SEL cmd)
{
	CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	Ivar ivar = class_getInstanceVariable(object_getClass(self), "_lookasideLabel");
	id label = ivar ? object_getIvar(self, ivar) : NULL;
	CGRect lookaside = label ? ((CGRect (*)(id, SEL))objc_msgSend)(label, sel_registerName("bounds")) : CGRectZero;
	CGSize fit = label ? ((CGSize (*)(id, SEL, CGSize))objc_msgSend)(label, sel_registerName("sizeThatFits:"), CGSizeMake(1000, 1000)) : CGSizeZero;
	cglog("cgprobe: legibility bounds %.2fx%.2f lookaside %.2fx%.2f fit %.2fx%.2f\n",
	    bounds.size.width, bounds.size.height, lookaside.size.width, lookaside.size.height, fit.width, fit.height);
	if (bounds.size.width > 100 && bounds.size.height > 80 &&
	    lookaside.size.width == 0 && lookaside.size.height == 0 &&
	    fit.width > 0 && fit.height > 0) {
		((void (*)(id, SEL, CGRect))objc_msgSend)(label, sel_registerName("setBounds:"),
		    CGRectMake(0, 0, fit.width, fit.height));
		((void (*)(id, SEL, CGRect))objc_msgSend)(label, sel_registerName("setFrame:"),
		    CGRectMake(0, 0, fit.width, fit.height));
		cglog("cgprobe: clock lookaside frame set %.2fx%.2f\n", fit.width, fit.height);
	}
	int previous_clock = in_clock_legibility;
	if (bounds.size.width > 100 && bounds.size.height > 80) in_clock_legibility = 1;
	original_legibility_update(self, cmd);
	in_clock_legibility = previous_clock;
}

static id
probe_sbf_image(id self, SEL cmd, CGSize size, CGFloat scale, long type, id pool, id drawing)
{
	static int calls;
	int count = __sync_fetch_and_add(&calls, 1);
	if (size.width <= 0 || size.height <= 0) {
		if (count < 3) {
			cglog("cgprobe: sbf_image called with zero size (%.3fx%.3f), caller:\n", size.width, size.height);
			report_bt();
		}
		size = CGSizeMake(360.0, 120.0);
	}
	id result = original_sbf_image(self, cmd, size, scale, type, pool, drawing);
	if (count < 15 || in_clock_legibility) {
		cglog("cgprobe: sbf_image size %.3fx%.3f scale %.3f type %ld pool %p result %p\n",
		    size.width, size.height, scale, (long)type, pool, result);
	}
	return result;
}

static void
report(const char *fn, size_t w, size_t h, size_t bpc, size_t bpr, CGColorSpaceRef cs, uint32_t info)
{
	static int reports;
	if (reports++ >= 100) return;
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
	cglog("cgprobe: %s NULL: %zux%zu bpc %zu bpr %zu info 0x%x cs %s\n",
	    fn, w, h, bpc, bpr, info, csname);
	report_bt();
}

static void
report_invalid(const char *fn)
{
	static int reports;
	if (reports++ >= 30) return;
	cglog("cgprobe: %s called with NULL context\n", fn);
	report_bt();
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

static void
probe_save_gstate(CGContextRef c)
{
	if (c == NULL) report_invalid("CGContextSaveGState");
	else CGContextSaveGState(c);
}

static CGImageRef
probe_create_image(CGContextRef c)
{
	if (c == NULL) {
		report_invalid("CGBitmapContextCreateImage");
		return NULL;
	}
	return CGBitmapContextCreateImage(c);
}

static void dump_class_info(Class cls)
{
	if (!cls) return;
	cglog("cgprobe: dump %s : %s\n", class_getName(cls), class_getName(class_getSuperclass(cls)));
	unsigned int n = 0;
	Method *m = class_copyMethodList(cls, &n);
	for (unsigned int i = 0; i < n; i++) {
		const char *mname = sel_getName(method_getName(m[i]));
		if (strstr(mname, "Backdrop") || strstr(mname, "blur") || strstr(mname, "Blur") ||
		    strstr(mname, "Weight") || strstr(mname, "weight") || strstr(mname, "Recipe") ||
		    strstr(mname, "recipe") || strstr(mname, "set") || strstr(mname, "is") ||
		    strstr(mname, "layout") || strstr(mname, "Style")) {
			cglog("  m: %s\n", mname);
		}
	}
	if (m) free(m);
	Ivar *iv = class_copyIvarList(cls, &n);
	for (unsigned int i = 0; i < n; i++) {
		cglog("  iv: %s (%s)\n", ivar_getName(iv[i]), ivar_getTypeEncoding(iv[i]));
	}
	if (iv) free(iv);
}

static void dump_instance_ivars(id obj)
{
	if (!obj) return;
	unsigned int n = 0;
	Ivar *iv = class_copyIvarList(object_getClass(obj), &n);
	cglog("cgprobe: instance %s %p ivars (%u):\n", object_getClassName(obj), obj, n);
	for (unsigned int i = 0; i < n; i++) {
		const char *name = ivar_getName(iv[i]);
		ptrdiff_t offset = ivar_getOffset(iv[i]);
		void **ptr = (void **)((char *)obj + offset);
		cglog("  iv: %s = %p\n", name, *ptr);
	}
	if (iv) free(iv);
}

static void (*original_set_wallpaper_obscured)(id, SEL, BOOL);
static void probe_set_wallpaper_obscured(id self, SEL cmd, BOOL obscured)
{
	cglog("cgprobe: %s %p setWallpaperObscured:%d (forcing NO)\n", object_getClassName(self), self, obscured);
	original_set_wallpaper_obscured(self, cmd, NO);
}

static void (*original_pbui_remote_set_wallpaper_obscured)(id, SEL, BOOL);
static void probe_pbui_remote_set_wallpaper_obscured(id self, SEL cmd, BOOL obscured)
{
	cglog("cgprobe: %s %p setWallpaperObscured:%d (forcing NO)\n", object_getClassName(self), self, obscured);
	original_pbui_remote_set_wallpaper_obscured(self, cmd, NO);
}

static void (*original_pbui_poster_set_wallpaper_obscured)(id, SEL, BOOL);
static void probe_pbui_poster_set_wallpaper_obscured(id self, SEL cmd, BOOL obscured)
{
	cglog("cgprobe: %s %p setWallpaperObscured:%d (forcing NO)\n", object_getClassName(self), self, obscured);
	original_pbui_poster_set_wallpaper_obscured(self, cmd, NO);
}

static void (*original_pbui_remote_layout)(id, SEL);
static void probe_pbui_remote_layout(id self, SEL cmd)
{
	original_pbui_remote_layout(self, cmd);
	id v = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("view"));
	if (v) {
		id subs = ((id (*)(id, SEL))objc_msgSend)(v, sel_registerName("subviews"));
		unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
		for (unsigned long i = 0; i < sc; i++) {
			id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
			const char *cname = object_getClassName(sv);
			if (strcmp(cname, "UIView") == 0) {
				id chs = ((id (*)(id, SEL))objc_msgSend)(sv, sel_registerName("subviews"));
				unsigned long cc = chs ? ((unsigned long (*)(id, SEL))objc_msgSend)(chs, sel_registerName("count")) : 0;
				if (cc == 0) {
					((void (*)(id, SEL, BOOL))objc_msgSend)(sv, sel_registerName("setHidden:"), YES);
					((void (*)(id, SEL, CGFloat))objc_msgSend)(sv, sel_registerName("setAlpha:"), 0.0f);
					cglog("cgprobe: hid wallpaper dimming view %p\n", sv);
				}
			}
		}
	}
}

static void dump_visible_windows(void)
{
	Class w = objc_getClass("UIWindow");
	if (!w) return;
	id wins = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)w,
	    sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:"), YES, YES);
	unsigned long nw = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;
	cglog("cgprobe: === VISIBLE WINDOWS (%lu) ===\n", nw);
	for (unsigned long i = 0; i < nw; i++) {
		id win = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
		id rvc = ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("rootViewController"));
		CGRect f = ((CGRect (*)(id, SEL))objc_msgSend)(win, sel_registerName("frame"));
		cglog("  win[%lu] %s (rvc: %s, frame: %.0f,%.0f %.0fx%.0f, hidden=%d, alpha=%.2f)\n",
		    i, object_getClassName(win), rvc ? object_getClassName(rvc) : "nil",
		    f.origin.x, f.origin.y, f.size.width, f.size.height,
		    ((BOOL (*)(id, SEL))objc_msgSend)(win, sel_registerName("isHidden")),
		    ((CGFloat (*)(id, SEL))objc_msgSend)(win, sel_registerName("alpha")));
	}
}

static void (*original_dock_layout)(id, SEL);
static void probe_dock_layout(id self, SEL cmd)
{
	original_dock_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBDockView") != 0) return;
	CGRect df = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("frame"));
	Ivar bg_iv = class_getInstanceVariable(object_getClass(self), "_backgroundView");
	id bg = bg_iv ? object_getIvar(self, bg_iv) : nil;
	if (bg) {
		CGRect bgf = ((CGRect (*)(id, SEL))objc_msgSend)(bg, sel_registerName("frame"));
		id bgl = ((id (*)(id, SEL))objc_msgSend)(bg, sel_registerName("layer"));
		if (bgl) {
			((void (*)(id, SEL, id))objc_msgSend)(bgl, sel_registerName("setFilters:"), nil);
			id bg_subs = ((id (*)(id, SEL))objc_msgSend)(bg, sel_registerName("subviews"));
			unsigned long bsc = bg_subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(bg_subs, sel_registerName("count")) : 0;
			for (unsigned long i = 0; i < bsc; i++) {
				id ch = ((id (*)(id, SEL, unsigned long))objc_msgSend)(bg_subs, sel_registerName("objectAtIndex:"), i);
				((void (*)(id, SEL, BOOL))objc_msgSend)(ch, sel_registerName("setHidden:"), YES);
			}
			id bg_subl = ((id (*)(id, SEL))objc_msgSend)(bgl, sel_registerName("sublayers"));
			unsigned long blc = bg_subl ? ((unsigned long (*)(id, SEL))objc_msgSend)(bg_subl, sel_registerName("count")) : 0;
			for (unsigned long i = 0; i < blc; i++) {
				id sl = ((id (*)(id, SEL, unsigned long))objc_msgSend)(bg_subl, sel_registerName("objectAtIndex:"), i);
				((void (*)(id, SEL, BOOL))objc_msgSend)(sl, sel_registerName("setHidden:"), YES);
			}
			CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
			CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.35f};
			CGColorRef col = CGColorCreate(cs, comps);
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(bgl, sel_registerName("setBackgroundColor:"), col);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(bgl, sel_registerName("setCornerRadius:"), 34.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(bgl, sel_registerName("setMasksToBounds:"), YES);
			CGColorRelease(col);
			CGColorSpaceRelease(cs);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(bg, sel_registerName("setAlpha:"), 1.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(bg, sel_registerName("setHidden:"), NO);
			cglog("cgprobe: dock_layout styled bg %p (f=%.1f,%.1f %.1fx%.1f, dock=%.1f,%.1f %.1fx%.1f) layer %p\n",
			    bg, bgf.origin.x, bgf.origin.y, bgf.size.width, bgf.size.height,
			    df.origin.x, df.origin.y, df.size.width, df.size.height, bgl);
		}
	}
}

static void (*original_material_set_content_replaced_with_snapshot)(id, SEL, BOOL);
static void probe_material_set_content_replaced_with_snapshot(id self, SEL cmd, BOOL replaced)
{
	original_material_set_content_replaced_with_snapshot(self, cmd, NO);
}

static BOOL (*original_material_is_content_replaced_with_snapshot)(id, SEL);
static BOOL probe_material_is_content_replaced_with_snapshot(id self, SEL cmd)
{
	return NO;
}

static void (*original_material_view_set_content_replaced_with_snapshot)(id, SEL, BOOL);
static void probe_material_view_set_content_replaced_with_snapshot(id self, SEL cmd, BOOL replaced)
{
	original_material_view_set_content_replaced_with_snapshot(self, cmd, NO);
}

static BOOL (*original_material_view_is_content_replaced_with_snapshot)(id, SEL);
static BOOL probe_material_view_is_content_replaced_with_snapshot(id self, SEL cmd)
{
	return NO;
}

static void (*original_focus_isolation_layout)(id, SEL);
static void probe_focus_isolation_layout(id self, SEL cmd)
{
	original_focus_isolation_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBFFocusIsolationView") != 0) return;
	id subs = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("subviews"));
	unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
	if (sc > 0) {
		id mtv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), 0);
		if (strcmp(object_getClassName(mtv), "MTMaterialView") == 0) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(mtv, sel_registerName("setHidden:"), NO);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(mtv, sel_registerName("setAlpha:"), 1.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(mtv, sel_registerName("setContentReplacedWithSnapshot:"), NO);
			id l = ((id (*)(id, SEL))objc_msgSend)(mtv, sel_registerName("layer"));
			if (l) {
				((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setHidden:"), NO);
				((void (*)(id, SEL, float))objc_msgSend)(l, sel_registerName("setOpacity:"), 1.0f);
				((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setContentReplacedWithSnapshot:"), NO);
			}
		}
	}
}

static void (*original_search_pill_layout)(id, SEL);
static void probe_search_pill_layout(id self, SEL cmd)
{
	original_search_pill_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBHSearchPillView") != 0) return;
	id layer = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (layer) {
		((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setFilters:"), nil);
		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.25f};
		CGColorRef col = CGColorCreate(cs, comps);
		((void (*)(id, SEL, CGColorRef))objc_msgSend)(layer, sel_registerName("setBackgroundColor:"), col);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(layer, sel_registerName("setCornerRadius:"), 14.0f);
		((void (*)(id, SEL, BOOL))objc_msgSend)(layer, sel_registerName("setMasksToBounds:"), YES);
		CGColorRelease(col);
		CGColorSpaceRelease(cs);
		cglog("cgprobe: search_pill layout styled layer %p\n", layer);
	}
}

static void (*original_folder_bg_layout)(id, SEL);
static void probe_folder_bg_layout(id self, SEL cmd)
{
	original_folder_bg_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBFolderBackgroundView") != 0) return;
	Ivar bv_ivar = class_getInstanceVariable(object_getClass(self), "_blurView");
	id bv = bv_ivar ? object_getIvar(self, bv_ivar) : nil;
	CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	if (bv && b.size.width > 50 && b.size.height > 50) {
		id blayer = ((id (*)(id, SEL))objc_msgSend)(bv, sel_registerName("layer"));
		if (blayer) {
			((void (*)(id, SEL, id))objc_msgSend)(blayer, sel_registerName("setFilters:"), nil);
			CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
			CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.28f};
			CGColorRef col = CGColorCreate(cs, comps);
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(blayer, sel_registerName("setBackgroundColor:"), col);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(blayer, sel_registerName("setCornerRadius:"), 30.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(blayer, sel_registerName("setMasksToBounds:"), YES);
			CGColorRelease(col);
			CGColorSpaceRelease(cs);
			cglog("cgprobe: folder_bg layoutSubviews styled blurView %p\n", bv);
		}
	}
}

static void (*original_floaty_folder_layout)(id, SEL);
static void probe_floaty_folder_layout(id self, SEL cmd)
{
	original_floaty_folder_layout(self, cmd);
	if (!strstr(object_getClassName(self), "FloatyFolderView")) return;

	id subviews = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("subviews"));
	unsigned long count = subviews ? ((unsigned long (*)(id, SEL))objc_msgSend)(subviews, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < count; i++) {
		id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews, sel_registerName("objectAtIndex:"), i);
		CGRect f = ((CGRect (*)(id, SEL))objc_msgSend)(sv, sel_registerName("frame"));
		const char *cname = object_getClassName(sv);
		if (strstr(cname, "Background") || strstr(cname, "Clip") || (f.size.width > 200 && f.size.height > 200)) {
			id l = ((id (*)(id, SEL))objc_msgSend)(sv, sel_registerName("layer"));
			if (l) {
				((void (*)(id, SEL, id))objc_msgSend)(l, sel_registerName("setFilters:"), nil);
				((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setShouldRasterize:"), NO);
				CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
				CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.35f};
				CGColorRef col = CGColorCreate(cs, comps);
				((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), col);
				((void (*)(id, SEL, CGFloat))objc_msgSend)(l, sel_registerName("setCornerRadius:"), 30.0f);
				((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setMasksToBounds:"), YES);
				CGColorRelease(col);
				CGColorSpaceRelease(cs);
				cglog("cgprobe: floaty_folder styled platter %s (%p)\n", cname, sv);
			}
		}
	}
}

static void (*original_folder_icon_img_layout)(id, SEL);
static void probe_folder_icon_img_layout(id self, SEL cmd)
{
	original_folder_icon_img_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBFolderIconImageView") != 0) return;

	id layer = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (layer) {
		((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContents:"), nil);
		((void (*)(id, SEL, BOOL))objc_msgSend)(layer, sel_registerName("setShouldRasterize:"), NO);
	}

	Ivar scb_iv = class_getInstanceVariable(object_getClass(self), "_solidColorBackgroundView");
	id scb = scb_iv ? object_getIvar(self, scb_iv) : nil;
	if (scb) {
		((void (*)(id, SEL, CGFloat))objc_msgSend)(scb, sel_registerName("setAlpha:"), 0.0f);
		((void (*)(id, SEL, BOOL))objc_msgSend)(scb, sel_registerName("setHidden:"), YES);
	}

	CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	id subviews = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("subviews"));
	unsigned long count = subviews ? ((unsigned long (*)(id, SEL))objc_msgSend)(subviews, sel_registerName("count")) : 0;
	if (count > 0 && b.size.width > 30 && b.size.height > 30) {
		id sv0 = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subviews, sel_registerName("objectAtIndex:"), 0);
		id l0 = ((id (*)(id, SEL))objc_msgSend)(sv0, sel_registerName("layer"));
		if (l0) {
			((void (*)(id, SEL, id))objc_msgSend)(l0, sel_registerName("setFilters:"), nil);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l0, sel_registerName("setShouldRasterize:"), NO);
			id sublayers = ((id (*)(id, SEL))objc_msgSend)(l0, sel_registerName("sublayers"));
			unsigned long slc = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers, sel_registerName("count")) : 0;
			for (unsigned long i = 0; i < slc; i++) {
				id sl = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), i);
				((void (*)(id, SEL, BOOL))objc_msgSend)(sl, sel_registerName("setHidden:"), YES);
			}
			id sv0_subs = ((id (*)(id, SEL))objc_msgSend)(sv0, sel_registerName("subviews"));
			unsigned long ssc = sv0_subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(sv0_subs, sel_registerName("count")) : 0;
			for (unsigned long i = 0; i < ssc; i++) {
				id ch = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sv0_subs, sel_registerName("objectAtIndex:"), i);
				((void (*)(id, SEL, BOOL))objc_msgSend)(ch, sel_registerName("setHidden:"), YES);
			}
			CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
			CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.35f};
			CGColorRef col = CGColorCreate(cs, comps);
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(l0, sel_registerName("setBackgroundColor:"), col);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(l0, sel_registerName("setCornerRadius:"), 14.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l0, sel_registerName("setMasksToBounds:"), YES);
			CGColorRelease(col);
			CGColorSpaceRelease(cs);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(sv0, sel_registerName("setAlpha:"), 1.0f);
			((void (*)(id, SEL, BOOL))objc_msgSend)(sv0, sel_registerName("setHidden:"), NO);
		}
	}
}

static void (*original_window_set_hidden)(id, SEL, BOOL);
static void probe_window_set_hidden(id self, SEL cmd, BOOL hidden)
{
	const char *cname = object_getClassName(self);
	if (strstr(cname, "Wallpaper")) {
		cglog("cgprobe: %s %p setHidden:%d -> forced NO\n", cname, self, hidden);
		original_window_set_hidden(self, cmd, NO);
		return;
	}
	original_window_set_hidden(self, cmd, hidden);
}

static void (*original_window_set_alpha)(id, SEL, CGFloat);
static void probe_window_set_alpha(id self, SEL cmd, CGFloat alpha)
{
	const char *cname = object_getClassName(self);
	if (strstr(cname, "Wallpaper")) {
		cglog("cgprobe: %s %p setAlpha:%.2f\n", cname, self, alpha);
		if (alpha < 0.05f) {
			cglog("cgprobe: %s %p forcing alpha 1.0\n", cname, self);
			original_window_set_alpha(self, cmd, 1.0f);
			return;
		}
	}
	original_window_set_alpha(self, cmd, alpha);
}

static void clear_blackout_in_view(id view);
static void fix_all_blackouts(void);

static void (*original_feather_blur_set_hidden)(id, SEL, BOOL);
static void (*original_feather_blur_set_alpha)(id, SEL, CGFloat);
static void (*original_feather_blur_layout)(id, SEL);

static void probe_feather_blur_layout(id self, SEL cmd)
{
	if (original_feather_blur_layout) original_feather_blur_layout(self, cmd);
	if (original_feather_blur_set_hidden) original_feather_blur_set_hidden(self, sel_registerName("setHidden:"), YES);
	if (original_feather_blur_set_alpha) original_feather_blur_set_alpha(self, sel_registerName("setAlpha:"), 0.0f);
}

static void probe_feather_blur_set_alpha(id self, SEL cmd, CGFloat alpha)
{
	if (original_feather_blur_set_alpha) original_feather_blur_set_alpha(self, cmd, 0.0f);
	if (original_feather_blur_set_hidden) original_feather_blur_set_hidden(self, sel_registerName("setHidden:"), YES);
}

static void probe_feather_blur_set_hidden(id self, SEL cmd, BOOL hidden)
{
	if (original_feather_blur_set_hidden) original_feather_blur_set_hidden(self, cmd, YES);
}

static void install_feather_blur_hooks(void)
{
	static int installed = 0;
	if (installed) return;
	const char *names[] = {"SBFFeatherBlurView", "SBHFeatherBlurView", NULL};
	for (int i = 0; names[i]; i++) {
		Class sfb = objc_getClass(names[i]);
		if (!sfb) continue;
		Method lay = class_getInstanceMethod(sfb, sel_registerName("layoutSubviews"));
		if (lay) {
			IMP orig = method_getImplementation(lay);
			if (!class_addMethod(sfb, sel_registerName("layoutSubviews"), (IMP)probe_feather_blur_layout, method_getTypeEncoding(lay))) {
				original_feather_blur_layout = (void *)method_setImplementation(lay, (IMP)probe_feather_blur_layout);
			} else {
				original_feather_blur_layout = (void *)orig;
			}
			installed = 1;
		}
		Method am = class_getInstanceMethod(sfb, sel_registerName("setAlpha:"));
		if (am) {
			IMP orig = method_getImplementation(am);
			if (!class_addMethod(sfb, sel_registerName("setAlpha:"), (IMP)probe_feather_blur_set_alpha, method_getTypeEncoding(am))) {
				original_feather_blur_set_alpha = (void *)method_setImplementation(am, (IMP)probe_feather_blur_set_alpha);
			} else {
				original_feather_blur_set_alpha = (void *)orig;
			}
			installed = 1;
		}
		Method hm = class_getInstanceMethod(sfb, sel_registerName("setHidden:"));
		if (hm) {
			IMP orig = method_getImplementation(hm);
			if (!class_addMethod(sfb, sel_registerName("setHidden:"), (IMP)probe_feather_blur_set_hidden, method_getTypeEncoding(hm))) {
				original_feather_blur_set_hidden = (void *)method_setImplementation(hm, (IMP)probe_feather_blur_set_hidden);
			} else {
				original_feather_blur_set_hidden = (void *)orig;
			}
			installed = 1;
		}
		if (installed) cglog("cgprobe: installed %s hooks\n", names[i]);
	}
}

static id (*original_folder_contents_image)(id, SEL);
static id probe_folder_contents_image(id self, SEL cmd)
{
	return nil;
}

static void (*original_layer_set_should_rasterize)(id, SEL, BOOL);
static void probe_layer_set_should_rasterize(id self, SEL cmd, BOOL rasterize)
{
	original_layer_set_should_rasterize(self, cmd, NO);
}

static void (*original_switcher_snapshot_set_image)(id, SEL, id);
static void probe_switcher_snapshot_set_image(id self, SEL cmd, id img)
{
	if (img == nil) {
		id current = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("image"));
		if (current != nil) {
			cglog("cgprobe: retaining switcher snapshot image %p instead of clearing to nil\n", current);
			return;
		}
	}
	original_switcher_snapshot_set_image(self, cmd, img);
	cglog("cgprobe: switcher snapshot %p setImage:%p\n", self, img);
}

static void (*original_switcher_snapshot_layout)(id, SEL);
static void probe_switcher_snapshot_layout(id self, SEL cmd)
{
	original_switcher_snapshot_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBSwitcherSnapshotImageView") != 0) return;
	Ivar iv_ivar = class_getInstanceVariable(object_getClass(self), "_imageView");
	id iv = iv_ivar ? object_getIvar(self, iv_ivar) : nil;
	id img = iv ? ((id (*)(id, SEL))objc_msgSend)(iv, sel_registerName("image")) : nil;
	CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	cglog("cgprobe: switcher snapshot %p layout bounds %.1fx%.1f iv %p img %p\n", self, b.size.width, b.size.height, iv, img);
}

static void (*original_backdrop_set_filters)(id, SEL, id);
static void probe_backdrop_set_filters(id self, SEL cmd, id filters)
{
	/* Unconditionally strip all CAFilters to prevent black GLES rect sampling on Adreno 506 */
	original_backdrop_set_filters(self, cmd, nil);
}

static CGImageRef create_rgba_image_from_mask(CGImageRef mask)
{
	if (!mask) return NULL;
	if (CFGetTypeID((CFTypeRef)mask) != CGImageGetTypeID()) return NULL;
	size_t bpp = CGImageGetBitsPerPixel(mask);
	CGImageAlphaInfo info = CGImageGetAlphaInfo(mask);
	if (bpp > 8 && info != kCGImageAlphaOnly) {
		return NULL;
	}
	size_t w = CGImageGetWidth(mask);
	size_t h = CGImageGetHeight(mask);
	if (w == 0 || h == 0) return NULL;

	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs,
		(CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease(cs);
	if (!ctx) return NULL;

	CGRect rect = CGRectMake(0, 0, (CGFloat)w, (CGFloat)h);
	CGContextClipToMask(ctx, rect, mask);
	CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
	CGContextFillRect(ctx, rect);

	CGImageRef rgba = CGBitmapContextCreateImage(ctx);
	CGContextRelease(ctx);
	return rgba;
}

static void (*original_layer_set_contents)(id, SEL, id);
static void probe_layer_set_contents(id self, SEL cmd, id contents)
{
	if (contents && CFGetTypeID((CFTypeRef)contents) == CGImageGetTypeID()) {
		CGImageRef cg = (CGImageRef)contents;
		size_t bpp = CGImageGetBitsPerPixel(cg);
		CGImageAlphaInfo info = CGImageGetAlphaInfo(cg);
		if (bpp <= 8 || info == kCGImageAlphaOnly) {
			CGImageRef rgba = create_rgba_image_from_mask(cg);
			if (rgba) {
				original_layer_set_contents(self, cmd, (id)rgba);
				CGImageRelease(rgba);
				return;
			}
		}
	}
	original_layer_set_contents(self, cmd, contents);
}

static void (*original_image_view_set_image)(id, SEL, id);
static void probe_image_view_set_image(id self, SEL cmd, id img)
{
	if (img) {
		CGImageRef cg = ((CGImageRef (*)(id, SEL))objc_msgSend)(img, sel_registerName("CGImage"));
		if (cg && CFGetTypeID((CFTypeRef)cg) == CGImageGetTypeID()) {
			size_t bpp = CGImageGetBitsPerPixel(cg);
			CGImageAlphaInfo info = CGImageGetAlphaInfo(cg);
			if (bpp <= 8 || info == kCGImageAlphaOnly) {
				CGImageRef rgba = create_rgba_image_from_mask(cg);
				if (rgba) {
					CGFloat scale = ((CGFloat (*)(id, SEL))objc_msgSend)(img, sel_registerName("scale"));
					long orient = ((long (*)(id, SEL))objc_msgSend)(img, sel_registerName("imageOrientation"));
					Class uic = objc_getClass("UIImage");
					id newImg = ((id (*)(id, SEL, CGImageRef, CGFloat, long))objc_msgSend)(
						(id)uic, sel_registerName("imageWithCGImage:scale:orientation:"), rgba, scale, orient);
					CGImageRelease(rgba);
					if (newImg) {
						original_image_view_set_image(self, cmd, newImg);
						return;
					}
				}
			}
		}
	}
	original_image_view_set_image(self, cmd, img);
}

static void dump_view_tree(id view, int depth)
{
	if (!view || depth > 10) return;
	char indent[64] = {0};
	for (int d = 0; d < depth && d < 28; d++) {
		indent[d * 2] = ' ';
		indent[d * 2 + 1] = ' ';
	}
	CGRect f = ((CGRect (*)(id, SEL))objc_msgSend)(view, sel_registerName("frame"));
	BOOL hid = ((BOOL (*)(id, SEL))objc_msgSend)(view, sel_registerName("isHidden"));
	CGFloat alpha = ((CGFloat (*)(id, SEL))objc_msgSend)(view, sel_registerName("alpha"));
	id bgc = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("backgroundColor"));
	id layer = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("layer"));
	id filters = layer ? ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("filters")) : nil;
	cglog("%s%s %p (%.0f,%.0f %.0fx%.0f) hid=%d a=%.2f bg=%s flt=%s\n",
	    indent, object_getClassName(view), view,
	    f.origin.x, f.origin.y, f.size.width, f.size.height,
	    hid, alpha, bgc ? object_getClassName(bgc) : "nil",
	    filters ? "yes" : "nil");

	id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
	unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < sc; i++) {
		id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
		dump_view_tree(sv, depth + 1);
	}
}

static void dump_all_windows_and_views(void)
{
	Class win_cls = objc_getClass("UIWindow");
	if (!win_cls) return;
	id wins = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)win_cls,
	    sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:"), YES, NO);
	unsigned long nw = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;
	cglog("cgprobe: ===== FULL WINDOW TREE (%lu windows) =====\n", nw);
	for (unsigned long i = 0; i < nw; i++) {
		id win = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
		id rvc = ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("rootViewController"));
		CGRect f = ((CGRect (*)(id, SEL))objc_msgSend)(win, sel_registerName("frame"));
		cglog("WIN[%lu]: %s %p (rvc: %s, frame: %.0f,%.0f %.0fx%.0f, hidden=%d, alpha=%.2f)\n",
		    i, object_getClassName(win), win, rvc ? object_getClassName(rvc) : "nil",
		    f.origin.x, f.origin.y, f.size.width, f.size.height,
		    ((BOOL (*)(id, SEL))objc_msgSend)(win, sel_registerName("isHidden")),
		    ((CGFloat (*)(id, SEL))objc_msgSend)(win, sel_registerName("alpha")));
		dump_view_tree(win, 1);
	}
	cglog("cgprobe: ===== END WINDOW TREE =====\n");
}

static void (*original_pbui_layout)(id, SEL);
static void probe_pbui_layout(id self, SEL cmd)
{
	original_pbui_layout(self, cmd);
	Ivar iv = class_getInstanceVariable(object_getClass(self), "_wallpaperContainerView");
	id cv = iv ? object_getIvar(self, iv) : nil;
	if (cv) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(cv, sel_registerName("setHidden:"), NO);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(cv, sel_registerName("setAlpha:"), 1.0f);
	}
	Ivar dim_iv = class_getInstanceVariable(object_getClass(self), "_inactiveAppearanceDimmingView");
	id dim = dim_iv ? object_getIvar(self, dim_iv) : nil;
	if (dim) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(dim, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(dim, sel_registerName("setAlpha:"), 0.0f);
	}
	Ivar hhr_iv = class_getInstanceVariable(object_getClass(self), "_hideHomescreenWallpaperReasons");
	id hhr = hhr_iv ? object_getIvar(self, hhr_iv) : nil;
	if (hhr) {
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(hhr, sel_registerName("count"));
		if (count > 0) {
			cglog("cgprobe: PBUI _hideHomescreenWallpaperReasons count=%lu: clearing!\n", count);
			((void (*)(id, SEL))objc_msgSend)(hhr, sel_registerName("removeAllObjects"));
		}
	}
}

static void (*original_library_layout)(id, SEL);
static void probe_library_layout(id self, SEL cmd)
{
	original_library_layout(self, cmd);
	id v = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("view"));
	if (v) {
		Class uic = objc_getClass("UIColor");
		id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));
		((void (*)(id, SEL, id))objc_msgSend)(v, sel_registerName("setBackgroundColor:"), clear);

		Ivar sdv_iv = class_getInstanceVariable(object_getClass(self), "_searchDimmingView");
		id sdv = sdv_iv ? object_getIvar(self, sdv_iv) : nil;
		if (sdv) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(sdv, sel_registerName("setHidden:"), YES);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(sdv, sel_registerName("setAlpha:"), 0.0f);
		}
		clear_blackout_in_view(v);
		fix_all_blackouts();
		static int logged;
		if (logged++ < 5) {
			cglog("cgprobe: library view %p tree:\n", v);
			dump_view_tree(v, 1);
		}
	}
}

static void (*original_switcher_layout)(id, SEL);
static void probe_switcher_layout(id self, SEL cmd)
{
	if (original_switcher_layout) original_switcher_layout(self, cmd);
	id v = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("view"));
	if (v) {
		Class uic = objc_getClass("UIColor");
		id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));
		((void (*)(id, SEL, id))objc_msgSend)(v, sel_registerName("setBackgroundColor:"), clear);
		((void (*)(id, SEL, BOOL))objc_msgSend)(v, sel_registerName("setOpaque:"), NO);
		id vl = ((id (*)(id, SEL))objc_msgSend)(v, sel_registerName("layer"));
		if (vl) {
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(vl, sel_registerName("setBackgroundColor:"), nil);
			((void (*)(id, SEL, BOOL))objc_msgSend)(vl, sel_registerName("setOpaque:"), NO);
		}

		clear_blackout_in_view(v);
	}

	Class win_cls = objc_getClass("UIWindow");
	if (win_cls) {
		id wins = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)win_cls,
		    sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:"), YES, NO);
		unsigned long nw = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;
		for (unsigned long i = 0; i < nw; i++) {
			id win = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
			const char *wname = object_getClassName(win);
			if (strstr(wname, "Wallpaper")) {
				id subs = ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("subviews"));
				unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
				for (unsigned long j = 1; j < sc; j++) {
					id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), j);
					((void (*)(id, SEL, BOOL))objc_msgSend)(sv, sel_registerName("setHidden:"), YES);
					((void (*)(id, SEL, CGFloat))objc_msgSend)(sv, sel_registerName("setAlpha:"), 0.0f);
					id l = ((id (*)(id, SEL))objc_msgSend)(sv, sel_registerName("layer"));
					if (l) {
						((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setHidden:"), YES);
						((void (*)(id, SEL, float))objc_msgSend)(l, sel_registerName("setOpacity:"), 0.0f);
					}
				}
			}
		}
	}
}

static void install_switcher_hooks(void)
{
	static int installed = 0;
	if (installed) return;
	Class sw = objc_getClass("SBFluidSwitcherViewController");
	if (sw) {
		Method swm = class_getInstanceMethod(sw, sel_registerName("viewWillLayoutSubviews"));
		if (swm) {
			IMP orig = method_getImplementation(swm);
			if (!class_addMethod(sw, sel_registerName("viewWillLayoutSubviews"), (IMP)probe_switcher_layout, method_getTypeEncoding(swm))) {
				original_switcher_layout = (void *)method_setImplementation(swm, (IMP)probe_switcher_layout);
			} else {
				original_switcher_layout = (void *)orig;
			}
			installed = 1;
			cglog("cgprobe: installed SBFluidSwitcherViewController layout hook\n");
		}
	}
}

static void (*original_frozen_layout)(id, SEL);
static void probe_frozen_layout(id self, SEL cmd)
{
	original_frozen_layout(self, cmd);
	Class uic = objc_getClass("UIColor");
	id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));
	((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setBackgroundColor:"), clear);
	id l = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (l) {
		((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
		((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setOpaque:"), NO);
	}
	((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setOpaque:"), NO);
}

static void (*original_wallpaper_window_layout)(id, SEL);
static void probe_wallpaper_window_layout(id self, SEL cmd)
{
	original_wallpaper_window_layout(self, cmd);
	id subs = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("subviews"));
	unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
	for (unsigned long i = 1; i < sc; i++) {
		id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
		((void (*)(id, SEL, BOOL))objc_msgSend)(sv, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(sv, sel_registerName("setAlpha:"), 0.0f);
		id l = ((id (*)(id, SEL))objc_msgSend)(sv, sel_registerName("layer"));
		if (l) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setHidden:"), YES);
			((void (*)(id, SEL, float))objc_msgSend)(l, sel_registerName("setOpacity:"), 0.0f);
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
		}
		cglog("cgprobe: hid wallpaper window overlay view[%lu] %p\n", i, sv);
	}
}

static void scan_objc_classes(void)
{
	int numClasses = objc_getClassList(NULL, 0);
	if (numClasses > 0) {
		Class *classes = (Class *)malloc(sizeof(Class) * numClasses);
		numClasses = objc_getClassList(classes, numClasses);
		cglog("cgprobe: scanning %d classes...\n", numClasses);
		for (int i = 0; i < numClasses; i++) {
			const char *name = class_getName(classes[i]);
			if (strstr(name, "LibraryViewController") ||
			    strstr(name, "SnapshotImageView") ||
			    strstr(name, "SwitcherSnapshot") ||
			    strstr(name, "FluidSwitcherItem") ||
			    strstr(name, "SearchPill") ||
			    strstr(name, "SearchBackdrop") ||
			    strstr(name, "FolderIcon")) {
				cglog("  match: %s\n", name);
			}
		}
		free(classes);
	}
}

static void dump_layer_tree(id layer, int depth)
{
	if (!layer || depth > 8) return;
	char indent[64] = {0};
	for (int d = 0; d < depth && d < 28; d++) {
		indent[d * 2] = ' ';
		indent[d * 2 + 1] = ' ';
	}
	CGRect f = ((CGRect (*)(id, SEL))objc_msgSend)(layer, sel_registerName("frame"));
	BOOL hid = ((BOOL (*)(id, SEL))objc_msgSend)(layer, sel_registerName("isHidden"));
	float op = ((float (*)(id, SEL))objc_msgSend)(layer, sel_registerName("opacity"));
	id bgc = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("backgroundColor"));
	id name = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("name"));
	id flt = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("filters"));
	id cmp = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("compositingFilter"));
	const char *ns = name ? ((const char * (*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : "";

	cglog("%sLYR: %s '%s' (%.0f,%.0f %.0fx%.0f) hid=%d op=%.2f bg=%p flt=%s cmp=%s\n",
	    indent, object_getClassName(layer), ns,
	    f.origin.x, f.origin.y, f.size.width, f.size.height,
	    hid, op, bgc, flt ? "yes" : "nil", cmp ? "yes" : "nil");

	id sublayers = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("sublayers"));
	unsigned long sc = sublayers ? ((unsigned long (*)(id, SEL))objc_msgSend)(sublayers, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < sc; i++) {
		id sl = ((id (*)(id, SEL, unsigned long))objc_msgSend)(sublayers, sel_registerName("objectAtIndex:"), i);
		dump_layer_tree(sl, depth + 1);
	}
}

static void clear_blackout_in_view(id view)
{
	if (!view) return;
	const char *cname = object_getClassName(view);
	Class uic = objc_getClass("UIColor");
	id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));

	if (strcmp(cname, "MTMaterialView") == 0) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setContentReplacedWithSnapshot:"), NO);
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setHidden:"), NO);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(view, sel_registerName("setAlpha:"), 1.0f);
		id l = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("layer"));
		if (l) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setContentReplacedWithSnapshot:"), NO);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setHidden:"), NO);
			((void (*)(id, SEL, float))objc_msgSend)(l, sel_registerName("setOpacity:"), 1.0f);
		}
	}
	if (strstr(cname, "LibraryFrozenSafeAreaInsetsView") || strstr(cname, "SBHLibraryViewController") ||
	    strstr(cname, "Dimming") || strstr(cname, "Underlay") ||
	    (strstr(cname, "Background") && !strstr(cname, "Pod") && !strstr(cname, "Folder"))) {
		((void (*)(id, SEL, id))objc_msgSend)(view, sel_registerName("setBackgroundColor:"), clear);
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setOpaque:"), NO);
		id l = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("layer"));
		if (l) {
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setOpaque:"), NO);
		}
		cglog("cgprobe: clear_blackout: cleared %s %p\n", cname, view);
	}
	if (strcmp(cname, "SBFFeatherBlurView") == 0 || strcmp(cname, "SBHFeatherBlurView") == 0) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(view, sel_registerName("setAlpha:"), 0.0f);
	}
	if (strcmp(cname, "SBHomeScreenBackdropView") == 0) {
		id hsubs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
		unsigned long hsc = hsubs ? ((unsigned long (*)(id, SEL))objc_msgSend)(hsubs, sel_registerName("count")) : 0;
		for (unsigned long j = 0; j < hsc; j++) {
			id hsv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(hsubs, sel_registerName("objectAtIndex:"), j);
			const char *hsn = object_getClassName(hsv);
			if (strcmp(hsn, "UIImageView") == 0) {
				((void (*)(id, SEL, BOOL))objc_msgSend)(hsv, sel_registerName("setHidden:"), YES);
				((void (*)(id, SEL, CGFloat))objc_msgSend)(hsv, sel_registerName("setAlpha:"), 0.0f);
			}
		}
	}
	id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
	unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
	for (unsigned long i = 0; i < sc; i++) {
		id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
		clear_blackout_in_view(sv);
	}
}

static void fix_all_blackouts(void)
{
	install_feather_blur_hooks();
	install_switcher_hooks();
	Class wcls = objc_getClass("UIWindow");
	if (!wcls) return;
	id wins = ((id (*)(id, SEL, BOOL, BOOL))objc_msgSend)((id)wcls, sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:"), YES, NO);
	unsigned long nw = wins ? ((unsigned long (*)(id, SEL))objc_msgSend)(wins, sel_registerName("count")) : 0;

	for (unsigned long i = 0; i < nw; i++) {
		id win = ((id (*)(id, SEL, unsigned long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
		const char *wname = object_getClassName(win);
		if (strstr(wname, "Wallpaper")) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(win, sel_registerName("setHidden:"), NO);
			((void (*)(id, SEL, CGFloat))objc_msgSend)(win, sel_registerName("setAlpha:"), 1.0f);
			id rvc = ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("rootViewController"));
			if (rvc) {
				Ivar hhr_iv = class_getInstanceVariable(object_getClass(rvc), "_hideHomescreenWallpaperReasons");
				id hhr = hhr_iv ? object_getIvar(rvc, hhr_iv) : nil;
				if (hhr) {
					((void (*)(id, SEL))objc_msgSend)(hhr, sel_registerName("removeAllObjects"));
				}
			}
			id subs = ((id (*)(id, SEL))objc_msgSend)(win, sel_registerName("subviews"));
			unsigned long sc = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count")) : 0;
			for (unsigned long j = 1; j < sc; j++) {
				id sv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), j);
				((void (*)(id, SEL, BOOL))objc_msgSend)(sv, sel_registerName("setHidden:"), YES);
				((void (*)(id, SEL, CGFloat))objc_msgSend)(sv, sel_registerName("setAlpha:"), 0.0f);
				id l = ((id (*)(id, SEL))objc_msgSend)(sv, sel_registerName("layer"));
				if (l) {
					((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setHidden:"), YES);
					((void (*)(id, SEL, float))objc_msgSend)(l, sel_registerName("setOpacity:"), 0.0f);
				}
				cglog("cgprobe: fix_all_blackouts: hid wallpaper overlay[%lu] %p\n", j, sv);
			}
		}
		clear_blackout_in_view(win);
	}
}

__attribute__((constructor)) static void
probe_loaded(void)
{
	cglog("cgprobe: loaded in pid %d\n", getpid());
	Class image = objc_getClass("UIImage");
	SEL selector = sel_registerName("sbf_imageFromContextWithSize:scale:type:pool:drawing:");
	Method method = image ? class_getClassMethod(image, selector) : NULL;
	if (method != NULL) {
		original_sbf_image = (void *)method_setImplementation(method, (IMP)probe_sbf_image);
	}
	cglog("cgprobe: sbf_image hook %s\n", original_sbf_image ? "installed" : "unavailable");
	Class wctrl = objc_getClass("SBWallpaperController");
	Method swo = wctrl ? class_getInstanceMethod(wctrl, sel_registerName("setWallpaperObscured:")) : NULL;
	if (swo != NULL) {
		original_set_wallpaper_obscured = (void *)method_setImplementation(swo, (IMP)probe_set_wallpaper_obscured);
	}
	cglog("cgprobe: setWallpaperObscured hook %s\n", original_set_wallpaper_obscured ? "installed" : "unavailable");

	Class fimg = objc_getClass("SBFolderIconImageView");
	Method fimgl = fimg ? class_getInstanceMethod(fimg, sel_registerName("layoutSubviews")) : NULL;
	if (fimgl != NULL) {
		original_folder_icon_img_layout = (void *)method_setImplementation(fimgl, (IMP)probe_folder_icon_img_layout);
	}
	cglog("cgprobe: folder_icon_img layoutSubviews hook %s\n", original_folder_icon_img_layout ? "installed" : "unavailable");

	Method fci = fimg ? class_getInstanceMethod(fimg, sel_registerName("contentsImage")) : NULL;
	if (fci != NULL) {
		original_folder_contents_image = (void *)method_setImplementation(fci, (IMP)probe_folder_contents_image);
	}
	cglog("cgprobe: folder_contents_image hook %s\n", original_folder_contents_image ? "installed" : "unavailable");

	Class dock = objc_getClass("SBDockView");
	Method dockl = dock ? class_getInstanceMethod(dock, sel_registerName("layoutSubviews")) : NULL;
	if (dockl != NULL) {
		original_dock_layout = (void *)method_setImplementation(dockl, (IMP)probe_dock_layout);
	}
	cglog("cgprobe: dock layoutSubviews hook %s\n", original_dock_layout ? "installed" : "unavailable");

	Class cal = objc_getClass("CALayer");
	Method srm = cal ? class_getInstanceMethod(cal, sel_registerName("setShouldRasterize:")) : NULL;
	if (srm != NULL) {
		original_layer_set_should_rasterize = (void *)method_setImplementation(srm, (IMP)probe_layer_set_should_rasterize);
	}
	cglog("cgprobe: CALayer setShouldRasterize hook %s\n", original_layer_set_should_rasterize ? "installed" : "unavailable");

	Class mtl = objc_getClass("MTMaterialLayer");
	Method mtl_set = mtl ? class_getInstanceMethod(mtl, sel_registerName("setContentReplacedWithSnapshot:")) : NULL;
	if (mtl_set) {
		original_material_set_content_replaced_with_snapshot = (void *)method_setImplementation(mtl_set, (IMP)probe_material_set_content_replaced_with_snapshot);
		cglog("cgprobe: installed MTMaterialLayer setContentReplacedWithSnapshot hook\n");
	}
	Method mtl_is = mtl ? class_getInstanceMethod(mtl, sel_registerName("isContentReplacedWithSnapshot")) : NULL;
	if (mtl_is) {
		original_material_is_content_replaced_with_snapshot = (void *)method_setImplementation(mtl_is, (IMP)probe_material_is_content_replaced_with_snapshot);
		cglog("cgprobe: installed MTMaterialLayer isContentReplacedWithSnapshot hook\n");
	}

	Class mtv = objc_getClass("MTMaterialView");
	Method mtv_set = mtv ? class_getInstanceMethod(mtv, sel_registerName("setContentReplacedWithSnapshot:")) : NULL;
	if (mtv_set) {
		original_material_view_set_content_replaced_with_snapshot = (void *)method_setImplementation(mtv_set, (IMP)probe_material_view_set_content_replaced_with_snapshot);
		cglog("cgprobe: installed MTMaterialView setContentReplacedWithSnapshot hook\n");
	}
	Method mtv_is = mtv ? class_getInstanceMethod(mtv, sel_registerName("isContentReplacedWithSnapshot")) : NULL;
	if (mtv_is) {
		original_material_view_is_content_replaced_with_snapshot = (void *)method_setImplementation(mtv_is, (IMP)probe_material_view_is_content_replaced_with_snapshot);
		cglog("cgprobe: installed MTMaterialView isContentReplacedWithSnapshot hook\n");
	}

	install_feather_blur_hooks();
	install_switcher_hooks();

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4LL * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		cglog("cgprobe: 4s delayed inspection & hooking pass\n");
		install_feather_blur_hooks();
		install_switcher_hooks();
		scan_objc_classes();
		dump_visible_windows();
		dump_class_info(objc_getClass("SBFolderIconImageView"));
		dump_class_info(objc_getClass("SBWallpaperController"));
		dump_class_info(objc_getClass("PBUIWallpaperViewController"));
		dump_class_info(objc_getClass("PBUIPosterWallpaperRemoteViewController"));
		dump_class_info(objc_getClass("SBFloatyFolderView"));
		dump_class_info(objc_getClass("SBFolderBackgroundView"));
		dump_class_info(objc_getClass("SBFolderView"));
		dump_class_info(objc_getClass("SBFluidSwitcherViewController"));
		dump_class_info(objc_getClass("SBSwitcherViewController"));
		dump_class_info(objc_getClass("SBHomeScreenBackdropView"));
		dump_class_info(objc_getClass("SBFFeatherBlurView"));
		dump_class_info(objc_getClass("SBHLibraryViewController"));
		dump_class_info(objc_getClass("MTMaterialView"));
		dump_class_info(objc_getClass("MTMaterialLayer"));

		Class fiv = objc_getClass("SBFFocusIsolationView");
		Method fivm = fiv ? class_getInstanceMethod(fiv, sel_registerName("layoutSubviews")) : NULL;
		if (fivm) {
			original_focus_isolation_layout = (void *)method_setImplementation(fivm, (IMP)probe_focus_isolation_layout);
			cglog("cgprobe: installed SBFFocusIsolationView layout hook\n");
		}

		Class pill = objc_getClass("SBHSearchPillView");
		Method pillm = pill ? class_getInstanceMethod(pill, sel_registerName("layoutSubviews")) : NULL;
		if (pillm) {
			original_search_pill_layout = (void *)method_setImplementation(pillm, (IMP)probe_search_pill_layout);
			cglog("cgprobe: installed SBHSearchPillView layout hook\n");
		}

		Class snap = objc_getClass("SBSwitcherSnapshotImageView");
		Method snap_set = snap ? class_getInstanceMethod(snap, sel_registerName("setImage:")) : NULL;
		if (snap_set) {
			original_switcher_snapshot_set_image = (void *)method_setImplementation(snap_set, (IMP)probe_switcher_snapshot_set_image);
			cglog("cgprobe: installed SBSwitcherSnapshotImageView setImage hook\n");
		}
		Method snap_lay = snap ? class_getInstanceMethod(snap, sel_registerName("layoutSubviews")) : NULL;
		if (snap_lay) {
			original_switcher_snapshot_layout = (void *)method_setImplementation(snap_lay, (IMP)probe_switcher_snapshot_layout);
			cglog("cgprobe: installed SBSwitcherSnapshotImageView layoutSubviews hook\n");
		}

		Class pbui = objc_getClass("PBUIWallpaperViewController");
		Method pbuim = pbui ? class_getInstanceMethod(pbui, sel_registerName("viewWillLayoutSubviews")) : NULL;
		if (pbuim) {
			original_pbui_layout = (void *)method_setImplementation(pbuim, (IMP)probe_pbui_layout);
			cglog("cgprobe: installed PBUIWallpaperViewController layout hook\n");
		}

		Class lib = objc_getClass("SBHLibraryViewController");
		Method libm = lib ? class_getInstanceMethod(lib, sel_registerName("viewWillLayoutSubviews")) : NULL;
		if (libm) {
			original_library_layout = (void *)method_setImplementation(libm, (IMP)probe_library_layout);
			cglog("cgprobe: installed SBHLibraryViewController layout hook\n");
		}

		Class pbr = objc_getClass("PBUIPosterWallpaperRemoteViewController");
		Method pbr_swo = pbr ? class_getInstanceMethod(pbr, sel_registerName("setWallpaperObscured:")) : NULL;
		if (pbr_swo) {
			original_pbui_remote_set_wallpaper_obscured = (void *)method_setImplementation(pbr_swo, (IMP)probe_pbui_remote_set_wallpaper_obscured);
			cglog("cgprobe: installed PBUIPosterWallpaperRemoteViewController setWallpaperObscured hook\n");
		}
		Method pbr_lay = pbr ? class_getInstanceMethod(pbr, sel_registerName("viewWillLayoutSubviews")) : NULL;
		if (pbr_lay) {
			original_pbui_remote_layout = (void *)method_setImplementation(pbr_lay, (IMP)probe_pbui_remote_layout);
			cglog("cgprobe: installed PBUIPosterWallpaperRemoteViewController layout hook\n");
		}

		Class pbp = objc_getClass("PBUIPosterWallpaperViewController");
		Method pbp_swo = pbp ? class_getInstanceMethod(pbp, sel_registerName("setWallpaperObscured:")) : NULL;
		if (pbp_swo) {
			original_pbui_poster_set_wallpaper_obscured = (void *)method_setImplementation(pbp_swo, (IMP)probe_pbui_poster_set_wallpaper_obscured);
			cglog("cgprobe: installed PBUIPosterWallpaperViewController setWallpaperObscured hook\n");
		}


	});

	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^{
		while (1) {
			usleep(500000); /* 500ms auto-fix loop */
			dispatch_async(dispatch_get_main_queue(), ^{
				fix_all_blackouts();
			});
			if (access("/tmp/dump_now", F_OK) == 0) {
				unlink("/tmp/dump_now");
				dispatch_async(dispatch_get_main_queue(), ^{
					dump_all_windows_and_views();
				});
			}
		}
	});
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)probe_create, (const void *)CGBitmapContextCreate },
	{ (const void *)probe_create_with_data, (const void *)CGBitmapContextCreateWithData },
	{ (const void *)probe_save_gstate, (const void *)CGContextSaveGState },
	{ (const void *)probe_create_image, (const void *)CGBitmapContextCreateImage },
};
