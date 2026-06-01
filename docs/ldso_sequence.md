# ld.so Mapping Sequence (model)

This is the authoritative description of how the dynamic loader
(`ld-linux.so`, glibc `_dl_map_object_from_fd` / `_dl_map_segments`) lays a
shared object into the address space, expressed as the mmap/mprotect sequence
our simulator must reproduce. The replay test
(`tests/test_ldso_replay.c`) derives its assertions from the synthetic example
below; keep the two in sync.

The five steps, as a sequence of operations on the address space:

```mermaid
sequenceDiagram
  participant L as ld.so
  participant AS as address space
  L->>AS: mmap PROT_NONE [0,7) (reserve whole image)
  L->>AS: mmap MAP_FIXED r-x [0,2) file (text)
  L->>AS: mmap MAP_FIXED rw- [3,5) file (data; [2,3) gap stays NONE)
  L->>AS: mmap MAP_FIXED rw- [5,7) anon (bss)
  L->>AS: mprotect r-- [3,4) (GNU_RELRO)
  Note over AS: canonical: 5 VMAs, as_wf holds
```

> Step through this sequence interactively below:

```vma-viz ldso-mapping
```

## The real loader sequence

1. **Whole-object reservation.** The loader computes the total memory span of
   all `PT_LOAD` segments (`l_map_start .. l_map_end`) and reserves it with a
   single `mmap` at `PROT_NONE` (private). This claims a contiguous region so
   that segment placements are relative to one base, and any inter-segment
   alignment gaps stay inaccessible.

2. **Per-`PT_LOAD` segment mapping.** For each loadable segment, the loader
   `mmap`s with `MAP_FIXED | MAP_PRIVATE`, file-backed, at the page-down
   segment virtual address (`base + (p_vaddr & ~pagemask)`), length covering
   `p_filesz`, with the segment's real protection derived from `p_flags`:
   - text segment → `R | X`
   - data segment → `R | W`
   The `MAP_FIXED` overlay replaces the corresponding part of the `PROT_NONE`
   reservation. Alignment gaps between segments remain at `PROT_NONE`.

3. **bss / zero-fill.** Where `p_memsz > p_filesz`, the loader provides
   zero-initialized memory:
   - The partial last file page is made writable (if needed) and the tail
     zeroed.
   - Whole anonymous pages beyond the file content are mapped
     `MAP_FIXED | MAP_ANONYMOUS` `R | W`.

4. **GNU_RELRO.** After relocations are applied, the loader `mprotect`s the
   `PT_GNU_RELRO` sub-range of the data segment to read-only (`PROT_READ`),
   hardening relocated pointers.

## Synthetic example used by the replay test

A shared object with two `PT_LOAD` segments (page `P` = 4096):

| Segment | p_vaddr | p_offset | p_filesz | p_memsz | prot  |
|---------|---------|----------|----------|---------|-------|
| text    | 0       | 0        | 2P       | 2P      | R \| X |
| data    | 3P      | 3P       | 2P       | 4P      | R \| W |

Total image span = `3P + 4P = 7P`. There is a one-page alignment gap at
`[2P, 3P)` between the segments.

### Replayed operations (against `base` from step 1)

1. `mm_mmap(addr=0, len=7P, PROT_NONE, PRIVATE|ANON)` → reservation, returns `base`.
2. `mm_mmap(base+0,  len=2P, R|X, FIXED|PRIVATE, file fd, off=0)`  → text.
3. `mm_mmap(base+3P, len=2P, R|W, FIXED|PRIVATE, file fd, off=3P)` → data file part.
4. `mm_mmap(base+5P, len=2P, R|W, FIXED|PRIVATE|ANON)`             → bss anon pages.
5. `mm_mprotect(base+3P, len=P, PROT_READ)`                        → GNU_RELRO.

### Resulting canonical layout (5 VMAs)

