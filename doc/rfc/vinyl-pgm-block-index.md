# Vinyl PGM Index: Replace Page Index with Block-Based PGM Format

## Context

The current Vinyl run format organizes data into **pages** (~8KB compressed,
~100 rows each). The page index (array of `vy_page_info` with min_key per
page) is loaded entirely into memory. Point lookups must decompress an
entire page to read a single row.

The new design introduces **blocks** as a metadata grouping layer above
pages. Pages remain as the I/O framing unit (xlog transactions) but shrink
to 2 KB. Pages are compressed as a whole using a per-run ZSTD dictionary
(same `zrow_marker` format as v1, just with dictionary and smaller pages).
Block metadata (fuse8 filters, PGM index, page offsets, LSN/TTL ranges)
is loaded at boot and kept in memory permanently.

**Branch**: `kostja-vinyl-pgm` (based on `kostja-vinyl-dict-compression`,
which is based on `kostja-vinyl-si`).

**PGM library**: https://github.com/gvinciguerra/PGM-index (Apache 2.0,
C++17 header-only, has C interface in `c-interface/`).

## Value Proposition

v2 enables 4x smaller pages (decompress 2 KB instead of 8 KB per
point lookup) while keeping metadata memory at or below current v1
levels:

| Scenario                      | Per-row index memory |
|-------------------------------|----------------------|
| v1, 8 KB pages (current)      | ~1.0-1.5 bytes/row   |
| v1, 2 KB pages (hypothetical) | ~4-6 bytes/row       |
| v2, 2 KB pages (proposed)     | ~1.35 bytes/row      |

Simply shrinking pages to 2 KB under the v1 format would 4x the page
index memory (4x more page_info entries). v2 replaces the per-page
index with per-block metadata (PGM + fuse8), keeping memory flat.

A prior attempt (`kostja-vinyl-page-index`) tried to solve the memory
problem by demand-loading metadata via an LRU cache. This required
cache size tunables and introduced yield points during metadata
access -- both unacceptable. The "all metadata in memory, always"
principle is a direct lesson from that failure.

Dictionary compression is already implemented in
`kostja-vinyl-dict-compression`. Bundling it with the index redesign
avoids two separate format migrations.

## Design Principles

- **No configurables/tunables.** Block size, page size, PGM epsilon,
  filter parameters are compile-time constants. Memory overhead is a
  natural function of row sizes. The user never sets anything.

- **All block metadata in memory, always.** Loaded once at boot. No LRU
  cache, no demand-loading. Yield-free metadata access during lookups.

- **Unchanged .run format.** Pages remain standard xlog transactions
  with `zrow_marker`. No alignment padding, no framing changes.
  Dictionary compression (from `kostja-vinyl-dict-compression`) uses
  shared, refcounted dictionaries persisted in vylog -- the .run
  file itself is unchanged. All new metadata lives in .index2 files.

- **Single runtime path.** On recovery, if a run lacks .index2,
  rebuild it from .run (using the dictionary from vylog). After
  that, all lookups use the block-based reader. No permanent v1
  code path -- just a one-time rebuild for legacy runs.

- **Memory budget**: ~1/100 of uncompressed dataset size for primary keys.
  Relaxed to ~1/50 for small rows (< ~115 bytes). Secondary keys are exempt
  from the ratio constraint since their absolute memory footprint is small
  relative to the full dataset.

## Three-Level Hierarchy

```
Run (.run file, xlog format preserved)
  |
  +-- Block (metadata unit, 200 pages, in-memory directory)
  |     |-- boundary_key, fuse8 filter, PGM model
  |     |-- page offsets (up to 200 x uint32, file-absolute)
  |     |-- min/max LSN, min/max TTL
  |     |
  |     +-- Page (I/O unit, 2 KB uncompressed, xlog transaction)
  |     |     |-- [fixheader: zrow_marker, 19 bytes]
  |     |     |-- [ZSTD_dict(row0+row1+...+rowN+row_index)]
  |     |     |    ^-- page compressed as a whole with per-run dict
  |     |
  |     +-- Page ...  (up to 200 pages per block)
  |
  +-- Block ...
```

- **Block** (up to 200 pages): metadata grouping unit. Variable
  size -- writer closes the block at 200 pages or when compressed
  size reaches a cap. Actual row count varies with row size (~4000
  rows for 100-byte rows). All block metadata kept in memory.
