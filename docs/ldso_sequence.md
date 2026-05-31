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
