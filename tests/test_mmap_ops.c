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
    /* a second adjacent identical mapping placed right below should merge */
    ASSERT_STATUS(mm_mmap(&as, a - 2 * PG, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &b), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(b, a - 2 * PG);
    /* merged into a single VMA */
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, a - 2 * PG);
    ASSERT_EQ_U64(as.vmas[0].end, a + 2 * PG);
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

int main(void)
{
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_anon_place_and_merge);
    RUN_TEST(test_fixed_overlay_split);
    RUN_TEST(test_mprotect_split_and_gap);
    RUN_TEST(test_munmap_hole_and_remerge);
    return TEST_SUMMARY();
}