- **Page** (2048 bytes uncompressed): I/O and framing unit. Compressed
  as a whole with per-run ZSTD dictionary using existing `zrow_marker`
  format. On-disk size varies with compression ratio. 19-byte fixheader
  amortized over ~20 rows (for 100-byte rows).
- **Row**: uncompressed within the page (page-level compression).

## Page Format

The .run file format is **unchanged**. Pages are standard xlog
transactions with `zrow_marker` headers, no padding, no framing
changes. Two things change at the application level:

1. Smaller target page size: 2048 bytes uncompressed (down from ~8KB).
2. ZSTD compression uses a shared dictionary (from vylog) for better
   compression of small pages.

```
Page on disk (same format as v1):
[fixheader: zrow_marker, 19 bytes]
[ZSTD_dict(row0+row1+...+rowN+row_index)]
```

The existing `vy_page_read()` and `vy_page_find_key()` code paths
work unchanged for decompression -- decompress the whole page with
the run's dictionary, then binary search the uncompressed data using
row_index.

With 2KB pages, decompressing the entire page is fast (~1-2 us at
ZSTD's ~1-2 GB/s rate). This is small relative to the ~10-15 us pread
latency on NVMe. Per-row compression was considered but adds complexity
(new page format, per-row ZSTD calls with setup overhead) for minimal
gain at this page size.

Page size reduced from ~8KB to 2KB (2048 bytes). The 19-byte fixheader
overhead is amortized over ~20 rows (for 100-byte rows) = ~0.95
bytes/row on disk.

## Memory Budget Math

### Per-row in-memory costs (independent of block size)

| Component              | Bytes/row | Notes                         |
|------------------------|-----------|-------------------------------|
| Fuse8 filter           | 1.125     | 9 bits/key, binary fuse8      |
| **Total per-row**      | **~1.125**|                               |

### Per-block fixed costs

| Component              | Bytes     | Notes                         |
|------------------------|-----------|-------------------------------|
| Page offsets (all)      | 800      | 200 x uint32                           |
| Boundary key (msgpack) | ~20       | First key in block            |
| Min/max LSN            | 16        | 2 x int64                     |
| Min/max TTL            | 16        | 2 x double                    |
| PGM model              | ~11-50    | LCP(1) + segments(8-48) + err(2) |
| **Total fixed**        | **~863-902** |                            |

Call it ~880 bytes/block.

### Block size (up to 200 pages)

Block size is defined in pages, not rows. Variable -- writer closes
the block at 200 pages or when compressed size reaches a cap. Actual
rows per block varies:

| Avg row size | Rows/page | Rows/block (200 pg) | Fixed/row | + fuse8 | 1/100 threshold |
|-------------|-----------|---------------------|-----------|---------|-----------------|
| 50 bytes    | 40        | 8000                | 0.110     | 1.24    | 62 bytes (1/50) |
| 100 bytes   | 20        | 4000                | 0.220     | 1.35    | 135 bytes       |
| 200 bytes   | 10        | 2000                | 0.440     | 1.57    | 157 bytes       |
| 500 bytes   | 4         | 800                 | 1.100     | 2.23    | 223 bytes       |
| 2 KB        | 1         | 200                 | 4.400     | 5.53    | n/a (fallback)  |
| 5 KB        | 1         | 200                 | 4.400     | 5.53    | n/a (fallback)  |

For rows >= ~500 bytes (< 4 rows/page), blocks use the min_keys
fallback (see below). The per-block cost for fallback blocks is
higher (~3400 bytes) but still negligible relative to row size.

### Example: 1 TB uncompressed, 200-byte rows

- 5 billion rows, 10 rows/page, 2000 rows/block, ~2.5M blocks
- Per-row fuse8: 5B x 1.125 = 5.63 GB
- Per-block fixed: 2.5M x 880 = 2.2 GB
- Shared dictionaries: ~5 x 64 KB = 320 KB (negligible)
- **Total: ~7.83 GB** (0.78% of 1 TB -- fits 1/100)

### Page offset storage scheme

All 200 page offsets are stored as uint32 file-absolute values in
the .index2 block metadata. The block's starting file offset is
implicit (= page_offsets[0]).

Page file offset = `page_offsets[P]`

No alignment padding in .run files. Offsets point to exact byte
positions of each page's xlog fixheader.

PGM-to-pread conversion (epsilon = 1 page):

- PGM predicts page P. Target may be P-1, P, or P+1.
- pread from offset[P-1] to offset[P+2] (3 pages).
- All offsets are known, so pread length is exact.
- Within the pread buffer, the fixheader at each page start contains
  the data length, so page boundaries are found by parsing the
  19-byte fixheader (no marker scanning needed).

Total pread: 3 compressed pages, typically 3-6 KB, well within 8 KB.

### Run overlap for compaction decisions

MinHash sketches are not used. Instead, run overlap is estimated from
block boundary keys: count how many blocks in run A overlap with the
key range of run B. If only one boundary block overlaps, treat the
runs as non-overlapping (skip compaction). This covers the common
time-series case where runs are roughly contiguous in key space.

## PGM Index Design

**Scope: per-block.** Each block has its own PGM model.

PGM maps **key -> approximate page number** within the block. Built
from (normalized_min_key[j], j) for each page j in the block (~200
data points). Given a probe key, PGM returns an estimated page number
with bounded error epsilon = 1 page. This directly identifies which
page(s) to read -- no row-to-page conversion needed.

Why page numbers, not row positions: row count per page varies for
variable-length rows (15-25 rows depending on size). PGM over row
positions would require dividing by a non-constant rows_per_page to
get a page number, causing over/undershoot beyond +/-1 page. PGM
over page numbers avoids this entirely. 200 data points per block
is well-suited to PGM -- typically 1-4 linear segments suffice.

### Key encoding: two-level LCP + uint64 normalization

Variable-length msgpack keys are converted to uint64 for PGM input
via a two-level LCP extraction followed by type-specific normalization.

**Level 1: Field-level LCP (common fields)**

Compare the block's first and last key field by field using raw
msgpack bytes. No type dispatch needed -- msgpack canonical encoding
guarantees equal values produce identical bytes:

```
for each field i:
    a = mp_field(first_key, i),  a_len = mp_sizeof(a)
    b = mp_field(last_key, i),   b_len = mp_sizeof(b)
    if a_len == b_len && memcmp(a, b, a_len) == 0:
        field i is part of LCP
    else:
        field i is the discriminating field, stop
```

Uses only `mp_next()` + `memcmp`. If msgpack types differ (e.g.,
`MP_UINT` vs `MP_INT`), the bytes differ and we correctly identify
the field as discriminating.

**Safety with `number`/`scalar` field types**: these accept multiple
msgpack types (e.g., MP_UINT(42) vs MP_DOUBLE(42.0) for the same
logical value). The bytes differ, so memcmp sees them as different
and stops LCP early. This is a conservative underestimate (safe),
never an overestimate. Msgpuck uses canonical minimal encoding
within each type -- same type + same value = identical bytes. LCP
can only underestimate (stop early) or be exact; it cannot
overestimate because identical bytes always mean identical values.

**Level 2: Intra-field byte LCP (discriminating field, strings only)**

For the discriminating field, if its msgpack type is `MP_STR` or
`MP_BIN`, extend the LCP into the payload bytes:

1. Decode length headers of both strings.
2. `memcmp` the payload bytes to find the common byte prefix.
3. Strip the common bytes; the remaining suffix is the
   discriminating content.

String/varbinary payloads are memcmp-comparable, so this preserves
sort order. Other types (numeric, UUID, etc.) are not extended --
they're small enough that intra-field LCP savings are negligible.

**PGM uint64 normalization (discriminating field)**

After LCP stripping, normalize the discriminating field to a
sort-preserving uint64:

| Msgpack type     | Normalization                              |
|------------------|--------------------------------------------|
| `MP_STR/MP_BIN`  | First 8 suffix bytes, big-endian uint64    |
| `MP_UINT`        | Direct value                               |
| `MP_INT`         | Bias: `(uint64_t)(value + INT64_MIN)`      |
| `MP_DOUBLE`      | Flip sign bit + conditional complement     |
| `MP_EXT` (UUID)  | First 8 bytes of 16-byte payload           |
| `MP_EXT` (other) | Fallback to min_keys                       |
| `MP_BOOL`        | 0 or 1                                     |

If normalization is not possible (e.g., mixed msgpack types in the
same field position across keys), the block falls back to min_keys.

Per-block metadata: `lcp_field_count` (uint8) + `lcp_fields`
(raw msgpack, stored once) + for strings: `lcp_str_prefix_len`
(uint16) + PGM segments.

Inspired by the approach recommended by PGM authors (Vinciguerra,
GitHub issue #17) and proven in Bourbon (Dai et al. 2020).

### PGM parameters (compile-time constants)

- **Page size**: 2048 bytes (uncompressed).
- **Block size**: up to 200 pages (variable, capped by page count
  or compressed size).
- **PGM epsilon**: 1 page. PGM prediction is within +/- 1 page of
  the target. Input: ~200 (normalized_min_key, page_number) pairs.
- **I/O budget**: 3 pages per point lookup (PGM epsilon 1, all
  offsets stored), typically 3-6 KB compressed, within 8 KB budget.
- Segment size: slope (4 bytes) + intercept (4 bytes) = 8 bytes.
- For well-distributed keys, a single linear segment per block may
  suffice: **11 bytes per block** (slope + intercept + max_error + LCP).
  For skewed distributions, 2-4 segments: **30-50 bytes per block**.

### Per-block fallback to min_keys binary search

Each block independently chooses its lookup strategy at construction
time. A block uses **per-page min_keys with binary search** (instead
of PGM + every-2nd offsets) when any of these conditions hold:

1. **Large rows** (avg rows/page < 4 in this block): pages are large,
   so reading 4 pages (PGM path) wastes I/O vs reading 1 page (binary
   search). Memory savings from PGM are irrelevant for large rows.
2. **Non-binary collation**: any key part in the discriminating field
   (or earlier) uses an ICU/unicode collation (coll_id != 0). Both
   LCP extraction (memcmp on raw bytes) and uint64 normalization
   (big-endian byte packing) only preserve sort order for binary
   collation. With ICU collation, memcmp order != collation order,
   so LCP may overestimate the common prefix and PGM predictions
   would be wrong. This is a per-index property, known at index
   creation time -- all blocks in such an index use min_keys.
3. **Normalization failure**: the discriminating field has a msgpack
   type that can't be normalized to a sort-preserving uint64 (e.g.,
   MP_EXT for decimal/datetime, or mixed types across keys).
4. **PGM construction produces too many segments**: the key distribution
   is too complex for a compact PGM model.

Fallback block metadata:
- LCP-compressed min_keys: 200 x ~10-20 bytes suffix = ~2000-4000 bytes
- Page offsets: same as PGM blocks (200 x uint32 = 800 bytes)
- LCP prefix: stored once per block
- Total: ~2900-4900 bytes/block

**Min_keys are LCP-compressed** using the same two-level LCP as
PGM blocks. The block stores the LCP once (complete matching fields
+ string byte prefix for the discriminating field), and each min_key
stores only its suffix. This makes storage cost nearly independent
of full key size:

| Full key size | Typical LCP | Stored suffix | 200 suffixes/block |
|--------------|-------------|---------------|-------------------|
| 20 B         | ~10 B       | ~10 B         | ~2000 B           |
| 50 B         | ~40 B       | ~10 B         | ~2000 B           |
| 100 B        | ~85 B       | ~15 B         | ~3000 B           |
| 500 B        | ~480 B      | ~20 B         | ~4000 B           |

Cost for large-row blocks (1 row/page, 200 rows/block):
- ~3500/200 = ~17.5 bytes/row. For 2 KB rows = 0.9%, for 5 KB = 0.4%.
  Well within any budget, regardless of key size.

Binary search on LCP-compressed min_keys: strip the probe key's LCP
fields (and string byte prefix for the discriminating field), then
compare suffixes directly using the existing comparator starting
from the discriminating field. No allocation, no key reconstruction.

Lookup in a fallback block: binary search the in-memory min_keys
array -> find exact page -> look up its offset -> pread 1 page ->
decompress -> row_index binary search within page. One page read
instead of four.

A single flag byte in the block metadata header indicates the
representation (PGM vs min_keys).

### Point lookup flow

**PGM path** (small rows, most blocks):
1. Strip block's LCP from k, pack next 8 bytes as uint64 x.
2. PGM(x) -> estimated page number P (within block).
3. pread 3 pages [P-1, P, P+1] using stored offsets (one I/O, ~3-6 KB).
4. Parse fixheader in buffer to find page boundaries.
5. Decompress target page (ZSTD with per-run dict, ~2 KB).
6. Binary search within decompressed page using row_index.
7. Found -> return row. Not found -> next run.

**Min_keys path** (large rows, degenerate keys):
1. Binary search per-page min_keys array -> exact page number.
2. Look up page offset (all offsets stored for fallback blocks).
3. pread 1 page (one I/O, size varies with row size).
4. Decompress page, binary search using row_index.
5. Found -> return row. Not found -> next run.

## ZSTD Dictionary Compression

Pages use existing `zrow_marker` page-level compression, but with a
per-run ZSTD dictionary for better compression of small (2 KB) pages.
Without a dictionary, small pages compress poorly because ZSTD has
little context. With a trained dictionary, compression ratio recovers
to near what large pages achieve without one.

**Design**: shared, refcounted dictionaries per LSM tree. Already
implemented in `kostja-vinyl-dict-compression`.

- **Shared**: multiple runs reference the same dictionary via `dict_id`.
  A single LSM with 100 runs may have only 3-5 active dictionaries.
- **Refcounted**: `struct vy_dict` has `refs` count. Freed when last
  referencing run is compacted away.
- **Persisted in vylog**: `VY_LOG_CREATE_DICT` stores raw dict bytes;
  `VY_LOG_PREPARE_RUN` stores `dict_id` per run. Not stored in .run
  or .index2 files.
- **Adaptive training**: trains every N dumps (N self-adjusts 4-256),
  requires >= 5% compression gain vs current dict.
- **Per-LSM state**: `dict_last.dict` = active dict for new runs;
  `dict_hash` = all dicts referenced by existing runs.
- **Target size**: 64 KB (`VY_DICT_TARGET_SIZE`).

Dictionary memory: ~64 KB per active dictionary, shared across many
runs. For an LSM with 5 active dicts: ~320 KB. For 20 LSMs: ~6.4 MB.
Negligible relative to block metadata.

## Lookup Flow

### Point lookup

```
1. Binary search block directory boundary keys -> block_no    (in memory)
   For EQ/GE/GT iterators: if search_key == boundary_key[block_no],
   also check block_no-1 (the previous block may contain the first
   occurrence of a duplicate key that spans a block boundary).
   The fuse8 filter on block_no-1 catches the common case where
   the key is not present there.
2. Fuse8 filter check for block_no                            (in memory)
   -> miss? skip to next run
3. Check block type flag (PGM vs min_keys)                    (in memory)

PGM block (small rows):
4a. LCP-strip search key, pack next 8 bytes as uint64 x      (in memory)
5a. PGM(x) -> estimated page P, error +/- 1 page             (in memory)
6a. Look up offsets[P-1] and offsets[P+2]                     (in memory)
7a. pread 3 pages [P-1..P+1] (~3-6 KB, ONE I/O)              (yields)
8a. Parse fixheader to find page boundaries                   (CPU, instant)

Min_keys block (large rows / fallback):
4b. Binary search per-page min_keys -> exact page             (in memory)
5b. Look up page offset                                       (in memory)
6b. pread 1 page (ONE I/O, size varies)                       (yields)

Both paths:
9. Decompress target page with per-run ZSTD dict              (CPU)
10. Binary search within decompressed page using row_index    (CPU)
11. Return found row or NOT_FOUND
```

Steps 1-6/8 are yield-free (all in memory).
PGM path: single pread of ~3-6 KB (3 compressed 2 KB pages).
Min_keys path: single pread of 1 page (size proportional to row size).

### Range scan

```
1. Binary search block directory -> starting block
2. PGM -> starting page number, resolve to offset via stored offsets
3. Sequential page reads with read-ahead (fadvise or io_uring)
4. Decompress each page as a whole, iterate rows within
```

Read-ahead unit = 1 block of pages (200 pages, ~100-400 KB compressed).

## Range Split

Two split paths exist:

1. **Primary**: `vy_range_find_best_split()` -- sweep-based heuristic over
   slice begin/end keys. Independent of block structure, no changes needed.

2. **Fallback**: takes median page's min_key from oldest run. With blocks,
   this becomes the **median block's boundary_key**. The split key does
   NOT have to align with a block boundary -- slices clip their view with
   begin/end keys, just like today with pages. A block straddling the split
   point is referenced by both resulting slices (fuse8/PGM remain valid
   for partial blocks).

   `referenced_pages` -> `referenced_blocks` for bloat detection accounting.

## Compaction Optimizations

Blocks enable two compaction optimizations that the current row-by-row
merge cannot do.

### Block-overlap-based compaction skipping

Before compacting two runs, measure their overlap using block
boundary keys. If overlap < 5%, skip compaction -- the runs are
effectively disjoint.

**How it works:**

1. For each block boundary key in run A, binary search run B's
   block directory to check if it falls within B's key range.
2. Count overlapping blocks. Overlap ratio = overlapping blocks /
   total blocks in the smaller run.
3. If overlap < 5%, mark the runs as disjoint for compaction
   planning. The scheduler treats them as separate levels.

This replaces the MinHash/Jaccard approach from `kostja-vinyl-page-index`
with a simpler, exact measurement. MinHash had false-positive risk with
very different run sizes; block-boundary overlap is deterministic.

**Integration point:** `vy_compaction_plan_trim()` in `vy_range.c`
(already exists in `kostja-vinyl-page-index`, needs adaptation from
page-level to block-level boundary keys).

**When to skip the check:** forced compaction (`range->needs_compaction`)
always proceeds regardless of overlap.

### Block-level copy during compaction

When compacting multiple runs, if a block in one run has **no overlap**
with any other run being compacted, copy the entire block's pages to
the output .run file without decompressing or re-encoding. This saves
all the CPU of decompress -> decode rows -> merge -> encode -> compress
for non-overlapping regions.

**Current path (row-by-row):**
```
for each input run:
    vy_slice_stream: pread page -> ZSTD decompress -> decode rows
        -> vy_write_src heap: merge by key
            -> vy_run_writer: encode row -> accumulate page -> compress -> write
```

**Optimized path (block copy):**
```
if block has no overlap with other input runs:
    pread block's raw pages from input .run (compressed, as-is)
    write raw bytes to output .run (no decompress/recompress)
    copy block metadata to output .index2
    advance past block in merge iterator
```

**How to detect non-overlapping blocks:**

During compaction, the write iterator merges rows from N input runs.
Before pulling rows from a block, check if the block's key range
`[boundary_key, next_boundary_key)` overlaps with any other input
run's key range (using the other runs' block directories):

