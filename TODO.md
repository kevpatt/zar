# TODO

## App bundles

Using zar as a package format for `.app` bundles on a small OS, where a
loader reads the archive in place rather than unpacking it (much as Android
maps native libraries straight out of an APK).

1. **Store matching files uncompressed.** `--contiguous --align=4K` already
   gives each file a single page-aligned block, but zstd still compresses it
   if that helps. To `mmap` a library, it must be stored as-is: add
   something like `--store='*.so'` (or by extension).
2. **Reproducible builds.** An option to set every stored timestamp to a fixed
   value (like `SOURCE_DATE_EPOCH`), so packages are byte-for-byte
   reproducible. Entries are already sorted.
3. **Manifest convention.** Name, version, entry point, permissions: keep this
   as an ordinary file inside the bundle, by convention, rather than in the
   format.

## Signing

BLAKE3 block hashes and Ed25519 signatures are done (`--hash`, `--sign`,
`--verify`; FORMAT.md §5.6, §6.7). Left from PROPOSAL-signing.md:

- **CONTENT_HASH** (tag `0x0C`): an optional BLAKE3 hash of each file's
  whole content, the cryptographic counterpart of CONTENT_CRC, for checking
  an extracted file end to end or comparing files across archives chunked
  differently. It costs a second pass over everything.
- **Detached signatures** (`archive.zar.sig`, a SIGNATURE payload on its
  own) for archives that can't be rewritten. The signed message doesn't
  depend on where the block lives, so this needs no format change.
- **Rollback protection** is the application's job, but unzar could help:
  an option to require a SEQUENCE above a given number, or a NAME.

## Mounting

A read-only FUSE mount, which shows off random access better than anything:
`zar --mount ARCHIVE DIR` (or a separate `zarfs`), using libfuse 3 on Linux
and macFUSE or FUSE-T on macOS.

- Opening the archive reads only the index; each `read()` decompresses just
  the chunks it covers, through a small cache of decompressed chunks.
- Block hashes are checked as chunks are read, and `--verify=KEYFILE.pub`
  checks the signature before mounting, so a signed archive can be used in
  place without being unpacked.
- Holes read as zeros and report as holes (`SEEK_HOLE`); hard links share
  an inode number; owners, modes, and times come from the index.
- An optional dependency: built only if libfuse is found.

## Encryption

Signing proves who made an archive, but anyone can read it. Encryption with
AEAD per block fits the design, since every block is already independent:

- **Cipher:** XChaCha20-Poly1305, which Monocypher (already vendored)
  provides. Its 24-byte nonces can be random per block, so blocks stay
  valid when rezar copies them to a new offset.
- **Keys:** a random archive key, wrapped for each recipient: by a
  passphrase through Argon2id (also in Monocypher), or for X25519 public
  keys, so an archive can be made for someone without a shared secret.
- **What's covered:** compress, then encrypt. The index must be encrypted
  too, or file names and sizes leak. The block type and position belong in
  the associated data, so blocks can't be swapped.
- **With hashes and signatures:** hash the ciphertext, so anyone can check
  integrity and signatures without the key, and the reader rejects tampering
  before decrypting.
- **Leaks to decide on:** deduplication reveals which chunks are equal, and
  block sizes reveal roughly how big each file is. Possibly allow padding,
  and turn deduplication off (or key it) when encrypting.
- A format change, and one that needs outside review before anyone relies
  on it.

## Appending

rezar always writes a new archive. The format also allows appending (§5.3):
new blocks, a new index, and a new trailer after the old end, with the
header rewritten to point at the new root. Adding a file to a big archive
would then cost only the file, at the price of leaving replaced or deleted
data in place until the next full rewrite.

- `zar --append ARCHIVE PATH...`, or `rezar --in-place`, with the new index
  sharing the old entries' blocks.
- Deduplication against the existing blocks, as `rezar --dedup=deep` does.
- Only for a seekable file, and not for a solid archive. An archive with a
  head index (`--head-index`) loses it: the old copy is left stale, so the
  rewritten header must point at the new tail index, and the archive can no
  longer be read from a stream.
- A signed archive must be signed again, since its root changes. The old
  signature could be kept, or a new SIGNATURE field could name the previous
  root, giving a chain of signed versions.
- Safe against a crash midway: write everything new first and rewrite the
  header last, so a reader sees either the old archive or the new one.

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

## rezar

- **Solid archives.** rezar refuses them, since their data can only be
  copied by recompressing everything. It could do that when asked, or turn
  them into normal archives.
- **Several dictionaries.** rezar keeps one dictionary, the only kind zar
  writes; it refuses archives whose entries use more than one.

## Other

- **Windows.** CI (`.github/workflows/ci.yml`) covers Linux and macOS.
  Windows needs its own file and path handling, since zar uses POSIX APIs
  throughout, and then a CI job.
- **Content-defined chunking for deduplication.** Deduplication only
  matches whole chunks at fixed boundaries, so a copy with one byte
  inserted near the start shares nothing. Choosing chunk boundaries from
  the content (a rolling hash, as restic and borg do) would fix that. It needs a format
  change: every chunk but the last is `CHUNK_SIZE` bytes, so variable-length
  chunks need a new field listing each chunk's length.
- **Parallel file creation.** Extraction decompresses in parallel, but files
  and directories are created one at a time, which dominates for trees of
  many small files.
- **`--normalize=nfc|nfd`.** Unicode normalization of stored names, off by
  default. Needs a Unicode library (utf8proc is small).
- **Sub-indexes in the writer.** The reader supports them (§6.6); the writer
  always produces a single index. Worth it for enormous directories.
