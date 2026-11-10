# Proposal: BLAKE3 hashes and Ed25519 signatures

Status: **implemented** in format 0.6.0 (zar 0.7.0), as specified in FORMAT.md
§5.6 and §6.7, with these differences:

- **CONTENT_HASH** (§4) is left for later (TODO.md).
- **Head and tail SIGNATURE blocks** (§5.2) are not identical: each names its
  own copy of the root index in ROOT, and they agree in every other field.
  Identical blocks would have had to name the same copy, losing the fallback
  from a damaged head copy to the tail's.
- **Solid archives can't be signed:** a signed solid archive would have to be
  decompressed before anything in it could be checked.
- **Readers check hashes whenever an archive has them,** not only when
  verifying a signature, and reject a signed archive that uses a hash
  algorithm they don't know.

This is TODO.md's item 2, "Cryptographic integrity": a cryptographic hash for every block an
archive's contents depend on, and a signature over the root, so that one signature covers
everything. CRC-32C stays as it is. It catches accidental damage cheaply, and the new hashes catch
tampering.

## 1. Goals

- **One signature covers the whole archive.** It signs the root index's hash. The index holds the
  hash of every block it references, sub-indexes included, so verification runs down the tree.
- **Verification is per block and never needs decompression.** A block's hash covers its stored
  bytes, so a reader that fetches one file, perhaps over HTTP Range requests, checks just that
  file's blocks, and checks each one before decompressing it. Nothing unauthenticated reaches zstd.
- **Block copying keeps every hash valid.** rezar, and anything else that copies blocks as they are,
  carries the hashes over unchanged. Only the signature is lost, since the result has a new root.
- **Old archives and old readers are unaffected.** The hash fields are new index tags, which older
  readers skip (§6.1). Only a signed archive changes the root, and it does so the way solid archives
  already do (§5.5).
- **The hash is BLAKE3-256, but the format names it.** Every hash field starts with an algorithm
  byte, so a second algorithm can be added later without a new format.

What this proposal does not do: encryption, key distribution, or trust policy (which keys a reader
accepts, and whether it accepts an older archive than one it has seen). Those belong to the
application. §7 says what the format provides for them.

## 2. Why BLAKE3

- **256 bits out, 128-bit security** for collisions and preimages, the same collision strength as
  SHA-256.
- **Fast in software.** That matters because none of the RISC-V cores this was written for (Andes
  A25, SiFive X280, T-Head C920) is known to have SHA-256 instructions (Zknh, Zvknh). Scalar BLAKE3
  is roughly 3x faster than SHA-256, and BLAKE3 works on 32-bit words, which suits RV32.
- **A tree of 1 KiB chunks,** so one block's hash can use SIMD lanes or several cores: a 1 MiB chunk
  is 1,024 independent leaves. SHA-256 is one sequential chain.
- **No length extension.**

XXH3 stays zar's deduplication hash. It is not cryptographic: nothing in its design resists someone
choosing inputs to collide, so it can't back a signature.

## 3. The block hash

```
block_hash(B) = BLAKE3-256( type(B) as u16 LE  ||  payload(B) )
```

The hash covers the block's `type` and its stored payload, exactly the `payload_size` bytes after
the header, without the padding. Including `type` stops a DATA_ZSTD block from being presented as
DATA_RAW, or the reverse, with the same bytes. `offset`, `payload_size` and the CRCs are not hashed:
the offset is where the reference points, the size is the payload's length, and neither changes
when a block is copied elsewhere.

## 4. New index fields

| Tag    | Name          | Value                              | Applies to      |
|-------:|---------------|------------------------------------|-----------------|
| `0x09` | CHUNK_HASHES  | `u8 alg` + digest × n              | FILE, HARDLINK  |
| `0x0A` | DICT_HASH     | `u8 alg` + digest                  | FILE, HARDLINK  |
| `0x0B` | SUBINDEX_HASH | `u8 alg` + digest                  | DIR             |
| `0x0C` | CONTENT_HASH  | `u8 alg` + digest                  | FILE, HARDLINK  |

Hash algorithms (`alg`):

