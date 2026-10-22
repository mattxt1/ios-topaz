/*
 * River EAGL adapter, stage 1: observe only.
 *
 * Injected into backboardd with riverenv=backboardd:DYLD_INSERT_LIBRARIES=
 * (kernel patch 0182). When OpenGLES's classes are loaded it logs, through
 * RiverGPU's LOG selector (the USB log):
 *  - every method of EAGLContext and EAGLSharegroup with its type encoding;
 *  - calls to each of those methods (the first three, then at 10, 100, ...)
 *    with self and the first two arguments, via river-eagl-stubs.S, which
 *    then runs Apple's original implementation unchanged;
 *  - for -getMacroContextPrivate (CoreAnimation's GLI entry point), the
 *    returned pointer, its first words and how many consecutive words point
 *    into the GL engine image, i.e. the dispatch table size on this build.
 * Nothing in the process changes behaviour; later stages route contexts to
 * the Mesa library (librivergl) while keeping this path as the fallback.
 *   tools/river-eagl/build.sh OUT
 */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RIVER_EAGL_STUBS 512    /* river-eagl-stubs.S */

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

extern char river_eagl_stubs[];

static struct hook {
	IMP orig;
	const char *cls;
	const char *sel;
	char kind;              /* '-' instance, '+' class */
	_Atomic uint64_t calls;
} hooks[RIVER_EAGL_STUBS];
static int nhooks;

static int
worth_logging(uint64_t n)
{
	if (n <= 3) {
		return 1;
	}
	while (n % 10 == 0) {
		n /= 10;
	}
	return n == 1;
}

/* Called by every stub; returns the implementation to continue with. */
IMP river_eagl_enter(unsigned idx, void *self, SEL cmd, uintptr_t a2, uintptr_t a3);

IMP
river_eagl_enter(unsigned idx, void *self, SEL cmd, uintptr_t a2, uintptr_t a3)
{
	struct hook *h = &hooks[idx];
	uint64_t n = atomic_fetch_add(&h->calls, 1) + 1;
	(void)cmd;
	if (worth_logging(n)) {
		uint64_t tid = 0;
		pthread_threadid_np(NULL, &tid);
		say("eagl: %c[%s %s] #%llu self %p args %#lx %#lx tid %llu",
		    h->kind, h->cls, h->sel, n, self, a2, a3, tid);
	}
	return h->orig;
}

/*
 * -getMacroContextPrivate: log what it returns (CoreAnimation reads the GLI
 * context and dispatch table from it).
 */
static IMP macro_orig;
static _Atomic uint64_t macro_calls;

static void
describe_word(const char *what, int i, uintptr_t w)
{
	Dl_info info;
	if (w != 0 && dladdr((void *)w, &info) && info.dli_fname != NULL) {
		const char *f = strrchr(info.dli_fname, '/');
		say("eagl: macro %s[%d] = %#lx -> %s %s+%#lx", what, i, w, f ? f + 1 : info.dli_fname,
		    info.dli_sname ? info.dli_sname : "?", w - (uintptr_t)(info.dli_saddr ? info.dli_saddr : info.dli_fbase));
	} else {
		say("eagl: macro %s[%d] = %#lx", what, i, w);
	}
}

/* Read a word without faulting if the guess about the layout is wrong. */
static int
peek(const uintptr_t *p, uintptr_t *out)
{
	vm_size_t got = 0;
	return vm_read_overwrite(mach_task_self(), (vm_address_t)p, sizeof(*out),
	           (vm_address_t)out, &got) == KERN_SUCCESS && got == sizeof(*out);
}

