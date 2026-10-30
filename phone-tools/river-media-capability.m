#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
int main(int argc, char **argv) {
 @autoreleasepool {
  if (argc < 2) return 2;
  if (!strcmp(argv[1], "video")) {
   fprintf(stderr,"begin video discovery\n");
   AVCaptureDeviceDiscoverySession *s = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera] mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
   fprintf(stderr,"video_devices=%lu\n", (unsigned long)s.devices.count);
   for (AVCaptureDevice *d in s.devices) fprintf(stderr,"position=%ld torch=%d flash=%d\n", (long)d.position, d.hasTorch, d.hasFlash);
  } else if (!strcmp(argv[1], "audio") || !strcmp(argv[1], "record-route")) {
   fprintf(stderr,"begin audio session\n");
   AVAudioSession *a = [AVAudioSession sharedInstance];
   fprintf(stderr,"audio session obtained\n");
   if (!strcmp(argv[1], "record-route")) {
    NSError *error = nil;
    BOOL ok = [a setCategory:AVAudioSessionCategoryPlayAndRecord error:&error];
    fprintf(stderr,"setCategory=%d error=%s\n", ok, error ? error.localizedDescription.UTF8String : "none");
   }
   NSArray *inputs = a.availableInputs;
   fprintf(stderr,"audio_available_inputs=%s count=%lu inputAvailable=%d\n", inputs ? "present" : "nil", (unsigned long)inputs.count, a.inputAvailable);
  }
 }
 return 0;
}
