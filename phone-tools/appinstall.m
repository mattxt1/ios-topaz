/* Install a locally prepared stock-app tar stream and register its bundles.
 * appinstall --install < stock-apps.tar (top-level NAME.app entries only)
 * appinstall --register /Applications/NAME.app
 * Files are extracted to a unique staging directory, then moved without
 * replacing existing apps. Failed staging directories are retained for repair.
 * Apple bundles and archives remain private; this source contains none.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <unistd.h>
struct archive; struct archive_entry;
extern struct archive *archive_read_new(void);
extern int archive_read_support_format_tar(struct archive *);
extern int archive_read_support_filter_none(struct archive *);
extern int archive_read_open_fd(struct archive *, int, size_t);
extern int archive_read_next_header(struct archive *, struct archive_entry **);
extern const char *archive_entry_pathname(struct archive_entry *);
extern int archive_read_extract(struct archive *, struct archive_entry *, int);
extern const char *archive_error_string(struct archive *);
extern int archive_read_free(struct archive *);
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)registerApplicationDictionary:(NSDictionary *)info;
@end
int main(int argc, char **argv) { @autoreleasepool {
 if (argc == 3 && !strcmp(argv[1], "--register")) {
 dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_NOW);
 id ws=[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
 NSString *path=@(argv[2]);
 NSMutableDictionary *info=[[NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]] mutableCopy];
 if (!info || !ws) return 1;
 info[@"Path"]=path; info[@"ApplicationType"]=@"System"; info[@"IsDeletable"]=@YES;
 BOOL ok=[ws registerApplicationDictionary:info];
 printf("%s: registered=%s\n",argv[2],ok?"yes":"NO");return ok?0:1;
 }
 if (argc != 2 || strcmp(argv[1], "--install")) { fprintf(stderr,"usage: appinstall --install < stock-apps.tar | --register /Applications/NAME.app\n"); return 2; }
 NSFileManager *fm = NSFileManager.defaultManager;
 NSString *stage = [@"/Applications/.river-stock-" stringByAppendingString:NSUUID.UUID.UUIDString];
 NSError *error = nil;
 if (![fm createDirectoryAtPath:stage withIntermediateDirectories:NO attributes:nil error:&error] || chdir(stage.fileSystemRepresentation)) { NSLog(@"stage: %@",error); return 1; }
 struct archive *a=archive_read_new(); struct archive_entry *e;
 archive_read_support_format_tar(a); archive_read_support_filter_none(a);
 if (archive_read_open_fd(a,0,65536)) { fprintf(stderr,"archive open failed\n"); return 1; }
 int status;
 while ((status=archive_read_next_header(a,&e))==0) {
  const char *p=archive_entry_pathname(e);
  NSString *path=p ? @(p) : @"";
  NSArray *parts=path.pathComponents;
  if (!parts.count || path.isAbsolutePath || [parts containsObject:@".."] || ![parts[0] hasSuffix:@".app"]) { fprintf(stderr,"unsafe archive path\n"); return 1; }
  /* Preserve modes/times, refuse traversal through symlinks and dot-dot. */
  if (archive_read_extract(a,e,0x2|0x4|0x100|0x200|0x10000)<0) { fprintf(stderr,"extract: %s\n",archive_error_string(a)); return 1; }
 }
 archive_read_free(a);
 if(status!=1) { fprintf(stderr,"incomplete archive\n"); return 1; }
 dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",RTLD_NOW);
 id workspace=[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
 if(!workspace) { fprintf(stderr,"LaunchServices unavailable; staged files retained\n");return 1; }
 int failures=0;
 for (NSString *name in [[fm contentsOfDirectoryAtPath:stage error:&error] sortedArrayUsingSelector:@selector(compare:)]) {
  NSString *dest=[@"/Applications" stringByAppendingPathComponent:name];
  if([fm fileExistsAtPath:dest]) { fprintf(stderr,"skip existing %s\n",name.UTF8String);failures++;continue; }
  if(![fm moveItemAtPath:[stage stringByAppendingPathComponent:name] toPath:dest error:&error]) { NSLog(@"move %@: %@",name,error);failures++;continue; }
  NSMutableDictionary *info=[[NSDictionary dictionaryWithContentsOfFile:[dest stringByAppendingPathComponent:@"Info.plist"]] mutableCopy];
  info[@"Path"]=dest; info[@"ApplicationType"]=@"System"; info[@"IsDeletable"]=@YES;
  BOOL ok=[workspace registerApplicationDictionary:info];
  printf("%s: registered=%s\n",name.UTF8String,ok?"yes":"NO");fflush(stdout); if(!ok)failures++;
 }
 if(!failures) [fm removeItemAtPath:stage error:NULL];
 return failures?1:0;
} }