| Value | Algorithm | Digest |
|------:|-----------|-------:|
| 0     | invalid   |        |
| 1     | BLAKE3-256 | 32 bytes |
| 2–127 | reserved |       |
| 128–255 | private/experimental use | |

- **CHUNK_HASHES** has one digest per `CHUNKS` offset, in the same order: the block hash (§3) of the
  block that offset references. A hole (offset -1) has a digest of 32 zero bytes, and readers check
  that it does. The field's length must be `1 + 32 × n`, where `n` is the number of `CHUNKS`. A
  block shared by several chunks or entries simply has the same digest in each place.
- **DICT_HASH** is the block hash of the ZSTD_DICT block that `DICT` references. A dictionary changes
  what every chunk decompresses to, so it must be covered as well.
- **SUBINDEX_HASH** is the block hash of the INDEX block that `SUBINDEX` references, so a signature
  on the root covers every sub-index below it.
- **CONTENT_HASH** is optional: BLAKE3-256 of the file's whole uncompressed content, holes included
  as zeros. It is the cryptographic counterpart of CONTENT_CRC, for checking an extracted file end to
  end, or for comparing files across archives without the archives' block layout. Writers add it only
  when asked, since it means hashing everything a second time.

A writer may add hashes to an unsigned archive (`zar --hash`): rezar keeps them, and they make an
unsigned archive ready to be signed later without rehashing anything.

## 5. The signature

### 5.1 The SIGNATURE block

| Value    | Name      | Payload |
|---------:|-----------|---------|
| `0x0008` | SIGNATURE | a signed statement and one or more signatures (below) |

**A signed archive's root is its SIGNATURE block.** The zar header's `index_offset` and the trailer
both point at it, as they would point at an ARCHIVE_ZSTD block (§5.5), and it names the real root
index. This is the one change older readers notice: they see a root type they don't know and
reject the archive. That is correct, because they couldn't verify it. A reader that knows the type
but has no keys simply follows the SIGNATURE block to the index.

Its payload is a version byte, then fields encoded as index fields are (§6.1: varint tag, varint
length, value):

```
u8 version = 1
field*
```

| Tag    | Name      | Value | |
|-------:|-----------|-------|---|
| `0x01` | ROOT      | i64: the root INDEX_RAW or INDEX_ZSTD block | required |
| `0x02` | ROOT_HASH | `u8 alg` + digest: that block's block hash | required |
| `0x03` | SEQUENCE  | u64: the publisher's release counter | optional |
| `0x04` | TIME      | i64: seconds since the Unix epoch, when signed | optional |
| `0x05` | NAME      | bytes (UTF-8): what this is, e.g. a channel name | optional |
| `0x10` | SIG       | `u8 sig_alg` + public key + signature | one or more, **last** |

Signature algorithms (`sig_alg`): 1 = Ed25519 (RFC 8032, pure, not prehashed), with a 32-byte public
key and a 64-byte signature, so the field is 97 bytes long. Values 2–127 are reserved and 128–255
are for private use.

**The signed message** is

```
"zar signature v1" || 0x00 || payload[0 .. first SIG field)
```

that is, the context string, a NUL, and the payload's bytes from the version byte up to, but not
including, the first SIG field. Every SIG field signs the same message, so several keys may sign
one archive, for example during key rotation. A SIG field may not be followed by any other kind of
field. Unknown non-SIG tags are skipped as usual, but they are inside the signed bytes.

### 5.2 Where it goes

- **At the end:** the SIGNATURE block, then the trailer pointing at it. A writer to a pipe can do
  that, because the root index's hash is known once the index has been written.
- **At the front (§5.4):** for streaming, the SIGNATURE block goes before the base with the head
  index, and the zar header's negative `index_offset` points at it.
- **Both:** the two SIGNATURE blocks must be identical, as two copies of a root index must be (§5.2).

### 5.3 Signed archives must be complete

In an archive whose root is a SIGNATURE block, every FILE and HARDLINK with `CHUNKS` must have
CHUNK_HASHES, every `DICT` must have a DICT_HASH, and every `SUBINDEX` must have a SUBINDEX_HASH.
Readers reject a signed archive that breaks this rule, rather than skipping the check for the
entries that have no hash. Otherwise one missing field would leave a hole in the signature's coverage.

## 6. Verifying

