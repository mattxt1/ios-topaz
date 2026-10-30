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
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0) return;

	int fd = open("/var/mobile/Library/SpringBoard/cgprobe.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) {
		fd = open("/tmp/cgprobe.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	}
	if (fd >= 0) {
		write(fd, buf, (size_t)n);
		close(fd);
	}
	os_log(OS_LOG_DEFAULT, "CGPROBE: %{public}s", buf);
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

/* Rasterize only these small fallback plates once per size. Rounded alpha is
 * baked into pixels: no material child, backdrop filter, or corner-mask pass. */
static char plate_key;
static CGImageRef create_plate_image(CGSize size, CGFloat radius, CGFloat alpha)
{
	const CGFloat scale = 3.0;
	size_t w = (size_t)(size.width * scale + 0.5), h = (size_t)(size.height * scale + 0.5);
	if (!w || !h || w > 2048 || h > 2048) return NULL;
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs,
	    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
	CGColorSpaceRelease(cs);
	if (!ctx) return NULL;
	CGPathRef path = CGPathCreateWithRoundedRect(CGRectMake(0, 0, w, h), radius * scale, radius * scale, NULL);
	CGContextAddPath(ctx, path);
	CGContextSetRGBFillColor(ctx, 1, 1, 1, alpha);
	CGContextFillPath(ctx);
	CGPathRelease(path);
	CGImageRef image = CGBitmapContextCreateImage(ctx);
	CGContextRelease(ctx);
	return image;
}

static void style_plate(id host, CGFloat radius, CGFloat alpha)
{
	CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(host, sel_registerName("bounds"));
	if (bounds.size.width <= 0 || bounds.size.height <= 0) return;
	id plate = objc_getAssociatedObject(host, &plate_key);
	CGRect frame = plate ? ((CGRect (*)(id, SEL))objc_msgSend)(plate, sel_registerName("frame")) : CGRectZero;
	if (plate && CGRectEqualToRect(frame, bounds)) return;
	CGImageRef image = create_plate_image(bounds.size, radius, alpha);
	if (!image) return;
	if (!plate) {
		plate = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("UIView"), sel_registerName("alloc"));
		plate = ((id (*)(id, SEL, CGRect))objc_msgSend)(plate, sel_registerName("initWithFrame:"), bounds);
		((void (*)(id, SEL, BOOL))objc_msgSend)(plate, sel_registerName("setUserInteractionEnabled:"), NO);
		((void (*)(id, SEL, BOOL))objc_msgSend)(plate, sel_registerName("setOpaque:"), NO);
		((void (*)(id, SEL, id, long))objc_msgSend)(host, sel_registerName("insertSubview:atIndex:"), plate, 0);
		objc_setAssociatedObject(host, &plate_key, plate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		((void (*)(id, SEL))objc_msgSend)(plate, sel_registerName("release"));
	} else {
		((void (*)(id, SEL, CGRect))objc_msgSend)(plate, sel_registerName("setFrame:"), bounds);
	}
	id layer = ((id (*)(id, SEL))objc_msgSend)(plate, sel_registerName("layer"));
	((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setContents:"), (id)image);
	((void (*)(id, SEL, CGFloat))objc_msgSend)(layer, sel_registerName("setContentsScale:"), 3.0);
	CGImageRelease(image);
}

static void hide_material_children(id host)
{
	id subs = ((id (*)(id, SEL))objc_msgSend)(host, sel_registerName("subviews"));
	unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
	for (unsigned long i = 0; i < n; i++) {
		id child = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), i);
		const char *name = object_getClassName(child);
		if (strstr(name, "Material") || strstr(name, "Backdrop") || strstr(name, "Background")) {
			if (!((BOOL (*)(id, SEL))objc_msgSend)(child, sel_registerName("isHidden")))
				((void (*)(id, SEL, BOOL))objc_msgSend)(child, sel_registerName("setHidden:"), YES);
		}
	}
}

/* The page control beneath the Search pill has its own square material. */
static void hide_search_page_material(id pill)
{
    id parent = ((id (*)(id, SEL))objc_msgSend)(pill, sel_registerName("superview"));
    if (!parent || strcmp(object_getClassName(parent), "SBFolderScrollAccessoryView")) return;
    id siblings = ((id (*)(id, SEL))objc_msgSend)(parent, sel_registerName("subviews"));
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(siblings, sel_registerName("count"));
    for (unsigned long i = 0; i < n; i++) {
        id pc = ((id (*)(id, SEL, unsigned long))objc_msgSend)(siblings, sel_registerName("objectAtIndex:"), i);
        if (strcmp(object_getClassName(pc), "SBIconListPageControl")) continue;
        id subs = ((id (*)(id, SEL))objc_msgSend)(pc, sel_registerName("subviews"));
        unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(subs, sel_registerName("count"));
        for (unsigned long j = 0; j < count; j++) {
            id content = ((id (*)(id, SEL, unsigned long))objc_msgSend)(subs, sel_registerName("objectAtIndex:"), j);
            if (!strcmp(object_getClassName(content), "_UIPageControlContentView")) hide_material_children(content);
        }
    }
}

static void (*original_search_pill_layout)(id, SEL);
static void probe_search_pill_layout(id self, SEL cmd)
{
	original_search_pill_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBHSearchPillView") != 0) return;
	id layer = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (((CGColorRef (*)(id, SEL))objc_msgSend)(layer, sel_registerName("backgroundColor")))
		((void (*)(id, SEL, CGColorRef))objc_msgSend)(layer, sel_registerName("setBackgroundColor:"), NULL);
	((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setOpaque:"), NO);
	((void (*)(id, SEL, BOOL))objc_msgSend)(layer, sel_registerName("setOpaque:"), NO);
	Ivar bgiv = class_getInstanceVariable(object_getClass(self), "_backgroundView");
	id bg = bgiv ? object_getIvar(self, bgiv) : nil;
	if (bg) ((void (*)(id, SEL, BOOL))objc_msgSend)(bg, sel_registerName("setHidden:"), YES);
	hide_material_children(self);
	hide_search_page_material(self);
	CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	style_plate(self, b.size.height / 2, 0.25);
}

static void (*original_folder_bg_layout)(id, SEL);
static void probe_folder_bg_layout(id self, SEL cmd)
{
	if (original_folder_bg_layout) original_folder_bg_layout(self, cmd);
	if (!strstr(object_getClassName(self), "FolderBackgroundView")) return;
	CGRect b = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
	if (b.size.width < 10.0f || b.size.height < 10.0f) return;

	id layer = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (layer) {
		((void (*)(id, SEL, id))objc_msgSend)(layer, sel_registerName("setFilters:"), nil);
		CGFloat radius = (b.size.width < 120.0f) ? 14.0f : 30.0f;
		((void (*)(id, SEL, CGFloat))objc_msgSend)(layer, sel_registerName("setCornerRadius:"), radius);
		((void (*)(id, SEL, BOOL))objc_msgSend)(layer, sel_registerName("setMasksToBounds:"), YES);

		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGFloat alpha = (b.size.width < 120.0f) ? 0.32f : 0.38f;
		CGFloat comps[4] = {1.0f, 1.0f, 1.0f, alpha};
		CGColorRef col = CGColorCreate(cs, comps);
		((void (*)(id, SEL, CGColorRef))objc_msgSend)(layer, sel_registerName("setBackgroundColor:"), col);
		CGColorRelease(col);
		CGColorSpaceRelease(cs);
	}

	Ivar tv_iv = class_getInstanceVariable(object_getClass(self), "_tintView");
	id tv = tv_iv ? object_getIvar(self, tv_iv) : nil;
	if (tv) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(tv, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(tv, sel_registerName("setAlpha:"), 0.0f);
	}

	Ivar bv_iv = class_getInstanceVariable(object_getClass(self), "_blurView");
	id bv = bv_iv ? object_getIvar(self, bv_iv) : nil;
	if (bv) {
		id bl = ((id (*)(id, SEL))objc_msgSend)(bv, sel_registerName("layer"));
		if (bl) {
			((void (*)(id, SEL, id))objc_msgSend)(bl, sel_registerName("setFilters:"), nil);
		}
		((void (*)(id, SEL, BOOL))objc_msgSend)(bv, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(bv, sel_registerName("setAlpha:"), 0.0f);
	}

	((void (*)(id, SEL, CGFloat))objc_msgSend)(self, sel_registerName("setAlpha:"), 1.0f);
	((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setHidden:"), NO);
}

static void (*original_floaty_folder_layout)(id, SEL);
static void probe_floaty_folder_layout(id self, SEL cmd)
{
	if (original_floaty_folder_layout) original_floaty_folder_layout(self, cmd);
	if (!strstr(object_getClassName(self), "FloatyFolderView")) return;

	Ivar pbv_iv = class_getInstanceVariable(object_getClass(self), "_pageBackgroundViews");
	id pbv = pbv_iv ? object_getIvar(self, pbv_iv) : nil;
	if (pbv) {
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(pbv, sel_registerName("count"));
		for (unsigned long i = 0; i < count; i++) {
			id bgv = ((id (*)(id, SEL, unsigned long))objc_msgSend)(pbv, sel_registerName("objectAtIndex:"), i);
			if (bgv) {
				((void (*)(id, SEL, CGFloat))objc_msgSend)(bgv, sel_registerName("setAlpha:"), 1.0f);
				((void (*)(id, SEL, BOOL))objc_msgSend)(bgv, sel_registerName("setHidden:"), NO);
				id bgl = ((id (*)(id, SEL))objc_msgSend)(bgv, sel_registerName("layer"));
				if (bgl) {
					((void (*)(id, SEL, id))objc_msgSend)(bgl, sel_registerName("setFilters:"), nil);
					((void (*)(id, SEL, BOOL))objc_msgSend)(bgl, sel_registerName("setShouldRasterize:"), NO);
					CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
					CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.40f};
					CGColorRef col = CGColorCreate(cs, comps);
					((void (*)(id, SEL, CGColorRef))objc_msgSend)(bgl, sel_registerName("setBackgroundColor:"), col);
					((void (*)(id, SEL, CGFloat))objc_msgSend)(bgl, sel_registerName("setCornerRadius:"), 30.0f);
					((void (*)(id, SEL, BOOL))objc_msgSend)(bgl, sel_registerName("setMasksToBounds:"), YES);
					CGColorRelease(col);
					CGColorSpaceRelease(cs);
				}
			}
		}
	}

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
				CGFloat comps[4] = {1.0f, 1.0f, 1.0f, 0.40f};
				CGColorRef col = CGColorCreate(cs, comps);
				((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), col);
				((void (*)(id, SEL, CGFloat))objc_msgSend)(l, sel_registerName("setCornerRadius:"), 30.0f);
				((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setMasksToBounds:"), YES);
				CGColorRelease(col);
				CGColorSpaceRelease(cs);
			}
		}
	}
}

static void (*original_folder_icon_img_layout)(id, SEL);
static void probe_folder_icon_img_layout(id self, SEL cmd)
{
	if (original_folder_icon_img_layout) original_folder_icon_img_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBFolderIconImageView") != 0) return;
	/* Preserve the native preview grid; replace only the material/solid plate. */
	const char *names[] = {"_backgroundView", "_solidColorBackgroundView"};
	for (unsigned i = 0; i < 2; i++) {
		Ivar iv = class_getInstanceVariable(object_getClass(self), names[i]);
		id bg = iv ? object_getIvar(self, iv) : nil;
		if (bg && !((BOOL (*)(id, SEL))objc_msgSend)(bg, sel_registerName("isHidden")))
			((void (*)(id, SEL, BOOL))objc_msgSend)(bg, sel_registerName("setHidden:"), YES);
	}
	style_plate(self, 14, 0.35);
}

static void (*original_search_backdrop_layout)(id, SEL);
static void probe_search_backdrop_layout(id self, SEL cmd)
{
	if (original_search_backdrop_layout) original_search_backdrop_layout(self, cmd);
	if (!strstr(object_getClassName(self), "SearchBackdropView")) return;

	Class uic = objc_getClass("UIColor");
	id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));
	((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("setBackgroundColor:"), clear);
	((void (*)(id, SEL, BOOL))objc_msgSend)(self, sel_registerName("setOpaque:"), NO);

	id l = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("layer"));
	if (l) {
		((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
		((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setOpaque:"), NO);
		((void (*)(id, SEL, id))objc_msgSend)(l, sel_registerName("setFilters:"), nil);
	}

	Ivar mv_iv = class_getInstanceVariable(object_getClass(self), "_materialView");
	id mv = mv_iv ? object_getIvar(self, mv_iv) : nil;
	if (mv) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(mv, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(mv, sel_registerName("setAlpha:"), 0.0f);
	}
}

static void (*original_search_bar_layout)(id, SEL);
static void probe_search_bar_layout(id self, SEL cmd)
{
	if (original_search_bar_layout) original_search_bar_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBHSearchBar") != 0) return;

	Ivar bg_iv = class_getInstanceVariable(object_getClass(self), "_backgroundView");
	id bg = bg_iv ? object_getIvar(self, bg_iv) : nil;
	if (bg) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(bg, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(bg, sel_registerName("setAlpha:"), 0.0f);
	}
}

static void (*original_library_search_layout)(id, SEL);
static void probe_library_search_layout(id self, SEL cmd)
{
	if (original_library_search_layout) original_library_search_layout(self, cmd);
	if (strcmp(object_getClassName(self), "SBHLibrarySearchController") != 0) return;

	Class uic = objc_getClass("UIColor");
	id clear = ((id (*)(id, SEL))objc_msgSend)((id)uic, sel_registerName("clearColor"));

	Ivar sbg_iv = class_getInstanceVariable(object_getClass(self), "_searchBackgroundView");
	id sbg = sbg_iv ? object_getIvar(self, sbg_iv) : nil;
	if (sbg) {
		((void (*)(id, SEL, id))objc_msgSend)(sbg, sel_registerName("setBackgroundColor:"), clear);
		((void (*)(id, SEL, BOOL))objc_msgSend)(sbg, sel_registerName("setOpaque:"), NO);
		id l = ((id (*)(id, SEL))objc_msgSend)(sbg, sel_registerName("layer"));
		if (l) {
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setOpaque:"), NO);
			((void (*)(id, SEL, id))objc_msgSend)(l, sel_registerName("setFilters:"), nil);
		}
	}

	Ivar sdv_iv = class_getInstanceVariable(object_getClass(self), "_searchDimmingView");
	id sdv = sdv_iv ? object_getIvar(self, sdv_iv) : nil;
	if (sdv) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(sdv, sel_registerName("setHidden:"), YES);
		((void (*)(id, SEL, CGFloat))objc_msgSend)(sdv, sel_registerName("setAlpha:"), 0.0f);
	}

	Ivar cvg_iv = class_getInstanceVariable(object_getClass(self), "_contentViewGradientMaskLayers");
	id cvg = cvg_iv ? object_getIvar(self, cvg_iv) : nil;
	if (cvg) {
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(cvg, sel_registerName("count"));
		for (unsigned long i = 0; i < count; i++) {
			id lyr = ((id (*)(id, SEL, unsigned long))objc_msgSend)(cvg, sel_registerName("objectAtIndex:"), i);
			((void (*)(id, SEL, BOOL))objc_msgSend)(lyr, sel_registerName("setHidden:"), YES);
			((void (*)(id, SEL, float))objc_msgSend)(lyr, sel_registerName("setOpacity:"), 0.0f);
		}
	}

	Ivar srg_iv = class_getInstanceVariable(object_getClass(self), "_searchResultsGradientMaskLayers");
	id srg = srg_iv ? object_getIvar(self, srg_iv) : nil;
	if (srg) {
		unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(srg, sel_registerName("count"));
		for (unsigned long i = 0; i < count; i++) {
			id lyr = ((id (*)(id, SEL, unsigned long))objc_msgSend)(srg, sel_registerName("objectAtIndex:"), i);
			((void (*)(id, SEL, BOOL))objc_msgSend)(lyr, sel_registerName("setHidden:"), YES);
			((void (*)(id, SEL, float))objc_msgSend)(lyr, sel_registerName("setOpacity:"), 0.0f);
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
		if (alpha < 0.10f) {
			cglog("cgprobe: %s %p setAlpha:%.2f -> forced 1.0\n", cname, self, alpha);
			original_window_set_alpha(self, cmd, 1.0f);
			return;
		}
	}
	original_window_set_alpha(self, cmd, alpha);
}

static void clear_blackout_in_view_mode(id view, BOOL switcher);
static void clear_blackout_in_view(id view) { clear_blackout_in_view_mode(view, NO); }
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
	/* A recycled snapshot view must be allowed to clear its previous app image. */
	original_switcher_snapshot_set_image(self, cmd, img);
}

static void (*original_switcher_snapshot_layout)(id, SEL);
static void probe_switcher_snapshot_layout(id self, SEL cmd)
{
	/* Keep the layout path free of synchronous diagnostic I/O. */
	original_switcher_snapshot_layout(self, cmd);
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

static void (*original_pbui_set_wallpaper_hidden)(id, SEL, BOOL, long long, id);
static void probe_pbui_set_wallpaper_hidden(id self, SEL cmd, BOOL hidden, long long variant, id reason)
{
	cglog("cgprobe: PBUI setWallpaperHidden:%d variant:%lld -> forced NO\n", hidden, (long long)variant);
	original_pbui_set_wallpaper_hidden(self, cmd, NO, variant, reason);
}

static void (*original_pbui_set_wallpaper_hidden_internal)(id, SEL, BOOL, long long, id);
static void probe_pbui_set_wallpaper_hidden_internal(id self, SEL cmd, BOOL hidden, long long variant, id reason)
{
	cglog("cgprobe: PBUI _setWallpaperHidden:%d variant:%lld -> forced NO\n", hidden, (long long)variant);
	original_pbui_set_wallpaper_hidden_internal(self, cmd, NO, variant, reason);
}

static BOOL (*original_pbui_is_wallpaper_hidden)(id, SEL, long long);
static BOOL probe_pbui_is_wallpaper_hidden(id self, SEL cmd, long long variant)
{
	return NO;
}

static void (*original_pbui_set_alpha_variant)(id, SEL, double, long long);
static void probe_pbui_set_alpha_variant(id self, SEL cmd, double alpha, long long variant)
{
	if (alpha < 0.10) {
		cglog("cgprobe: PBUI setAlpha:%.2f forWallpaperVariant:%lld -> forced 1.0\n", alpha, (long long)variant);
		alpha = 1.0;
	}
	original_pbui_set_alpha_variant(self, cmd, alpha, variant);
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

		clear_blackout_in_view_mode(v, YES);
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

static void clear_blackout_in_view_mode(id view, BOOL switcher)
{
	if (!view) return;
	const char *cname = object_getClassName(view);
	if (switcher && strcmp(cname, "SBWallpaperEffectView") == 0 &&
	    ((CGFloat (*)(id, SEL))objc_msgSend)(view, sel_registerName("alpha")) != 0.0f)
		((void (*)(id, SEL, CGFloat))objc_msgSend)(view, sel_registerName("setAlpha:"), 0.0f);
	/* App content and snapshots are not wallpaper backdrops. Do not mutate them. */
	if (strstr(cname, "ApplicationScene") || strstr(cname, "SwitcherSnapshot")) return;
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
	    strstr(cname, "SBHLibrarySearchController") || strstr(cname, "SBSearchBackdropView") ||
	    strstr(cname, "SBFluidSwitcherViewController") || strstr(cname, "SBSwitcherWallpaperPageBackgroundView") ||
	    strstr(cname, "Dimming") || strstr(cname, "Underlay") ||
	    (strstr(cname, "Background") && !strstr(cname, "Pod") && !strstr(cname, "Folder"))) {
		((void (*)(id, SEL, id))objc_msgSend)(view, sel_registerName("setBackgroundColor:"), clear);
		((void (*)(id, SEL, BOOL))objc_msgSend)(view, sel_registerName("setOpaque:"), NO);
		id l = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("layer"));
		if (l) {
			((void (*)(id, SEL, CGColorRef))objc_msgSend)(l, sel_registerName("setBackgroundColor:"), nil);
			((void (*)(id, SEL, BOOL))objc_msgSend)(l, sel_registerName("setOpaque:"), NO);
			((void (*)(id, SEL, id))objc_msgSend)(l, sel_registerName("setFilters:"), nil);
		}
		/* This traversal runs during layout; logging each view stalls gestures. */
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
		clear_blackout_in_view_mode(sv, switcher);
	}
}

static void install_all_hooks(void)
{
	static bool sbf_done = false;
	if (!sbf_done) {
		Class image = objc_getClass("UIImage");
		SEL selector = sel_registerName("sbf_imageFromContextWithSize:scale:type:pool:drawing:");
		Method method = image ? class_getClassMethod(image, selector) : NULL;
		if (method != NULL) {
			original_sbf_image = (void *)method_setImplementation(method, (IMP)probe_sbf_image);
			sbf_done = true;
			cglog("cgprobe: installed sbf_image hook\n");
		}
	}

	static bool swo_done = false;
	if (!swo_done) {
		Class wctrl = objc_getClass("SBWallpaperController");
		Method swo = wctrl ? class_getInstanceMethod(wctrl, sel_registerName("setWallpaperObscured:")) : NULL;
		if (swo != NULL) {
			original_set_wallpaper_obscured = (void *)method_setImplementation(swo, (IMP)probe_set_wallpaper_obscured);
			swo_done = true;
			cglog("cgprobe: installed SBWallpaperController setWallpaperObscured hook\n");
		}
	}

	static bool pbr_done = false;
	if (!pbr_done) {
		Class pbr = objc_getClass("PBUIPosterWallpaperRemoteViewController");
		if (pbr) {
			Method pbr_swo = class_getInstanceMethod(pbr, sel_registerName("setWallpaperObscured:"));
			if (pbr_swo) {
				original_pbui_remote_set_wallpaper_obscured = (void *)method_setImplementation(pbr_swo, (IMP)probe_pbui_remote_set_wallpaper_obscured);
				cglog("cgprobe: installed PBUIPosterWallpaperRemoteViewController setWallpaperObscured hook\n");
			}
			Method pbr_lay = class_getInstanceMethod(pbr, sel_registerName("viewWillLayoutSubviews"));
			if (pbr_lay) {
				original_pbui_remote_layout = (void *)method_setImplementation(pbr_lay, (IMP)probe_pbui_remote_layout);
				cglog("cgprobe: installed PBUIPosterWallpaperRemoteViewController layout hook\n");
			}
			pbr_done = true;
		}
	}

	static bool pbp_done = false;
	if (!pbp_done) {
		Class pbp = objc_getClass("PBUIPosterWallpaperViewController");
		if (pbp) {
			Method pbp_swo = class_getInstanceMethod(pbp, sel_registerName("setWallpaperObscured:"));
			if (pbp_swo) {
				original_pbui_poster_set_wallpaper_obscured = (void *)method_setImplementation(pbp_swo, (IMP)probe_pbui_poster_set_wallpaper_obscured);
				pbp_done = true;
				cglog("cgprobe: installed PBUIPosterWallpaperViewController setWallpaperObscured hook\n");
			}
		}
	}

	static bool pbui_done = false;
	if (!pbui_done) {
		Class pbui = objc_getClass("PBUIWallpaperViewController");
		if (pbui) {
			Method pbuim = class_getInstanceMethod(pbui, sel_registerName("viewWillLayoutSubviews"));
			if (pbuim) {
				original_pbui_layout = (void *)method_setImplementation(pbuim, (IMP)probe_pbui_layout);
			}
			Method set_hid = class_getInstanceMethod(pbui, sel_registerName("setWallpaperHidden:variant:reason:"));
			if (set_hid) {
				original_pbui_set_wallpaper_hidden = (void *)method_setImplementation(set_hid, (IMP)probe_pbui_set_wallpaper_hidden);
			}
			Method set_hid_int = class_getInstanceMethod(pbui, sel_registerName("_setWallpaperHidden:variant:reason:"));
			if (set_hid_int) {
				original_pbui_set_wallpaper_hidden_internal = (void *)method_setImplementation(set_hid_int, (IMP)probe_pbui_set_wallpaper_hidden_internal);
			}
			Method is_hid = class_getInstanceMethod(pbui, sel_registerName("_isWallpaperHiddenForVariant:"));
			if (is_hid) {
				original_pbui_is_wallpaper_hidden = (void *)method_setImplementation(is_hid, (IMP)probe_pbui_is_wallpaper_hidden);
			}
			Method set_alp = class_getInstanceMethod(pbui, sel_registerName("setAlpha:forWallpaperVariant:"));
			if (set_alp) {
				original_pbui_set_alpha_variant = (void *)method_setImplementation(set_alp, (IMP)probe_pbui_set_alpha_variant);
			}
			pbui_done = true;
			cglog("cgprobe: installed PBUIWallpaperViewController layout and visibility hooks\n");
		}
	}

	static bool win_done = false;
	if (!win_done) {
		Class wcls = objc_getClass("UIWindow");
		if (wcls) {
			Method whid = class_getInstanceMethod(wcls, sel_registerName("setHidden:"));
			if (whid) {
				original_window_set_hidden = (void *)method_setImplementation(whid, (IMP)probe_window_set_hidden);
			}
			Method walp = class_getInstanceMethod(wcls, sel_registerName("setAlpha:"));
			if (walp) {
				/* setAlpha: is inherited from UIView on iOS 16. Installing on
				 * the returned Method mutates UIView globally, forcing every
				 * Wallpaper-named effect view opaque. Override UIWindow only. */
				original_window_set_alpha = (void *)method_getImplementation(walp);
				if (!class_addMethod(wcls, sel_registerName("setAlpha:"),
				    (IMP)probe_window_set_alpha, method_getTypeEncoding(walp))) {
					original_window_set_alpha = (void *)method_setImplementation(walp, (IMP)probe_window_set_alpha);
				}
			}
			win_done = true;
			cglog("cgprobe: installed UIWindow visibility hooks\n");
		}
	}

	static bool sbd_done = false;
	if (!sbd_done) {
		Class sbd = objc_getClass("SBSearchBackdropView");
		if (sbd) {
			Method sbdl = class_getInstanceMethod(sbd, sel_registerName("layoutSubviews"));
			if (sbdl) {
				original_search_backdrop_layout = (void *)method_setImplementation(sbdl, (IMP)probe_search_backdrop_layout);
				sbd_done = true;
				cglog("cgprobe: installed SBSearchBackdropView layout hook\n");
			}
		}
	}

	static bool sbar_done = false;
	if (!sbar_done) {
		Class sbar = objc_getClass("SBHSearchBar");
		if (sbar) {
			Method sbarl = class_getInstanceMethod(sbar, sel_registerName("layoutSubviews"));
			if (sbarl) {
				original_search_bar_layout = (void *)method_setImplementation(sbarl, (IMP)probe_search_bar_layout);
				sbar_done = true;
				cglog("cgprobe: installed SBHSearchBar layout hook\n");
			}
		}
	}

	static bool lsc_done = false;
	if (!lsc_done) {
		Class lsc = objc_getClass("SBHLibrarySearchController");
		if (lsc) {
			Method lscl = class_getInstanceMethod(lsc, sel_registerName("viewWillLayoutSubviews"));
			if (lscl) {
				original_library_search_layout = (void *)method_setImplementation(lscl, (IMP)probe_library_search_layout);
				lsc_done = true;
				cglog("cgprobe: installed SBHLibrarySearchController layout hook\n");
			}
		}
	}

	static bool lib_done = false;
	if (!lib_done) {
		Class lib = objc_getClass("SBHLibraryViewController");
		if (lib) {
			Method libm = class_getInstanceMethod(lib, sel_registerName("viewWillLayoutSubviews"));
			if (libm) {
				original_library_layout = (void *)method_setImplementation(libm, (IMP)probe_library_layout);
				lib_done = true;
				cglog("cgprobe: installed SBHLibraryViewController layout hook\n");
			}
		}
	}

	install_switcher_hooks();
	install_feather_blur_hooks();

	static bool snap_done = false;
	if (!snap_done) {
		Class snap = objc_getClass("SBSwitcherSnapshotImageView");
		if (snap) {
			Method snap_set = class_getInstanceMethod(snap, sel_registerName("setImage:"));
			if (snap_set) {
				original_switcher_snapshot_set_image = (void *)method_setImplementation(snap_set, (IMP)probe_switcher_snapshot_set_image);
				cglog("cgprobe: installed SBSwitcherSnapshotImageView setImage hook\n");
			}
			Method snap_lay = class_getInstanceMethod(snap, sel_registerName("layoutSubviews"));
			if (snap_lay) {
				original_switcher_snapshot_layout = (void *)method_setImplementation(snap_lay, (IMP)probe_switcher_snapshot_layout);
				cglog("cgprobe: installed SBSwitcherSnapshotImageView layoutSubviews hook\n");
			}
			snap_done = true;
		}
	}

	static bool fimg_done = false;
	if (!fimg_done) {
		Class fimg = objc_getClass("SBFolderIconImageView");
		if (fimg) {
			Method fimgl = class_getInstanceMethod(fimg, sel_registerName("layoutSubviews"));
			if (fimgl != NULL) {
				original_folder_icon_img_layout = (void *)method_setImplementation(fimgl, (IMP)probe_folder_icon_img_layout);
				cglog("cgprobe: installed SBFolderIconImageView layout hook\n");
			}
			fimg_done = true;
		}
	}

	static bool fbg_done = false;
	if (!fbg_done) {
		Class fbg = objc_getClass("SBFolderBackgroundView");
		if (fbg) {
			Method fbgl = class_getInstanceMethod(fbg, sel_registerName("layoutSubviews"));
			if (fbgl != NULL) {
				original_folder_bg_layout = (void *)method_setImplementation(fbgl, (IMP)probe_folder_bg_layout);
				fbg_done = true;
				cglog("cgprobe: installed SBFolderBackgroundView layout hook\n");
			}
		}
	}

	static bool floaty_done = false;
	if (!floaty_done) {
		Class floaty = objc_getClass("SBFloatyFolderView");
		if (floaty) {
			Method floatyl = class_getInstanceMethod(floaty, sel_registerName("layoutSubviews"));
			if (floatyl != NULL) {
				original_floaty_folder_layout = (void *)method_setImplementation(floatyl, (IMP)probe_floaty_folder_layout);
				floaty_done = true;
				cglog("cgprobe: installed SBFloatyFolderView layout hook\n");
			}
		}
	}

	static bool dock_done = false;
	if (!dock_done) {
		Class dock = objc_getClass("SBDockView");
		if (dock) {
			Method dockl = class_getInstanceMethod(dock, sel_registerName("layoutSubviews"));
			if (dockl != NULL) {
				original_dock_layout = (void *)method_setImplementation(dockl, (IMP)probe_dock_layout);
				dock_done = true;
				cglog("cgprobe: installed SBDockView layout hook\n");
			}
		}
	}

	static bool fiv_done = false;
	if (!fiv_done) {
		Class fiv = objc_getClass("SBFFocusIsolationView");
		if (fiv) {
			Method fivm = class_getInstanceMethod(fiv, sel_registerName("layoutSubviews"));
			if (fivm) {
				original_focus_isolation_layout = (void *)method_setImplementation(fivm, (IMP)probe_focus_isolation_layout);
				fiv_done = true;
				cglog("cgprobe: installed SBFFocusIsolationView layout hook\n");
			}
		}
	}

	static bool pill_done = false;
	if (!pill_done) {
		Class pill = objc_getClass("SBHSearchPillView");
		if (pill) {
			Method pillm = class_getInstanceMethod(pill, sel_registerName("layoutSubviews"));
			if (pillm) {
				original_search_pill_layout = (void *)method_setImplementation(pillm, (IMP)probe_search_pill_layout);
				pill_done = true;
				cglog("cgprobe: installed SBHSearchPillView layout hook\n");
			}
		}
	}

	static bool cal_done = false;
	if (!cal_done) {
		Class cal = objc_getClass("CALayer");
		if (cal) {
			Method srm = class_getInstanceMethod(cal, sel_registerName("setShouldRasterize:"));
			if (srm != NULL) {
				original_layer_set_should_rasterize = (void *)method_setImplementation(srm, (IMP)probe_layer_set_should_rasterize);
				cal_done = true;
				cglog("cgprobe: installed CALayer setShouldRasterize hook\n");
			}
		}
	}

	static bool mtl_done = false;
	if (!mtl_done) {
		Class mtl = objc_getClass("MTMaterialLayer");
		if (mtl) {
			Method mtl_set = class_getInstanceMethod(mtl, sel_registerName("setContentReplacedWithSnapshot:"));
			if (mtl_set) {
				original_material_set_content_replaced_with_snapshot = (void *)method_setImplementation(mtl_set, (IMP)probe_material_set_content_replaced_with_snapshot);
				cglog("cgprobe: installed MTMaterialLayer setContentReplacedWithSnapshot hook\n");
			}
			Method mtl_is = class_getInstanceMethod(mtl, sel_registerName("isContentReplacedWithSnapshot"));
			if (mtl_is) {
				original_material_is_content_replaced_with_snapshot = (void *)method_setImplementation(mtl_is, (IMP)probe_material_is_content_replaced_with_snapshot);
			}
			mtl_done = true;
		}
	}

	static bool mtv_done = false;
	if (!mtv_done) {
		Class mtv = objc_getClass("MTMaterialView");
		if (mtv) {
			Method mtv_set = class_getInstanceMethod(mtv, sel_registerName("setContentReplacedWithSnapshot:"));
			if (mtv_set) {
				original_material_view_set_content_replaced_with_snapshot = (void *)method_setImplementation(mtv_set, (IMP)probe_material_view_set_content_replaced_with_snapshot);
				cglog("cgprobe: installed MTMaterialView setContentReplacedWithSnapshot hook\n");
			}
			Method mtv_is = class_getInstanceMethod(mtv, sel_registerName("isContentReplacedWithSnapshot"));
			if (mtv_is) {
				original_material_view_is_content_replaced_with_snapshot = (void *)method_setImplementation(mtv_is, (IMP)probe_material_view_is_content_replaced_with_snapshot);
			}
			mtv_done = true;
		}
	}
}

static void fix_all_blackouts(void)
{
	install_all_hooks();
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
				id rvc_view = ((id (*)(id, SEL))objc_msgSend)(rvc, sel_registerName("view"));
				if (rvc_view) {
					((void (*)(id, SEL, BOOL))objc_msgSend)(rvc_view, sel_registerName("setHidden:"), NO);
					((void (*)(id, SEL, CGFloat))objc_msgSend)(rvc_view, sel_registerName("setAlpha:"), 1.0f);
				}
				Ivar wcv_iv = class_getInstanceVariable(object_getClass(rvc), "_wallpaperContainerView");
				id wcv = wcv_iv ? object_getIvar(rvc, wcv_iv) : nil;
				if (wcv) {
					((void (*)(id, SEL, BOOL))objc_msgSend)(wcv, sel_registerName("setHidden:"), NO);
					((void (*)(id, SEL, CGFloat))objc_msgSend)(wcv, sel_registerName("setAlpha:"), 1.0f);
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
	install_all_hooks();

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		install_all_hooks();
	});

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		install_all_hooks();
		/* Full diagnostics are available through the one-shot dump signal. */
	});

	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^{
		while (1) {
			sleep(2);
			if (access("/tmp/dump_now", F_OK) == 0) {
				unlink("/tmp/dump_now");
				dispatch_async(dispatch_get_main_queue(), ^{
					scan_objc_classes();
					dump_visible_windows();
					dump_all_windows_and_views();
				});
			}
			if (access("/tmp/unlock", F_OK) == 0) {
				unlink("/tmp/unlock");
				dispatch_async(dispatch_get_main_queue(), ^{
					Class sblm = objc_getClass("SBLockScreenManager");
					if (sblm) {
						id mgr = ((id (*)(id, SEL))objc_msgSend)(sblm, sel_registerName("sharedInstance"));
						if (mgr) {
							SEL sel = sel_registerName("unlockUIFromSource:withOptions:");
							if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(mgr, sel_registerName("respondsToSelector:"), sel)) {
								((void (*)(id, SEL, int, id))objc_msgSend)(mgr, sel, 0, nil);
								cglog("cgprobe: triggered unlockUIFromSource:withOptions:\n");
							}
						}
					}
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
