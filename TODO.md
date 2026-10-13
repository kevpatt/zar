# TODO

## App bundles

Using zar as a package format for `.app` bundles on a small OS, where a
loader reads the archive in place rather than unpacking it (much as Android
maps native libraries straight out of an APK).

1. **Store files as one contiguous block.** To `mmap` a whole library, it must
   be a single uncompressed, page-aligned block, not a series of chunks with
   block headers in between. Add something like `--store='*.so'` (or by
   extension) that stores matching files uncompressed, unchunked, and aligned
   (with `--align=4K`).
2. **Cryptographic integrity.** CRC-32C only catches accidental damage. For
   apps from untrusted sources, add a cryptographic hash per block or per
   file, plus a signed index so one signature covers everything. Needs a new
   block type and new index fields in FORMAT.md.
3. **Dictionaries.** Bundles often hold many small resources. `--solid`
   compresses those well but gives up random access and `mmap`; a trained
   zstd dictionary (ZSTD_DICT blocks, `DICT` fields) keeps both. The reader
   already supports dictionaries; the writer needs to train and use them.
4. **Reproducible builds.** An option to set every stored timestamp to a fixed
   value (like `SOURCE_DATE_EPOCH`), so packages are byte-for-byte
   reproducible. Entries are already sorted.
5. **Manifest convention.** Name, version, entry point, permissions: keep this
   as an ordinary file inside the bundle, by convention, rather than in the
   format.

## Dictionary-compressed alternative to `--solid`

A solid archive compresses well because zstd sees matches across files, but
it can't be browsed or extracted piecemeal without decompressing everything
before the wanted file. A shared dictionary could recover most of that gain
while keeping a normal, random-access archive:

1. While building the solid candidate, also train a zstd dictionary on the
   same file data. `ZDICT_trainFromBuffer` takes the data as one concatenated
   buffer plus a list of sample sizes, so use the files (or their chunks) as
   the samples. Training can run alongside the solid compression.
2. Before writing anything, also build a normal archive whose files are
   compressed with that dictionary, stored once in a ZSTD_DICT block.
3. Keep whichever result is best: the normal archive, the dictionary archive,
   or the solid one. Prefer the dictionary archive when it's within some
   margin of the solid size, since it keeps random access. The margin is set
   with `--dict-over-solid=PERCENT`, default 120: the dictionary archive is
   kept if it is at most 120% of the solid size. If neither the dictionary
   archive nor the solid one fits the `--solid` limit, write the normal
   archive.

The format already supports this, so it is writer-only work. Each FILE entry's
`DICT` field holds the offset of its ZSTD_DICT block, so no separate
dictionary ID is needed. Different groups of files can even use different
dictionaries. A zstd frame also records its dictionary's ID in its header,
which a reader could cross-check. Alternatively, the writer could leave the ID
out of each frame to save 4 bytes per chunk, since `DICT` is authoritative.

Caveats:

- A trained dictionary is small, typically 100 KB or less. It captures common
  content, not long cross-file matches, so on sets of large or
  near-duplicate files it will fall short of solid.
- The dictionary block counts toward the archive's size. For tiny inputs it
  can cost more than it saves.
- With `--head-index`, the ZSTD_DICT block must go before the base so a
  streaming reader has it before the data (§5.4).

This overlaps with App bundles item 3; the training code would serve both.

## Other

- **Hard links on extraction.** HARDLINK entries are extracted as independent
  copies; use `link(2)` where the filesystem allows.
- **Parallel file creation.** Extraction decompresses in parallel, but files
  and directories are created one at a time, which dominates for trees of
  many small files.
- **`--normalize=nfc|nfd`.** Unicode normalization of stored names, off by
  default. Needs a Unicode library (utf8proc is small).
- **Sub-indexes in the writer.** The reader supports them (§6.6); the writer
  always produces a single index. Worth it for enormous directories.
- **Non-UTF-8 name tests on Linux.** APFS refuses such names, so
  `writer_handles_non_utf8_names` skips itself on macOS.
