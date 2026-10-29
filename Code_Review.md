# Code review, 2026-10-14

A review of zar and unzar at 0.2.5 (commit b47d354) for clarity, elegance,
correctness, and security, covering everything in `src/`, the tests, and
the build. Everything found was fixed in 0.3.6.

## Method

- **Reading:** all of `src/`, `tests/`, `CMakeLists.txt`, and the parts of
  FORMAT.md and README.md the code relies on.
- **Confirming:** each suspected bug was reproduced before being fixed,
  with crafted archives (run under AddressSanitizer and UBSan) or with the
  command-line tools.
- **Regression tests:** each one was run against the 0.2.5 code to make
  sure it catches the original bug.
- **Fuzzing:** about 1.8 million mutated archives under AddressSanitizer
  and UBSan, the last 600,000 against the finished code. None found a
  problem.

## Summary

| # | Severity | Finding | Where (0.2.5) |
|---|---|---|---|
| 1 | Serious | Heap overflow reading a stream whose `base_offset` is under 32 | `archive_reader.cpp` `open_stream` |
| 2 | Serious | A tiny stream can make unzar allocate any amount of memory | `archive_reader.cpp` `block_header`, `next_block`, `open_stream` |
| 3 | Serious | `--dirs=replace` deletes directories outside the selection | `extractor.cpp` `prepare` |
| 4 | Medium | Signed overflow computing block positions | `archive_reader.cpp` `block_header`, `next_block` |
| 5 | Medium | Compressed blocks can claim memory they never fill; no default limit on total output | `archive_reader.cpp`, `extractor.cpp` |
| 6 | Medium | Extraction memory is limited by chunk count, not bytes | `extractor.cpp` `in_flight` |
| 7 | Medium | Archive paths in messages can send terminal control sequences | `unzar_main.cpp`, `zar_main.cpp` |
| 8 | Medium | `zar -y` includes the archive it's replacing | `creator.cpp` `add`, `output.cpp` |
| 9 | Medium | Data race on `stream_error` | `extractor.cpp` `run_stream`, `complete` |
| 10 | Low | As root, set-id bits are kept even when no owner is restored | `extractor.cpp` `apply_metadata` |
| 11 | Low | A FIFO swapped in during archiving hangs zar | `creator.cpp` `add_file` |
| 12 | Low | A file that appears during archiving is replaced without `-y` | `output.cpp` `create_file`, `commit` |
| 13 | Low | User and group lookups fail for large records | `creator.cpp`, `extractor.cpp` |
| 14 | Low | Short writes in `Output::write_at` are reported as errors | `output.cpp` |
| 15 | Clarity | Duplicated POSIX plumbing and other tidying | throughout |
| 16 | Tests | No fuzzing; ARM CRC fallback is slow on Linux | `tests/`, `crc32c.cpp` |

## Serious

### 1. Heap overflow reading a stream

**Problem:**
- `open_stream` sized its buffer to the header's `base_offset`, then copied
  the 32-byte header into it.
- Nothing required `base_offset` to be at least 32.
- With `base_offset = 8` (aligned, so accepted), the copy wrote 24 bytes past
  the end of the buffer.
- This is reachable from `curl … | unzar -`.

**Evidence:** a 96-byte input piped to `unzar --scan -` triggered an
AddressSanitizer heap-buffer-overflow at `archive_reader.cpp:598`.

**Fix:**
- FORMAT.md §2.2 now requires `base_offset` to be 0 or at least 32, so the
  base can't fall inside the zar header. `ArchiveHeader::decode` enforces it.
- `open_stream` also refuses a negative index offset when `base_offset` is
  0, and reads the rest of the head piecewise.

**Test:** `stream_rejects_a_base_inside_the_header`, and an extra case in
`archive_header_rejects_damage`.

### 2. Unbounded allocation from a stream

**Problem:**
- `block_header()` checked a block's `payload_size` against the file size,
  but skipped the check for streams.
- `payload()` then allocated and zero-filled whatever size the header
  claimed.
- `open_stream` likewise allocated `base_offset` bytes up front (up to 4 GiB).
- `next_block` allocated a data block's claimed size (up to 4 GiB) before
  reading it.
- This contradicted the README's claim that lengths and offsets are checked
  against the archive's bounds.

**Evidence:** a 160-byte stream claiming a 1 TB index block drove unzar to
a 156 GB peak memory footprint before the process was killed, after about
20 seconds.

**Fix:**
- For a stream, `block_header()` checks against what has actually been read
  (`File::reachable()`).
- The head and data blocks are read through `File::read_next(Bytes&,
  count)`, which grows the buffer 1 MiB at a time as data arrives.
- The same input now fails immediately, with a 1.3 MB peak.

**Test:** `stream_blocks_cannot_claim_more_than_the_stream_holds`. It
covers both the head index and a 3 GiB data block in a truncated stream,
and checks that peak memory stays under 1 GiB.

