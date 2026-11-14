# zar

[![CI](https://github.com/kevpatt/zar/actions/workflows/ci.yml/badge.svg)](https://github.com/kevpatt/zar/actions/workflows/ci.yml)

**Random-access archives, compressed with [zstd](https://github.com/facebook/zstd).**

`zar` packs a file or directory into a single `.zar` archive; `unzar` lists,
verifies, and extracts it; `rezar` adds, replaces, and deletes files in it
without recompressing the rest. Unlike a `.tar.zst`, which is one long compressed
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

> **Status:** development (0.8.0). All three tools work and are well
> tested, but the [archive format](FORMAT.md) may still change before 1.0.

## Contents

- [Features](#features)
- [How it compares](#how-it-compares)
- [Installing](#installing)
- [Building](#building)
- [Using zar](#using-zar)
- [Using unzar](#using-unzar)
- [Editing archives with rezar](#editing-archives-with-rezar)
- [Solid archives](#solid-archives)
- [Dictionaries](#dictionaries)
- [Deduplication](#deduplication)
- [Sparse files](#sparse-files)
- [Streaming through pipes](#streaming-through-pipes)
- [Hashes and signatures](#hashes-and-signatures)
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
- **Signed archives.** `--hash` stores a BLAKE3 hash of every block, and
  `--sign` an Ed25519 signature over them all, so `unzar --verify` can tell
  an archive is exactly what its publisher made, checking each block before
  it's decompressed, even when extracting a single file.
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
- **Editing without recompressing.** `rezar` adds, replaces, and deletes
  files, copying everything it keeps block for block, and reclaims the space
  of what it deletes.
- **Sparse files.** Holes in sparse files, such as disk images, are
  recorded rather than read and stored, and `unzar` recreates them as
  holes, so a mostly empty 100 GB image doesn't take 100 GB to extract.
- **Alignment.** `--align` places payloads on cache-line or page boundaries,
  and `--contiguous` keeps each file in a single block, so stored data can be
  `mmap`ed straight from the archive. Archives end on the same boundary, so
  ones concatenated end to end (appended to a boot image, say) stay aligned,
  and each can be found from the end by following its trailer back.
- **Safe extraction.** No absolute paths, no `..`, no writing through
  symlinks, nothing overwritten without `-y`, and an optional cap on how
  much will be decompressed.
- **Predictable inputs.** `proj`, `proj/` and `proj/.` follow POSIX path
  resolution, so it's always clear what gets stored.
- **Deterministic.** Entries are stored in sorted order, so archiving the
  same files with the same options gives the same archive. (A signature
  also records when it was made; set `SOURCE_DATE_EPOCH` to fix that.)
- **Open format.** The format is [fully specified](FORMAT.md) and small
  enough to reimplement.

## How it compares

|  | zar | tar + zstd | zip |
|---|---|---|---|
| Extract one file without decompressing others | ✅ | ❌ | ✅ |
| Random access *within* a large file | ✅ (per chunk) | ❌ | ❌ |
| Compression across similar small files | ✅ with `--solid` or `--dict` | ✅ | ❌ |
| Duplicate files and data stored once | ✅ (by default) | ⚠️ only within zstd's window | ❌ |
| Add or delete files without recompressing the rest | ✅ `rezar` | ❌ | ✅ |
| Sparse files kept sparse | ✅ (per chunk) | ✅ GNU tar `--sparse`, bsdtar | ❌ |
| Checksums | every block, and every file | whole stream (optional) | every file |
| Built-in signatures, checked per file | ✅ `--sign`, `--verify` | ❌ (sign the whole file separately) | ⚠️ `jarsigner` for JARs |
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

That installs `zar`, `unzar`, and `rezar` with their manual pages, built against
Homebrew's zstd and xxHash. `brew upgrade` picks up new releases, and
`brew install --HEAD kevpatt/tap/zar` builds the latest commit instead.

On Linux, download the package for your distribution and architecture from
the [latest release](https://github.com/kevpatt/zar/releases/latest) and
install it with your package manager, which also installs its dependency,
libzstd:

| Distribution | Package | Install with |
|---|---|---|
| Debian 12+, Ubuntu 22.04+ | `zar_X.Y.Z_amd64.deb`, `zar_X.Y.Z_arm64.deb` | `sudo apt install ./zar_*_amd64.deb` |
| RHEL, Rocky, AlmaLinux 9+, Fedora | `zar-X.Y.Z-1.x86_64.rpm`, `zar-X.Y.Z-1.aarch64.rpm` | `sudo dnf install ./zar-*.x86_64.rpm` |
| openSUSE | the same `.rpm` | `sudo zypper install ./zar-*.x86_64.rpm` |

Elsewhere, build zar from source.

## Building

You need CMake 3.20 or newer, libzstd 1.4 or newer, and C and C++20
compilers (zar is developed with Apple Clang 17); the C compiler builds the
bundled BLAKE3 and Monocypher. On macOS it builds against the
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
cmake --install build         # install the commands (default prefix /usr/local)
```

`cmake --install` puts the commands in `bin`, their manual pages
(`man zar`, `man unzar`, `man rezar`) in `share/man/man1`, and the README, format
specification, and licenses in `share/doc/zar`, under the prefix. Use
`--prefix DIR` to install elsewhere.

On Linux, `cpack -G DEB` or `cpack -G RPM` in the build directory makes a
package of the same files, under `/usr`, like those on the releases page
(they need `dpkg-dev` or `rpm-build`).

zar builds and passes its full test suite on both macOS and Linux: on macOS
(Apple Silicon) with Apple Clang 17 and GCC 16, and on Linux with GCC 15,
and CI also builds it with GCC 12 and 13 and with Clang.
CI (`.github/workflows/ci.yml`) builds and tests every push on Linux
(x64 and arm64) and macOS, with the sanitizers and a short fuzzing run. It
also builds the `.deb` and `.rpm` packages and tries them on Debian,
Ubuntu, AlmaLinux 9 and 10 (for RHEL), Fedora, and openSUSE.
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
| `ZAR_USE_SYSTEM_BLAKE3` | `OFF` | link an installed libblake3 (found with pkg-config) instead of compiling in the bundled copy |
| `ZAR_USE_SYSTEM_MONOCYPHER` | `OFF` | link an installed Monocypher 4 (found with pkg-config, built with its Ed25519 module) instead of the bundled copy |

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

### Disks

Devices are normally skipped, but with `--read-devices`, a disk given as an
input (`/dev/sdb`, `/dev/disk4` or the faster raw `/dev/rdisk4` on macOS, or
a symlink to one, like Linux's `/dev/disk/by-id/...`) is stored as a file
holding the disk's contents, named after the input:

```sh
sudo zar --read-devices -o usb.zar /dev/sdb    # stored as the file "sdb"
unzar --sparse=auto usb.zar                    # an image file, sparse where it's zeros
```

Only devices given as inputs are read, never ones found inside a directory,
and only disks: something like `/dev/zero`, which has no size, is skipped.
Runs of zeros, like a disk's unused space, are stored once by deduplication.
A disk has no holes for zar to find, so `unzar --sparse=auto` is what makes
the image sparse again. To write it back to a disk, use `dd` or similar.
Add `--progress` to see how far along a big disk is.

### Progress

`--progress`, for `zar`, `unzar`, and `rezar`, keeps one line at the bottom
of the terminal up to date while they work:

```
1.2G / 4.0G  30.0%  412.5M/s  0:07 left
```

The amount is file data, uncompressed: read by `zar` (holes included),
extracted, tested, or scanned by `unzar`, and copied or added by `rezar`. So
the rate is how fast the files go by, whatever the compression ratio. `zar`
first adds up the sizes of its inputs to know the total. With `--solid` or
`--dict`, it reads everything more than once, and labels each pass
(`solid:`, `dictionary:`, `sampling:`). The line is drawn only when standard
error is a terminal, so logs and pipes stay clean, and it steps aside for
`-v`, listings, and warnings.

### Options

| Option | Meaning |
|---|---|
| `-o, --output=FILE` | archive to write (`-` for stdout); see above for the default |
| `-m, --many` | archive several inputs (requires `-o`) |
| `-y, --yes` | overwrite an existing archive |
| `--read-devices` | store a disk device given as an input as a file holding its contents; see [Disks](#disks) |
| `-l, --level=N` | zstd compression level (default 3) |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--chunk-size=SIZE` | split files into independently compressed chunks of SIZE bytes: a power of two from 4K to 1G, with an optional `K`, `M`, or `G` suffix (default 1M) |
| `--align[=N]` | align block payloads to N bytes; with no N, the CPU's cache-line size. The default is 8. N must be a power of two up to 1M; values below 8 pack tighter but leave fields unaligned. The archive's length is a multiple of N too, so concatenated archives stay aligned. |
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
| `--hash` | store each block's BLAKE3 hash; see [Hashes and signatures](#hashes-and-signatures) |
| `--sign=KEYFILE` | sign the archive with a secret key from `--keygen` (may be repeated); implies `--hash` |
| `--sequence=N`, `--name=TEXT` | with `--sign`: a release number, and what the archive is (a channel, say), both signed |
| `--keygen=KEYFILE` | make a key pair: `KEYFILE` (secret, mode 0600) and `KEYFILE.pub` |
| `-v, --verbose` | list files as they are added, and print a summary |
| `--progress` | show progress on the terminal: how much is done, the rate, and the time left; see [Progress](#progress) |
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
| `--verify=PUBFILE` | require a valid signature by this public key (may be repeated, for several); see [Hashes and signatures](#hashes-and-signatures) |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--max-size=SIZE` | refuse to decompress more than SIZE bytes in all (e.g. `10G`); without it, the limit is the free space where the data would go |
| `-h, --human-readable` | show sizes as `512`, `1.0K`, `31.4M`, `2.0G`, … |
| `-v, --verbose` | add permissions and owner to the listing, and a signed archive's signature and signers before it; print a summary |
| `--progress` | show progress on the terminal; see [Progress](#progress) |
| `--help`, `--version` | |

### Exit status

All three commands exit with:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | finished, but some files had problems, each reported on stderr |
| 2 | fatal error: bad usage, unreadable or invalid archive, existing targets without `-y`, ... |

A fatal error in `zar` or `rezar` leaves no archive behind: each writes to a
temporary file and renames it into place only on success.

## Editing archives with rezar

```sh
rezar -C ~/src --replace=proj/main.c -o new.zar proj.zar     # replace a file
rezar -C ~/src --add='proj/docs/**' -o new.zar proj.zar      # add a directory tree
rezar -n --delete-list=gone.txt proj.zar                     # what would deleting these do?
rezar --delete-list=gone.txt -o trimmed.zar proj.zar         # delete them
rezar --align=4K --head-index -o aligned.zar proj.zar        # repack, changing nothing else
```

`rezar` writes a new archive: a copy of the old one with paths added,
replaced, or deleted. The old archive is never changed. Everything kept is
copied block for block, never decompressed or recompressed, so editing a
large archive takes about as long as copying it. Blocks that only deleted
or replaced files used are left out, so their space is reclaimed, and blocks
that several files share stay shared. New files are stored as `zar` would
store them, with the old archive's dictionary if it has one. Without
options, the alignment and head index are as in the old archive.

The paths to change come in lists, one per line (`-0` for NUL-separated
lists, as from `find -print0`; `-` reads a list from standard input), or one
at a time with `--add=PATH` and the like:

| Option | The paths listed |
|---|---|
| `--add-list=FILE`, `--add=PATH` | are added; they must not be in the archive yet |
| `--replace-list=FILE`, `--replace=PATH` | replace what's there; they must be in the archive |
| `--add-replace-list=FILE`, `--add-replace=PATH` | are added, or replace what's there |
| `--delete-list=FILE`, `--delete=PATH` | are deleted; they must be in the archive |

Each is a path in the archive, and what's added there is read from the same
path below `-C DIR` (default: the current directory):

| Path | Means |
|---|---|
| `a/b` | the file or symlink `a/b` (not followed) |
| `a/d/` | the directory `a/d` alone; deleted only if it ends up empty |
| `a/d/**` | the directory `a/d` and everything below it (quote it in a shell) |

All deletions happen first, then all additions, so the order of lines
doesn't matter, but a path may be listed only once in all. Missing parent
directories are added too. A hard link to a file that's deleted or replaced
keeps the old content, as an ordinary file. If anything listed is wrong,
`rezar` reports every problem and writes nothing; `-n` shows what would
change without writing anything.

New chunks are deduplicated among themselves by default. `--dedup=deep`
also reuses blocks already in the archive that hold the same data. Finding
those means decompressing them, but only the ones whose uncompressed length
matches a new chunk's, each once. `--dedup=off` turns deduplication off.

The options for new files are those of `zar`: `-l`, `-j`, `--chunk-size`,
`--owner`, `--nsec`, `--all-times`, `--names`, `--compress-all`, and
`--contiguous`. `--align` and `--head-index[=on|off]` apply to the whole
archive, and `--progress` shows how far along it is. A solid archive can't be edited, since everything in it would be
recompressed. See `rezar --help` or `man rezar` for the rest.

`rezar` checks the hashes of every block it copies, keeps them, and hashes
what it adds when the archive had hashes (or with `--hash`). The result has
a new root, so it's no longer signed unless `--sign` signs it again;
`--verify=PUBFILE` checks the original's signature first.

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
archive; a solid one only ends on that boundary.

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

An archive needn't start at the beginning of its file. If it follows other
data, such as a boot image it was appended to, `unzar` finds it from the
trailer at the end, which records where the archive starts. Of several
archives appended one after another, that finds the last; each one's start
is where the one before it ends, so a loader can walk back through them all
(FORMAT.md §5.1).

## Hashes and signatures

```sh
zar --keygen=release.key                        # release.key and release.key.pub
zar --sign=release.key --sequence=42 proj       # a signed proj.zar
unzar --verify=release.key.pub proj.zar         # extract only if it checks out
unzar -lv --verify=release.key.pub proj.zar     # show who signed it, and what
```

CRC-32C catches accidental damage, but anyone can recompute it. For
tampering, `zar --hash` also stores a BLAKE3 hash of every block in the
index, and `zar --sign=KEYFILE` signs the root index's hash with Ed25519.
Since the index holds every block's hash, that one signature covers the
whole archive.

Verification runs down from the signature, one block at a time, and every
block is checked before it's decompressed, so nothing unauthenticated
reaches zstd. Extracting one file checks just that file's blocks and the
index: a reader fetching a single file from a big signed archive verifies
only what it reads. `unzar` checks every hash an archive has, in `--test`
and `--scan` too, whether or not it's given a key. `--verify=PUBFILE`
also requires a valid signature from that key; without it, `unzar` notes
that the signature went unchecked.

Keys are one-line text files: `zar-ed25519-secret` or `zar-ed25519-public`,
then 64 hex digits. Secret keys must be readable only by their owner, like
SSH keys. `--sign` may be given several times, to sign with several keys
during a key rotation, and `--verify` several times, to accept any of
several keys. `unzar -lv` shows each key's fingerprint.

The signature also covers `--sequence=N`, `--name=TEXT`, and the time of
signing (`SOURCE_DATE_EPOCH` if set). A valid signature on an old archive
stays valid, so an updater that mustn't go backwards should compare the
sequence with the newest it has installed, and check the name, so that an
archive signed for one channel isn't accepted on another. Deciding which
keys to trust is up to you: `unzar` never trusts a key just because the
archive names it.

Hashes add 32 bytes per chunk to the index, and a BLAKE3 pass over each
block when writing and reading, which is cheaper than decompressing it. A
solid archive can't be signed, since it would have to be decompressed
before anything in it could be checked. The format is specified in
[FORMAT.md](FORMAT.md), §5.6 and §6.7.

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
detects accidental damage, not tampering; for that, sign archives (see
[Hashes and signatures](#hashes-and-signatures)).

## The archive format

The format is specified in [FORMAT.md](FORMAT.md). In outline:

```
+------------+---------+---------+-----+-------+-------------+---------+
| zar header | block 0 | block 1 | ... | index | [signature] | trailer |
+------------+---------+---------+-----+-------+-------------+---------+
   32 bytes    32-byte header + payload in every block
```

- A 32-byte **zar header** holds the magic number `\x89ZAR\r\n\x1a\n`, the
  format version, the alignment, and the location of the index.
- Everything else is a **block**: a 32-byte header (type, size, its own
  offset, and CRCs of both the header and the payload) followed by the
  payload. Blocks hold file data (compressed or stored), indexes,
  dictionaries, a whole compressed archive, or a signature. A chunk of a
  sparse file's hole has no block at all.
- The **index** lists every entry as a path plus optional, typed metadata
  fields, so new fields can be added without breaking older readers. Large
  directories can be split into sub-indexes.
- A **trailer** at the end points to the index, so archives can be written
  in one pass, even to a pipe, and back to the zar header, so an archive can
  be found from the end of a file.

All integers are little-endian, all checksums CRC-32C (with optional BLAKE3
hashes for tampering), and all offsets
relative to a movable base, so an index can be added at the front of an
existing archive without changing any other offset.

## Project layout

```
.github/workflows/     CI: tests, sanitizers, fuzzing, and Linux packages
CMakeLists.txt         the build, and .deb and .rpm packaging (cpack)
FORMAT.md              the archive format specification
LICENSE.txt            BSD 3-Clause
PROPOSAL-signing.md    the design of hashes and signatures, and how it differs
                       from what was built
TODO.md                planned work
cmake/build_info.cmake records the git revision and build time
cmake/crypto.cmake     builds the bundled BLAKE3 and Monocypher, or finds
                       installed ones
man/                   manual pages for zar(1), unzar(1), and rezar(1)
src/
  zar_main.cpp         the zar command
  unzar_main.cpp       the unzar command
  rezar_main.cpp       the rezar command
  cli.hpp              command-line helpers shared by all three
  create_options.hpp   options for storing new files, shared by zar and rezar
  progress.hpp         --progress's line on the terminal, for all three
  zar/                 libzar, the format library
    format.*           zar and block headers
    index.*            index entries
    archive_writer.*   block layout, alignment, head index, trailer
    archive_reader.*   locating and reading archives, from files or streams
    creator.*          walking a tree and building an archive, or rebuilding
                       one from another (rezar)
    editor.*           what rezar's lists change in an archive
    extractor.*        extracting, testing, and scanning
    compressor.*       parallel chunk compression
    dictionary.*       collecting samples and training dictionaries
    dedup.*            chunk hashes for deduplication
    thread_pool.*      worker threads and in-flight limits for extraction
    posix.*            file descriptors, I/O loops, holes, free space,
                       temporary files, and user lookups
    output.*           buffered output, temporary files, solid compression
    crc32c.*           CRC-32C, hardware-accelerated
    hash.*             BLAKE3 block hashes
    signing.*          Ed25519 keys, signatures, and key files
    signature.*        SIGNATURE blocks
    fields.hpp         tagged fields, for the index and SIGNATURE blocks
    names.*            UTF-8 and path rules
    policy.*           which files are already compressed
    bytes.hpp          little-endian integers and varints
    error.hpp          format errors
    version.hpp.in     version information, filled in by CMake
tests/                 unit and end-to-end tests, and cli_test.sh for the
                       commands themselves (all run with ctest)
fuzz/                  a fuzz target for everything unzar reads
third_party/xxhash/    xxHash 0.8.3 (BSD 2-Clause), for XXH3-128
third_party/blake3/    BLAKE3 1.8.7 (CC0 / Apache 2.0), for block hashes
third_party/monocypher/ Monocypher 4.0.3 (BSD 2-Clause / CC0), for Ed25519
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

A fuzzing build skips CRC and hash checks, so that mutations get past them;
it has no install rules, and its commands must not be used.

## Roadmap

See [TODO.md](TODO.md). The main plans:

- **Application bundles** read in place by a loader: storing libraries
  uncompressed, so their single, page-aligned blocks can be `mmap`ed, and
  reproducible timestamps.
- **Mounting** an archive read-only with FUSE, decompressing only what's
  read.
- **Encryption**, with AEAD per block, so contents are private as well as
  signed.
- **Appending** to an archive in place, instead of writing a new one.

## License

zar is released under the [BSD 3-Clause License](LICENSE.txt). It includes
[xxHash](https://github.com/Cyan4973/xxHash) by Yann Collet, under the
[BSD 2-Clause License](third_party/xxhash/LICENSE),
[BLAKE3](https://github.com/BLAKE3-team/BLAKE3), under
[CC0 1.0](third_party/blake3/LICENSE_CC0) or Apache 2.0, and
[Monocypher](https://monocypher.org) by Loup Vaillant, under the
[2-clause BSD licence or CC0](third_party/monocypher/LICENCE.md), each
unless it's built with the matching `ZAR_USE_SYSTEM_*` option.

Copyright (C) 2026 Kevin H. Patterson
