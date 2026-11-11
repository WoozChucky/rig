// llvm_compression_stubs.cpp: the zlib and zstd functions LLVM's libraries
// call, written here so building Rig needs nothing but LLVM's archive. The
// Windows archive doesn't ship the libraries it was built with
// (llvm-config --system-libs names zs.lib and zstd_static.lib), and on Linux
// they'd make zlib's and zstd's development packages a requirement.
//
// LLVM uses them only to compress or decompress sections and data, which
// Rig never asks it to do, so every compression function fails cleanly: LLVM
// turns the failure into an error message. The exception is crc32, which
// llvm::crc32 forwards to zlib's when LLVM is built with zlib: that one is a
// real CRC-32 (the one zlib computes: reflected, polynomial 0xEDB88320).
//
// The signatures are zlib's and zstd's own (zlib's uLong is unsigned long:
// 32 bits on Windows, 64 on Linux); only the names matter to the linker, but
// the types keep the calls well-defined. A stub either fails or has nothing
// to act on, so it ignores what it's given and its parameters are
// [[maybe_unused]]: they're there to match the library's declaration, not to
// be read.

extern "C" {

// zlib

enum {
    Z_STREAM_ERROR = -2,
    Z_DATA_ERROR = -3,
};

unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len) {
    if (!buf) return 0;  // zlib: crc32(0, NULL, 0) gives the initial value
    u32 c = ~(u32)crc;
    for (unsigned int i = 0; i < len; i++) {
        c ^= buf[i];
        for (int bit = 0; bit < 8; bit++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

unsigned long compressBound(unsigned long source_len) { return source_len; }

int compress2([[maybe_unused]] unsigned char *dest, [[maybe_unused]] unsigned long *dest_len,
              [[maybe_unused]] const unsigned char *source, [[maybe_unused]] unsigned long source_len,
              [[maybe_unused]] int level) {
    return Z_STREAM_ERROR;
}

int uncompress([[maybe_unused]] unsigned char *dest, [[maybe_unused]] unsigned long *dest_len,
               [[maybe_unused]] const unsigned char *source, [[maybe_unused]] unsigned long source_len) {
    return Z_DATA_ERROR;
}

// zstd. Its errors are the last values of size_t; ZSTD_isError knows them.

struct ZSTD_CCtx;

static const size_t ZSTD_GENERIC_ERROR = (size_t)-1;

size_t ZSTD_compressBound(size_t source_size) { return source_size; }

ZSTD_CCtx *ZSTD_createCCtx(void) { return nullptr; }

// ZSTD_createCCtx never makes a context, so there is none to free: freeing
// one (null) succeeds, as zstd's does.
size_t ZSTD_freeCCtx([[maybe_unused]] ZSTD_CCtx *cctx) { return 0; }

size_t ZSTD_CCtx_setParameter([[maybe_unused]] ZSTD_CCtx *cctx, [[maybe_unused]] int param,
                              [[maybe_unused]] int value) {
    return ZSTD_GENERIC_ERROR;
}

size_t ZSTD_compress2([[maybe_unused]] ZSTD_CCtx *cctx, [[maybe_unused]] void *dst,
                      [[maybe_unused]] size_t dst_capacity, [[maybe_unused]] const void *src,
                      [[maybe_unused]] size_t src_size) {
    return ZSTD_GENERIC_ERROR;
}

size_t ZSTD_decompress([[maybe_unused]] void *dst, [[maybe_unused]] size_t dst_capacity,
                       [[maybe_unused]] const void *src, [[maybe_unused]] size_t compressed_size) {
    return ZSTD_GENERIC_ERROR;
}

unsigned ZSTD_isError(size_t code) { return code > (size_t)-120; }

// Every error is the stubs' own, so whichever code it is, the name is the same.
const char *ZSTD_getErrorName([[maybe_unused]] size_t code) { return "zstd isn't part of this Rig build"; }

}  // extern "C"
