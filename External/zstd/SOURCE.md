# Zstandard 1.5.7

Upstream: https://github.com/meta/zstd/tree/f8745da6ff1ad1e7bab384bd1f9d742439278e99

The included code is used under the BSD license in LICENSE. Preserve that
license with source and binary distributions. combine.py is upstream's CC0
generator (its notice is retained). The upstream files are unmodified UTF-8
files fetched from the pinned commit, including its single-file generator.
Their original Git blob IDs are recorded in upstream-hashes.json and checked
before generation, independently of the generated-file SHA256 manifest.

Regenerate offline from the repository root with `python scripts/vendor-zstd.py`.
Verify without rewriting with `python scripts/vendor-zstd.py --check`.
This runs the official generator with `-r ../../lib -x legacy/zstd_legacy.h
-k zstd.h -o <absolute-output>/zstd.c zstd-in.c`, copies public headers and BSD
license, and records SHA256 hashes in manifest.json. Python/network access is
not needed by application builds. No installed zstd DLL is used.

The generated C file's retained `../zstd.h` include is rewritten to `zstd.h`
so that the two output files can live together. Upstream source is unchanged.

The project compiles the generated file as C without precompiled headers.
The shader cache uses synchronous level-3 compression, with no dictionary or
worker threads requested. Upstream's default amalgamation configuration is
retained, including disabling legacy formats and assembly.
