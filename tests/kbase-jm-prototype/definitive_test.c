/*
 * Prueba definitiva y reproducible: envia un job WRITE_VALUE usando el
 * struct base_jd_atom_v2 EXACTO segun lo documenta ARM oficialmente en
 * jm/mali_base_jm_kernel.h (GPLv2), sin un solo offset calculado a mano.
 *
 * Dispositivo de referencia: Oppo A38, Mali-G52 MC2, kernel 6.6.118-android15,
 * Kbase UAPI 11.46 (r49p1). Mismo resultado esperado en cualquier dispositivo
 * con UAPI 11.4x+.
 *
 * Resultado esperado si esto NO reproduce el fault: target_after == 0
 * Resultado que estamos documentando: event_code=0x40 (JOB_CONFIG_FAULT),
 * target sin modificar, con TODOS los campos del atom correctos y en cero
 * donde corresponde segun la documentacion oficial.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>

#define PAN_ARCH 7
#include "genxml/v7_pack.h"

/* ---- ioctls kbase (sin cambios respecto a pruebas anteriores) ---- */
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

/* =====================================================================
 * struct base_jd_atom_v2 -- copiado literal de jm/mali_base_jm_kernel.h
 * (ARM Limited, GPLv2), sin modificar ni un solo campo ni su orden.
 * ===================================================================== */
typedef uint8_t  base_atom_id;
typedef uint8_t  base_jd_dep_type;
typedef uint8_t  base_jd_prio;
typedef uint32_t base_jd_core_req;

struct base_jd_udata { uint64_t blob[2]; };
struct base_dependency { base_atom_id atom_id; base_jd_dep_type dependency_type; };

struct base_jd_atom_v2 {
    uint64_t jc;
    struct base_jd_udata udata;
    uint64_t extres_list;
    uint16_t nr_extres;
    uint8_t jit_id[2];
    struct base_dependency pre_dep[2];
    base_atom_id atom_number;
    base_jd_prio prio;
    uint8_t device_nr;
    uint8_t jobslot;
    base_jd_core_req core_req;
    uint8_t renderpass_id;
    uint8_t padding[7];
};

#define BASE_JD_REQ_V ((base_jd_core_req)1 << 4) /* Requires value writeback */

struct base_jd_event_v2 {
    uint32_t event_code;
    base_atom_id atom_number;
    uint8_t padding[3];
    struct base_jd_udata udata;
};

/* Nombres oficiales de eventos relevantes, de mali_base_jm_kernel.h */
#define BASE_JD_EVENT_DONE 0x01
#define BASE_JD_EVENT_JOB_CONFIG_FAULT 0x40

int main(void) {
    /* Verificacion en tiempo de compilacion: si esto no compila, el
       tamano real de tu struct no da 56 bytes en tu toolchain -- avisa
       antes de perder tiempo corriendo la prueba. */
    _Static_assert(sizeof(struct base_jd_atom_v2) == 56,
                   "sizeof(base_jd_atom_v2) debe ser 56 segun ARM oficial");

    printf("sizeof(struct base_jd_atom_v2) = %zu (esperado: 56)\n\n",
           sizeof(struct base_jd_atom_v2));

    int fd = open("/dev/mali0", O_RDWR);
    if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }

    struct kbase_ioctl_version_check ver = {0, 0};
    if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &ver)) { printf("VERSION_CHECK failed\n"); return 1; }
    printf("Kbase UAPI %u.%u\n", ver.major, ver.minor);

    struct kbase_ioctl_set_flags sf = { .create_flags = 0 };
    if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &sf)) { printf("SET_FLAGS failed\n"); return 1; }

    void *tp = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE);
    if (tp == MAP_FAILED) { printf("tracking mmap failed\n"); return 1; }
    printf("handshake ok\n");

    /* Memoria para el job chain (header + payload) */
    union kbase_ioctl_mem_alloc alloc_req = { .in = {
        .va_pages = 1, .commit_pages = 1, .extension = 0,
        .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
                 BASE_MEM_PROT_GPU_WR | BASE_MEM_COHERENT_LOCAL | BASE_MEM_SAME_VA,
    }};
    if (ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &alloc_req)) { printf("MEM_ALLOC failed\n"); return 1; }
    void *cpu_ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc_req.out.gpu_va);
    if (cpu_ptr == MAP_FAILED) { printf("mmap(bo) failed\n"); return 1; }
    uint64_t gpu_va = (uint64_t)(uintptr_t)cpu_ptr;
    uint8_t *buf = (uint8_t *)cpu_ptr;
    uint64_t target_addr = gpu_va + 256;
    uint64_t *target_ptr = (uint64_t *)(buf + 256);
    *target_ptr = 0xCAFECAFECAFECAFEull;
    printf("bo mapped at gpu_va=0x%llx, target before=0x%llx\n",
           (unsigned long long)gpu_va, (unsigned long long)*target_ptr);

    /* Job chain WRITE_VALUE, construido con las macros genxml reales de
       Mesa para PAN_ARCH=7 -- ya verificado byte a byte en sesiones
       anteriores contra las mismas macros. */
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

    /* ---- El atom real, campo por campo, segun la documentacion oficial ---- */
    struct base_jd_atom_v2 atom;
    memset(&atom, 0, sizeof(atom));           /* jit_id, pre_dep, jobslot,
                                                   renderpass_id, padding:
                                                   todos en 0, como exige
                                                   la documentacion cuando
                                                   no se usan */
    atom.jc = gpu_va;                          /* direccion del job chain */
    atom.atom_number = 1;                      /* identificador unico */
    atom.prio = 0;                             /* BASE_JD_PRIO_MEDIUM */
    atom.device_nr = 0;
    atom.jobslot = 0;                          /* no aplica: sin BASE_JD_REQ_JOB_SLOT */
    atom.core_req = BASE_JD_REQ_V;             /* 0x10: unico requerimiento real */
    atom.renderpass_id = 0;                    /* no aplica: sin START/END_RENDERPASS */

    struct kbase_ioctl_job_submit submit = {
        .addr = (uint64_t)(uintptr_t)&atom,
        .nr_atoms = 1,
        .stride = sizeof(struct base_jd_atom_v2), /* 56, calculado por el compilador */
    };

    printf("\nEnviando atom: jc=0x%llx core_req=0x%x stride=%u (todos los demas\n"
           "campos en 0 segun especifica la documentacion oficial de ARM)\n",
           (unsigned long long)atom.jc, atom.core_req, submit.stride);

    if (ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
        printf("JOB_SUBMIT ioctl failed: %s\n", strerror(errno));
        return 1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pret = poll(&pfd, 1, 2000);
    if (pret > 0 && (pfd.revents & POLLIN)) {
        struct base_jd_event_v2 event;
        ssize_t n = read(fd, &event, sizeof(event));
        printf("\nread %zd bytes: event_code=0x%x\n", n, event.event_code);
        if (event.event_code == BASE_JD_EVENT_DONE)
            printf("=> BASE_JD_EVENT_DONE (0x01) -- exito\n");
        else if (event.event_code == BASE_JD_EVENT_JOB_CONFIG_FAULT)
            printf("=> BASE_JD_EVENT_JOB_CONFIG_FAULT (0x40) -- FAULT, incluso\n"
                   "   con el struct oficial completo y todos los campos correctos.\n");
        else
            printf("=> event_code inesperado: 0x%x\n", event.event_code);
    } else {
        printf("\nno event (timeout)\n");
    }

    printf("\ntarget after job: 0x%llx (esperado si hubiera exito: 0x0)\n",
           (unsigned long long)*target_ptr);

    close(fd);
    return 0;
}
