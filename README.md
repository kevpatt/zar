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

The version comes from `project()` in `CMakeLists.txt`. Each build also
records the git revision (with `-dirty` if there are uncommitted changes)
and the build time, shown by `--version` and `--help`; set
`SOURCE_DATE_EPOCH` to fix the build time for reproducible builds.

## Usage

```sh
zar ~/src/project                    # writes ~/src/project.zar
zar -o - ~/src/project | ssh host 'cat > project.zar'
zar --head-index -o - proj | ssh host 'unzar -'    # extract as it arrives
zar -m -o bundle.zar notes.txt photos    # several inputs need -m and -o

unzar project.zar                    # extract into the current directory
unzar -C /tmp/out project.zar        # extract elsewhere
unzar project.zar project/src/main.cpp   # extract one file
unzar --list --scan project.zar      # list contents while checking the archive
unzar --test project.zar             # decompress and verify everything
curl -s https://example.com/proj.zar | unzar -   # needs a --head-index archive
```

### What gets stored

zar follows POSIX path rules, as `tar` and `cp` do:

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
named after the current directory and written inside it.

zar archives one input unless `-m` is given. With `-m`, inputs that would
store the same path (two directories both named `src`, say) are an error.

Both commands exit with 0 on success, 1 if some files had problems (each
reported on stderr), and 2 on a fatal error. A fatal error in `zar` leaves no
archive behind.

### zar options

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
| `--nsec` | store the sub-second part of timestamps (omitted by default) |
| `--all-times` | store access, status-change, and birth times as well as modification time |
| `--names=MODE` | file names that aren't valid UTF-8: `raw` stores the bytes and flags the entry (default), `skip` leaves the file out, `latin1` converts from Latin-1. All three warn. |
| `--compress-all` | compress files even if they're usually already compressed |
| `--head-index` | also put the index at the front, so the archive can be extracted from a pipe. The index is written twice, at the front and at the end; data is staged in a temporary file until the index is known. |
| `--solid=SIZE` | first try compressing the whole archive as one stream, and keep that if it's at most SIZE bytes (e.g. `1M`); otherwise write a normal archive. See below. |
| `-v, --verbose` | list files as they are added |

Files whose extension marks them as already compressed — `.jpg`, `.png`,
`.mp3`, `.m4a`, `.mp4`, `.mov`, `.zip`, `.gz`, `.zst`, `.docx`, and others
(see `src/zar/policy.cpp`) — are stored without compression unless
`--compress-all` is given. Independently of that, any chunk that zstd fails to
shrink is stored raw.

### Solid archives

Normally every chunk of every file is compressed separately, so any file can
be read without the rest. For lots of small, similar files that costs
compression ratio, because zstd can't exploit what the files have in common.

With `--solid=SIZE`, zar first builds the archive with nothing compressed and
compresses it whole, as one stream. If the result is at most SIZE bytes, that
is the archive; if it grows past SIZE, zar stops and writes a normal archive
instead (so the attempt never costs more than SIZE bytes of output). Options
like `--align` then apply to the normal archive.

On a few small trees, a solid archive came out 14–41% smaller than a
normal one:

| Files | Normal | Solid |
|---|---|---|
| 13 config files | 5.8K | 5.0K |
| 38 source files | 72K | 58K |
| 135 headers | 461K | 270K |

`unzar` reads solid archives transparently, from files or pipes, but has to
decompress the whole thing first (into memory, or a temporary file when
it's large). So `--scan` decompresses too.

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
| `--max-size=SIZE` | refuse to decompress more than SIZE bytes in all (e.g. `10G`): the selected files' total is checked before anything is written, and a solid archive stops decompressing once it passes SIZE |
| `-h, --human-readable` | show sizes as `512`, `1.0K`, `31.4M`, `2.0G`, … (bytes below 1 KiB, then one decimal place) |
| `-v, --verbose` | add permissions and owner to the listing; print a summary |

`ARCHIVE` may be `-` for standard input. If that's a pipe, the archive must
have been made with `zar --head-index`: `unzar` reads the index from the
front, then extracts each file as its data arrives.

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