1. For each input run, maintain a cursor into its block directory.
2. When a run's current block starts, check if any other run has
   blocks whose key range overlaps with this block's range.
3. If no overlap: this block can be copied raw.
4. If overlap: fall back to row-by-row merge for this block.

**Implementation details:**

- **Raw page copy**: Read the contiguous byte range from input .run
  file (from first page offset to last page offset + last page size)
  and write it directly to the output .run file. No xlog re-framing
  needed -- pages are already valid xlog transactions.
- **Block metadata copy**: The block's fuse8 filter, PGM model,
  boundary key, and page offsets are valid as-is. Page offsets must
  be adjusted to the new file position in the output .run. LCP and
  PGM remain valid because they describe the block's internal key
  distribution, which hasn't changed.
- **Dictionary compatibility**: The input block was compressed with
  a specific dictionary. If the output run uses the same dictionary
  (common case -- shared, refcounted dicts), pages decompress
  correctly without re-encoding. If the dictionary changes (new
  dict was trained), raw copy is still valid -- the output run just
  needs to record which dict each block's pages use.
- **Fuse8 filter**: Copied as-is (filter is a function of the keys
  in the block, which haven't changed).
- **Boundary conditions**: A block is eligible for raw copy only if
  it is fully within the compaction range (not clipped by slice
  begin/end keys) and has zero overlap with other inputs.

**Dictionary handling for raw-copied blocks:**

Dictionary is per-run (one `dict_id` per run, same as current model).
Raw copy is only used for blocks whose input run uses the **same
dictionary** as the output run. If the input block's dict differs
(because a new dict was trained), that block falls back to row-by-row
re-encoding with the output run's dict.

This works well because dictionaries are shared and refcounted:
runs created close in time typically share the same dict (adaptive
training changes the dict only every 4-256 dumps). For time-series
workloads where most blocks are non-overlapping, the dict usually
hasn't changed, so most blocks qualify for raw copy.

**Expected savings:**

For time-series workloads (append-mostly, runs barely overlap):
most blocks are non-overlapping. Compaction degenerates to sequential
raw copy with occasional row-by-row merge at the overlap boundary.
CPU savings: proportional to the fraction of non-overlapping blocks
(often 90%+ for time-series).

## Migration / Compatibility

The .run file format is unchanged. All new metadata lives in .index2
files, which are rebuildable from .run (using dictionaries from vylog).

### Single runtime path

There is **one steady-state lookup path**: the block-based reader
using .index2 metadata. No permanent v1 page_info code path.

On recovery, if a run has .index but no .index2:
1. Read the .run file (decompressing pages with dict from vylog).
2. Build block metadata: boundary keys, fuse8 filters, PGM models,
   page offsets.
3. Write .index2 to disk.
4. Proceed with the block-based reader.

This rebuild happens once per legacy run, during the first recovery
after upgrade. Subsequent startups find .index2 already present.

```c
/* One lookup path -- always block-based */
block_no = block_dir_search(run->block_dir, key);
if (!fuse8_contains(&block->filter, key_hash))
    return NOT_FOUND;
page_no = pgm_or_min_keys_search(block, key);
/* pread + decompress + row search */
```

### Per-space format version

The format version is a **per-space property**, stored per-LSM in
vylog. It controls the **write path** (which format new dumps and
compactions produce), not the read path (which is always block-based
after .index2 rebuild).

- **New spaces** created on a v2-capable binary default to v2.
- **Existing spaces** remain v1 (8 KB pages, no dict) after upgrade
  until explicitly migrated via `space:alter()`.

### Migration via space:alter()

`space:alter({vinyl_format = 2})` switches the write format:

1. Sets the space's format version to v2 in vylog.
2. New dumps immediately use v2 (2 KB pages, dict compression).
3. Triggers major compaction to rewrite existing v1 runs with
   smaller pages and dictionary compression.
4. .index2 files are built for all new runs during dump/compaction.

The read path doesn't change -- it's already block-based (from
.index2 rebuild at recovery). The migration just changes what the
writer produces.

### Downgrade

Since .run format is unchanged, downgrade is straightforward:

1. `space:alter({vinyl_format = 1})` switches writes back to v1
   (8 KB pages, no dict).
2. Major compaction rewrites v2 runs to v1 format.
3. Drop .index2 files, old binary rebuilds .index from .run.

If dictionary compression is in use, the old binary must be able
to obtain the dictionary to rebuild .index from .run. Dictionaries
are persisted in vylog; old binaries that don't understand dict
vylog records will need `--force-recovery` to skip them and rebuild
.index without dict (re-reading uncompressed page data).

### .index2 as rebuildable metadata

.index2 is derived metadata, not primary data. It can always be
rebuilt from .run + dict (from vylog). This means:
- Corruption of .index2 is recoverable (rebuild from .run).
- .index2 format can evolve freely between versions (rebuild on
  upgrade if format changes).
- No need to version .index2 format carefully -- just rebuild.

## Open Questions

1. **PGM epsilon validation**: epsilon = 1 page (~20 rows for 100-byte
   rows). Validate with real data that this accuracy is achievable for
   typical key distributions after LCP-stripping. If not, epsilon = 2
   pages (5-page pread = 10 KB) is the fallback, still under 2 FS blocks.

2. **Future per-block analytics metadata**: zone maps (min/max per
   column), count/sum/null_count per column, HLL distinct count
   sketches. Requires predicate pushdown into the iterator API.
   Deferred to a future release; msgpack block metadata format is
   backward-compatible, and block metadata can be rebuilt from .run
   files on upgrade.

## Implementation Phases

### Phase 1: Dictionary compression + small pages (mostly done)
- Merge `kostja-vinyl-dict-compression` (shared/refcounted dict
  model already implemented, persisted in vylog).
- Adjust page target to 2048 bytes (uncompressed).
- Add per-space format version flag in vylog.
- No .run format changes (no padding).
- Validate against existing test suite -- minimal new tests needed.

### Phase 2: PGM library integration
- Add PGM-index as submodule or vendor the C interface
- Write thin C wrapper if needed (the library has `c-interface/`)
- Unit test: build PGM from sorted (normalized_min_key, page_no)
  pairs, query, verify bounds

### Phase 3: Block data structure
- Define `struct vy_block_info` (in-memory block directory entry):
  boundary_key, fuse8, PGM model, page offsets (200 x uint32),
  lsn/ttl ranges
- Block size = up to 200 pages (variable, capped by count or
  compressed size)
- Define on-disk block metadata format (.index2)
- Implement block directory construction during dump/compaction
- Implement block directory loading at boot/recovery

### Phase 4: PGM-based page offset lookup
- Build per-block PGM from page offsets during dump
- Serialize PGM model to disk (in block metadata)
- Replace `vy_page_index_find_page` with block directory + PGM
- Modify `vy_run_iterator` to use block + PGM for page access

### Phase 5: Fuse8 filters per block
- Port fuse8 from `kostja-vinyl-page-index` branch
- Build per-block fuse8 during dump
- Integrate fuse8 into point lookup path

### Phase 6: Block-overlap compaction skipping
- Adapt `vy_compaction_plan_trim()` from `kostja-vinyl-page-index`
  to use block boundary keys instead of MinHash/Jaccard
- For each pair of runs in a compaction plan, count overlapping
  blocks via binary search on block directories
- Skip compaction if overlap < 5%
- Forced compaction (`needs_compaction`) bypasses the check

### Phase 7: Block-level copy during compaction
- Add raw-copy path to write iterator: when a block has no overlap
  with other input runs AND uses the same dict as the output run,
  copy its pages as raw bytes from input .run to output .run
  (no decompress/re-encode)
- Copy block metadata (fuse8, PGM, offsets) to output .index2,
  adjusting page offsets for new file position
- Fall back to row-by-row merge for overlapping blocks or blocks
  with a different dict_id

### Phase 8: Range split adaptation
- Adapt fallback split to use median block's boundary_key
- Replace `first_page_no`/`last_page_no` with `first_block`/`last_block`
- Replace `referenced_pages` with `referenced_blocks`

### Phase 9: Format version + .index2 rebuild + alter space
- Add `vinyl_format` property to space options (default v2 for new)
- Store format version per-LSM in vylog
- Implement .index2 rebuild from .run for legacy runs at recovery
  (read pages with dict from vylog, build block metadata, write .index2)
- Remove v1 page_info runtime lookup path -- single block-based reader
- `space:alter({vinyl_format = 2})` switches write format + triggers
  major compaction to rewrite runs with 2 KB pages + dict compression
- `space:alter({vinyl_format = 1})` for downgrade (reverse)
- Integration test: upgrade with legacy runs, alter v1->v2->v1

### Phase 10: io_uring / direct I/O (future)
- Replace reader thread pool pread with io_uring submissions
- Batch read-ahead for range scans

## Key Files to Modify

- `src/box/vy_run.h` -- new block structures, page/block size constants
- `src/box/vy_run.c` -- dict compression, block-based read/write/recovery
- `src/box/vy_range.h` -- slice: page refs -> block refs
- `src/box/vy_range.c` -- split logic, bloat detection
- `src/box/vy_write_iterator.c` -- block-level copy during compaction
- `src/box/vy_scheduler.c` -- compaction with block structure
- New: `src/box/vy_pgm.h` / `src/box/vy_pgm.c` -- PGM C wrapper
- New: `src/box/vy_block.h` / `src/box/vy_block.c` -- block directory

## Verification

- Unit test: PGM build + query on synthetic offset arrays
- Unit test: per-run dictionary compression/decompression of small pages
- Unit test: block directory construction and lookup
- Integration: existing `vinyl/*.test.lua` suite (single block-based path)
- Integration: `vinyl/stat.test.lua` for memory accounting
- Recovery test: .index2 rebuild from legacy .run files
- Benchmark: point lookup latency (v1 8KB page vs v2 2KB page + PGM)
- Benchmark: range scan throughput
- Compaction test: block-overlap skip with < 5% overlap (time-series)
- Compaction test: block-level raw copy correctness (verify output
  identical to row-by-row merge)
- Compaction test: mixed dict_ids in output after raw copy
- Stress test: concurrent reads during compaction
- Migration test: v1 -> v2 upgrade via alter space, downgrade v2 -> v1
