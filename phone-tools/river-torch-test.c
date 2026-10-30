/* Explicitly request the experimental one-shot kernel test; no normal torch UI. */
#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
 if (argc != 2 || strcmp(argv[1], "--pulse") != 0) {
  fprintf(stderr, "usage: river-torch-test --pulse\n");
  return 2;
 }
 int request = 1;
 if (sysctlbyname("debug.river_torch_test", NULL, NULL, &request, sizeof(request))) {
  perror("torch request"); return 1;
 }
 for (unsigned i = 0; i < 100; i++) {
  int status = 0; size_t n = sizeof(status);
  if (sysctlbyname("debug.river_torch_test", &status, &n, NULL, 0)) {
   perror("torch status"); return 1;
  }
  if (status != 2) {
   printf("torch test status=%d\n", status);
   return status == 1 ? 0 : 1;
  }
  usleep(50000);
 }
 fprintf(stderr, "torch test still pending; do not assume the LED is off\n");
 return 1;
}
