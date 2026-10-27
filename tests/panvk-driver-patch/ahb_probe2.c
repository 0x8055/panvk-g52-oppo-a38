#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <android/hardware_buffer.h>
struct nh { int version, numFds, numInts; int data[]; };
typedef int (*alloc_fn)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*rel_fn)(AHardwareBuffer*);
typedef const struct nh *(*gnh_fn)(const AHardwareBuffer*);
#define NO_AFBC (1ULL << 29)
int main(void) {
  void *l = dlopen("libnativewindow.so", RTLD_NOW);
  alloc_fn al = dlsym(l, "AHardwareBuffer_allocate");
  rel_fn rl = dlsym(l, "AHardwareBuffer_release");
  gnh_fn gh = dlsym(l, "AHardwareBuffer_getNativeHandle");
  if (!al || !rl || !gh) { puts("dlsym fallo"); return 1; }
  uint64_t S = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, F = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
  uint64_t cr = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, cw = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
  uint64_t co = AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
  struct { const char *n; uint64_t u; } c[] = {
    {"S", S}, {"F", F}, {"S|F", S|F}, {"S|F|cr", S|F|cr}, {"S|F|cw", S|F|cw},
    {"S|F|comp", S|F|co}, {"S|F|NOAFBC", S|F|NO_AFBC}, {"S|F|comp|NOAFBC", S|F|co|NO_AFBC},
    {"S|F|bit57", S|F|(1ULL << 57)}, {"S|F|bit57|bit60", S|F|(1ULL << 57)|(1ULL << 60)} };
  int fm[] = { AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM };
  for (int f = 0; f < 2; f++) for (int i = 0; i < 10; i++) {
    AHardwareBuffer_Desc d = {0};
    d.width = 200; d.height = 100; d.layers = 1; d.format = fm[f]; d.usage = c[i].u;
    AHardwareBuffer *b = NULL;
    int r = al(&d, &b);
    if (r || !b) { printf("fmt%d %-16s alloc=%d\n", fm[f], c[i].n, r); continue; }
    const struct nh *h = gh(b);
    const int *x = h->data + h->numFds;
    long long sz = -1;
    for (int k = 0; k < h->numFds; k++)
      if (h->data[k] >= 0) { long long s = lseek(h->data[k], 0, SEEK_END); if (s > sz) sz = s; }
    printf("fmt%d %-16s buf=%lld alloc_fmt(hi:lo)=%x:%x plane={stride %x w %x h %x}\n",
           fm[f], c[i].n, sz, x[30], x[29], x[34], x[35], x[36]);
    rl(b);
  }
  return 0;
}
