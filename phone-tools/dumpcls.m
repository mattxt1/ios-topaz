#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		dlopen("/System/Library/PrivateFrameworks/SpringBoardFoundation.framework/SpringBoardFoundation", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/SpringBoardHome.framework/SpringBoardHome", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/PaperBoardUI.framework/PaperBoardUI", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/CoverSheet.framework/CoverSheet", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/CoreMaterial.framework/CoreMaterial", RTLD_NOW);
		dlopen("/System/Library/PrivateFrameworks/MaterialKit.framework/MaterialKit", RTLD_NOW);
		dlopen("/System/Library/CoreServices/SpringBoard.app/SpringBoard", RTLD_NOW);

		if (argc < 2) {
			printf("usage: dumpcls <ClassName> [filter]\n");
			return 1;
		}
	const char *cls_name = argv[1];
	const char *filt = argc > 2 ? argv[2] : NULL;
	Class cls = objc_getClass(cls_name);
	if (!cls) {
		printf("class %s not found\n", cls_name);
		return 1;
	}
	printf("=== Class: %s (super: %s) ===\n", class_getName(cls), class_getName(class_getSuperclass(cls)));
	unsigned int count = 0;
	Method *methods = class_copyMethodList(cls, &count);
	printf("Methods (%u):\n", count);
	for (unsigned int i = 0; i < count; i++) {
		const char *mname = sel_getName(method_getName(methods[i]));
		if (!filt || strcasestr(mname, filt)) {
			printf("  -[%s %s]\n", cls_name, mname);
		}
	}
	if (methods) free(methods);

	Ivar *ivars = class_copyIvarList(cls, &count);
	printf("Ivars (%u):\n", count);
	for (unsigned int i = 0; i < count; i++) {
		const char *iname = ivar_getName(ivars[i]);
		if (!filt || strcasestr(iname, filt)) {
			printf("  %s (%s)\n", iname, ivar_getTypeEncoding(ivars[i]));
		}
	}
	if (ivars) free(ivars);
	}
	return 0;
}
