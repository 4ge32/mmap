/*
 * test_ldso_replay.c - Model-level reproduction of the ld.so mapping
 * sequence for a shared object, asserting the well-formedness invariant and
 * targeted structural facts after each loader step.
 *
 * The synthetic object (see docs/ldso_sequence.md) has two PT_LOAD segments:
 *   text: vaddr 0,    offset 0,    filesz 2P, memsz 2P, prot R|X
 *   data: vaddr 3P,   offset 3P,   filesz 2P, memsz 4P, prot R|W  (2P of bss)
 * total image span = 7 pages. A one-page alignment gap [2P,3P) stays at the
 * PROT_NONE reservation protection.
 */
#include "test_harness.h"
#include "mm_api.h"

#define PG       PAGE_SIZE
#define FD_OBJ   11
#define TOTAL    (7 * PG)

static int prot_at(const struct addr_space *as, uint64_t a)
{
    for (size_t k = 0; k < as->count; k++)
        if (as->vmas[k].start <= a && a < as->vmas[k].end)
            return as->vmas[k].prot;
    return -1;
}

static enum vma_backing backing_at(const struct addr_space *as, uint64_t a)
{
    for (size_t k = 0; k < as->count; k++)
        if (as->vmas[k].start <= a && a < as->vmas[k].end)
            return as->vmas[k].backing;
    return VMA_ANON;
}

static void test_ldso_full_sequence(void)
{
    struct addr_space as; as_init(&as);
    uint64_t base, out;

    /* Step 1: reserve the whole image span PROT_NONE (private anon). */
    ASSERT_STATUS(mm_mmap(&as, 0, TOTAL, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &base), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_INT(prot_at(&as, base), PROT_NONE);

    /* Step 2: map the text segment R|X, file-backed, MAP_FIXED. */
    ASSERT_STATUS(mm_mmap(&as, base + 0, 2 * PG, PROT_READ | PROT_EXEC,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_INT(prot_at(&as, base), PROT_READ | PROT_EXEC);
    ASSERT_EQ_INT(backing_at(&as, base), VMA_FILE);

    /* Step 3: map the data segment's file part R|W, file-backed, MAP_FIXED.
     * Leaves the alignment gap [base+2P, base+3P) at PROT_NONE. */
    ASSERT_STATUS(mm_mmap(&as, base + 3 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ, 3 * PG, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_INT(prot_at(&as, base + 2 * PG), PROT_NONE);       /* gap intact */
    ASSERT_EQ_INT(prot_at(&as, base + 3 * PG), PROT_READ | PROT_WRITE);

    /* Step 4: bss tail as anonymous R|W pages, MAP_FIXED over the reservation. */
    ASSERT_STATUS(mm_mmap(&as, base + 5 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_INT(prot_at(&as, base + 5 * PG), PROT_READ | PROT_WRITE);
    ASSERT_EQ_INT(backing_at(&as, base + 5 * PG), VMA_ANON);

    /* Step 5: GNU_RELRO -> mprotect the first data page to read-only. */
    ASSERT_STATUS(mm_mprotect(&as, base + 3 * PG, PG, PROT_READ), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_INT(prot_at(&as, base + 3 * PG), PROT_READ);
    ASSERT_EQ_INT(prot_at(&as, base + 4 * PG), PROT_READ | PROT_WRITE);

    /*
     * Final canonical layout (5 VMAs):
     *  [base+0, +2P)  R|X  file@0
     *  [base+2P,+3P)  NONE anon       (reservation alignment gap)
     *  [base+3P,+4P)  R    file@3P    (RELRO)
     *  [base+4P,+5P)  R|W  file@4P
     *  [base+5P,+7P)  R|W  anon       (bss)
     */
    ASSERT_EQ_U64(as.count, 5);
    ASSERT_EQ_U64(as.vmas[0].end - as.vmas[0].start, 2 * PG);
    ASSERT_EQ_INT(as.vmas[1].prot, PROT_NONE);
    ASSERT_EQ_U64(as.vmas[4].end, base + TOTAL);
}

int main(void)
{
    TEST_SUITE("ld.so mapping-sequence replay");
    RUN_TEST(test_ldso_full_sequence,
             "Full ld.so mapping sequence preserves as_wf at each step");
    return TEST_SUMMARY();
}