static void *
macro_wrapper(void *self, SEL cmd)
{
	void *r = ((void *(*)(void *, SEL))macro_orig)(self, cmd);
	uint64_t n = atomic_fetch_add(&macro_calls, 1) + 1;
	if (!worth_logging(n)) {
		return r;
	}
	say("eagl: -[EAGLContext getMacroContextPrivate] #%llu self %p -> %p", n, self, r);
	if (r == NULL || n > 2) {
		return r;
	}
	const uintptr_t *w = r;
	uintptr_t v;
	for (int i = 0; i < 4 && peek(&w[i], &v); i++) {
		describe_word("word", i, v);
	}
	/* Find the first word that points into code and measure the run of
	 * words pointing into that same image: the dispatch table. */
	for (int i = 0; i < 8 && peek(&w[i], &v); i++) {
		Dl_info info;
		if (v == 0 || !dladdr((void *)v, &info) || info.dli_fbase == NULL) {
			continue;
		}
		const void *base = info.dli_fbase;
		int j = i;
		uintptr_t last = v, after = 0;
		while (j < i + 4096 && peek(&w[j], &after)) {
			Dl_info e;
			if (after != 0 && (!dladdr((void *)after, &e) || e.dli_fbase != base)) {
				break;
			}
			if (after != 0) {
				last = after;
			}
			j++;
		}
		const char *f = strrchr(info.dli_fname, '/');
		say("eagl: macro words %d..%d (%d) point into %s (zero words allowed)", i, j - 1, j - i,
		    f ? f + 1 : info.dli_fname);
		for (int k = i; k < j && k < i + 3 && peek(&w[k], &v); k++) {
			describe_word("table", k - i, v);
		}
		describe_word("table-last-nonzero", j - 1 - i, last);
		describe_word("after", j - i, after);
		break;
	}
	return r;
}

static int
skipped(const char *sel)
{
	static const char *const skip[] = {
		/* objc retain/release fast paths and destruction */
		"retain", "release", "autorelease", "retainCount", "_tryRetain",
		"_isDeallocating", "allowsWeakReference", "retainWeakReference",
		".cxx_destruct", ".cxx_construct",
	};
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strcmp(sel, skip[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
hook_class(Class cls, char kind, const char *name)
{
	unsigned int count = 0;
	Method *m = class_copyMethodList(cls, &count);
	say("eagl: %s has %u %s methods", name, count, kind == '+' ? "class" : "instance");
	for (unsigned int i = 0; i < count; i++) {
		const char *sel = sel_getName(method_getName(m[i]));
		const char *types = method_getTypeEncoding(m[i]);
		say("eagl: %c[%s %s] %s", kind, name, sel, types ? types : "?");
		if (skipped(sel)) {
			continue;
		}
		size_t tl = types ? strlen(types) : 0;
		if (kind == '-' && strcmp(sel, "getMacroContextPrivate") == 0 &&
		    tl > 6 && types[0] == '^' && strcmp(types + tl - 6, "16@0:8") == 0) {
			macro_orig = method_setImplementation(m[i], (IMP)macro_wrapper);
			continue;
		}
		if (nhooks == RIVER_EAGL_STUBS) {
			say("eagl: out of stubs at %s", sel);
			break;
		}
		struct hook *h = &hooks[nhooks];
		h->cls = name;
		h->sel = sel;   /* selector names live as long as the process */
		h->kind = kind;
		h->orig = method_getImplementation(m[i]);
		method_setImplementation(m[i], (IMP)(river_eagl_stubs + 8 * nhooks));
		nhooks++;
	}
	free(m);
}

static void
hook_named(const char *name)
{
	Class cls = objc_getClass(name);
	if (cls == Nil) {
		say("eagl: class %s not found", name);
		return;
	}
	hook_class(object_getClass((id)cls), '+', name);
	hook_class(cls, '-', name);
}

static _Atomic int hooked;

static void
image_loaded(const struct mach_header *mh)
{
	Dl_info info;
	if (atomic_load(&hooked) || !dladdr(mh, &info) || info.dli_fname == NULL ||
	    strcmp(info.dli_fname, "/System/Library/Frameworks/OpenGLES.framework/OpenGLES") != 0) {
		return;
	}
	if (atomic_exchange(&hooked, 1)) {
		return;
	}
	hook_named("EAGLContext");
	hook_named("EAGLSharegroup");
	say("eagl: %d methods hooked (observe only)%s", nhooks,
	    macro_orig ? ", getMacroContextPrivate wrapped" : ", getMacroContextPrivate NOT wrapped");
}

__attribute__((constructor))
static void
river_eagl_init(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RiverGPU"));
	if (svc == IO_OBJECT_NULL) {
		return;         /* no log channel: stay out of the way */
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		conn = MACH_PORT_NULL;
		return;
	}
	say("eagl: stage 1 (observe) loaded in %s pid %d", getprogname(), getpid());
	/* Called for loaded images now and each later one, after objc has
	 * set up its classes. */
	objc_addLoadImageFunc(image_loaded);
}
