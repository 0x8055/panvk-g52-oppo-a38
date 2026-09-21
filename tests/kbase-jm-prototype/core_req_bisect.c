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

#define KBASE_IOCTL_TYPE 0x80
struct kbase_ioctl_version_check { uint16_t major; uint16_t minor; };
#define KBASE_IOCTL_VERSION_CHECK _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
struct kbase_ioctl_set_flags { uint32_t create_flags; };
#define KBASE_IOCTL_SET_FLAGS _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
struct kbase_ioctl_job_submit { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };
#define KBASE_IOCTL_JOB_SUBMIT _IOW(KBASE_IOCTL_TYPE, 2, struct kbase_ioctl_job_submit)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)

struct base_jd_event_v2 {
    uint32_t event_code;
    uint8_t atom_number;
    uint8_t padding[3];
    uint64_t udata[2];
};

static void try_offset(int fd, uint32_t value, int place_offset) {
    uint8_t atom[64];
    memset(atom, 0, sizeof(atom));
    atom[40] = 1; /* atom_number, posicion ya confirmada */
    if (place_offset >= 0)
        memcpy(atom + place_offset, &value, 4);

    struct kbase_ioctl_job_submit submit = {
        .addr = (uint64_t)(uintptr_t)atom, .nr_atoms = 1, .stride = 64,
    };

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
        printf("offset=%2d -> JOB_SUBMIT ioctl failed: %s\n", place_offset, strerror(errno));
        return;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pret = poll(&pfd, 1, 1000);
    if (pret > 0 && (pfd.revents & POLLIN)) {
        struct base_jd_event_v2 event;
        ssize_t n = read(fd, &event, sizeof(event));
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long usec = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        printf("offset=%2d -> event_code=0x%02x (%ld us)%s\n",
               place_offset, event.event_code, usec,
               event.event_code == 0x1 ? "  [DONE instantaneo -> probablemente ignorado]"
                                        : "  [NO-DONE -> probablemente SI se leyo aqui]");
    } else {
        printf("offset=%2d -> no event (timeout)\n", place_offset);
    }
}

int main(void) {
    int fd = open("/dev/mali0", O_RDWR);
    if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }
    struct kbase_ioctl_version_check ver = {0,0};
    if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver)) { printf("VERSION_CHECK failed\n"); return 1; }
    printf("UAPI %u.%u\n", ver.major, ver.minor);
    struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
    if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf)) { printf("SET_FLAGS failed\n"); return 1; }
    void *tp = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
    if (tp == MAP_FAILED) { printf("tracking mmap failed\n"); return 1; }
    printf("handshake ok\n\n");

    printf("--- Control: core_req=0 real (nada mas puesto) ---\n");
    try_offset(fd, 0, -1);

    printf("\n--- Bisección: 0x10 (BASE_JD_REQ_V) en cada offset candidato, jc=0 a proposito ---\n");
    int candidates[] = {44, 48, 52, 56, 60};
    for (size_t i = 0; i < 5; i++)
        try_offset(fd, 0x10, candidates[i]);

    close(fd);
    return 0;
}
