/*
 * vma_dump.c - Human-readable dump of an address space. NOT part of the
 * provable core; never fed to Frama-C (uses stdio). Linked into tools/tests
 * for debugging only.
 */
#include <stdio.h>
#include <inttypes.h>
#include "mm_api.h"

static void prot_str(int prot, char out[4])
{
    out[0] = (prot & PROT_READ)  ? 'r' : '-';
    out[1] = (prot & PROT_WRITE) ? 'w' : '-';
    out[2] = (prot & PROT_EXEC)  ? 'x' : '-';
    out[3] = '\0';
}

void as_dump(const struct addr_space *as, FILE *f);
void as_dump(const struct addr_space *as, FILE *f)
{
    fprintf(f, "address_space: %zu VMA(s) [%#" PRIx64 ", %#" PRIx64 ")\n",
            as->count, as->as_min, as->as_max);
    for (size_t k = 0; k < as->count; k++) {
        const struct vma *v = &as->vmas[k];
        char p[4];
        prot_str(v->prot, p);
        if (v->backing == VMA_FILE)
            fprintf(f, "  [%#" PRIx64 ", %#" PRIx64 ") %s file fd=%d off=%#" PRIx64 " id=%u\n",
                    v->start, v->end, p, v->fd, v->file_offset, v->map_id);
        else
            fprintf(f, "  [%#" PRIx64 ", %#" PRIx64 ") %s anon id=%u\n",
                    v->start, v->end, p, v->map_id);
    }
}