```
[base+0,  +2P)  R|X   file@0       (text)
[base+2P, +3P)  ----  anon         (reservation alignment gap, PROT_NONE)
[base+3P, +4P)  R     file@3P      (RELRO, read-only)
[base+4P, +5P)  R|W   file@4P      (data, writable)
[base+5P, +7P)  R|W   anon         (bss)
```

The replay test asserts `as_wf` after every step plus these protections,
backings, and the final VMA count — demonstrating that PROT_NONE reservation,
MAP_FIXED overlay (multiple mappings into one reservation), and mprotect-based
protection switching together suffice to model loading a shared object.

---

# M3 scenarios

The sections above describe the baseline single-object, page-aligned load. The
two milestone-3 (M3) scenarios below extend that baseline without changing it:
the five-step sequence and its synthetic example remain exactly as documented.
Each M3 section is self-contained and supplies its own worked example.

## Scenario A — alignment-overshoot reservation + slack trim

### Why the loader overshoots

A `PT_LOAD` segment may request an alignment `p_align` larger than the system
page size (e.g. 2 MiB for huge-page-friendly objects, or any power of two the
linker chose). The mapping must satisfy `(base + p_vaddr) % p_align ==
p_offset % p_align`; for the page-down image base this reduces to *the image
base must be `p_align`-aligned*. A plain `mmap` of exactly the image span gives
no control over the kernel-chosen base's alignment.

glibc's `_dl_map_segments` handles this (the `MAP_COPY`/overshoot path,
formerly the `_dl_map_object_from_fd` "maplength" logic) by **reserving more
than it needs and trimming the slack**:

1. `mmap` a `PROT_NONE`, private reservation of length
   `span + p_align - PAGE` at an arbitrary kernel-chosen base. The extra
   `p_align - PAGE` bytes guarantee that a `p_align`-aligned address exists
   inside the reservation with the full `span` still fitting after it.
2. Compute `aligned_base = round_up(base, p_align)`.
3. **Head trim.** `munmap` the head slack `[base, aligned_base)` if non-empty.
4. **Tail trim.** `munmap` the tail slack
   `[aligned_base + span, base + reservation_len)` if non-empty.
5. The usual per-`PT_LOAD` `MAP_FIXED` overlays, bss zero-fill, and the
   `GNU_RELRO` `mprotect` then proceed exactly as in the baseline sequence,
   but addressed relative to `aligned_base` instead of the raw reservation
   `base`.

The head and tail trims are independent: either may be empty. The head is
empty iff `base` was already `p_align`-aligned; the tail is empty iff the
aligned base landed at the very start (which, given a non-empty head whenever
unaligned, only happens when `base` was already aligned and `p_align == PAGE`).
After trimming, the kept region is exactly `[aligned_base, aligned_base + span)`
and is `p_align`-aligned, so segment placement matches the baseline case.

### Worked example (align = 4P, span = 7P)

Let page `P` = 4096, segment alignment `p_align = 4P`, and reuse the baseline
image (text + data, total span `7P`; same per-segment table as the synthetic
example above). Reservation length is `span + p_align - PAGE = 7P + 4P - P =
10P`.

Pick a concrete unaligned `base` so the arithmetic is visible. Suppose the
kernel returns `base = 2P` (not a multiple of `4P`). Then:

- `aligned_base = round_up(2P, 4P) = 4P`.
- Head slack `[2P, 4P)` — length `2P`, non-empty → `munmap`.
- `aligned_base + span = 4P + 7P = 11P`; reservation end `base +
  reservation_len = 2P + 10P = 12P`.
- Tail slack `[11P, 12P)` — length `1P`, non-empty → `munmap`.
- Kept region `[4P, 11P)`, length `7P`, and `4P % 4P == 0` — aligned. Good.

#### Replayed operations

1. `mm_mmap(addr=0, len=10P, PROT_NONE, PRIVATE|ANON)` → returns `base` (the
   example fixes `base = 2P`).
2. `mm_munmap(base+0,  len=2P)`  → head trim, removes `[2P, 4P)`.
3. `mm_munmap(base+9P, len=1P)`  → tail trim, removes `[11P, 12P)`
   (`base+9P == 11P`).
