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
#define FD_OBJ1  11
#define FD_OBJ2  12
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

/*
 * M3 Scenario A — alignment-overshoot reservation + slack trim.
 * See docs/ldso_sequence.md "Scenario A". A PT_LOAD requests p_align = 4P,
 * larger than the page size, so the loader reserves span + p_align - PAGE and
 * trims the head/tail slack to land the kept region on a 4P boundary.
 *
 * Page P, align = 4P, image span = 7P, reservation len = 10P. mm_mmap's
 * non-FIXED placement is first-fit (not a kernel-chosen base), so to make the
 * arithmetic deterministic we reserve with MAP_FIXED at a chosen raw base that
 * stands in for the kernel's pick. To actually exercise BOTH the head and tail
 * trims we pick raw_base = 0x10000000 + 2P, so it is NOT 4P-aligned and
 * aligned_base = round_up(raw_base, 4P) = raw_base + 2P.
 */
static void test_ldso_align_overshoot(void)
{
    struct addr_space as; as_init(&as);
    uint64_t out;

    const uint64_t raw_base     = 0x10000000ULL + 2 * PG;     /* kernel pick */
    const uint64_t aligned_base = raw_base + 2 * PG;          /* round_up 4P  */
    const uint64_t span         = 7 * PG;
    const uint64_t resv_len     = 10 * PG;                    /* span+4P-P    */

    /* raw_base is unaligned to 4P; aligned_base is 4P-aligned. */
    ASSERT_EQ_U64(aligned_base % (4 * PG), 0);
    ASSERT_TRUE(raw_base % (4 * PG) != 0);

    /* Step 1: overshoot reservation of span + p_align - PAGE, PROT_NONE. */
    ASSERT_STATUS(mm_mmap(&as, raw_base, resv_len, PROT_NONE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_INT(prot_at(&as, raw_base), PROT_NONE);

    /* Step 2: head trim — munmap [raw_base, aligned_base), length 2P. */
    ASSERT_STATUS(mm_munmap(&as, raw_base, 2 * PG), MM_OK);
    ASSERT_WF(as);

    /* Step 3: tail trim — munmap [aligned_base+span, raw_base+resv_len),
     * which is [raw_base+9P, raw_base+10P), i.e. munmap(raw_base+9P, 1P). */
    ASSERT_STATUS(mm_munmap(&as, aligned_base + span, 1 * PG), MM_OK);
    ASSERT_WF(as);
    /* The kept reservation is exactly [aligned_base, aligned_base+7P). */
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, aligned_base);
    ASSERT_EQ_U64(as.vmas[0].end, aligned_base + span);
    ASSERT_EQ_INT(prot_at(&as, aligned_base), PROT_NONE);

    /* Steps 4-7: the baseline per-PT_LOAD overlays + RELRO, but addressed
     * relative to aligned_base instead of the raw reservation base. */

    /* Step 4: text segment R|X, file-backed, MAP_FIXED. */
    ASSERT_STATUS(mm_mmap(&as, aligned_base + 0, 2 * PG, PROT_READ | PROT_EXEC,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ1, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* Step 5: data file part R|W, file-backed, MAP_FIXED (gap [+2P,+3P) stays NONE). */
    ASSERT_STATUS(mm_mmap(&as, aligned_base + 3 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ1, 3 * PG, &out), MM_OK);
    ASSERT_WF(as);

    /* Step 6: bss tail as anonymous R|W pages, MAP_FIXED. */
    ASSERT_STATUS(mm_mmap(&as, aligned_base + 5 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* Step 7: GNU_RELRO -> mprotect the first data page read-only. */
    ASSERT_STATUS(mm_mprotect(&as, aligned_base + 3 * PG, PG, PROT_READ), MM_OK);
    ASSERT_WF(as);

    /*
     * Final canonical layout (5 VMAs), all within [aligned_base, +7P):
     *  [+0,  +2P)  R|X  file@0    (text)
     *  [+2P, +3P)  NONE anon      (reservation alignment gap)
     *  [+3P, +4P)  R    file@3P   (RELRO)
     *  [+4P, +5P)  R|W  file@4P   (data)
     *  [+5P, +7P)  R|W  anon      (bss)
     */
    ASSERT_EQ_U64(as.count, 5);
    ASSERT_EQ_U64(as.vmas[0].start, aligned_base);
    ASSERT_EQ_U64(as.vmas[4].end, aligned_base + span);

    ASSERT_EQ_INT(prot_at(&as, aligned_base + 0 * PG), PROT_READ | PROT_EXEC);
    ASSERT_EQ_INT(backing_at(&as, aligned_base + 0 * PG), VMA_FILE);
    ASSERT_EQ_INT(prot_at(&as, aligned_base + 2 * PG), PROT_NONE);           /* gap */
    ASSERT_EQ_INT(prot_at(&as, aligned_base + 3 * PG), PROT_READ);           /* RELRO */
    ASSERT_EQ_INT(backing_at(&as, aligned_base + 3 * PG), VMA_FILE);
    ASSERT_EQ_INT(prot_at(&as, aligned_base + 4 * PG), PROT_READ | PROT_WRITE);
    ASSERT_EQ_INT(backing_at(&as, aligned_base + 4 * PG), VMA_FILE);
    ASSERT_EQ_INT(prot_at(&as, aligned_base + 5 * PG), PROT_READ | PROT_WRITE);
    ASSERT_EQ_INT(backing_at(&as, aligned_base + 5 * PG), VMA_ANON);

    /* The kept region is 4P-aligned, just as p_align demanded. */
    ASSERT_EQ_U64(aligned_base % (4 * PG), 0);
}

/* Locate the VMA index whose range contains address `a`, or -1. */
static int vma_index_at(const struct addr_space *as, uint64_t a)
{
    for (size_t k = 0; k < as->count; k++)
        if (as->vmas[k].start <= a && a < as->vmas[k].end)
            return (int)k;
    return -1;
}

/* Load object1 = the baseline synthetic object at a FIXED base b1, giving the
 * canonical 5-VMA layout, all sharing one map_id. */
static void load_object1_at(struct addr_space *as, uint64_t b1)
{
    uint64_t out;

    ASSERT_STATUS(mm_mmap(as, b1, TOTAL, PROT_NONE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(*as);
    ASSERT_STATUS(mm_mmap(as, b1 + 0, 2 * PG, PROT_READ | PROT_EXEC,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ1, 0, &out), MM_OK);
    ASSERT_WF(*as);
    ASSERT_STATUS(mm_mmap(as, b1 + 3 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ1, 3 * PG, &out), MM_OK);
    ASSERT_WF(*as);
    ASSERT_STATUS(mm_mmap(as, b1 + 5 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(*as);
    ASSERT_STATUS(mm_mprotect(as, b1 + 3 * PG, PG, PROT_READ), MM_OK);
    ASSERT_WF(*as);
    ASSERT_EQ_U64(as->count, 5);
}

/*
 * M3 Scenario B — multi-object load with a per-object map_id boundary.
 * See docs/ldso_sequence.md "Scenario B". Object1 is the baseline object;
 * object2 is loaded adjacent above it with its own fresh map_id. The two
 * objects abut at b1+7P but never coalesce: per-object identity (map_id)
 * keeps the boundary even were the regions otherwise compatible.
 */
static void test_ldso_multi_object(void)
{
    struct addr_space as; as_init(&as);
    uint64_t out;

    const uint64_t b1 = 0x20000000ULL;
    const uint64_t b2 = b1 + 7 * PG;             /* object2 reservation base */

    /* Object1: baseline five-step load -> 5 VMAs. In this model each MAP_FIXED
     * overlay stamps a fresh map_id (the reservation, text, data and bss are
     * distinct mappings; the RELRO mprotect splits the data mapping but both
     * halves keep the data overlay's id). So object1 owns a contiguous block of
     * map_ids strictly below next_map_id; we capture that high-water mark to
     * distinguish object2's ids below. */
    load_object1_at(&as, b1);
    const uint32_t obj1_id_max = as.next_map_id;   /* every obj1 id < this */

    /* Snapshot object1's 5 VMAs to confirm they are untouched by object2. */
    struct vma obj1[5];
    for (size_t k = 0; k < 5; k++)
        obj1[k] = as.vmas[k];

    /* Object2 reservation (own fresh map_id), placed adjacent above object1. */
    ASSERT_STATUS(mm_mmap(&as, b2, 3 * PG, PROT_NONE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* Object2 text R|X file-backed. */
    ASSERT_STATUS(mm_mmap(&as, b2 + 0, 2 * PG, PROT_READ | PROT_EXEC,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ2, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* Object2 data R|W file-backed. */
    ASSERT_STATUS(mm_mmap(&as, b2 + 2 * PG, 1 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, FD_OBJ2, 2 * PG, &out), MM_OK);
    ASSERT_WF(as);

    /* Final layout: object1's 5 VMAs + object2's 2 VMAs = 7 total. */
    ASSERT_EQ_U64(as.count, 7);

    /* Object1's 5 VMAs are unchanged (start/end/prot/backing/map_id), and each
     * still carries an id below the pre-object2 high-water mark. */
    for (size_t k = 0; k < 5; k++) {
        ASSERT_EQ_U64(as.vmas[k].start, obj1[k].start);
        ASSERT_EQ_U64(as.vmas[k].end, obj1[k].end);
        ASSERT_EQ_INT(as.vmas[k].prot, obj1[k].prot);
        ASSERT_EQ_INT(as.vmas[k].backing, obj1[k].backing);
        ASSERT_EQ_U64(as.vmas[k].map_id, obj1[k].map_id);
        ASSERT_TRUE(as.vmas[k].map_id < obj1_id_max);
    }

    /* The VMA at b2 belongs to object2: its map_id is freshly stamped (>= the
     * object1 high-water mark), hence distinct from every object1 mapping. */
    int ib2 = vma_index_at(&as, b2);
    ASSERT_TRUE(ib2 >= 0);
    const uint32_t m2 = as.vmas[ib2].map_id;
    ASSERT_TRUE(m2 >= obj1_id_max);

    /* The boundary at b1+7P (== b2) is preserved: two distinct VMAs meet there,
     * object1's bss ending at b2 and object2's text starting at b2, with
     * different map_ids. */
    int ibelow = vma_index_at(&as, b2 - 1);   /* object1 bss */
    int iabove = vma_index_at(&as, b2);        /* object2 text */
    ASSERT_TRUE(ibelow >= 0 && iabove >= 0);
    ASSERT_EQ_U64(as.vmas[ibelow].end, b2);
    ASSERT_EQ_U64(as.vmas[iabove].start, b2);
    ASSERT_TRUE(as.vmas[ibelow].map_id < obj1_id_max);   /* object1 */
    ASSERT_EQ_U64(as.vmas[iabove].map_id, m2);           /* object2 */
    ASSERT_TRUE(as.vmas[ibelow].map_id != as.vmas[iabove].map_id);

    /*
     * map_id-in-isolation stress: in a SEPARATE address space, create two
     * adjacent, IDENTICAL-prot/flags/backing anon mappings from two distinct
     * mm_mmap calls (hence distinct map_id). They must NOT coalesce purely
     * because of differing map_id — canonicalization leaves two VMAs.
     */
    struct addr_space st; as_init(&st);
    const uint64_t X = 0x30000000ULL;
    ASSERT_STATUS(mm_mmap(&st, X, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(st);
    ASSERT_STATUS(mm_mmap(&st, X + 2 * PG, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(st);
    /* Adjacent + identical in prot/flags/backing, but different map_id: stay split. */
    ASSERT_EQ_U64(st.count, 2);
    ASSERT_TRUE(st.vmas[0].map_id != st.vmas[1].map_id);
    ASSERT_EQ_U64(st.vmas[0].end, st.vmas[1].start);
    ASSERT_WF(st);
}

int main(void)
{
    TEST_SUITE("ld.so mapping-sequence replay");
    RUN_TEST(test_ldso_full_sequence,
             "Full ld.so mapping sequence preserves as_wf at each step");
    RUN_TEST(test_ldso_align_overshoot,
             "Alignment-overshoot reservation trims slack to a 4P-aligned base");
    RUN_TEST(test_ldso_multi_object,
             "Multi-object load keeps a per-object map_id boundary (no coalesce)");
    return TEST_SUMMARY();
}
