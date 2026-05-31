/*
 * test_vma.c - Unit tests for the VMA primitives and arithmetic guards.
 */
#include "test_harness.h"
#include "mm_api.h"

#define PG PAGE_SIZE

static struct vma anon_vma(uint64_t s, uint64_t e, int prot)
{
    struct vma v = {0};
    v.start = s; v.end = e; v.prot = prot;
    v.flags = MAP_PRIVATE | MAP_ANONYMOUS;
    v.backing = VMA_ANON; v.fd = -1; v.file_offset = 0; v.map_id = 1;
    return v;
}

static void test_arith_guards(void)
{
    ASSERT_FALSE(add_overflows(1, 2));
    ASSERT_TRUE(add_overflows(UINT64_MAX, 1));
    ASSERT_FALSE(round_up_overflows(10 * PG));
    ASSERT_TRUE(round_up_overflows(UINT64_MAX));
    ASSERT_EQ_U64(round_up_page(1), PG);
    ASSERT_EQ_U64(round_up_page(PG), PG);
    ASSERT_EQ_U64(round_up_page(PG + 1), 2 * PG);
    ASSERT_TRUE(is_page_aligned(0));
    ASSERT_TRUE(is_page_aligned(PG));
    ASSERT_FALSE(is_page_aligned(1));
}

static void test_mergeable(void)
{
    struct vma a = anon_vma(0, PG, PROT_READ);
    struct vma b = anon_vma(PG, 2 * PG, PROT_READ);
    ASSERT_TRUE(vma_mergeable(&a, &b));

    b.prot = PROT_WRITE;
    ASSERT_FALSE(vma_mergeable(&a, &b)); /* differing prot */

    b.prot = PROT_READ;
    b.start = PG + PG; /* not adjacent */
    ASSERT_FALSE(vma_mergeable(&a, &b));

    /* file-backed contiguity */
    struct vma fa = anon_vma(0, PG, PROT_READ);
    fa.backing = VMA_FILE; fa.fd = 3; fa.file_offset = 0;
    struct vma fb = anon_vma(PG, 2 * PG, PROT_READ);
    fb.backing = VMA_FILE; fb.fd = 3; fb.file_offset = PG;
    ASSERT_TRUE(vma_mergeable(&fa, &fb));
    fb.file_offset = 2 * PG; /* offset gap inconsistent */
    ASSERT_FALSE(vma_mergeable(&fa, &fb));
    fb.file_offset = PG; fb.fd = 4; /* different fd */
    ASSERT_FALSE(vma_mergeable(&fa, &fb));
}

static void test_split_insert_remove(void)
{
    struct addr_space as;
    as_init(&as);

    /* one VMA [0, 4*PG) */
    ASSERT_STATUS(as_insert_at(&as, 0, anon_vma(0, 4 * PG, PROT_READ)), MM_OK);
    ASSERT_EQ_U64(as.count, 1);

    /* split at 2*PG -> [0,2PG) [2PG,4PG) */
    ASSERT_STATUS(as_split_at(&as, 0, 2 * PG), MM_OK);
    ASSERT_EQ_U64(as.count, 2);
    ASSERT_EQ_U64(as.vmas[0].end, 2 * PG);
    ASSERT_EQ_U64(as.vmas[1].start, 2 * PG);
    ASSERT_EQ_U64(as.vmas[1].end, 4 * PG);

    /* reject non-interior / unaligned splits */
    ASSERT_STATUS(as_split_at(&as, 0, 0), MM_EINVAL);
    ASSERT_STATUS(as_split_at(&as, 0, PG + 1), MM_EINVAL);

    /* remove the first -> [2PG,4PG) */
    as_remove_range(&as, 0, 1);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, 2 * PG);
}

static void test_file_split_offset(void)
{
    struct addr_space as;
    as_init(&as);
    struct vma v = anon_vma(0, 4 * PG, PROT_READ);
    v.backing = VMA_FILE; v.fd = 7; v.file_offset = 8 * PG;
    ASSERT_STATUS(as_insert_at(&as, 0, v), MM_OK);
    ASSERT_STATUS(as_split_at(&as, 0, 2 * PG), MM_OK);
    /* right half offset advances by the left half size (2 pages) */
    ASSERT_EQ_U64(as.vmas[0].file_offset, 8 * PG);
    ASSERT_EQ_U64(as.vmas[1].file_offset, 10 * PG);
}

static void test_canonicalize(void)
{
    struct addr_space as;
    as_init(&as);
    /* three adjacent same-prot anon VMAs should collapse to one */
    as_insert_at(&as, 0, anon_vma(0, PG, PROT_READ));
    as_insert_at(&as, 1, anon_vma(PG, 2 * PG, PROT_READ));
    as_insert_at(&as, 2, anon_vma(2 * PG, 3 * PG, PROT_READ));
    as_canonicalize(&as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, 0);
    ASSERT_EQ_U64(as.vmas[0].end, 3 * PG);

    /* differing prot in the middle prevents full merge */
    as_init(&as);
    as_insert_at(&as, 0, anon_vma(0, PG, PROT_READ));
    as_insert_at(&as, 1, anon_vma(PG, 2 * PG, PROT_WRITE));
    as_insert_at(&as, 2, anon_vma(2 * PG, 3 * PG, PROT_READ));
    as_canonicalize(&as);
    ASSERT_EQ_U64(as.count, 3);
    ASSERT_WF(as);
}

int main(void)
{
    RUN_TEST(test_arith_guards);
    RUN_TEST(test_mergeable);
    RUN_TEST(test_split_insert_remove);
    RUN_TEST(test_file_split_offset);
    RUN_TEST(test_canonicalize);
    return TEST_SUMMARY();
}