4. `mm_mmap(aligned_base+0,  len=2P, R|X, FIXED|PRIVATE, file fd, off=0)`  → text.
5. `mm_mmap(aligned_base+3P, len=2P, R|W, FIXED|PRIVATE, file fd, off=3P)` → data file part.
6. `mm_mmap(aligned_base+5P, len=2P, R|W, FIXED|PRIVATE|ANON)`             → bss anon pages.
7. `mm_mprotect(aligned_base+3P, len=P, PROT_READ)`                        → GNU_RELRO.

Here `aligned_base = 4P`. Steps 4–7 are the baseline steps 2–5 shifted to the
aligned base.

#### Resulting canonical layout (5 VMAs)

After the trims the reservation has shrunk to exactly `[4P, 11P)`; nothing maps
outside it. The interior layout is identical in shape to the baseline:

```
[4P,  6P)   R|X   file@0       (text)
[6P,  7P)   ----  anon         (reservation alignment gap, PROT_NONE)
[7P,  8P)   R     file@3P      (RELRO, read-only)
[8P,  9P)   R|W   file@4P      (data, writable)
[9P,  11P)  R|W   anon         (bss)
```

Key assertions a replay test can derive: the trimmed reservation's start is
`4P`-aligned; the address space contains no VMA below `4P` or at/above `11P`
that belongs to this object (the slack is gone); and the final canonical count
is 5 VMAs, all carrying the same `map_id` as the reservation they were overlaid
into.

## Scenario B — multi-object load (two shared objects)

### The modeling rule: per-object identity blocks coalescing

A real program maps many objects (the executable, then each dependency). Each
is loaded into its **own** reservation. The loader does **not** force a base for
a dependency: it calls `mmap` with `addr = 0` (no `MAP_FIXED`), letting the
kernel pick a free region; in the model this is `as_find_free` first-fit
placement. Two such objects may, by chance, end up adjacent in the address
space.

The crucial rule the simulator must honor: **each object's mappings carry a
distinct logical identity (`map_id`), and adjacent-but-compatible VMAs from
different objects do NOT coalesce.** The per-object boundary survives even when
two objects abut and happen to share protection, flags, and (vacuously, for
anon) backing.

This is exactly the model's `map_id` contract (`include/mm_types.h`,
`include/mm_acsl.h`, `src/vma.c`):

- Each `mm_mmap` that *creates* a mapping stamps it with a fresh `map_id`
  (`as->next_map_id++`). All overlays/splits of that mapping inherit the id.
- `vma_mergeable` requires **equal `map_id`** (in addition to equal prot,
  flags, and backing/offset contiguity). So `as_canonicalize` never merges
  across an object boundary.
- Within a *single* object, the two halves produced by the `GNU_RELRO`
  `mprotect` (and the data/bss split) share one `map_id`, so they *do* re-merge
  once protections line up again — the baseline behavior is unchanged.

In short: same object ⇒ may merge; different object ⇒ never merge, even if
otherwise identical and adjacent.

### Worked example (object1 then object2)

Object1 is the baseline synthetic object, loaded by the existing five-step
sequence at its reservation base `b1` (`map_id = m1`), yielding its 5-VMA
layout. Object2 is a second, simpler shared object — one text and one data
`PT_LOAD`, no bss, no RELRO for brevity — loaded next:

| Segment | p_vaddr | p_offset | p_filesz | p_memsz | prot  |
|---------|---------|----------|----------|---------|-------|
| text    | 0       | 0        | 2P       | 2P      | R \| X |
| data    | 2P      | 2P       | 1P       | 1P      | R \| W |

Object2's span is `3P`. It is placed without a fixed base; suppose first-fit
lands it immediately **above** object1 so the two reservations abut at
`b2 = b1 + 7P`. Object2 is stamped with a fresh `map_id = m2` (`m2 != m1`).

