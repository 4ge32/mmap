/*
 * mm_types.h - Core data types for the mmap/ld.so logical simulator.
 *
 * This is the PROVABLE CORE surface: no dynamic allocation, no recursion.
 * The entire virtual address space is modeled as a fixed-capacity, sorted,
 * non-overlapping, maximally-merged (canonical) array of VMAs.
 */
#ifndef MM_TYPES_H
#define MM_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* Page geometry. Fixed at compile time (Linux x86-64 base page). */
#define PAGE_SIZE  ((uint64_t)4096)
#define PAGE_MASK  (PAGE_SIZE - 1u)

/* Maximum number of VMAs the model can hold. ld.so needs ~6-10 per object;
 * 256 is generous and keeps WP array reasoning small. */
#define VMA_CAP 256u

/* Protection bits (mirror of <sys/mman.h> PROT_*). */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define PROT_ALL   (PROT_READ | PROT_WRITE | PROT_EXEC)

/* Mapping flags subset (mirror of MAP_*). MAP_FIXED and MAP_FIXED_NOREPLACE are
 * operation modifiers, not stored properties. */
#define MAP_PRIVATE   0x01
#define MAP_ANONYMOUS 0x02
#define MAP_FIXED     0x04
#define MAP_FIXED_NOREPLACE 0x08
/* Persistent flags retained on a stored VMA. */
#define MAP_PERSIST_MASK (MAP_PRIVATE | MAP_ANONYMOUS)
#define MAP_ALL          (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | \
                          MAP_FIXED_NOREPLACE)

/* Address-space bounds: x86-64 canonical lower-half user space. */
#define AS_MIN ((uint64_t)0)
#define AS_MAX ((uint64_t)0x0000800000000000ULL)

/* Operation status codes (mirror of errno values relevant to mmap). */
typedef enum {
    MM_OK = 0,
    MM_EINVAL,  /* invalid argument (alignment, length, prot, flags) */
    MM_ENOMEM,  /* out of model capacity, or range covers an unmapped gap */
    MM_EEXIST,  /* MAP_FIXED_NOREPLACE-style collision (reserved, unused yet) */
    MM_EACCES   /* permission mismatch (reserved, unused yet) */
} mm_status;

/* Backing kind of a mapping. */
enum vma_backing {
    VMA_ANON = 0,
    VMA_FILE = 1
};

/*
 * A single virtual memory area, half-open [start, end).
 * start/end are page-aligned; end is exclusive.
 */
struct vma {
    uint64_t start;        /* page-aligned, inclusive */
    uint64_t end;          /* page-aligned, exclusive; start < end */
    int      prot;         /* bitmask within PROT_ALL (PROT_NONE == 0) */
    int      flags;        /* persistent flags within MAP_PERSIST_MASK */
    enum vma_backing backing;
    int      fd;           /* meaningful iff backing == VMA_FILE, else -1 */
    uint64_t file_offset;  /* page-aligned; meaningful iff VMA_FILE */
    uint32_t map_id;       /* logical mapping group (ld.so replay aid) */
};

/*
 * The whole address space. The vmas[] array IS the static arena: there is
 * no malloc anywhere in the core. Entries [0, count) are valid and kept in
 * canonical form.
 */
struct addr_space {
    struct vma vmas[VMA_CAP];
    size_t     count;
    uint64_t   as_min;
    uint64_t   as_max;
    uint32_t   next_map_id; /* monotonic id source for new mappings */
};

#endif /* MM_TYPES_H */
