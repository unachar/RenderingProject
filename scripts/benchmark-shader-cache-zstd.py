"""Read-only zstd storage probe for the existing v1 DXC cache.

Uses an explicitly supplied local DLL; neither installs nor ships a dependency.
Times include Python/ctypes overhead and are not renderer load-time benchmarks.
"""
import argparse
import ctypes
import json
from pathlib import Path
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path("EditorCache/Shaders"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--levels", type=int, nargs="+", default=[1, 3, 9])
    args = parser.parse_args()
    library_path = args.library.resolve(strict=True)
    library = ctypes.CDLL(str(library_path))
    library.ZSTD_versionString.restype = ctypes.c_char_p
    library.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
    library.ZSTD_compressBound.restype = ctypes.c_size_t
    library.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                    ctypes.c_size_t, ctypes.c_int]
    library.ZSTD_compress.restype = ctypes.c_size_t
    library.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    library.ZSTD_decompress.restype = ctypes.c_size_t
    library.ZSTD_isError.argtypes = [ctypes.c_size_t]
    library.ZSTD_isError.restype = ctypes.c_uint
    library.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
    library.ZSTD_getErrorName.restype = ctypes.c_char_p

    def checked(result):
        if library.ZSTD_isError(result):
            raise RuntimeError(library.ZSTD_getErrorName(result).decode())
        return result

    payloads = []
    skipped = 0
    for path in sorted(args.cache.glob("*.bin")):
        try:
            data = path.read_bytes()
        except FileNotFoundError:
            skipped += 1
            continue
        if len(data) < 32:
            skipped += 1
            continue
        magic, object_size, diagnostic_size, _ = struct.unpack_from("<4Q", data)
        if (magic != 0x3143584443484143 or not 0 < object_size <= 64 * 1024 * 1024
                or diagnostic_size > 1024 * 1024 or len(data) != 32 + object_size + diagnostic_size):
            skipped += 1
            continue
        payloads.append(data[32:])
    if not payloads:
        raise RuntimeError("No v1 cache payloads found")
    original_bytes = sum(map(len, payloads))
    results = []
    for level in args.levels:
        compressed_bytes = stored_bytes = raw_fallbacks = 0
        compress_seconds = decompress_seconds = 0.0
        for payload in payloads:
            capacity = library.ZSTD_compressBound(len(payload))
            encoded = ctypes.create_string_buffer(capacity)
            decoded = ctypes.create_string_buffer(len(payload))
            start = time.perf_counter()
            size = checked(library.ZSTD_compress(encoded, capacity, payload, len(payload), level))
            compress_seconds += time.perf_counter() - start
            start = time.perf_counter()
            restored = checked(library.ZSTD_decompress(decoded, len(payload), encoded, size))
            decompress_seconds += time.perf_counter() - start
            if restored != len(payload) or decoded.raw != payload:
                raise RuntimeError("zstd roundtrip changed cache bytes")
            compressed_bytes += size
            stored_bytes += min(size, len(payload))
            raw_fallbacks += size >= len(payload)
        result = dict(level=level, compressed_payload_bytes=compressed_bytes,
                      payload_bytes_with_raw_fallback=stored_bytes, raw_fallback_files=raw_fallbacks,
                      saved_fraction=1 - stored_bytes / original_bytes,
                      compress_seconds=compress_seconds, decompress_seconds=decompress_seconds,
                      roundtrip_files=len(payloads))
        results.append(result)
        print(f"level {level}: {original_bytes} -> {stored_bytes} payload bytes, "
              f"{result['saved_fraction']:.1%} saved; {len(payloads)} exact roundtrips; "
              f"compress {compress_seconds:.3f}s / decode {decompress_seconds:.3f}s")
    report = dict(library=str(library_path), version=library.ZSTD_versionString().decode(),
                  cache=str(args.cache.resolve()), files=len(payloads), skipped_files=skipped,
                  original_payload_bytes=original_bytes, original_header_bytes=32 * len(payloads),
                  scope="Snapshot of structurally valid v1 cache payloads, including old/duplicate variants. "
                        "Existing checksums are not audited. Header bytes excluded; new format overhead unspecified. "
                        "Single Python/ctypes run; no renderer startup/FPS claim.", results=results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"zstd {report['version']}; probe made no source-cache writes; report: {args.output}")


if __name__ == "__main__":
    main()
