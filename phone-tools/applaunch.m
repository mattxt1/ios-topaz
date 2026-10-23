#import <Foundation/Foundation.h>
#include <dlfcn.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
@end

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) {
        fprintf(stderr, "usage: applaunch BUNDLE_ID\n");
        return 2;
    }
    dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    id workspace = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
    if (!workspace || ![workspace respondsToSelector:@selector(openApplicationWithBundleID:)]) {
        fprintf(stderr, "LaunchServices open API unavailable\n");
        return 1;
    }
    NSString *bundleID = [NSString stringWithUTF8String:argv[1]];
    BOOL opened = [workspace openApplicationWithBundleID:bundleID];
    printf("%s: open=%s\n", argv[1], opened ? "yes" : "NO");
    return opened ? 0 : 1;
} }