A reader that requires a signature:

1. Reads the root reference (header or trailer) and the SIGNATURE block, checking its CRCs as for
   any block.
2. Checks that one of the SIG fields has an accepted key and a valid signature over the message
   (§5.1). Which keys are accepted is the caller's decision.
3. Reads the ROOT block and checks its block hash against ROOT_HASH **before decompressing it**.
4. For each entry it uses: if the entry came through a SUBINDEX, checks that sub-index block's hash
   before decompressing it (the same as step 3, one level down). For each chunk block it reads, it
   checks the block hash against CHUNK_HASHES before decompressing, and checks the DICT block
   against DICT_HASH before loading it.
5. Checks CONTENT_CRC as usual, and CONTENT_HASH if the entry has one and the caller asked for it.

A reader that extracts one file verifies only that file's blocks and the indexes on its path.
`unzar --scan` on a signed archive also checks every hash, still without decompressing.

A reader that doesn't require a signature but knows the SIGNATURE type may skip steps 2–4, and
should then say that the signature went unchecked.

## 7. What the application decides

- **Which keys to trust.** The format carries the public key in each SIG field, but a reader must
  accept only keys its caller supplied. Never trust a key just because the archive names it.
- **Rollback.** A valid signature on an old archive is still valid. An updater that must not go
  backwards compares SEQUENCE, or TIME, with the newest it has installed, and refuses anything older.
  The format signs both, but only the application can remember what it saw last.
- **What NAME must be,** so that a signed archive from one channel isn't accepted on another.

## 8. Tools

- `zar --hash`: write the hash fields (CHUNK_HASHES, DICT_HASH, SUBINDEX_HASH).
  `--hash=content` also writes CONTENT_HASH.
- `zar --sign=KEYFILE [--sequence=N] [--name=TEXT]`: implies `--hash` and makes the root a SIGNATURE
  block. `--sign` may be given several times. TIME is the signing time unless
  `SOURCE_DATE_EPOCH` is set (TODO item 3).
- `zar --keygen=KEYFILE`: a new Ed25519 key pair. It writes `KEYFILE` (secret, mode 0600) and
  `KEYFILE.pub`.
- `unzar --verify=PUBFILE` (repeatable): require a valid signature from one of these keys, and check
  every hash on the way. `unzar --list --verbose` shows whether an archive is signed, its SEQUENCE,
  TIME, NAME and the keys' fingerprints.
- `rezar` keeps every hash field and drops the signature: the result has a new root. `--sign=KEYFILE`
  signs the result. With `--verify=PUBFILE`, rezar checks the source archive first and then every
  block it copies.

**Libraries:** BLAKE3's reference C implementation (CC0 / Apache-2.0, with SIMD paths that can be
switched off) and Monocypher for Ed25519 (BSD-2-Clause / CC0: small, portable, no allocation, so it
also fits a freestanding reader). Both go in `third_party/`, beside zstd.

## 9. Cost

- **Index size:** 33 bytes per single-chunk file (one algorithm byte and a 32-byte digest), plus
  32 bytes for each further chunk. With 1 MiB chunks that is about 0.003% of the content, and for an
  archive of small files the index grows by roughly a third to a half (an entry is typically
  70–100 bytes without them).
- **Writing:** one BLAKE3 pass over the stored bytes, which are already in memory after compressing.
- **Reading:** one BLAKE3 pass over each block read, before it is decompressed. For a compressed
  block that is fewer bytes than decompression produces.

## 10. Versioning

This would be format **0.6.0**: new field tags (ignored by older readers) and a new root block type
(rejected by older readers, as a solid archive's ARCHIVE_ZSTD root is). Archives without
`--sign` stay readable by 0.5 readers, hash fields and all.

## 11. Open questions

- **CONTENT_HASH by default?** It costs a second full pass, but it is what allows comparing files
  across archives that were chunked differently.
- **A detached signature** (`archive.zar.sig`, holding just a SIGNATURE block's payload) for
  archives that can't be rewritten. It's easy to add later, because the signed message doesn't
  depend on where the block lives.
- **Signing appended archives (§5.3):** each append writes a new root, so it needs a new signature.
  That falls out of the design, but rezar's future in-place appending should say so.
