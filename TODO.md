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