#### Replayed operations (object2, after object1's five steps)

1. `mm_mmap(addr=0, len=3P, PROT_NONE, PRIVATE|ANON)` → first-fit, returns
   `b2` (the example fixes `b2 = b1 + 7P`); stamped `map_id = m2`.
2. `mm_mmap(b2+0,  len=2P, R|X, FIXED|PRIVATE, file fd2, off=0)`  → object2 text.
3. `mm_mmap(b2+2P, len=1P, R|W, FIXED|PRIVATE, file fd2, off=2P)` → object2 data.

(No bss/RELRO steps for object2 in this minimal example.)

#### Resulting canonical layout

The whole layout, relative to object1's base `b1` (with `b2 = b1 + 7P`):

```
map_id m1 (object1):
  [b1+0,  +2P)   R|X   file fd1@0    (text)
  [b1+2P, +3P)   ----  anon          (alignment gap, PROT_NONE)
  [b1+3P, +4P)   R     file fd1@3P   (RELRO)
  [b1+4P, +5P)   R|W   file fd1@4P   (data)
  [b1+5P, +7P)   R|W   anon          (bss)
map_id m2 (object2):
  [b1+7P, +9P)   R|X   file fd2@0    (text)     == [b2+0,  +2P)
  [b1+9P, +10P)  R|W   file fd2@2P   (data)     == [b2+2P, +3P)
```

That is **7 VMAs total**, not fewer. Note the boundary at `b1+7P`
(`== b2`): object1's bss `[b1+5P, +7P)` (`R|W` anon) abuts object2's text
`[b1+7P, +9P)` (`R|X` file). These differ in prot and backing, so they would not
merge regardless — the boundary here is uncontroversial.

To exhibit the `map_id` rule in isolation, the replay test also builds a
*stress* variant where the two objects' adjacent VMAs are deliberately made
identical in prot, flags, and backing (both anon `R|W`). There, the ONLY thing
keeping them apart is the differing `map_id`: canonicalization must still leave
two VMAs, proving the per-object boundary is enforced by identity, not by any
incidental prot/backing difference.

Key assertions a replay test can derive: object1's 5 VMAs are unchanged by
object2's load; the final count is 7; the VMA at `b2` has `map_id == m2 != m1`;
and — in the stress variant — two adjacent anon `R|W` VMAs with different
`map_id` stay split after `as_canonicalize`.

### Multi-object sequence diagram

```mermaid
sequenceDiagram
  participant L as ld.so
  participant AS as address space
  Note over L,AS: object1 (map_id m1) — baseline 5-step load
  L->>AS: mmap PROT_NONE [b1, b1+7P) (reserve obj1)
  L->>AS: mmap MAP_FIXED r-x [b1, b1+2P) file fd1 (text)
  L->>AS: mmap MAP_FIXED rw- [b1+3P, b1+5P) file fd1 (data; gap stays NONE)
  L->>AS: mmap MAP_FIXED rw- [b1+5P, b1+7P) anon (bss)
  L->>AS: mprotect r-- [b1+3P, b1+4P) (GNU_RELRO)
  Note over AS: 5 VMAs, all map_id m1
  Note over L,AS: object2 (map_id m2) — own reservation, first-fit
  L->>AS: mmap PROT_NONE len=3P (no FIXED) → kernel picks b2 = b1+7P
  L->>AS: mmap MAP_FIXED r-x [b2, b2+2P) file fd2 (text)
  L->>AS: mmap MAP_FIXED rw- [b2+2P, b2+3P) file fd2 (data)
  Note over AS: 7 VMAs; m1 and m2 abut at b1+7P but never coalesce
```

---

## Note for the replay test

`tests/test_ldso_replay.c` derives its assertions from the worked examples in
this document — the baseline synthetic example above and the two M3 examples
(alignment-overshoot trim, and the two-object load). The test-engineer will add
the M3 assertions; keep the concrete page numbers, operation sequences, and
resulting VMA layouts here in sync with that test.
