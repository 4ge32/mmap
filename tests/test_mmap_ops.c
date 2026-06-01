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

/* Count VMAs whose [start,end) intersects address `a` (for layout snapshots). */
static int vma_index_at(const struct addr_space *as, uint64_t a)
{
    for (size_t k = 0; k < as->count; k++)
        if (as->vmas[k].start <= a && a < as->vmas[k].end)
            return (int)k;
    return -1;
}

/*
 * MAP_FIXED_NOREPLACE — like MAP_FIXED in placement but refuses (MM_EEXIST) to
 * overlay anything already mapped, mutating nothing on collision. This mirrors
 * the kernel flag a loader uses to detect a clobbered reservation.
 */
static void test_fixed_noreplace(void)
{
    struct addr_space as; as_init(&as);
    uint64_t base = 0x600000000ULL, out;

    /* reserve [base, base+4P) so the range is occupied */
    ASSERT_STATUS(mm_mmap(&as, base, 4 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    /* Snapshot the exact layout so we can prove no-mutation on EEXIST. */
    size_t saved_count = as.count;
    struct vma saved = as.vmas[0];

    /* exact-overlap: NOREPLACE at the occupied base -> EEXIST, unchanged. */
    ASSERT_STATUS(mm_mmap(&as, base, 4 * PG, PROT_WRITE,
                          MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_EEXIST);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, saved_count);
    ASSERT_EQ_U64(as.vmas[0].start, saved.start);
    ASSERT_EQ_U64(as.vmas[0].end, saved.end);
    ASSERT_EQ_INT(as.vmas[0].prot, saved.prot);

    /* partial-overlap: a range straddling the upper edge [base+2P, base+6P)
     * still collides -> EEXIST, unchanged. */
    ASSERT_STATUS(mm_mmap(&as, base + 2 * PG, 4 * PG, PROT_READ,
                          MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_EEXIST);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, saved_count);
    ASSERT_EQ_U64(as.vmas[0].end, saved.end);

    /* free range: NOREPLACE into a hole well above succeeds at the exact addr. */
    uint64_t hole = base + 16 * PG;
    ASSERT_STATUS(mm_mmap(&as, hole, 2 * PG, PROT_READ,
                          MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, hole);
    ASSERT_EQ_INT(prot_at(&as, hole), PROT_READ);

    /* contrast: plain MAP_FIXED at the same occupied base DOES overlay (MM_OK),
     * showing NOREPLACE is the only differentiator. */
    ASSERT_STATUS(mm_mmap(&as, base, 4 * PG, PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, base);
    ASSERT_EQ_INT(prot_at(&as, base), PROT_WRITE); /* overlaid */
}

/* mremap shrink: tail is unmapped, base unchanged, single VMA shrinks. */
static void test_mremap_shrink(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x700000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 4 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    ASSERT_STATUS(mm_mremap(&as, b, 4 * PG, 2 * PG, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, b);                       /* base stays put */
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, b);
    ASSERT_EQ_U64(as.vmas[0].end, b + 2 * PG);   /* tail gone */
    ASSERT_EQ_INT(prot_at(&as, b + 2 * PG), -1); /* [b+2P, b+4P) unmapped */
}

/* mremap grow-in-place: free space above lets it extend without moving and the
 * extension (shared prot/flags/backing/map_id) re-merges into one VMA. */
static void test_mremap_grow_in_place(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x710000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* No MAYMOVE needed: [b+2P, b+4P) is free, so it grows in place. */
    ASSERT_STATUS(mm_mremap(&as, b, 2 * PG, 4 * PG, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, b);
    /* The extension shares identity, so it merges -> exactly one VMA covering
     * [b, b+4P). */
    int i = vma_index_at(&as, b);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ_U64(as.vmas[i].start, b);
    ASSERT_EQ_U64(as.vmas[i].end, b + 4 * PG);
    ASSERT_EQ_U64(as.count, 1);
}

/* mremap grow blocked (something abuts the tail) and no MAYMOVE -> ENOMEM,
 * nothing mutated. */
static void test_mremap_grow_blocked(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x720000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    /* Block the immediate tail [b+2P, b+3P) with a distinct mapping. */
    ASSERT_STATUS(mm_mmap(&as, b + 2 * PG, PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    size_t saved = as.count;

    ASSERT_STATUS(mm_mremap(&as, b, 2 * PG, 4 * PG, 0, &out), MM_ENOMEM);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, saved);
    /* source still spans exactly its original range */
    int i = vma_index_at(&as, b);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ_U64(as.vmas[i].start, b);
    ASSERT_EQ_U64(as.vmas[i].end, b + 2 * PG);
}

/* mremap grow with MREMAP_MAYMOVE when the tail is blocked: the mapping is
 * relocated, old range freed, new region present at new_len. */
static void test_mremap_grow_move(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x730000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 2 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_STATUS(mm_mmap(&as, b + 2 * PG, PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);

    uint64_t newb;
    ASSERT_STATUS(mm_mremap(&as, b, 2 * PG, 4 * PG, MREMAP_MAYMOVE, &newb), MM_OK);
    ASSERT_WF(as);
    ASSERT_TRUE(newb != b);                       /* relocated */
    ASSERT_EQ_INT(prot_at(&as, b), -1);           /* old range [b,b+2P) freed */
    /* new region exists, spans the full new_len with the source's prot */
    int i = vma_index_at(&as, newb);
    ASSERT_TRUE(i >= 0);
    ASSERT_EQ_U64(as.vmas[i].start, newb);
    ASSERT_EQ_U64(as.vmas[i].end, newb + 4 * PG);
    ASSERT_EQ_INT(as.vmas[i].prot, PROT_READ | PROT_WRITE);
}

/* mremap grow-in-place of a file-backed VMA preserves file_offset and stays a
 * single contiguous VMA (the extension's offset continues from the source). */
static void test_mremap_file_grow(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x740000000ULL, out;
    const uint64_t O = 3 * PG; /* page-aligned file offset */
    ASSERT_STATUS(mm_mmap(&as, b, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, 7, O, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.vmas[0].file_offset, O);

    ASSERT_STATUS(mm_mremap(&as, b, 2 * PG, 4 * PG, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, b);
    /* one contiguous file VMA, still starting at offset O */
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_INT(as.vmas[0].backing, VMA_FILE);
    ASSERT_EQ_U64(as.vmas[0].file_offset, O);
    ASSERT_EQ_U64(as.vmas[0].start, b);
    ASSERT_EQ_U64(as.vmas[0].end, b + 4 * PG);
}

/* mremap whole-mapping requirement & no-op:
 *  - resizing a sub-range that is not exactly one full VMA -> EINVAL.
 *  - new_len == old_len -> MM_OK no-op, out == old_addr, unchanged. */
static void test_mremap_whole_and_noop(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x750000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 4 * PG, PROT_READ | PROT_WRITE,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);

    /* middle sub-range [b+P, b+3P) is not a whole VMA -> EINVAL. */
    ASSERT_STATUS(mm_mremap(&as, b + PG, 2 * PG, 4 * PG, MREMAP_MAYMOVE, &out),
                  MM_EINVAL);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    /* no-op resize: new_len == old_len. */
    size_t saved = as.count;
    struct vma v0 = as.vmas[0];
    ASSERT_STATUS(mm_mremap(&as, b, 4 * PG, 4 * PG, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(out, b);
    ASSERT_EQ_U64(as.count, saved);
    ASSERT_EQ_U64(as.vmas[0].start, v0.start);
    ASSERT_EQ_U64(as.vmas[0].end, v0.end);
}

/* munmap a MIDDLE page of a 3-page single VMA -> head+tail split into two. */
static void test_munmap_middle_split(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x760000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 3 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    ASSERT_STATUS(mm_munmap(&as, b + PG, PG), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 2);
    ASSERT_EQ_U64(as.vmas[0].start, b);
    ASSERT_EQ_U64(as.vmas[0].end, b + PG);
    ASSERT_EQ_U64(as.vmas[1].start, b + 2 * PG);
    ASSERT_EQ_U64(as.vmas[1].end, b + 3 * PG);
    ASSERT_EQ_INT(prot_at(&as, b + PG), -1);     /* middle unmapped */
}

/* mprotect a 4-page VMA into differing prots, then re-equalize all back to one
 * prot -> the splits re-merge into a single VMA. */
static void test_mprotect_differ_then_remerge(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x770000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 4 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    /* differ the inner two pages -> three VMAs */
    ASSERT_STATUS(mm_mprotect(&as, b + PG, 2 * PG, PROT_READ | PROT_WRITE), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 3);

    /* re-equalize the WHOLE range back to PROT_READ -> re-merge to one. */
    ASSERT_STATUS(mm_mprotect(&as, b, 4 * PG, PROT_READ), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, b);
    ASSERT_EQ_U64(as.vmas[0].end, b + 4 * PG);
}

/* as_find_free exhaustion: fill the space so no hole large enough remains, then
 * a non-FIXED mmap of a large len -> ENOMEM. */
static void test_find_free_exhaustion(void)
{
    struct addr_space as; as_init(&as);
    uint64_t out;
    /* Shrink the usable window to a tiny one so we can fill it deterministically:
     * one VMA fully occupying [AS_MIN, AS_MIN+2P) leaves [AS_MIN+2P, as_max). */
    as.as_max = AS_MIN + 4 * PG; /* total window = 4 pages */
    ASSERT_STATUS(mm_mmap(&as, AS_MIN, 3 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    /* Only [AS_MIN+3P, AS_MIN+4P) (1 page) is free; ask for 2 pages non-FIXED. */
    ASSERT_STATUS(mm_mmap(&as, 0, 2 * PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &out),
                  MM_ENOMEM);
    ASSERT_WF(as);
    /* a 1-page request still fits the remaining hole */
    ASSERT_STATUS(mm_mmap(&as, 0, PG, PROT_READ,
                          MAP_PRIVATE | MAP_ANONYMOUS, VMA_ANON, -1, 0, &out),
                  MM_OK);
    ASSERT_WF(as);
}

/* AS_MAX boundary: a MAP_FIXED mapping ending exactly at AS_MAX succeeds; one
 * whose range would exceed AS_MAX -> ENOMEM. */
static void test_as_max_boundary(void)
{
    struct addr_space as; as_init(&as);
    uint64_t out;
    uint64_t at = AS_MAX - 2 * PG; /* page-aligned, ends exactly at AS_MAX */

    ASSERT_STATUS(mm_mmap(&as, at, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.vmas[as.count - 1].end, AS_MAX);

    /* one page higher would end at AS_MAX+PG > AS_MAX -> ENOMEM. */
    ASSERT_STATUS(mm_mmap(&as, AS_MAX - PG, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_ENOMEM);
    ASSERT_WF(as);
}

/* sub-page length rounds up: length of 1 byte -> succeeds, VMA spans one page. */
static void test_subpage_length_rounds_up(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x780000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 1, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
                          VMA_ANON, -1, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].start, b);
    ASSERT_EQ_U64(as.vmas[0].end, b + PG); /* exactly one page */
}

/* file contiguity at the op level: a 2-page file VMA from ONE mmap, split by an
 * mprotect, re-merges once the prot is equalized — proving file_offset stays
 * continuous across the boundary so the two halves are mergeable. */
static void test_file_contiguity_merge(void)
{
    struct addr_space as; as_init(&as);
    uint64_t b = 0x790000000ULL, out;
    ASSERT_STATUS(mm_mmap(&as, b, 2 * PG, PROT_READ,
                          MAP_FIXED | MAP_PRIVATE, VMA_FILE, 5, 0, &out), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);

    /* force a split: change the second page's prot. */
    ASSERT_STATUS(mm_mprotect(&as, b + PG, PG, PROT_READ | PROT_WRITE), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 2);
    /* the second half's offset continues from the first (O + 1P). */
    ASSERT_EQ_U64(as.vmas[1].file_offset, PG);

    /* re-equalize: the two file halves are contiguous in offset & share id, so
     * canonicalization merges them back into one VMA. */
    ASSERT_STATUS(mm_mprotect(&as, b, 2 * PG, PROT_READ), MM_OK);
    ASSERT_WF(as);
    ASSERT_EQ_U64(as.count, 1);
    ASSERT_EQ_U64(as.vmas[0].file_offset, 0);
    ASSERT_EQ_U64(as.vmas[0].end - as.vmas[0].start, 2 * PG);
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
    RUN_TEST(test_fixed_noreplace,
             "MAP_FIXED_NOREPLACE refuses overlap (EEXIST) but maps a free range");
    RUN_TEST(test_mremap_shrink,
             "mremap shrink unmaps the tail and keeps the base");
    RUN_TEST(test_mremap_grow_in_place,
             "mremap grows in place into free space, re-merging to one VMA");
    RUN_TEST(test_mremap_grow_blocked,
             "mremap grow with no room and no MAYMOVE returns ENOMEM, no mutation");
    RUN_TEST(test_mremap_grow_move,
             "mremap grow with MREMAP_MAYMOVE relocates and frees the old range");
    RUN_TEST(test_mremap_file_grow,
             "mremap grow-in-place preserves a file-backed VMA's offset & contiguity");
    RUN_TEST(test_mremap_whole_and_noop,
             "mremap requires a whole VMA (EINVAL on sub-range) and no-ops equal sizes");
    RUN_TEST(test_munmap_middle_split,
             "munmap of a middle page splits a single VMA into head and tail");
    RUN_TEST(test_mprotect_differ_then_remerge,
             "mprotect differing then re-equalizing prots re-merges to one VMA");
    RUN_TEST(test_find_free_exhaustion,
             "Non-FIXED mmap returns ENOMEM when no hole is large enough");
    RUN_TEST(test_as_max_boundary,
             "MAP_FIXED at the AS_MAX boundary succeeds; beyond it returns ENOMEM");
    RUN_TEST(test_subpage_length_rounds_up,
             "Sub-page mmap length rounds up to a single full page");
    RUN_TEST(test_file_contiguity_merge,
             "File-backed VMAs split then re-merge with continuous file_offset");
    return TEST_SUMMARY();
}
