#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#define PAN_ARCH 7
#include "genxml/v7_pack.h"

#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_version_check { uint16_t major; uint16_t minor; };
#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
struct kbase_ioctl_set_flags { uint32_t create_flags; };
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
union kbase_ioctl_mem_alloc {
    struct { uint64_t va_pages, commit_pages, extension, flags; } in;
    struct { uint64_t flags, gpu_va; } out;
};
#define KBASE_IOCTL_MEM_ALLOC _IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)
#define BASE_MEM_PROT_CPU_RD ((uint64_t)1 << 0)
#define BASE_MEM_PROT_CPU_WR ((uint64_t)1 << 1)
#define BASE_MEM_PROT_GPU_RD ((uint64_t)1 << 2)
#define BASE_MEM_PROT_GPU_WR ((uint64_t)1 << 3)
#define BASE_MEM_COHERENT_LOCAL ((uint64_t)1 << 11)
#define BASE_MEM_SAME_VA ((uint64_t)1 << 13)

struct base_jd_event_v2 {
    uint32_t event_code;
    uint8_t atom_number;
    uint8_t padding[3];
    uint64_t udata[2];
};

static int fd;
static uint64_t gpu_va;
static uint8_t *buf;
static uint64_t target_addr;
static uint64_t *target_ptr;

static void setup_job(void) {
    /* reescribe header+payload como job WRITE_VALUE valido, identico al que
       ya confirmamos correcto byte a byte en una prueba anterior */
    struct MALI_JOB_HEADER header = {0};
    header.type = MALI_JOB_TYPE_WRITE_VALUE;
    header.index = 1;
    header.invalidate_cache = true;
    struct mali_job_header_packed packed_header;
    MALI_JOB_HEADER_pack(&packed_header, &header);
    memcpy(buf + 0, &packed_header, sizeof(packed_header));

    struct MALI_WRITE_VALUE_JOB_PAYLOAD payload = {0};
    payload.address = target_addr;
    payload.type = MALI_WRITE_VALUE_TYPE_ZERO;
    payload.immediate_value = 0x1234567890ABCDEFull;
    struct mali_write_value_job_payload_packed packed_payload;
    MALI_WRITE_VALUE_JOB_PAYLOAD_pack(&packed_payload, &payload);
    memcpy(buf + 32, &packed_payload, sizeof(packed_payload));
}

static void try_tail(int word, uint32_t value) {
    *target_ptr = 0xCAFECAFECAFECAFEull; /* reset marcador antes de cada intento */
    setup_job();

    uint8_t atom_bytes[64];
    memset(atom_bytes, 0, sizeof(atom_bytes));
    memcpy(atom_bytes + 0, &gpu_va, 8);
    atom_bytes[40] = 1;
    uint32_t core_req_val = 0x10;
    memcpy(atom_bytes + 44, &core_req_val, 4);
    int off = 48 + word * 4;
    memcpy(atom_bytes + off, &value, 4);

    struct kbase_ioctl_job_submit submit = {
        .addr = (uint64_t)(uintptr_t)atom_bytes, .nr_atoms = 1, .stride = 64,
    };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
        printf("word@%d = 0x%08x -> JOB_SUBMIT failed: %s\n", off, value, strerror(errno));
        return;
    }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pret = poll(&pfd, 1, 1500);
    if (pret > 0 && (pfd.revents & POLLIN)) {
        struct base_jd_event_v2 event;
        ssize_t n = read(fd, &event, sizeof(event));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long usec = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        printf("word@%d = 0x%08x -> event_code=0x%x (%ld us) target=0x%llx\n",
               off, value, event.event_code, usec, (unsigned long long)*target_ptr);
    } else {
        printf("word@%d = 0x%08x -> no event (timeout), target=0x%llx\n",
               off, value, (unsigned long long)*target_ptr);
    }
}

int main(void) {
    fd = open("/dev/mali0", O_RDWR);
    if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }
    struct kbase_ioctl_version_check ver = {0,0};
    if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver)) { printf("VERSION_CHECK failed\n"); return 1; }
    struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
    if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf)) { printf("SET_FLAGS failed\n"); return 1; }
    void *tp = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
    if (tp == MAP_FAILED) { printf("tracking mmap failed\n"); return 1; }

    union kbase_ioctl_mem_alloc alloc_req = { .in = {
        .va_pages = 1, .commit_pages = 1, .extension = 0,
        .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
                 BASE_MEM_PROT_GPU_WR | BASE_MEM_COHERENT_LOCAL | BASE_MEM_SAME_VA,
    }};
    if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc_req)) { printf("MEM_ALLOC failed\n"); return 1; }
    void *cpu_ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc_req.out.gpu_va);
    if (cpu_ptr == MAP_FAILED) { printf("mmap(bo) failed\n"); return 1; }
    gpu_va = (uint64_t)(uintptr_t)cpu_ptr;
    buf = (uint8_t *)cpu_ptr;
    target_addr = gpu_va + 256;
    target_ptr = (uint64_t *)(buf + 256);
    printf("setup ok, bo mapped at gpu_va=0x%llx\n\n", (unsigned long long)gpu_va);

    uint32_t candidates[] = {0x00000000, 0xFFFFFFFF};
    for (int word = 0; word < 4; word++)
        for (int c = 0; c < 2; c++)
            try_tail(word, candidates[c]);

    close(fd);
    return 0;
}
