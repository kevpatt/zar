# TODO

## App bundles

Using zar as a package format for `.app` bundles on a small OS, where a
loader reads the archive in place rather than unpacking it (much as Android
maps native libraries straight out of an APK).

1. **Store matching files uncompressed.** `--contiguous --align=4K` already
   gives each file a single page-aligned block, but zstd still compresses it
   if that helps. To `mmap` a library, it must be stored as-is: add
   something like `--store='*.so'` (or by extension).
2. **Cryptographic integrity.** CRC-32C only catches accidental damage. For
   apps from untrusted sources, add a cryptographic hash per block or per
   file, plus a signed index so one signature covers everything. Needs a new
   block type and new index fields in FORMAT.md.
3. **Reproducible builds.** An option to set every stored timestamp to a fixed
   value (like `SOURCE_DATE_EPOCH`), so packages are byte-for-byte
   reproducible. Entries are already sorted.
4. **Manifest convention.** Name, version, entry point, permissions: keep this
   as an ordinary file inside the bundle, by convention, rather than in the
   format.

## Dictionaries

`--dict` and `--dict-over-solid` are done. Possible improvements:

- **Compressed dictionaries.** A ZSTD_DICT block is stored raw, which counts
  heavily against small archives when compared with `--solid`. Compressing
  it needs a new block type (or a flag).
- **Several dictionaries.** Each entry's `DICT` field can name a different
  dictionary, so files could be grouped (by extension, say) with one
  dictionary per group.
- **Faster training.** `ZDICT_trainFromBuffer` is single-threaded; the
  fastCover API can use threads, but isn't exported by every libzstd build.

## Other

- **Continuous integration.** Linux has never been built or tested. A
  GitHub Actions workflow could build and test on Linux and macOS, with a
  sanitizer job, on every push. That would also cover the non-UTF-8 name
  test, which skips itself on macOS because APFS refuses such names.
- **Content-defined chunking for deduplication.** Deduplication only
  matches whole chunks at fixed boundaries, so a copy with one byte
  inserted near the start shares nothing. Choosing chunk boundaries from the content (a
  rolling hash, as restic and borg do) would fix that. It needs a format
  change: every chunk but the last is `CHUNK_SIZE` bytes, so variable-length
  chunks need a new field listing each chunk's length.
- **Parallel file creation.** Extraction decompresses in parallel, but files
  and directories are created one at a time, which dominates for trees of
  many small files.
- **`--normalize=nfc|nfd`.** Unicode normalization of stored names, off by
  default. Needs a Unicode library (utf8proc is small).
- **Sub-indexes in the writer.** The reader supports them (§6.6); the writer
  always produces a single index. Worth it for enormous directories.
