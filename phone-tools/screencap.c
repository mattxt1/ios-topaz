#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>

#pragma pack(push, 1)
typedef struct {
	uint16_t bfType;
	uint32_t bfSize;
	uint16_t bfReserved1;
	uint16_t bfReserved2;
	uint32_t bfOffBits;
	uint32_t biSize;
	int32_t  biWidth;
	int32_t  biHeight;
	uint16_t biPlanes;
	uint16_t biBitCount;
	uint32_t biCompression;
	uint32_t biSizeImage;
	int32_t  biXPelsPerMeter;
	int32_t  biYPelsPerMeter;
	uint32_t biClrUsed;
	uint32_t biClrImportant;
} BMPHeader;
#pragma pack(pop)

int main(int argc, char **argv) {
	const char *outpath = (argc > 1) ? argv[1] : "/tmp/screen.bmp";

	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("AppleCLCD"));
	if (!service) {
		fprintf(stderr, "AppleCLCD service not found\n");
		return 1;
	}
	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
		return 1;
	}

	uint64_t out = 0;
	uint32_t outCount = 1;
	kr = IOConnectCallScalarMethod(conn, 3 /* kSweetFBGetLayerDefaultSurface */, NULL, 0, &out, &outCount);
	IOServiceClose(conn);

	if (kr != KERN_SUCCESS || out == 0) {
		fprintf(stderr, "kSweetFBGetLayerDefaultSurface failed: kr=0x%x\n", kr);
		return 1;
	}

	IOSurfaceRef surf = IOSurfaceLookupFromMachPort((mach_port_t)out);
	if (!surf) {
		fprintf(stderr, "IOSurfaceLookupFromMachPort failed for port 0x%llx\n", out);
		return 1;
	}

	size_t w = IOSurfaceGetWidth(surf);
	size_t h = IOSurfaceGetHeight(surf);
	size_t bpr = IOSurfaceGetBytesPerRow(surf);

	kr = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "IOSurfaceLock failed: 0x%x\n", kr);
		CFRelease(surf);
		return 1;
	}

	const uint8_t *base = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
	if (!base) {
		fprintf(stderr, "IOSurfaceGetBaseAddress returned NULL\n");
		IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
		CFRelease(surf);
		return 1;
	}

	FILE *fp = fopen(outpath, "wb");
	if (!fp) {
		fprintf(stderr, "Cannot open %s for writing\n", outpath);
		IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
		CFRelease(surf);
		return 1;
	}

	uint32_t rowSize = (uint32_t)(w * 4);
	uint32_t imageSize = rowSize * (uint32_t)h;

	BMPHeader hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.bfType = 0x4D42; /* 'BM' */
	hdr.bfSize = sizeof(BMPHeader) + imageSize;
	hdr.bfOffBits = sizeof(BMPHeader);
	hdr.biSize = 40;
	hdr.biWidth = (int32_t)w;
	hdr.biHeight = -(int32_t)h; /* top-down */
	hdr.biPlanes = 1;
	hdr.biBitCount = 32;
	hdr.biCompression = 0; /* BI_RGB */
	hdr.biSizeImage = imageSize;
	hdr.biXPelsPerMeter = 2835;
	hdr.biYPelsPerMeter = 2835;

	fwrite(&hdr, 1, sizeof(hdr), fp);

	for (size_t y = 0; y < h; y++) {
		fwrite(base + y * bpr, 1, rowSize, fp);
	}

	fclose(fp);
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	CFRelease(surf);

	printf("Captured %zux%zu screen to %s (%u bytes)\n", w, h, outpath, (unsigned int)(sizeof(hdr) + imageSize));
	return 0;
}
