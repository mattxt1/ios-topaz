#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>


extern CFTypeRef IOPSCopyPowerSourcesInfo(void);
extern CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob);
extern CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps);

int main(int argc, char **argv)
{
	@autoreleasepool {
		io_service_t sb = IOServiceGetMatchingService(0, IOServiceMatching("SweetBattery"));
		printf("SweetBattery matching: 0x%x\n", sb);
		if (sb) {
			CFMutableDictionaryRef props = NULL;
			if (IORegistryEntryCreateCFProperties(sb, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
				NSLog(@"SweetBattery props: %@", (__bridge id)props);
				CFRelease(props);
			}
			IOObjectRelease(sb);
		}

		io_service_t ps = IOServiceGetMatchingService(0, IOServiceMatching("IOPMPowerSource"));
		printf("IOPMPowerSource matching: 0x%x\n", ps);
		if (ps) {
			CFMutableDictionaryRef props = NULL;
			if (IORegistryEntryCreateCFProperties(ps, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
				NSLog(@"IOPMPowerSource props: %@", (__bridge id)props);
				CFRelease(props);
			}
			IOObjectRelease(ps);
		}

		CFTypeRef info = IOPSCopyPowerSourcesInfo();
		printf("IOPSCopyPowerSourcesInfo: %p\n", info);
		if (info) {
			CFArrayRef list = IOPSCopyPowerSourcesList(info);
			printf("IOPSCopyPowerSourcesList: %p (count=%ld)\n", list, list ? CFArrayGetCount(list) : 0);
			if (list) {
				for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
					CFTypeRef source = CFArrayGetValueAtIndex(list, i);
					CFDictionaryRef desc = IOPSGetPowerSourceDescription(info, source);
					NSLog(@"Source[%ld]: %@", i, (__bridge id)desc);
				}
				CFRelease(list);
			}
			CFRelease(info);
		}
	}
	return 0;
}
