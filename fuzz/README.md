# Fuzzing

Coverage-guided fuzz targets for the untrusted input boundaries of libdrmtap.

## fuzz_convert

Fuzzes `drmtap_convert_dmabuf()`, the split-capture convert entry point. In the
split model an unprivileged converter receives a `drmtap_dmabuf_desc` plus a
DMA-BUF fd over IPC from another process, so the descriptor is untrusted. The
target drives a hostile descriptor (geometry, num_planes, per-plane
offsets/pitches, format, modifier, HDR state) together with a REAL dma-buf of
arbitrary size (a sealed memfd wrapped by udmabuf, since the convert path now
rejects non-dma-buf fds), and forces the CPU mmap+deswizzle path via
`DRMTAP_NO_EGL=1` (that is where the untrusted geometry is read). Under
AddressSanitizer any out-of-bounds access, overflow, or fault (e.g. a fd smaller
than the declared geometry) on a malformed descriptor is a finding.

Needs `/dev/udmabuf` (the target no-ops when it or a DRM device is absent).

## Build and run

Needs clang with libFuzzer (`clang -fsanitize=fuzzer,address`).

    fuzz/build.sh /tmp/fuzz_convert
    mkdir -p corpus
    ASAN_OPTIONS=detect_leaks=0 /tmp/fuzz_convert \
        -max_len=65536 -rss_limit_mb=4096 -max_total_time=300 corpus

A crash writes the reproducing input to `crash-<hash>`; replay it with
`/tmp/fuzz_convert crash-<hash>`. `detect_leaks=0` suppresses the deliberate
process-lifetime EGL/Mesa allocation (same rationale as tests/lsan.supp).

The target needs a DRM render node (or card) to open a context; it no-ops when
none is present.