### 3. `--dirs=replace` deleted unselected files

**Problem:**
- Replace mode deleted every existing top-level directory among the groups.
- Selecting `a/b` creates a group for its parent `a`.
- So `unzar -y --dirs=replace r.zar a/b` deleted all of `a`, including files
  that had nothing to do with the selection.

**Evidence:** `dst/a/keep/data` was deleted by that command.

**Fix:**
- Replace mode now deletes only directories extracted whole, meaning those
  with their own entry: a selection includes a directory's entry only along
  with everything below it.
- Only the topmost of those is removed.
- The help text and README describe this.

**Test:** `replacing_directories_spares_what_was_not_selected`.

## Medium

### 4. Signed overflow computing block positions

**Problem:** `base_ + offset` was computed as `int64_t` before the range
check, so a crafted offset caused undefined behaviour.

**Evidence:** UBSan reported "signed integer overflow: 4096 +
9223372036854775792" at `archive_reader.cpp:396`.

**Fix:** a single `position_of(base, offset)` helper validates offsets with
unsigned arithmetic. It's used by `block_header`, `payload`, `next_block`
and `unwrap`.

**Test:** `block_offsets_cannot_overflow` (UBSan-clean).

### 5. Decompression bombs

**Problem:**
- `decompress_frame` allocated and zero-filled whatever content size a zstd
  frame declared: up to 4 GiB for an index, or a chunk's length.
- `--max-size` was off by default, so dedup-amplified or solid archives
  could fill memory or disk.

**Evidence:** an 80-byte archive whose index frame declares 4 GiB made
`unzar --scan` use 4.3 GB.

**Fix:**
- *Maximum ratio:* a zstd block yields at most 128 KiB and takes at least 4
  bytes (a 3-byte header plus 1 byte of content). So no valid frame
  decompresses to more than 32,768 times its size, and
  `frame_content_size()` rejects any frame claiming more.
  - Measured: zeros reach 32,740× at level 22, so the bound is tight but
    holds.
  - It runs before any allocation and also catches lying frames in
    `--scan`.
- *Free-space default:* without `--max-size`, extraction refuses selections
  whose total exceeds the free space at the destination.
- *Solid archives:* without a limit, a solid archive spilling to `$TMPDIR`
  stops before exceeding the free space there.

**Tests:**
- `compressed_blocks_cannot_claim_more_than_they_hold` covers the index and
  a chunk, with peak memory checked. It also checks that a genuine 64 MiB
  run of zeros in a single block still extracts.
- `extraction_is_limited_to_the_free_space` covers the free-space default.
- The cap on `$TMPDIR` spilling has no automated test, because testing it
  would mean filling a disk.

**Note:** an intermediate fix decompressed in streaming mode into a growing
buffer. It was dropped for three reasons:
- zstd's streaming decoder keeps an internal window and rejects windows over
  128 MiB, which the one-shot decoder accepts.
- It was 1.7× slower on `--contiguous` archives.
- Its memory savings depended on how the allocator committed pages.

### 6. Extraction memory bounded only by chunk count

**Problem:** up to `threads × 4` chunks could be in flight. With
`--contiguous`, one chunk can be 4 GiB, so even a legitimate archive could
need tens of GB.

**Fix:**
- A `Budget` (`thread_pool.hpp`) limits work in flight both by count and by
  bytes: 16 MiB per thread.
- A chunk larger than the byte limit doesn't count against it, but only one
  such chunk runs at a time. So memory stays below the limit plus the
  largest chunk, and small chunks keep flowing beside a big one.
- On Utsuri-R (680 MB, normal and `--contiguous`), time and peak memory match
  0.2.5.

**Test:** `budget_limits_items_and_bytes`.

### 7. Terminal escape injection

**Problem:** file names were escaped in `--list` output but not in
warnings or errors. A name containing `ESC[31m` reached stderr raw.

**Fix:**
- `printable()` moved to `cli.hpp`.
- Every warning and error from both tools now goes through it, as do zar's
  `-v` entry names.

**Test:** `printable_names_cannot_control_the_terminal`.

### 8. `zar -y` archived its previous output

**Problem:** zar skipped only the temporary file it was writing, so
`cd proj; zar .; zar -y .` stored the old `proj.zar` inside the new one.

**Fix:**
- `Output::create_file` records the device and inode of the file it will
  replace.
- `Output::is_file()` matches either that file or the one being written.
- Compressing outputs pass both along.

**Test:** `writer_skips_the_archive_it_replaces`.

### 9. Data race on `stream_error`

**Problem:**
- The main thread assigned `stream_error` in `run_stream` while workers read
  it in `complete()`.
- Lost files were also recognised by comparing error strings, so an
  unrelated error with the same text could be miscounted.

**Fix:**
- A block that a broken stream will never deliver now throws `StreamLost`.
- `FileJob::fail(what, lost)` records whether a job's first failure was
  `StreamLost`, and `complete()` reads that flag.
