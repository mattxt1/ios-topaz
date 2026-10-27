#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>

int main(void) {
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("AppleCLCD"));
	if (!service) {
		printf("AppleCLCD service not found\n");
		return 1;
	}
	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		printf("IOServiceOpen failed: 0x%x\n", kr);
		return 1;
	}
	printf("Opened AppleCLCD conn=%d\n", conn);

	uint64_t out[2] = {0, 0};
	uint32_t outCount = 2;
	kr = IOConnectCallScalarMethod(conn, 8 /* kSweetFBGetDisplaySize */, NULL, 0, out, &outCount);
	printf("GetDisplaySize: kr=0x%x w=%llu h=%llu\n", kr, out[0], out[1]);

	out[0] = 0;
	outCount = 1;
	kr = IOConnectCallScalarMethod(conn, 3 /* kSweetFBGetLayerDefaultSurface */, NULL, 0, out, &outCount);
	printf("GetLayerDefaultSurface: kr=0x%x port=0x%llx\n", kr, out[0]);

	if (out[0] != 0) {
		IOSurfaceRef surf = IOSurfaceLookupFromMachPort((mach_port_t)out[0]);
		printf("IOSurfaceLookup: surf=%p\n", surf);
		if (surf) {
			size_t w = IOSurfaceGetWidth(surf);
			size_t h = IOSurfaceGetHeight(surf);
			size_t bpr = IOSurfaceGetBytesPerRow(surf);
			printf("Surface dimensions: %zux%zu bpr=%zu\n", w, h, bpr);
			CFRelease(surf);
		}
	}

	IOServiceClose(conn);
	return 0;
}
