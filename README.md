# zar

[![CI](https://github.com/kevpatt/zar/actions/workflows/ci.yml/badge.svg)](https://github.com/kevpatt/zar/actions/workflows/ci.yml)

**Random-access archives, compressed with [zstd](https://github.com/facebook/zstd).**

`zar` packs a file or directory into a single `.zar` archive; `unzar` lists,
verifies, and extracts it. Unlike a `.tar.zst`, which is one long compressed
stream, a zar archive is compressed piece by piece: every file, and every
chunk of a large file, is its own independently checksummed block, and an
index records where each one lives. Any file can be listed, verified, or
extracted without decompressing anything else.

```sh
$ zar ~/src/project
$ unzar --list -h project.zar
project/
  README.md	5.7K	2026-10-13 09:22:27
  ...
$ unzar project.zar project/src/main.cpp
```

> **Status:** development (0.5.2). Both tools work and are well
> tested, but the [archive format](FORMAT.md) may still change before 1.0.

## Contents

- [Features](#features)
- [How it compares](#how-it-compares)
- [Installing](#installing)
- [Building](#building)
- [Using zar](#using-zar)
- [Using unzar](#using-unzar)
- [Solid archives](#solid-archives)
- [Dictionaries](#dictionaries)
- [Deduplication](#deduplication)
- [Sparse files](#sparse-files)
- [Streaming through pipes](#streaming-through-pipes)
- [Safety](#safety)
- [The archive format](#the-archive-format)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)
- [License](#license)

## Features

- **Random access.** Read any file, or any 1 MiB chunk of a large file,
  without touching the rest of the archive.
- **Integrity everywhere.** Every block has a CRC-32C of its header and of its
  payload, and every file a CRC of its whole content. `unzar --scan` checks
  an archive's structure and checksums without decompressing anything;
  `--test` decompresses and verifies everything.
- **Fast.** Compression and extraction run chunk by chunk on every core.
  CRC-32C uses the CPU's CRC instructions on ARM64 and x86.
- **Streaming.** `zar` can write to a pipe, and with `--head-index`, `unzar`
  can extract from one, file by file, as the data arrives.
- **Solid mode for small archives.** `--solid=SIZE` compresses everything as
  one stream when that's worth it, often 15–40% smaller for many small,
  similar files, and falls back to a normal archive when it isn't.
- **Trained dictionaries.** `--dict` trains a zstd dictionary on the files
  and compresses them with it: most of solid mode's gain on small, similar
  files, while keeping random access. With `--solid`, zar tries both and
  prefers the dictionary archive if it's nearly as small.
- **Deduplication.** Identical chunks are stored once, by default, so
  duplicate files (or repeated chunks, like runs of zeros in disk images)
  cost almost nothing. Candidates are found by an XXH3-128 hash, and confirmed by
  comparing bytes before a block is reused.
- **Sparse files.** Holes in sparse files, such as disk images, are
  recorded rather than read and stored, and `unzar` recreates them as
  holes, so a mostly empty 100 GB image doesn't take 100 GB to extract.
- **Alignment.** `--align` places payloads on cache-line or page boundaries,
  and `--contiguous` keeps each file in a single block, so stored data can be
  `mmap`ed straight from the archive.
- **Safe extraction.** No absolute paths, no `..`, no writing through
  symlinks, nothing overwritten without `-y`, and an optional cap on how
  much will be decompressed.
- **Predictable inputs.** `proj`, `proj/` and `proj/.` follow POSIX path
  resolution, so it's always clear what gets stored.
- **Deterministic.** Entries are stored in sorted order, so archiving the
  same files with the same options gives the same archive.
- **Open format.** The format is [fully specified](FORMAT.md) and small
  enough to reimplement.

## How it compares

|  | zar | tar + zstd | zip |
|---|---|---|---|
| Extract one file without decompressing others | ✅ | ❌ | ✅ |
| Random access *within* a large file | ✅ (per chunk) | ❌ | ❌ |
| Compression across similar small files | ✅ with `--solid` or `--dict` | ✅ | ❌ |
| Duplicate files and data stored once | ✅ (by default) | ⚠️ only within zstd's window | ❌ |
| Sparse files kept sparse | ✅ (per chunk) | ✅ GNU tar `--sparse`, bsdtar | ❌ |
| Checksums | every block, and every file | whole stream (optional) | every file |
| Verify without decompressing | ✅ `--scan` | ❌ | ❌ |
| Write to a pipe | ✅ | ✅ | ⚠️ limited |
| Extract from a pipe | ✅ with `--head-index` | ✅ | ⚠️ limited |
| Parallel compression and extraction | ✅ | compression only | ❌ (typically) |
| Page-aligned stored data (`mmap`) | ✅ `--align`, `--contiguous` | ❌ | ⚠️ via `zipalign` |
| Ubiquity | new | everywhere | everywhere |

If you're sharing files with people who don't have zar, use tar or zip.
zar is for when the archive is *read in place*: picking files out of big
archives, verifying backups cheaply, or shipping bundles that a loader
reads without unpacking.

## Installing

With [Homebrew](https://brew.sh), from the
[kevpatt/tap](https://github.com/kevpatt/homebrew-tap) tap:

```sh
brew install kevpatt/tap/zar
```

That installs `zar` and `unzar` with their manual pages, built against
Homebrew's zstd and xxHash. `brew upgrade` picks up new releases, and
`brew install --HEAD kevpatt/tap/zar` builds the latest commit instead.

On Debian or Ubuntu, download the package for your architecture (amd64 or
arm64) from the [latest release](https://github.com/kevpatt/zar/releases/latest)
and install it with apt, which also installs its dependency, libzstd:

```sh
sudo apt install ./zar_*_amd64.deb    # or _arm64
```

The packages need Ubuntu 22.04 or Debian 12, or later. Elsewhere, build zar
from source.

## Building

You need CMake 3.20 or newer, libzstd 1.4 or newer, and a C++20 compiler
(zar is developed with Apple Clang 17). On macOS it builds against the
macOS 13 SDK and newer.

```sh
# macOS
brew install cmake ninja zstd

# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config libzstd-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config libzstd-devel
```

Then:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build        # run the tests
cmake --install build         # install zar and unzar (default prefix /usr/local)
```

`cmake --install` puts the commands in `bin`, their manual pages
(`man zar`, `man unzar`) in `share/man/man1`, and the README, format
specification, and licenses in `share/doc/zar`, under the prefix. Use
`--prefix DIR` to install elsewhere.

On Debian or Ubuntu, `cpack -G DEB` in the build directory makes a package
of the same files, under `/usr`, like those on the releases page (it needs
`dpkg-dev`).

zar builds and passes its full test suite on both macOS and Linux: on macOS
(Apple Silicon) with Apple Clang 17 and GCC 16, and on Linux with GCC 15.
CI (`.github/workflows/ci.yml`) builds and tests every push on Linux
(x64 and arm64) and macOS, with the sanitizers and a short fuzzing run, and
builds the Debian packages.
It uses only POSIX APIs and should build on the BSDs, but those aren't
tested yet.

Useful CMake options:

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | `Debug`, `Release`, ... |
| `ZAR_BUILD_TESTS` | `ON` | build the test suite |
| `ZAR_WARNINGS_AS_ERRORS` | `OFF` | treat compiler warnings as errors |
| `ZAR_BUILD_FUZZERS` | `OFF` | build the fuzz target instead of an installable build |
| `ZAR_USE_SYSTEM_XXHASH` | `OFF` | link an installed libxxhash 0.8 or newer (found like libzstd) instead of compiling in the bundled copy |

The version comes from `project()` in `CMakeLists.txt`. Each build also
records the git revision (with `-dirty` if there are uncommitted changes)
and the build time, shown by `--help` and `--version`. Set
`SOURCE_DATE_EPOCH` to fix the build time for reproducible builds.

`build/compile_commands.json` is generated for clangd and other tools.

## Using zar

```sh
zar ~/src/proj                    # writes proj.zar here
zar -o backup.zar ~/Documents     # choose the archive's name
zar -o - proj | ssh host 'cat > proj.zar'     # write to a pipe
zar -m -o bundle.zar notes.txt photos         # several inputs need -m and -o
zar -l 19 --solid=4M config/      # small and similar: try one solid stream
zar --dict headers/               # many small files, still random access
zar --contiguous --align=4K app/  # one page-aligned block per file
```

### What gets stored

zar follows POSIX path resolution:

| Input | Stored as | Extracting creates |
|---|---|---|
| `proj` (a directory) | `proj`, `proj/src/main.cpp`, … | `proj/` |
| `proj/` | the same, but fails if `proj` isn't a directory | `proj/` |
| `proj/.` | `src/main.cpp`, … (the contents only) | the contents, directly |
| `.` | the current directory's contents | the contents, directly |
| `notes.txt` | `notes.txt` | `notes.txt` |
| `link` (a symlink) | the symlink itself | the symlink |
| `link/` | the directory it points to, named `link` | `link/` |

Without `-o`, the archive is named after the input and written in the
current directory: `proj`, `proj/`, `proj/.` and `~/src/proj` all produce
`./proj.zar`. For `.`, the archive is named after the current directory (and
so is written inside it; zar never archives the archive it's writing). An
existing archive is only replaced with `-y`.

zar archives one input unless `-m` is given. With `-m`, inputs that would
store the same path (two directories both named `src`, say) are an error.

By default zar stores permissions and modification times. Ownership,
nanosecond timestamps, and access, change, and birth times are opt-in.

### Options

| Option | Meaning |
|---|---|
| `-o, --output=FILE` | archive to write (`-` for stdout); see above for the default |
| `-m, --many` | archive several inputs (requires `-o`) |
| `-y, --yes` | overwrite an existing archive |
| `-l, --level=N` | zstd compression level (default 3) |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--chunk-size=SIZE` | split files into independently compressed chunks of SIZE bytes: a power of two from 4K to 1G, with an optional `K`, `M`, or `G` suffix (default 1M) |
| `--align[=N]` | align block payloads to N bytes; with no N, the CPU's cache-line size. The default is 8. N must be a power of two up to 1M; values below 8 pack tighter but leave fields unaligned. |
| `--owner[=WHICH]` | store ownership: `ids`, `names`, or `both` (the default when given); omitted otherwise |
| `--nsec` | store the sub-second part of timestamps |
| `--all-times` | store access, status-change, and birth times as well as modification time |
| `--names=MODE` | file names that aren't valid UTF-8: `raw` stores the bytes and flags the entry (default), `skip` leaves the file out, `latin1` converts from Latin-1. All three warn. |
| `--compress-all` | compress files even if they're usually already compressed |
| `--dedup=on\|off` | store identical chunks only once (default: on, except when writing to a pipe without `--head-index`); see [Deduplication](#deduplication) |
| `--contiguous[=SIZE]` | store each file of at most SIZE bytes as a single block rather than in chunks; with no SIZE, files up to 4G (the largest block unzar reads) |
| `--dict[=SIZE]` | train a zstd dictionary of at most SIZE bytes (default 110K) on the files, and compress them with it; see [Dictionaries](#dictionaries) |
| `--head-index` | also put the index at the front, so the archive can be extracted from a pipe (the index is written twice) |
| `--solid=SIZE` | first try compressing the whole archive as one stream, and keep that if it's at most SIZE bytes; otherwise write a normal archive |
| `--dict-over-solid=PERCENT` | when the solid archive fits, also try a dictionary archive, and keep it instead if it's at most PERCENT of the solid size and also fits (default 120; 0 never tries) |
| `-v, --verbose` | list files as they are added, and print a summary |
| `--help`, `--version` | |

Short options can be combined, as in `zar -myv -o all.zar a b`. `-l`'s
level can be attached or separate (`-l19` or `-l 19`), but `-j`'s count
must be attached (`-j4`).

Files whose extension marks them as already compressed (`.jpg`, `.png`,
`.mp3`, `.m4a`, `.mp4`, `.mov`, `.zip`, `.gz`, `.zst`, `.docx`, and about 60
others; see [`src/zar/policy.cpp`](src/zar/policy.cpp)) are stored without
trying zstd, unless `--compress-all` is given. Independently of that, any
chunk that zstd fails to shrink is stored as-is.

## Using unzar

```sh
unzar proj.zar                    # extract into the current directory
unzar -C /tmp/out proj.zar        # extract elsewhere
unzar proj.zar proj/src           # extract one directory (or file)
unzar -lh proj.zar                # list, checking the archive, sizes like 1.5M
unzar -lx proj.zar                # list while extracting
unzar --test proj.zar             # decompress and verify everything
unzar -y --dirs=replace proj.zar  # replace an earlier extraction
```

`unzar` runs in one of three modes, and `--list` combines with any of them:

| Mode | Reads data | Decompresses | Writes files |
|---|---|---|---|
| `-x, --extract` (default) | ✅ | ✅ | ✅ |
| `-t, --test` | ✅ | ✅ | ❌ |
| `--scan` (default with `--list`) | ✅ | ❌ | ❌ |

Give at most one mode. A listing on its own never writes anything:
`unzar -l` lists and scans, `unzar -lt` lists and tests, and `unzar -lx`
lists and extracts. Short options can be combined, as in `-lxv`; `-j`'s
count must be attached, as in `-j4`.

`--scan` still checks every block's checksums and every chunk's recorded
size, so it's a quick way to verify an archive's integrity. On an archive of
the macOS SDK frameworks (9,178 files, 219 MiB) it takes 0.06 s, against
0.34 s for a single-threaded `--test`.

The listing shows each directory as a heading, followed by its files with
their size and modification time, separated by tabs (`-v` adds permissions
and owner):

```
  README	6	2024-01-02 15:30:00
  link -> src/main.cpp	-	2026-10-13 08:32:53

src/
  main.cpp	13	2026-10-13 08:32:53

src/sub/
  blob.bin	3000000	2026-10-13 08:32:53
```

### Options

| Option | Meaning |
|---|---|
| `-C, --directory=DIR` | extract into DIR (created if needed) |
| `-l, --list` | print a listing as entries are processed; on its own, scans rather than extracting |
| `-x, --extract` | extract (the default, except with `--list`) |
| `--scan` | check checksums, sizes, and paths without decompressing; write nothing |
| `-t, --test` | like `--scan`, but also decompress and verify all data |
| `-y, --yes` | overwrite files and directories that already exist |
| `--dirs=MODE` | existing directories: `merge` (default) extracts into them; `replace` first deletes each one that is extracted whole (not the parents of a selected path). Either needs `-y`. |
| `--hard-links` | recreate hard links with `link(2)` where their targets are extracted too; by default each is extracted as an independent copy. Falls back to a copy where the filesystem refuses. |
| `--sparse=MODE` | sparse files: `keep` (default) leaves holes where the archive has them; `auto` also wherever a whole chunk is zeros; `never` writes every byte. See [Sparse files](#sparse-files). |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--max-size=SIZE` | refuse to decompress more than SIZE bytes in all (e.g. `10G`); without it, the limit is the free space where the data would go |
| `-h, --human-readable` | show sizes as `512`, `1.0K`, `31.4M`, `2.0G`, … |
| `-v, --verbose` | add permissions and owner to the listing; print a summary |
| `--help`, `--version` | |

### Exit status

Both commands exit with:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | finished, but some files had problems, each reported on stderr |
| 2 | fatal error: bad usage, unreadable or invalid archive, existing targets without `-y`, ... |

A fatal error in `zar` leaves no archive behind: it writes to a temporary
file and renames it into place only on success.

## Solid archives

Normally every chunk of every file is compressed separately, which is what
makes random access possible. For lots of small, similar files, though, it
costs compression ratio, because zstd can't exploit what the files have in
common.

With `--solid=SIZE`, zar first builds the archive with nothing compressed
and compresses it whole, as one stream. If the result is at most SIZE bytes,
that's the archive. If it grows past SIZE, zar abandons the attempt right
then and writes a normal archive instead, so the attempt never costs more
than SIZE bytes of output. Options like `--align` then apply to the normal
archive.

| Files | Normal | Solid |
|---|---|---|
| 13 config files | 5.8K | 5.0K (−14%) |
| 38 source files | 72K | 58K (−20%) |
| 135 C headers | 461K | 270K (−41%) |
| 3,851 C headers | 8.1M | 5.1M (−37%) |

`unzar` reads solid archives transparently, from files or pipes, but has to
decompress the whole thing first: into memory, or into a temporary file
once it passes 64 MiB. So solid archives make the most sense when they're
small, and `--scan` has to decompress them too.

## Dictionaries

A trained zstd dictionary captures what files have in common (keywords,
boilerplate, structure) so that each chunk, compressed on its own, can
refer to it. With `--dict`, zar reads the files once to collect samples
(the start of each chunk, up to 32 MiB in all), trains a dictionary, and
then builds a normal archive with every compressible chunk compressed
against it. The dictionary is stored once, in front of the data, and each
file's index entry points to it, so random access and `--head-index`
streaming both still work.

On the 31 MiB of C and C++ headers in the macOS SDK:

| Archive | Size |
|---|---|
| normal | 7.7M |
| `--dict` | 5.8M (−25%) |
| `--solid` | 4.9M (−36%) |

The dictionary block itself is stored uncompressed, so a larger `--dict=SIZE`
isn't always better. The default, 110K (as with `zstd --train`), is scaled
down for small inputs.

With `--solid=SIZE`, zar tries a dictionary archive too whenever the solid
one fits, and keeps the dictionary archive if it's at most 120% of the solid
size (`--dict-over-solid=PERCENT`), trading a little size for random access.

## Deduplication

By default, zar hashes each chunk's uncompressed data with XXH3-128 as it's
read. If the hash matches a chunk already in the archive, zar reads
that block back, decompresses it if need be, and compares the bytes; only
if they're identical does the new chunk reuse the existing block, skipping
compression too. A hash collision can't corrupt an archive, it just stores
both chunks.

Chunks are compared whole, at chunk boundaries, so this finds duplicate
files of any size, and identical chunks anywhere, such as runs of zeros in
disk images. It doesn't find data that has merely shifted (a copy with a
byte inserted at the start).

Reading back needs an output zar can read: a file, or with `--head-index`,
the temporary file the data is staged in. So when writing to a pipe without
`--head-index`, zar doesn't deduplicate, and `--dedup=on` is an error. Nor
does it deduplicate inside a solid archive, where zstd finds repeats on its
own; it does in `--solid`'s fallback and dictionary archives. Memory use is
about 40 bytes per distinct chunk, and hashing costs little next to
compression. `--dedup=off` turns it off.

## Sparse files

A sparse file has **holes**: ranges the filesystem has no storage for,
which read as zeros. Disk images, database files, and core dumps are often
mostly holes.

zar asks the filesystem where a file's holes are (`SEEK_DATA` and
`SEEK_HOLE`). A chunk that lies entirely within a hole is recorded as a hole,
with no block and without being read, so a sparse 100 GB image archives as
quickly as the data in it. A chunk that is only partly a hole is stored as
usual; its zeros compress to almost nothing. `zar -v` reports how much was
in holes.

`unzar` leaves those chunks unallocated, so the file takes no more space
than the data in it, and `--max-size` and the free-space check count only
the data. The file is still verified as a whole: its CRC covers the zeros.
`--sparse` chooses how the zeros are written:

| Mode | Holes in the archive | Other chunks of zeros |
|---|---|---|
| `keep` (default) | left as holes | written |
| `auto` | left as holes | left as holes |
| `never` | written | written |

`auto` makes holes even in archives of files that weren't sparse, like
`cp --sparse=always`. `never` is for files that must be fully allocated,
such as swap files. Holes are kept a chunk at a time (1 MiB by default), so
smaller ones come back as written zeros; archive with a smaller
`--chunk-size` to keep them. A file stored with `--contiguous` is a single
chunk, so it is kept sparse only if it's entirely a hole. Some filesystems
allocate small holes anyway: APFS fills in a gap of under about 16 MiB
when data is written after it.

Holes depend on how a file was written, not just on its contents, so two
copies of the same data can archive differently, though they extract the
same. Holes are part of format 0.5.0; older versions of `unzar` reject a
file with holes (and extract the rest of the archive).

## Streaming through pipes

`zar -o -` writes an archive to standard output, and `unzar -` reads one
from standard input:

```sh
zar -o - proj | ssh host 'cat > proj.zar'
zar --head-index -o - proj | ssh host 'unzar -C /srv -'
curl -s https://example.com/proj.zar | unzar -
```

Normally the index is written last, after all the data, so reading an
archive from a pipe would mean buffering all of it. `--head-index` also
writes the index at the front, so `unzar -` can read the index first and
then extract each file, in parallel, as its data arrives. To make that
possible, zar holds the data in a temporary file until the index is known.
That temporary file also lets zar deduplicate (see
[Deduplication](#deduplication)), which it can't do when writing straight
to a pipe.

Archives without a head index can still be extracted from a pipe if you save
them to a file first. If standard input is a redirected file
(`unzar - < proj.zar`), `unzar` reads it directly.

## Safety

`unzar` is written to be safe on untrusted archives:

- **Paths.** Archives containing absolute paths, `..` or `.` components,
  empty components, or NUL bytes are rejected outright.
- **Symlinks.** Every file and directory is created relative to the
  destination with `openat(..., O_NOFOLLOW)`, one path component at a time,
  so nothing is ever written through a symlink: not one already in the
  destination, and not one the archive itself creates. Symlinks are created
  last, after all files.
- **Overwriting.** Before writing anything, `unzar` checks whether any target
  already exists, and stops if one does unless `-y` was given.
  `--dirs=replace` deletes only directories that are extracted whole.
- **Damage.** A file whose data fails a checksum or size check is removed
  rather than left half-written, and the rest of the archive is still
  extracted.
- **Size.** A small archive can describe enormous files, so `--max-size`
  caps how much will be decompressed, both for the files selected (checked
  up front) and for solid archives (checked as they expand). Without it,
  extraction is limited to the free space at the destination, and a solid
  archive to the free space in `$TMPDIR`. Holes left as holes don't count,
  since they take neither time nor space.
- **Memory.** Nothing is allocated at a size the archive merely claims:
  blocks read from a pipe grow as their data arrives, and a compressed block
  can't claim more than zstd could produce from it (32,768 times its size).
  The data in flight during extraction is bounded in bytes as well as in
  chunks.
- **Structure.** Sub-indexes must form a tree, so an archive can't loop or
  reuse index blocks to inflate its listing, and lengths and offsets are
  checked against the archive's bounds, including when reading from a pipe.
- **Permissions.** Ownership is only restored when running as root, and
  set-user-ID and set-group-ID bits only when the file's owner was restored
  too; otherwise they would apply to whoever ran `unzar`.
- **Output.** Names from the archive are escaped when printed, in messages
  as well as listings, so they can't send control sequences to the
  terminal.

The test suite includes deliberately malicious archives for each of these
cases, and `fuzz/` has a fuzz target for the reader (see below). CRC-32C
detects accidental damage, not tampering; cryptographic signatures are
[on the roadmap](TODO.md).

## The archive format

The format is specified in [FORMAT.md](FORMAT.md). In outline:

```
+------------+---------+---------+-----+-------+---------+
| zar header | block 0 | block 1 | ... | index | trailer |
+------------+---------+---------+-----+-------+---------+
   32 bytes    32-byte header + payload in every block
```

- A 32-byte **zar header** holds the magic number `\x89ZAR\r\n\x1a\n`, the
  format version, the alignment, and the location of the index.
- Everything else is a **block**: a 32-byte header (type, size, its own
  offset, and CRCs of both the header and the payload) followed by the
  payload. Blocks hold file data (compressed or stored), indexes,
  dictionaries, or a whole compressed archive. A chunk of a sparse file's
  hole has no block at all.
- The **index** lists every entry as a path plus optional, typed metadata
  fields, so new fields can be added without breaking older readers. Large
  directories can be split into sub-indexes.
- A **trailer** at the end points to the index, so archives can be written
  in one pass, even to a pipe.

All integers are little-endian, all checksums CRC-32C, and all offsets
relative to a movable base, so an index can be added at the front of an
existing archive without changing any other offset.

## Project layout

```
.github/workflows/     CI: tests, sanitizers, fuzzing, and Debian packages
CMakeLists.txt         the build, and Debian packaging (cpack -G DEB)
FORMAT.md              the archive format specification
LICENSE.txt            BSD 3-Clause
TODO.md                planned work
cmake/build_info.cmake records the git revision and build time
man/                   manual pages for zar(1) and unzar(1)
src/
  zar_main.cpp         the zar command
  unzar_main.cpp       the unzar command
  cli.hpp              command-line helpers shared by both
  zar/                 libzar, the format library
    format.*           zar and block headers
    index.*            index entries
    archive_writer.*   block layout, alignment, head index, trailer
    archive_reader.*   locating and reading archives, from files or streams
    creator.*          walking a tree and building an archive
    extractor.*        extracting, testing, and scanning
    compressor.*       parallel chunk compression
    dictionary.*       collecting samples and training dictionaries
    dedup.*            chunk hashes for deduplication
    thread_pool.*      worker threads and in-flight limits for extraction
    posix.*            file descriptors, I/O loops, holes, free space,
                       temporary files, and user lookups
    output.*           buffered output, temporary files, solid compression
    crc32c.*           CRC-32C, hardware-accelerated
    names.*            UTF-8 and path rules
    policy.*           which files are already compressed
    bytes.hpp          little-endian integers and varints
    error.hpp          format errors
    version.hpp.in     version information, filled in by CMake
tests/                 unit and end-to-end tests, and cli_test.sh for the
                       commands themselves (all run with ctest)
fuzz/                  a fuzz target for everything unzar reads
third_party/xxhash/    xxHash 0.8.3 (BSD 2-Clause), for XXH3-128
```

The tests include end-to-end round trips, damaged and malicious archives,
streaming through real pipes, and runs at several thread counts. They also
run clean under ThreadSanitizer, AddressSanitizer, and UBSan:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=thread
cmake --build build-tsan && build-tsan/zar_tests
```

The fuzz target covers index decoding, opening archives from files and
pipes, and scanning and testing them. It uses libFuzzer where the compiler
has it, and otherwise a built-in driver that mutates seed archives:

```sh
cmake -S . -B build-fuzz -G Ninja -DZAR_BUILD_FUZZERS=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build-fuzz --target zar_fuzz
build-fuzz/zar_fuzz -runs=100000 -artifact=crash.zar
```

A fuzzing build skips CRC checks, so that mutations get past them; it has
no install rules, and its `zar` and `unzar` must not be used.

## Roadmap

See [TODO.md](TODO.md). The main plans are aimed at using zar as an
application bundle format, read in place by a loader:

- storing libraries uncompressed, so their single, page-aligned blocks can
  be `mmap`ed,
- cryptographic hashes and a signed index,
- reproducible timestamps.

## License

zar is released under the [BSD 3-Clause License](LICENSE.txt). It includes
[xxHash](https://github.com/Cyan4973/xxHash) by Yann Collet, under the
[BSD 2-Clause License](third_party/xxhash/LICENSE), unless it's built with
`ZAR_USE_SYSTEM_XXHASH`.

Copyright (C) 2026 Kevin H. Patterson
