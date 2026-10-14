# zar

**Random-access archives, compressed with [zstd](https://github.com/facebook/zstd).**

`zar` packs a file or directory into a single `.zar` archive; `unzar` lists,
verifies, and extracts it. Unlike a `.tar.zst`, which is one long compressed
stream, a zar archive is compressed piece by piece: every file, and every
chunk of a large file, is its own independently checksummed block, and an
index records where each one lives. Any file can be listed, verified, or
extracted without decompressing anything else.

```sh
$ zar ~/src/project
$ unzar --list --scan -h project.zar
project/
  README.md	5.7K	2026-10-13 09:22:27
  ...
$ unzar project.zar project/src/main.cpp
```

> **Status:** early development (0.2.1). Both tools work and are well
> tested, but the [archive format](FORMAT.md) may still change before 1.0.

## Contents

- [Features](#features)
- [How it compares](#how-it-compares)
- [Building](#building)
- [Using zar](#using-zar)
- [Using unzar](#using-unzar)
- [Solid archives](#solid-archives)
- [Dictionaries](#dictionaries)
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

## Building

You need CMake 3.20 or newer, libzstd 1.4 or newer, and a C++20 compiler and
standard library that include `std::jthread` (zar is developed with Apple
Clang 17).

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

zar is developed on macOS (Apple Silicon). It uses only POSIX APIs and
should build on Linux and the BSDs, but those aren't regularly tested yet.

Useful CMake options:

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | `Debug`, `Release`, ... |
| `ZAR_BUILD_TESTS` | `ON` | build the test suite |
| `ZAR_WARNINGS_AS_ERRORS` | `OFF` | treat compiler warnings as errors |

The version comes from `project()` in `CMakeLists.txt`. Each build also
records the git revision (with `-dirty` if there are uncommitted changes)
and the build time, shown by `--help` and `--version`. Set
`SOURCE_DATE_EPOCH` to fix the build time for reproducible builds.

`build/compile_commands.json` is generated for clangd and other tools.

## Using zar

```sh
zar proj                          # writes proj.zar beside proj
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

Without `-o`, the archive is named after the input and written beside it:
`proj`, `proj/` and `proj/.` all produce `proj.zar`. For `.`, the archive is
named after the current directory and written inside it (zar never archives
the archive it's writing). An existing archive is only replaced with `-y`.

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
| `--contiguous[=SIZE]` | store each file of at most SIZE bytes as a single block rather than in chunks; with no SIZE, files up to 4G (the largest block unzar reads) |
| `--dict[=SIZE]` | train a zstd dictionary of at most SIZE bytes (default 110K) on the files, and compress them with it; see [Dictionaries](#dictionaries) |
| `--head-index` | also put the index at the front, so the archive can be extracted from a pipe (the index is written twice) |
| `--solid=SIZE` | first try compressing the whole archive as one stream, and keep that if it's at most SIZE bytes; otherwise write a normal archive |
| `--dict-over-solid=PERCENT` | when the solid archive fits, also try a dictionary archive, and keep it instead if it's at most PERCENT of the solid size and also fits (default 120; 0 never tries) |
| `-v, --verbose` | list files as they are added, and print a summary |
| `--help`, `--version` | |

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
unzar -l --scan -h proj.zar       # list, checking the archive, sizes like 1.5M
unzar --test proj.zar             # decompress and verify everything
unzar -y --dirs=replace proj.zar  # replace an earlier extraction
```

`unzar` runs in one of three modes, and `--list` combines with any of them:

| Mode | Reads data | Decompresses | Writes files |
|---|---|---|---|
| extract (default) | ✅ | ✅ | ✅ |
| `--test` | ✅ | ✅ | ❌ |
| `--scan` | ✅ | ❌ | ❌ |

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
| `-l, --list` | print a listing as entries are processed |
| `--scan` | check checksums, sizes, and paths without decompressing; write nothing |
| `-t, --test` | like `--scan`, but also decompress and verify all data |
| `-y, --yes` | overwrite files and directories that already exist |
| `--dirs=MODE` | existing directories: `merge` (default) extracts into them; `replace` deletes them first. Either needs `-y`. |
| `--hard-links` | recreate hard links with `link(2)` where their targets are extracted too; by default each is extracted as an independent copy. Falls back to a copy where the filesystem refuses. |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--max-size=SIZE` | refuse to decompress more than SIZE bytes in all (e.g. `10G`) |
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
- **Damage.** A file whose data fails a checksum or size check is removed
  rather than left half-written, and the rest of the archive is still
  extracted.
- **Size.** `--max-size` caps how much will be decompressed, both for the
  files selected (checked up front) and for solid archives (checked as they
  expand).
- **Structure.** Sub-indexes must form a tree, so an archive can't loop or
  reuse index blocks to inflate its listing, and lengths and offsets are
  checked against the archive's bounds.
- **Permissions.** Set-user-ID and set-group-ID bits and ownership are only
  restored when running as root.

The test suite includes deliberately malicious archives for each of these
cases. CRC-32C detects accidental damage, not tampering; cryptographic
signatures are [on the roadmap](TODO.md).

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
  dictionaries, or a whole compressed archive.
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
CMakeLists.txt
FORMAT.md              the archive format specification
LICENSE.txt            BSD 3-Clause
TODO.md                planned work
cmake/build_info.cmake records the git revision and build time
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
    thread_pool.*      worker threads for extraction
    output.*           buffered output, temporary files, solid compression
    crc32c.*           CRC-32C, hardware-accelerated
    names.*            UTF-8 and path rules
    policy.*           which files are already compressed
tests/                 unit and end-to-end tests (run with ctest)
```

The tests include end-to-end round trips, damaged and malicious archives,
streaming through real pipes, and runs at several thread counts. They also
run clean under ThreadSanitizer, AddressSanitizer, and UBSan:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=thread
cmake --build build-tsan && build-tsan/zar_tests
```

## Roadmap

See [TODO.md](TODO.md). The main plans are aimed at using zar as an
application bundle format, read in place by a loader:

- storing libraries uncompressed, so their single, page-aligned blocks can
  be `mmap`ed,
- cryptographic hashes and a signed index,
- reproducible timestamps.

## License

zar is released under the [BSD 3-Clause License](LICENSE.txt).

Copyright (C) 2026 Kevin H. Patterson