- `stream_error` is now used only on the main thread.

**Test:** `a_broken_stream_is_reported_once`, run five times per suite; it
is clean under ThreadSanitizer.

## Low

### 10. Set-id bits as root

**Problem:** running as root, a file whose mode includes `4755` but which
has no owner information became a setuid-root file.

**Fix:** `permitted_mode()` keeps set-id bits only if the file's owner was
actually restored from the archive.

**Test:** `set_id_bits_need_a_restored_owner`.

### 11. A FIFO swapped in during archiving hung zar

**Problem:** between `lstat` and `open`, a regular file replaced by a FIFO
made `open` block forever.

**Fix:**
- Files are opened with `O_NONBLOCK`.
- `fstat` must then show the same regular file; otherwise it's skipped with
  a warning.
- Size and metadata now come from the opened file.

**Test:** `writer_skips_files_replaced_while_reading`. It uses a test hook,
`before_open_for_test`, following the precedent of `hash_chunk_override`.

### 12. Overwrite race without `-y`

**Problem:** `create_file` checked whether the archive existed, then
`commit` renamed over it later. A file created in between was silently
replaced.

**Fix:**
- Without `-y`, `commit` uses an exclusive rename: `renamex_np(RENAME_EXCL)`
  on macOS, `renameat2(RENAME_NOREPLACE)` on Linux.
- Where neither is available, it falls back to `link()` plus `unlink()`.

**Test:** `output_never_replaces_a_file_that_appeared_without_overwrite`.

### 13. Owner name lookups with fixed buffers

**Problem:** the `getpw*_r` and `getgr*_r` calls used fixed 4 KB buffers and
gave up on `ERANGE`. Large directory-service groups silently lost their
names.

**Fix:** the lookups are shared helpers in `posix.cpp` that double the
buffer on `ERANGE`, up to 16 MiB.

**Test:** `user_and_group_lookups_grow_their_buffers`, which starts from a
1-byte buffer.

### 14. Short writes in `Output::write_at`

**Problem:** a short `pwrite` was treated as failure and reported a stale
`errno`.

**Fix:** it now uses `pwrite_all`, which retries.

**Test:** none specific; existing tests cover the path.

## Clarity and elegance

**Changes:**
- **New `posix.hpp`/`posix.cpp`:** `UniqueFd`, `throw_errno`,
  `read_full`/`pread_full`/`write_all`/`pwrite_all`, `temp_directory`,
  `anonymous_temp_file`, `free_space`, and the user and group lookups. They
  replace two fd wrappers, about seven hand-written I/O loops, three
  `throw_errno` functions, and duplicated `$TMPDIR` and passwd code.
- **Inflater:** one `Inflater::inflate(block, read)` replaces the two
  copies of the solid-archive decompression loop.
- **`Creator` constructor:** it returns early for the sampling pass,
  instead of relying on `&&` short-circuiting to avoid dereferencing an
  empty `writer_`.
- **Format version:** `kFormatVersion` is documented as changing only when
  the format does. It moves to 0.3.6 here because of the `base_offset` rule.
- **Smaller changes:**
  - unzar counts its own warnings, and takes the extractor's from
    `ExtractStats` rather than counting them twice.
  - `decode_entry` tracks repeated tags with a 64-bit mask, instead of
    allocating a `std::set` per entry.
  - zar's long-option enum is in order (`kHelp` was 255, ahead of the
    others).

## Tests and tooling

- **18 new or extended regression tests:** 72 in all, up from 58.
- **New `CHECK_THROWS_WITH`:** checks an exception's message as well as its
  type.
- **Fuzz target (`fuzz/fuzz_archive.cpp`, `-DZAR_BUILD_FUZZERS=ON`):**
  - It covers index decoding, opening archives from files and from pipes,
    and scanning and testing them.
  - It uses libFuzzer where the compiler has it. Otherwise a built-in driver
    makes seed archives (plain, head-index, dictionary, solid) and mutates
    them; `-artifact` saves each input so a crash can be reproduced.
  - Fuzzing builds define `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`, which
    makes `crc_matches()` accept any CRC, and have no install rules.
- **ARM CRC-32C:**
  - The hardware path is now used when the CPU has the CRC instructions,
    checked at run time (`getauxval` on Linux, `sysctl` on macOS), even if
    the compiler wasn't told to assume them.
  - Before, such builds fell back to byte-at-a-time code; that's typical on
    Linux aarch64.
  - Verified with a build using `-march=armv8-a`.

## Still open

- **CI:** the TODO's CI item should run the fuzzer for a fixed time and
  cover Linux. Since 0.3.7, zar builds and passes its tests on Linux with
  GCC, but only when run by hand.
- **Default limit for `unzar --test`:** the free-space default applies only
  when extracting. Test mode is limited by the 32,768× rule and the
  in-flight budget, but a dedup-amplified archive can still make it
  decompress a lot of data, though never much memory at once.
