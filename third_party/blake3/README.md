# BLAKE3 1.8.7

The C implementation from https://github.com/BLAKE3-team/BLAKE3, tag
`1.8.7` (source archive SHA-256
`c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c`): the
files in `c/` that zar compiles, unchanged. zar uses the portable code, the
NEON intrinsics on 64-bit ARM, and the SSE2, SSE4.1, AVX2, and AVX-512
intrinsics on x86, chosen at run time.

Licensed under CC0 1.0, Apache 2.0, or Apache 2.0 with LLVM exceptions, at
your choice: see `LICENSE_CC0`, `LICENSE_A2`, and `LICENSE_A2LLVM`.
