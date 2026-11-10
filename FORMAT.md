# The zar archive format

Version 0.6.0 (draft)

A zar archive is a sequence of **blocks** behind a fixed **zar header**.
Every block is self-describing and independently checksummed, and every
piece of file content lives in its own block (except the holes of sparse
files, which need none), so any file — or any chunk of a file — can be read
and decompressed without touching the rest of the archive. Optionally, the
index holds a cryptographic hash of every block it refers to, and a
signature over the root covers the whole archive (§5.6, §6.7).

## 1. Conventions

- All fixed-width integers are **little-endian**.
- `u8/u16/u32/u64` are unsigned; `i64` is two's-complement signed.
- **CRC** means CRC-32C (Castagnoli, polynomial `0x1EDC6F41`, reflected,
  init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`). The CRC of zero bytes is 0.
- **varint** is unsigned LEB128: 7 bits per byte, least significant group
  first, high bit set on all but the last byte. At most 10 bytes; the
  encoding must be minimal (no trailing `0x80`-only groups).
- **Offsets** are signed 64-bit byte offsets relative to the archive's
  **base** (§2.2). The value `-1` (all bits set) means "none", or in
  `CHUNKS`, a hole (§6.5); writers must never place a block at offset -1.
- "Must", "should", and "may" are used in the RFC 2119 sense.

## 2. File layout

```
+------------+---------+---------+-----+---------+-----------+
| zar header | block 0 | block 1 | ... | block N | [trailer] |
+------------+---------+---------+-----+---------+-----------+
   32 bytes    each block: 32-byte header + payload [+ padding]
```

The zar header is normally at the start of the file, but it may follow other
data, such as a self-extracting stub. Nothing in the archive depends on
where it is.

Blocks may appear in any order. A reader navigates only by offsets taken
from the zar header, the trailer, and the index; it never needs to scan.

### 2.1 Alignment

The zar header declares an alignment `A = 1 << align_log2`, where
`0 <= align_log2 <= 20`. The default is 8 bytes, so every multi-byte field
can be loaded as an aligned 64-bit word. Writers should use a smaller `A`
only when explicitly asked to, trading alignment for slightly tighter packing.

- Positions are measured from the zar header.
- Every block's **payload** begins at a multiple of `A`. Its 32-byte header
  sits immediately before it, so block headers share the payload's alignment
  up to 32 bytes.
- Gaps between blocks are filled with zero bytes.
- The trailer (§5) is aligned to `min(A, 8)`.

Larger alignments let a reader load headers and payloads straight into
aligned memory: 64 or 128 bytes matches common cache-line sizes, and 4096
lets stored data be `mmap`ed in place. For that to work, the zar header
itself must sit at a multiple of `A` in the file, which is always true when
it is at the start. Readers never compute padding; they simply follow
offsets, so every alignment is read identically.

### 2.2 Base and offsets

All offsets are relative to the archive's **base**:

```
base = position of the zar header + base_offset
```

`base_offset` is normally 0. It exists so a front index can be added after
the fact: a tool can insert `S` bytes of new blocks right after the zar header,
shift the rest of the archive forward by `S`, and add `S` to `base_offset`.
No stored offset changes. The inserted blocks lie before the base, so their
offsets are negative. `base_offset` must be a multiple of `A`, so inserting
blocks keeps every existing block aligned, and must be either 0 or at least
32: the base can't fall inside the zar header.

## 3. Zar header

Always 32 bytes. It has the same shape as a block header (§4): 28 bytes of
fields followed by a CRC of those 28 bytes.

| Offset | Type | Field         | Notes                                           |
|-------:|------|---------------|-------------------------------------------------|
| 0      | 8 B  | magic         | `89 5A 41 52 0D 0A 1A 0A` (`\x89ZAR\r\n\x1a\n`) |
| 8      | u16  | version_major |                                                 |
| 10     | u16  | version_minor |                                                 |
| 12     | u16  | version_patch |                                                 |
| 14     | u8   | flags         | none defined; must be 0                         |
| 15     | u8   | align_log2    | 0–20 (§2.1)                                     |
| 16     | i64  | index_offset  | the root index block, or -1 (§5)                |
| 24     | u32  | base_offset   | bytes from the zar header to the base (§2.2)    |
| 28     | u32  | header_crc    | CRC of bytes 0–27                               |

The magic borrows PNG's trick: the high-bit first byte and the CR/LF/^Z
sequence expose 7-bit and text-mode transfer damage immediately.

## 4. Blocks

### 4.1 Block header (32 bytes)

| Offset | Type | Field        | Notes                                      |
|-------:|------|--------------|--------------------------------------------|
| 0      | u16  | type         | §4.2                                       |
| 2      | u16  | flags        | none defined; must be 0                    |
| 4      | u32  | reserved     | must be 0                                  |
| 8      | u64  | payload_size | stored (possibly compressed) payload bytes |
| 16     | i64  | offset       | this block header's own offset (§2.2)      |
| 24     | u32  | payload_crc  | CRC of the payload (not the padding)       |
| 28     | u32  | header_crc   | CRC of bytes 0–27                          |

The payload follows immediately. Before trusting any other field, a reader
must verify `header_crc` and check that `offset` matches where it read the
header from. It must verify `payload_crc` before using the payload. Because a
block records its own offset, a recovery tool scanning a damaged archive at
`A`-byte steps can tell real block headers from data that merely looks like one.

### 4.2 Block types

| Value    | Name         | Payload                                            |
|---------:|--------------|----------------------------------------------------|
| `0x0000` | —            | invalid                                            |
| `0x0001` | INDEX_RAW    | index entries (§6), uncompressed                   |
| `0x0002` | INDEX_ZSTD   | one zstd frame decompressing to index entries      |
| `0x0003` | DATA_RAW     | file content, stored                               |
| `0x0004` | DATA_ZSTD    | one zstd frame decompressing to file content       |
| `0x0005` | ZSTD_DICT    | a zstd dictionary (as produced by `ZDICT_*`)       |
| `0x0006` | TRAILER      | §5                                                 |
| `0x0007` | ARCHIVE_ZSTD | one zstd frame decompressing to a whole archive (§5.5) |
| `0x0008` | SIGNATURE    | a signed statement naming the root index (§5.6)    |
| `0x8000`–`0xFFFF` | —   | reserved for private/experimental use              |

Every zstd frame in a zar archive:

- must include the content size in its frame header (except in an
  ARCHIVE_ZSTD block, whose writer may not know it in advance), and
- should omit zstd's own content checksum, which `payload_crc` makes redundant.

A reader must ignore blocks that nothing references, including ones with
unknown types.

## 5. Locating the root index

The root index can be referenced from two places: `index_offset` in the zar
header, and a **trailer** at the end of the archive. The **root** they
reference is usually the root index itself, but it may instead be an
ARCHIVE_ZSTD block (§5.5) or a SIGNATURE block naming the root index (§5.6).

### 5.1 Trailer

A trailer is a TRAILER block that ends the archive. It occupies the last
48 bytes and begins at a multiple of `min(A, 8)` from the zar header:

```
T+0   block header (type = TRAILER, payload_size = 16)
T+32  i64 index_offset     -- the root index block
T+40  u64 header_distance  -- bytes from T back to the zar header
```

A writer that can't seek back to fill in the header, for example because it
is writing to a pipe, sets the header's `index_offset` to -1 and writes a
trailer. Because the trailer records `header_distance`, a reader can find an
archive from the end of a file even when other data precedes it. Such a
reader checks the trailer's `header_crc`, follows `header_distance` to the
zar header, validates that, and then checks the trailer's `offset`.

### 5.2 Index copies

- If only one of the two references is present, it identifies the root index.
- If both are present, they must point to root indexes with identical
  contents. That is either one block referenced twice, or two copies, for
  example one at the front and one at the end. A reader should use the
  header's reference and fall back to the trailer's if that block fails
  verification.
- A copy of the root index alone does not protect sub-indexes (§6.6). A
  writer that wants full redundancy duplicates every sub-index block as
  well, and points each copy of the root at its own set.

### 5.3 Appending

A writer may append to an existing archive by writing new blocks, a new root
index, and a new trailer after the old end of the archive. It must also
rewrite the 32-byte zar header in place, setting `index_offset` to -1 or to
the new index. Superseded blocks, including the old trailer, become
unreferenced data. An appended archive has a new root, so a signed archive
must be signed again (§5.6).

### 5.4 Head index and streaming

A writer may put a copy of the root index before the base (§2.2), so the
zar header's `index_offset` is negative, and the trailer points to a second
copy at the end. Such an archive can be read from a stream, such as a pipe,
in a single forward pass:

1. Read the zar header and everything up to the base. That region holds the
   root index and, if the writer put them there, sub-indexes and
   dictionaries, so a reader can hold it in memory and look things up in it.
2. Read the data blocks that the index references, in increasing offset
   order, skipping anything in between.

Anything a streaming reader needs other than data blocks must lie before the
base. A writer can only produce a head index once it knows every data
block's offset, so a streaming writer has to stage the data (for example in
a temporary file) before emitting the archive.

### 5.5 Solid archives

The root block may be an ARCHIVE_ZSTD block instead of an index. Its payload
is a single zstd frame that decompresses to a complete zar archive (the
*inner* archive), with its own zar header, blocks, index, and trailer. A
reader decompresses it and then reads the inner archive in the usual way.

Compressing everything as one stream lets zstd exploit redundancy *between*
files, which matters most for many small, similar files. The price is random
access: nothing can be read without decompressing everything before it. A
writer should therefore store the inner archive's data and index
uncompressed, and use this form only for archives small enough that the
price doesn't matter.

- The inner archive must not itself be solid.
- The outer archive's `index_offset` should point to the ARCHIVE_ZSTD block,
  and it should directly follow the zar header, so the archive can be read
  from a stream (§5.4) with no further structure.
- The frame may omit its content size, so readers must be prepared to
  decompress without knowing the final size in advance.
- A solid archive can't be signed (§5.6): its frame would have to be
  decompressed before anything in it could be checked. Neither the outer
  nor the inner archive may have a SIGNATURE root.

### 5.6 Signatures

A signature covers the whole archive by signing the root index's hash. The
index holds the hash of every block it refers to (§6.7), sub-indexes
included, so verification runs down the tree, one block at a time, each
checked before it is decompressed or otherwise used.

**A signed archive's root is a SIGNATURE block.** The zar header's
`index_offset` and the trailer point at it, as they would at an ARCHIVE_ZSTD
block, and it names the root index. Readers that don't know the type
reject the archive, which is right, since they couldn't verify it. A
reader that knows the type but has no keys follows it to the index.

Its payload is a version byte, then fields encoded as index fields are
(§6.1: varint tag, varint length, value):

```
u8 version = 1
field*
```

| Tag    | Name      | Value                                              |          |
|-------:|-----------|----------------------------------------------------|----------|
| `0x01` | ROOT      | i64: the root INDEX_RAW or INDEX_ZSTD block        | required |
| `0x02` | ROOT_HASH | `u8 alg` + digest (§6.7): that block's block hash  | required |
| `0x03` | SEQUENCE  | u32 / u64: the publisher's release counter          | optional |
| `0x04` | TIME      | i32 / i64: seconds since the Unix epoch, when signed | optional |
| `0x05` | NAME      | bytes (UTF-8): what this is, such as a channel's name | optional |
| `0x10` | SIG       | `u8 sig_alg` + public key + signature              | one or more, **last** |

Each tag other than SIG may appear at most once. Unknown tags are skipped,
as in the index, but they are part of what is signed. Signature algorithms
(`sig_alg`):

| Value   | Algorithm | Public key | Signature |
|--------:|-----------|-----------:|----------:|
| 0       | invalid   |            |           |
| 1       | Ed25519 (RFC 8032, pure, not prehashed) | 32 bytes | 64 bytes |
| 2–127   | reserved  |            |           |
| 128–255 | private/experimental use | |      |

So an Ed25519 SIG field's value is 97 bytes long. A reader skips SIG fields
in algorithms it doesn't know.

**The signed message** is

```
"zar signature v1" || 0x00 || payload[0 .. first SIG field)
```

that is, the 16-byte context string, a NUL byte, and the payload from its
version byte up to, but not including, the first SIG field. Every SIG field
signs the same message, so several keys may sign one archive, for example
during a key rotation. No other field may follow a SIG field.

**Where SIGNATURE blocks go.** A writer puts one after the root index, so a
writer to a pipe can sign: the index's hash is known once the index is
written. With a head index (§5.4), there are two: one before the base,
after the head copy of the index, which the zar header's (negative)
`index_offset` points at, and one after the tail copy, which the trailer
points at. Each names its own copy of the root index (`ROOT` differs), and
the two must agree in every other field, including `ROOT_HASH`, since the
copies are identical. A reader uses the header's root and falls back to the
trailer's if that fails verification, as for unsigned index copies (§5.2).

**Signed archives must be complete.** In an archive whose root is a
SIGNATURE block, every FILE and HARDLINK with `CHUNKS` must have
`CHUNK_HASHES`, every `DICT` must have a `DICT_HASH`, and every `SUBINDEX`
must have a `SUBINDEX_HASH`, all in a hash algorithm the reader knows.
Readers reject a signed archive that breaks this rule, rather than skipping
the check for the entries without hashes, which would leave holes in the
signature's coverage.

**Verifying.** A reader that requires a signature:

1. Reads the root reference and the SIGNATURE block, checking its CRCs as
   for any block.
2. Checks that a SIG field has a key the caller accepts and a valid
   signature of the message. Which keys to accept is the caller's decision:
   a reader must never trust a key just because the archive names it.
3. Reads the ROOT block and checks its block hash against ROOT_HASH before
   decompressing it.
4. For each entry it uses: each sub-index on its path against
   `SUBINDEX_HASH`, the dictionary against `DICT_HASH`, and each chunk it
   reads against `CHUNK_HASHES`, each before decompressing it.
5. Checks CONTENT_CRC as usual.

A reader that extracts one file verifies only that file's blocks and the
indexes on its path. A reader that doesn't require a signature should still
check the hashes it meets, and should say that the signature went
unchecked.

**What the format leaves to the application:** which keys to trust;
**rollback** (a valid signature on an old archive stays valid, so an
updater that must not go backwards compares SEQUENCE or TIME with the
newest it has installed); and what NAME must be, so that an archive signed
for one channel isn't accepted on another.

Copying blocks elsewhere, as an archive editor does, keeps every block hash
valid, since a block's hash doesn't depend on its offset. Only the
signature is lost, because the copy has a new root, which must be signed
again.

## 6. Index

An index block's (decompressed) payload is a concatenation of **entries**,
running to the end of the payload. There is no count and no ordering
requirement, though writers should emit entries in depth-first pre-order.

### 6.1 Entry encoding

```
u8      entry_type
u8      entry_flags
varint  body_length        -- bytes of fields that follow
field*  (body_length bytes)
```

Each field is:

```
varint  tag
varint  length
u8[length] value
```

Readers must skip entries whose `entry_type` they don't recognize, and fields
whose `tag` they don't recognize. That is what lets minor versions add new
entry types and metadata without breaking older readers.

### 6.2 Entry types

| Value | Name     | Meaning                                                |
|------:|----------|--------------------------------------------------------|
| 1     | FILE     | regular file                                           |
| 2     | DIR      | directory                                              |
| 3     | SYMLINK  | symbolic link; target in `LINK_TARGET`                 |
| 4     | HARDLINK | regular file sharing content with another entry        |

### 6.3 Entry flags

| Bit | Name          | Meaning                                                |
|----:|---------------|--------------------------------------------------------|
| 0   | PATH_RAW      | `PATH` (and `LINK_TARGET`, if present) is raw bytes, not valid UTF-8 |
| 1–7 | —             | reserved, must be 0                                    |

### 6.4 Numeric fields

Numeric values are little-endian with a width given by the field's
`length`. Unsigned fields may be 4 or 8 bytes; signed fields (`i`) are 4 or
8 bytes, sign-extended. Writers should use 4 bytes whenever the value fits
and 8 bytes only when it doesn't, so a 32-bit size is the default and a
64-bit size is used only when needed. `MODE` is always 2 bytes, the
`*_NSEC` fields are always 4 bytes, and offsets (`CHUNKS`, `DICT`,
`SUBINDEX`) are always 8 bytes each. Hash fields are an algorithm byte
followed by digests (§6.7).

### 6.5 Fields

| Tag    | Name          | Value               | Applies to          |
|-------:|---------------|---------------------|---------------------|
| `0x01` | PATH          | bytes               | all — **required**  |
| `0x02` | SIZE          | u32 / u64           | FILE, HARDLINK      |
| `0x03` | CHUNK_SIZE    | u32 / u64           | FILE, HARDLINK      |
| `0x04` | CHUNKS        | i64 × n (-1: hole)  | FILE, HARDLINK      |
| `0x05` | DICT          | i64                 | FILE, HARDLINK      |
| `0x06` | CONTENT_CRC   | u32                 | FILE, HARDLINK      |
| `0x07` | LINK_TARGET   | bytes               | SYMLINK, HARDLINK   |
| `0x08` | SUBINDEX      | i64                 | DIR                 |
| `0x09` | CHUNK_HASHES  | `u8 alg` + digest × n | FILE, HARDLINK    |
| `0x0A` | DICT_HASH     | `u8 alg` + digest   | FILE, HARDLINK      |
| `0x0B` | SUBINDEX_HASH | `u8 alg` + digest   | DIR                 |
| `0x10` | MODE          | u16                 | all                 |
| `0x11` | UID           | u32 / u64           | all                 |
| `0x12` | GID           | u32 / u64           | all                 |
| `0x13` | UNAME         | bytes (UTF-8)       | all                 |
| `0x14` | GNAME         | bytes (UTF-8)       | all                 |
| `0x20` | MTIME         | i32 / i64           | all                 |
| `0x21` | ATIME         | i32 / i64           | all                 |
| `0x22` | CTIME         | i32 / i64           | all                 |
| `0x23` | BTIME         | i32 / i64           | all                 |
| `0x24` | MTIME_NSEC    | u32                 | all                 |
| `0x25` | ATIME_NSEC    | u32                 | all                 |
| `0x26` | CTIME_NSEC    | u32                 | all                 |
| `0x27` | BTIME_NSEC    | u32                 | all                 |

Each tag may appear at most once per entry.

- **PATH** — see §7.
- **SIZE** — the file's uncompressed length. Required if `CHUNKS` is present;
  if both are absent the file is empty.
- **CHUNK_SIZE / CHUNKS** — the content is split into consecutive chunks of
  `CHUNK_SIZE` bytes (the last may be shorter), and `CHUNKS` lists, in order,
  the offsets of the DATA_RAW or DATA_ZSTD block holding each one. Each chunk
  is compressed (or stored) independently, so byte `x` of a file lives in
  chunk `x / CHUNK_SIZE`. The number of chunks must equal
  `ceil(SIZE / CHUNK_SIZE)`, and each chunk's decompressed length must match
  what that implies. `CHUNK_SIZE` may be omitted when there is exactly one chunk.
  Data blocks may be shared: any number of chunks, in the same entry or in
  others, may reference one block holding identical data.

  An offset of -1 marks a **hole**: the chunk is all zeros, and no block
  holds it. Holes let a sparse file be stored, and extracted, without
  reading or writing its unallocated ranges. A writer should store a chunk
  as a hole when it lies entirely within one of the file's holes, and may
  for any chunk of zeros. An extractor may leave a hole unallocated, or
  write the zeros.
- **DICT** — offset of the ZSTD_DICT block needed to decompress this entry's
  DATA_ZSTD chunks. Since this identifies the dictionary, writers may omit
  the dictionary ID from those frames' headers.
- **CONTENT_CRC** — CRC of the file's entire uncompressed content, for
  end-to-end verification. It covers the zeros of any holes.
- **LINK_TARGET** — for SYMLINK, the link text exactly as `readlink(2)`
  returned it. For HARDLINK, the PATH (relative to the archive root) of the
  entry it is linked to.
- **SUBINDEX** — offset of an INDEX block whose entries live under this
  directory (§6.6).
- **CHUNK_HASHES** — one digest per `CHUNKS` offset, in the same order: the
  block hash (§6.7) of the block that offset references. A hole's digest is
  all zeros, and readers check that it is. The number of digests must equal
  the number of `CHUNKS`. A block shared by several chunks or entries has
  the same digest in each place.
- **DICT_HASH** — the block hash of the ZSTD_DICT block `DICT` references.
  A dictionary changes what every chunk decompresses to, so it is covered
  too.
- **SUBINDEX_HASH** — the block hash of the INDEX block `SUBINDEX`
  references, so a signature on the root covers every sub-index below it.
- **MODE** — permission bits (`07777`). The file type is given by
  `entry_type`, never by `MODE`.
- **UID/GID, UNAME/GNAME** — ownership. Either form, both, or neither may be
  stored.
- **MTIME, ATIME, CTIME, BTIME** — modification, access, status-change, and
  creation (birth) time, in seconds since the Unix epoch.
- **MTIME_NSEC … BTIME_NSEC** — the sub-second part of the matching time, in
  nanoseconds (0–999,999,999). Each is only meaningful alongside its
  seconds field, and the full time is `seconds + nsec / 10^9`, so a reader
  that ignores these fields still gets the correct time to the second.
  Writers omit them unless asked.

A HARDLINK entry carries the same `SIZE`, `CHUNK_SIZE`, `CHUNKS`, and `DICT`
(and their hashes, if any) as its target, so the data is stored once and the
entry can be extracted on its own. Extractors may instead create a real hard
link with `link(2)`.

### 6.6 Sub-indexes

A DIR entry with a `SUBINDEX` field delegates part of its contents to another
index block. Paths in that block are relative to the directory. Sub-indexes
may nest. They exist to keep enormous directories from being loaded or
rewritten all at once; they are always optional, and a writer may put an
entire tree in a single index.

The indexes reachable from a root index must form a tree: each index block
may be referenced by at most one `SUBINDEX` field, and never by an entry
inside itself or inside any index it leads to. Readers must reject an
archive that breaks this rule. The rule rules out cycles, and it also rules
out shared sub-indexes, which would let a small archive expand into an
exponentially large listing. Readers may also limit how deeply sub-indexes
nest.

A path may still name a deeper descendant directly (`a/b/c` in the root
index) even if `a` has a sub-index. If two entries resolve to the same path,
the one encountered later in a depth-first walk of the indexes wins.

### 6.7 Hashes

An archive may hold a cryptographic hash of every block its contents depend
on, so a reader can detect tampering, which CRC-32C doesn't: CRCs catch
accidental damage, and anyone can recompute them. Hashes are optional in an
unsigned archive, and required in a signed one (§5.6).

```
block_hash(B) = HASH( type(B) as u16 LE  ||  payload(B) )
```

A block's hash covers its `type` and its stored payload, exactly the
`payload_size` bytes after the header, without padding. Including the type
stops a DATA_ZSTD block from being presented as DATA_RAW, or the reverse,
with the same bytes. The offset, size, and CRCs are not hashed, so a block
copied elsewhere keeps its hash.

Every hash field starts with an algorithm byte:

| Value   | Algorithm  | Digest   |
|--------:|------------|---------:|
| 0       | invalid    |          |
| 1       | BLAKE3-256 | 32 bytes |
| 2–127   | reserved   |          |
| 128–255 | private/experimental use | |

A reader that meets a hash in an algorithm it doesn't know treats it as
absent, so it can still read an unsigned archive, but it rejects a signed
archive that uses one (§5.6). A reader that knows the algorithm checks each
block against its hash before decompressing it or otherwise using it.

## 7. Paths

Paths are relative; the writer decides what they are relative to, and
extractors place them under a destination directory. For example, `zar`
stores a directory `/home/me/proj` as `proj`, `proj/src/main.cpp`, and so
on, so extracting recreates `proj`; given `proj/.`, it stores only the
contents (`src/main.cpp`). Absolute paths are never stored.

A path:

- uses `/` as the separator and may contain multiple components;
- must not be empty, start with `/`, end with `/`, or contain an empty, `.`,
  or `..` component;
- must not contain a NUL byte;
- is UTF-8 and is stored without Unicode normalization, unless `PATH_RAW`
  is set, in which case it is arbitrary bytes satisfying the rules above.

Extractors must reject paths that break these rules. They must also never
create or write anything through a symbolic link, including links created
earlier while extracting the same archive.

## 8. Versioning

`major.minor.patch` in the zar header follows these rules:

- **major** — bumped for any change an older reader could misinterpret.
  Readers must reject a major version they don't support.
- **minor** — bumped for additions an older reader can safely ignore: new
  block types, entry types, or field tags. Readers must accept any minor
  version within their supported major.
- **patch** — clarifications only; it has no effect on compatibility.

While the major version is 0, the format is unstable and no compatibility
is promised, and a minor version may add things older readers must reject.

Changes in recent versions:

- **0.6.0** — hashes and signatures: the CHUNK_HASHES, DICT_HASH, and
  SUBINDEX_HASH fields (§6.5, §6.7), which older readers skip, and the
  SIGNATURE block (§5.6), which as a root older readers reject.
- **0.5.0** — holes: a `CHUNKS` offset of -1 is a chunk of zeros with no
  block (§6.5). Readers of earlier versions reject such entries.
- **0.3.6** — `base_offset` must be 0 or at least 32 (§2.2).
