/*
 * test_mmap_ops.c - Behavior tests for mm_mmap / mm_mprotect / mm_munmap,
 * asserting the well-formedness invariant after every operation.
 */
#include "test_harness.h"
#include "mm_api.h"

#define PG PAGE_SIZE

/* Count VMAs covering address `a`, returning its prot (or -1 if unmapped). */
static int prot_at(const struct addr_space *as, uint64_t a)
{
    for (size_t k = 0; k < as->count; k++)
        if (as->vmas[k].start <= a && a < as->vmas[k].end)
            return as->vmas[k].prot;
    return -1;
}

static void test_invalid_args(void)
{
    struct addr_space as; as_init(&as);
    uint64_t out;
    ASSERT_STATUS(mm_mmap(&as, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_EINVAL);
    ASSERT_STATUS(mm_mmap(&as, 0, PG, 0x100, MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_EINVAL);
    /* MAP_FIXED with unaligned addr */
    ASSERT_STATUS(mm_mmap(&as, 1, PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_EINVAL);
    ASSERT_WF(as);
}

static void test_anon_place_and_merge(void)
{
    struct addr_space as; as_init(&as);
    uint64_t a, b;
    ASSERT_STATUS(mm_mmap(&as, 0, 2 * PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &a), MM_OK);
    ASSERT_WF(as);
    /* A second adjacent identical mapping placed right below is a DISTINCT
     * logical mapping (its own map_id), so it stays separate even though it is
     * otherwise compatible — faithful to per-object boundaries. */
    ASSERT_STATUS(mm_mmap(&as, a - 2 * PG, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &b), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(b, a - 2 * PG);
    /* two adjacent-but-distinct VMAs, not coalesced */
    ASSERT_EQ_U64(as.count, 2);
    ASSERT_EQ_U64(as.vmas[0].start, a - 2 * PG);
    ASSERT_EQ_U64(as.vmas[0].end, a);
    ASSERT_EQ_U64(as.vmas[1].start, a);
    ASSERT_EQ_U64(as.vmas[1].end, a + 2 * PG);
    /* the two halves of a SINGLE mapping, however, do re-merge: split one and
     * confirm canonicalization collapses it back via the shared map_id. */
    struct addr_space one; as_init(&one);
    uint64_t c;
    ASSERT_STATUS(mm_mmap(&one, 0x500000000ULL, 4 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &c), MM_OK);
    ASSERT_EQ_U64(one.count, 1);
    /* mprotect the middle to a different prot then back: the split halves share
     * one map_id and re-merge to a single VMA. */
    ASSERT_STATUS(mm_mprotect(&one, c + PG, 2 * PG, PROT_READ | PROT_WRITE), MM_OK);
    ASSERT_WF(one);
    ASSERT_EQ_U64(one.count, 3);
    ASSERT_STATUS(mm_mprotect(&one, c + PG, 2 * PG, PROT_READ), MM_OK);
    ASSERT_WF(one);
    ASSERT_EQ_U64(one.count, 1);
}

static void test_fixed_overlay_split(void)
{
    struct addr_space as; as_init(&as);
    uint64_t base = 0x100000000ULL; /* aligned */
    uint64_t out;
    /* reserve [base, base+8PG) PROT_NONE */
    ASSERT_STATUS(mm_mmap(&as, base, 8 * PG, PROT_NONE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_EQ_U64(as.count, 1);

    /* overlay a R+X chunk in the middle [base+2PG, base+4PG) */
    ASSERT_STATUS(mm_mmap(&as, base + 2 * PG, 2 * PG, PROT_READ | PROT_EXEC,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, 9, 0, &out), MM_OK);
    ASSERT_WF(as);
    /* now: [base,+2PG) NONE | [+2PG,+4PG) R+X file | [+4PG,+8PG) NONE */
    ASSERT_EQ_U64(as.count, 3);
    ASSERT_EQ_INT(prot_at(&as, base), PROT_NONE);
    ASSERT_EQ_INT(prot_at(&as, base + 2 * PG), PROT_READ | PROT_EXEC);
    ASSERT_EQ_INT(prot_at(&as, base + 4 * PG), PROT_NONE);
}

static void test_mprotect_split_and_gap(void)
{
    struct addr_space as; as_init(&as);
    uint64_t base = 0x200000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, base, 4 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    /* mprotect a sub-range to PROT_READ -> split into 3 (or 2 at edge) */
    ASSERT_STATUS(mm_mprotect(&as, base + PG, PG, PROT_READ), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_INT(prot_at(&as, base), PROT_READ | PROT_WRITE);
    ASSERT_EQ_INT(prot_at(&as, base + PG), PROT_READ);
    ASSERT_EQ_INT(prot_at(&as, base + 2 * PG), PROT_READ | PROT_WRITE);

    /* mprotect across an unmapped gap -> ENOMEM */
    ASSERT_STATUS(mm_mprotect(&as, base + 100 * PG, PG, PROT_READ), MM_ENOMEM);
    ASSERT_WF(as);
}

static void test_munmap_hole_and_remerge(void)
{
    struct addr_space as; as_init(&as);
    uint64_t base = 0x300000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, base, 6 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    /* punch a hole in the middle */
    ASSERT_STATUS(mm_munmap(&as, base + 2 * PG, 2 * PG), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 2);
    ASSERT_EQ_INT(prot_at(&as, base + 3 * PG), -1); /* unmapped */

    /* unmapping an already-empty range is tolerated */
    ASSERT_STATUS(mm_munmap(&as, base + 2 * PG, 2 * PG), MM_OK);
    ASSERT_WF(as);
}

static void test_nonfixed_hint(void)
{
    struct addr_space as; as_init(&as);
    uint64_t hint = 0x400000000ULL, out;
    /* free, aligned, in-bounds hint must be honored verbatim */
    ASSERT_STATUS(mm_mmap(&as, hint, 2 * PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_EQ_U64(out, hint);
    ASSERT_WF(as);

    /* a second mapping hinting into the now-occupied range falls back to
     * first-fit (does not land on the taken hint) */
    uint64_t out2;
    ASSERT_STATUS(mm_mmap(&as, hint, 2 * PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &out2), MM_OK);
    ASSERT_TRUE(out2 != hint);
    ASSERT_WF(as);

    /* unaligned hint is ignored (still succeeds via fallback) */
    struct addr_space as2; as_init(&as2);
    uint64_t out3;
    ASSERT_STATUS(mm_mmap(&as2, hint + 1, PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &out3), MM_OK);
    ASSERT_TRUE(is_page_aligned(out3));
    ASSERT_WF(as2);
}

static void test_split_atomic_on_full(void)
{
    /* When the array is full, an interior operation that would need to split
     * a VMA must fail (MM_ENOMEM) WITHOUT mutating, so the address space stays
     * well-formed and canonical (regression for the split_boundaries preflight). */
    struct addr_space as; as_init(&as);
    uint64_t out;

    /* A 4-page splittable VMA up front. */
    uint64_t big = 0x10000000ULL;
    ASSERT_STATUS(mm_mmap(&as, big, 4 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);

    /* Fill capacity with isolated single pages (2-page stride => 1-page gaps,
     * so none ever merge) until count == VMA_CAP. */
    uint64_t p = big + 100 * PG;
    while (as.count < VMA_CAP) {
        ASSERT_STATUS(mm_mmap(&as, p, PG, PROT_READ,
                              MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                              VMA_ANON, -1, 0, &out), MM_OK);
        p += 2 * PG;
    }
    ASSERT_EQ_U64(as.count, VMA_CAP);
    ASSERT_WF(as);
    size_t saved = as.count;

    /* mprotect an interior page of the big VMA: needs two splits, but the
     * array is full -> must report ENOMEM and change nothing. */
    ASSERT_STATUS(mm_mprotect(&as, big + PG, PG, PROT_WRITE), MM_ENOMEM);
    ASSERT_EQ_U64(as.count, saved);
    ASSERT_WF(as);
    /* the big VMA is untouched (still a single 4-page PROT_READ region) */
    ASSERT_EQ_INT(prot_at(&as, big), PROT_READ);
    ASSERT_EQ_INT(prot_at(&as, big + PG), PROT_READ);
}

int main(void)
{
    TEST_SUITE("mmap/mprotect/munmap operations");
    RUN_TEST(test_invalid_args,
             "mmap rejects invalid length/prot/alignment");
    RUN_TEST(test_anon_place_and_merge,
             "Anonymous mmap placement & adjacent merge");
    RUN_TEST(test_fixed_overlay_split,
             "MAP_FIXED overlay splits the underlying reservation");
    RUN_TEST(test_mprotect_split_and_gap,
             "mprotect splits sub-ranges & rejects unmapped gaps");
    RUN_TEST(test_munmap_hole_and_remerge,
             "munmap punches holes & tolerates empty ranges");
    RUN_TEST(test_nonfixed_hint,
             "Non-FIXED mmap honors a free hint, else first-fit");
    RUN_TEST(test_split_atomic_on_full,
             "Boundary split is atomic at capacity (ENOMEM, no mutation)");
    return TEST_SUMMARY();
}
