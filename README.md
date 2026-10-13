# zar

`zar` and `unzar` create and extract archives of files and directories,
compressed with [zstd](https://github.com/facebook/zstd).

Unlike a `.tar.zst`, a zar archive is compressed piece by piece: every file
(and every chunk of a large file) is its own independently checksummed block,
and an index records where each one lives. Any file can be listed, verified,
or extracted without decompressing anything else.

The on-disk format is specified in [FORMAT.md](FORMAT.md).

> **Status:** early development. `zar` and `unzar` work; the format may
> still change before 1.0.

## Building

Requires a C++20 compiler, CMake 3.20+, and libzstd (`brew install zstd`,
`apt install libzstd-dev`).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

`build/compile_commands.json` is generated for clangd and other tools.

## Usage

```sh
zar ~/src/project                    # writes ~/src/project.zar
zar -o p.zar a b.txt                 # several inputs need -o
zar -o - ~/src/project | ssh host 'cat > project.zar'

unzar project.zar                    # extract into the current directory
unzar -C /tmp/out project.zar        # extract elsewhere
unzar project.zar src/main.cpp       # extract one file
unzar --list --scan project.zar      # list contents while checking the archive
unzar --test project.zar             # decompress and verify everything
```

Archiving a single directory stores paths relative to that directory, so
`zar ~/src/project` stores `src/main.cpp`, not `project/src/main.cpp`. With
several inputs, each is stored under its own name (`zar -o p.zar a b.txt`
stores `a/...` and `b.txt`).

Both commands exit with 0 on success, 1 if some files had problems (each
reported on stderr), and 2 on a fatal error. A fatal error in `zar` leaves no
archive behind.

### zar options

| Option | Meaning |
|---|---|
| `-o, --output=FILE` | archive to write (`-` for stdout); with a single input, defaults to the input's path plus `.zar` |
| `-y, --yes` | overwrite an existing archive |
| `-l, --level=N` | zstd compression level (default 3) |
| `-j, --threads[=N]` | worker threads (default: one per CPU core) |
| `--chunk-size=SIZE` | split files into independently compressed chunks of SIZE bytes (default 1M) |
| `--align[=N]` | align block payloads to N bytes; with no N, the CPU's cache-line size. The default is 8. N must be a power of two up to 1M; values below 8 pack tighter but leave fields unaligned. |
| `--owner[=WHICH]` | store ownership: `ids`, `names`, or `both` (the default when given); omitted otherwise |
| `--nsec` | store the sub-second part of timestamps (omitted by default) |
| `--all-times` | store access, status-change, and birth times as well as modification time |
| `--names=MODE` | file names that aren't valid UTF-8: `raw` stores the bytes and flags the entry (default), `skip` leaves the file out, `latin1` converts from Latin-1. All three warn. |
| `--compress-all` | compress files even if they're usually already compressed |
| `-v, --verbose` | list files as they are added |

Files whose extension marks them as already compressed — `.jpg`, `.png`,
`.mp3`, `.m4a`, `.mp4`, `.mov`, `.zip`, `.gz`, `.zst`, `.docx`, and others
(see `src/zar/policy.cpp`) — are stored without compression unless
`--compress-all` is given. Independently of that, any chunk that zstd fails to
shrink is stored raw.

### unzar options

| Option | Meaning |
|---|---|
| `-C, --directory=DIR` | extract into DIR |
| `-l, --list` | print a listing as entries are processed (see below) |
| `--scan` | check everything except decompression — index and block checksums, sizes, paths — and write nothing |
| `-t, --test` | like `--scan`, but also decompress and verify all data |
| `-y, --yes` | overwrite files and directories that already exist |
| `--dirs=MODE` | existing directories: `merge` (default) extracts into them; `replace` deletes them first. Either needs `-y`. |
| `-j, --threads[=N]` | worker threads (default: one per CPU core); chunks are verified, decompressed, and written in parallel |
| `-v, --verbose` | add permissions and owner to the listing; print a summary |

Before extracting anything, `unzar` checks whether any target already exists;
if one does and `-y` wasn't given, it stops without writing anything.

`unzar` rejects archives containing absolute paths or `..` components, never
writes through a symbolic link (including ones the archive itself creates),
and removes any file whose data fails verification.

The listing shows each directory as a heading, followed by its files with
their size and modification time, separated by tabs:

```
  README	6	2024-01-02 15:30:00
  link -> src/main.cpp	-	2026-10-13 08:32:53

src/
  main.cpp	13	2026-10-13 08:32:53

src/sub/
  blob.bin	3000000	2026-10-13 08:32:53
```

## Layout

```
FORMAT.md           format specification
src/zar/            the format library (libzar)
src/zar_main.cpp    zar command
src/unzar_main.cpp  unzar command
tests/              unit tests (ctest)
```
