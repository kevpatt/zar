# zar

`zar` and `unzar` create and extract archives of files and directories,
compressed with [zstd](https://github.com/facebook/zstd).

Unlike a `.tar.zst`, a zar archive is compressed piece by piece: every file
(and every chunk of a large file) is its own independently checksummed block,
and an index records where each one lives. Any file can be listed, verified,
or extracted without decompressing anything else.

The on-disk format is specified in [FORMAT.md](FORMAT.md).

> **Status:** early development. The format and library foundations are in
> place; the `zar` and `unzar` commands don't archive anything yet.

## Building

Requires a C++20 compiler, CMake 3.20+, and libzstd (`brew install zstd`,
`apt install libzstd-dev`).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

`build/compile_commands.json` is generated for clangd and other tools.

## Usage (planned)

```sh
zar -o project.zar ~/src/project     # archive a directory
zar -o - ~/src/project | ssh host 'cat > project.zar'
unzar -l project.zar                 # list
unzar -t project.zar                 # verify every checksum
unzar -C /tmp/out project.zar        # extract everything
unzar project.zar src/main.cpp       # extract one file
```

Archiving a directory stores paths relative to that directory, so
`zar -o p.zar ~/src/project` stores `src/main.cpp`, not
`project/src/main.cpp`.

### zar options

| Option | Meaning |
|---|---|
| `-o, --output=FILE` | archive to write (`-` for stdout) |
| `-l, --level=N` | zstd compression level (default 3) |
| `-j, --threads=N` | worker threads (default: one per CPU) |
| `--chunk-size=SIZE` | split files into independently compressed chunks of SIZE bytes (default 1M) |
| `--align[=N]` | align block payloads to N bytes; with no N, the CPU's cache-line size. The default is 8. N must be a power of two up to 1M; values below 8 pack tighter but leave fields unaligned. |
| `--owner` | store uid/gid and owner/group names (omitted by default) |
| `--nsec` | store the sub-second part of timestamps (omitted by default) |
| `--names=MODE` | file names that aren't valid UTF-8: `raw` stores the bytes and flags the entry (default), `skip` leaves the file out, `latin1` converts from Latin-1. All three warn. |
| `--normalize=FORM` | Unicode-normalize names to `nfc` or `nfd` (default: store names as the filesystem returns them) |
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
| `-l, --list` | list contents |
| `-t, --test` | verify all checksums without writing anything |
| `-k, --keep` | don't overwrite existing files |
| `-j, --threads=N` | worker threads (default: one per CPU) |
| `-v, --verbose` | list files as they are extracted |

`unzar` refuses paths that are absolute or contain `..`, and never writes
through a symbolic link.

## Layout

```
FORMAT.md           format specification
src/zar/            the format library (libzar)
src/zar_main.cpp    zar command
src/unzar_main.cpp  unzar command
tests/              unit tests (ctest)
```
