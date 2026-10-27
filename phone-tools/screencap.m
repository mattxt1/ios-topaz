#import <Foundation/Foundation.h>
#import <mach/mach.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

int main(int argc, char **argv) {
	@autoreleasepool {
		uint32_t target_id = 0;
		const char *outpath = "/tmp/screen.png";

		if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') {
			target_id = (uint32_t)atoi(argv[1]);
			if (argc > 2) outpath = argv[2];
		} else if (argc > 1) {
			outpath = argv[1];
		}

		IOSurfaceRef surf = NULL;
		if (target_id != 0) {
			surf = IOSurfaceLookup(target_id);
		} else {
			/* Automatically pick the display buffer with the most active pixel data */
			size_t max_nonzero = 0;
			uint32_t best_id = 0;
			IOSurfaceRef best_surf = NULL;

			for (uint32_t cid = 1; cid <= 32; cid++) {
				IOSurfaceRef s = IOSurfaceLookup(cid);
				if (s) {
					size_t w = IOSurfaceGetWidth(s);
					size_t h = IOSurfaceGetHeight(s);
					size_t bpr = IOSurfaceGetBytesPerRow(s);
					if ((w == 1125 && h == 2436) || (w == 1080 && h == 2270) || (w == 1080 && h == 2160) || (w == 1080 && h == 1920)) {
						if (IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {
							const uint32_t *p32 = (const uint32_t *)IOSurfaceGetBaseAddress(s);
							size_t nonzero = 0;
							if (p32) {
								for (size_t k = 0; k < (bpr / 4) * h; k += 4) {
									if (p32[k] != 0 && p32[k] != 0xff000000) nonzero++;
								}
							}
							IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
							if (nonzero > max_nonzero) {
								if (best_surf) CFRelease(best_surf);
								max_nonzero = nonzero;
								best_surf = s;
								best_id = cid;
								continue;
							}
						}
					}
					CFRelease(s);
				}
			}
			surf = best_surf ? best_surf : IOSurfaceLookup(1);
			target_id = best_surf ? best_id : 1;
		}

		if (!surf) {
			fprintf(stderr, "No valid surface found\n");
			return 1;
		}

		size_t w = IOSurfaceGetWidth(surf);
		size_t h = IOSurfaceGetHeight(surf);
		size_t bpr = IOSurfaceGetBytesPerRow(surf);

		kern_return_t kr = IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
		if (kr != KERN_SUCCESS) {
			fprintf(stderr, "IOSurfaceLock failed: 0x%x\n", kr);
			CFRelease(surf);
			return 1;
		}

		void *base = IOSurfaceGetBaseAddress(surf);
		if (!base) {
			fprintf(stderr, "IOSurfaceGetBaseAddress returned NULL\n");
			IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			CFRelease(surf);
			return 1;
		}

		/* Check how many non-zero pixels exist */
		const uint32_t *p32 = (const uint32_t *)base;
		size_t nonzero = 0;
		for (size_t i = 0; i < (bpr / 4) * h; i++) {
			if (p32[i] != 0 && p32[i] != 0xff000000) nonzero++;
		}

		CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
		CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, base, bpr * h, NULL);
		CGImageRef image = CGImageCreate(w, h, 8, 32, bpr, cs,
			(CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
			provider, NULL, false, kCGRenderingIntentDefault);
		CGDataProviderRelease(provider);
		CGColorSpaceRelease(cs);

		if (!image) {
			fprintf(stderr, "CGImageCreate failed\n");
			IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			CFRelease(surf);
			return 1;
		}

		CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)outpath, strlen(outpath), false);
		CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (CFStringRef)@"public.png", 1, NULL);
		CFRelease(url);

		if (!dest) {
			fprintf(stderr, "CGImageDestinationCreateWithURL failed\n");
			CGImageRelease(image);
			IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
			CFRelease(surf);
			return 1;
		}

		CGImageDestinationAddImage(dest, image, NULL);
		bool ok = CGImageDestinationFinalize(dest);
		CFRelease(dest);
		CGImageRelease(image);

		IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
		CFRelease(surf);

		if (!ok) {
			fprintf(stderr, "Failed to write PNG to %s\n", outpath);
			return 1;
		}

		printf("Captured surface %u (%zux%zu, non-zero pixels: %zu) to %s\n", target_id, w, h, nonzero, outpath);
		return 0;
	}
}
