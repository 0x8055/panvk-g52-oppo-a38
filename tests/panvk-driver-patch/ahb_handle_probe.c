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
    if (!al || !rl || !gh) { printf("dlsym fallo (gh=%p)\n", (void*)gh); return 1; }

    uint64_t gpu = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    uint64_t u[3] = { gpu | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY,
                      gpu | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | NO_AFBC,
                      gpu };
    const char *nm[3] = { "A baseline", "B composer+NO_AFBC", "C solo GPU" };
    uint32_t sz[2] = { 64, 500 };

    for (int s = 0; s < 2; s++) for (int v = 0; v < 3; v++) {
        AHardwareBuffer_Desc d = {0};
        d.width = d.height = sz[s]; d.layers = 1;
        d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM; d.usage = u[v];
        AHardwareBuffer *b = NULL;
        int r = al(&d, &b);
        printf("\n[%ux%u] %s -> alloc=%d\n", sz[s], sz[s], nm[v], r);
        if (r || !b) continue;
        const struct nh *h = gh(b);
        printf("  fds=%d ints=%d\n  ", h->numFds, h->numInts);
        for (int i = 0; i < h->numFds; i++)
            printf("fd%d size=%lld ", i, (long long)(h->data[i] >= 0 ? lseek(h->data[i], 0, SEEK_END) : -1));
        printf("\n  ints:");
        for (int i = 0; i < h->numInts; i++) printf(" %x", (unsigned)h->data[h->numFds + i]);
        printf("\n");
        rl(b);
    }
    return 0;
}
