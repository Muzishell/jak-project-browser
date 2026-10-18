#include "compress.h"

#include <cstdio>
#include <cstring>

#include "common/util/Assert.h"

#include "fmt/format.h"
#include "third-party/zstd/lib/zstd.h"

namespace compression {

#ifdef __EMSCRIPTEN__
// size_t is 4 bytes on wasm32; the files are written by 64-bit tools with an 8-byte header
using HeaderSize = u64;
#else
using HeaderSize = size_t;
#endif

/*!
 * Compress data with zstd.  There is an 8-byte header containing the decompressed data's size.
 */
std::vector<u8> compress_zstd(const void* data, size_t size) {
  auto max_compressed = ZSTD_compressBound(size);
  std::vector<u8> result(sizeof(HeaderSize) + max_compressed);
  HeaderSize header = size;
  memcpy(result.data(), &header, sizeof(HeaderSize));
  auto compressed_size =
      ZSTD_compress(result.data() + sizeof(HeaderSize), max_compressed, data, size, 1);
  if (ZSTD_isError(compressed_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(compressed_size)));
  }
  result.resize(sizeof(HeaderSize) + compressed_size);
  return result;
}

/*!
 * Decompress data with zstd.  The first 8-bytes of the data should be a header containing the
 * decompressed data's size.
 */
std::vector<u8> decompress_zstd(const void* data, size_t size) {
  ASSERT(size >= sizeof(HeaderSize));
  HeaderSize header;
  memcpy(&header, data, sizeof(HeaderSize));
  size_t decompressed_size = header;
  size_t compressed_size = size - sizeof(HeaderSize);

  std::vector<u8> result(decompressed_size);
  auto decomp_size = ZSTD_decompress(result.data(), decompressed_size,
                                     (const u8*)data + sizeof(HeaderSize), compressed_size);
  if (ZSTD_isError(decomp_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(decomp_size)));
  }

  ASSERT(decomp_size == decompressed_size);
  return result;
}

std::vector<u8> compress_zstd_no_header(const void* data, size_t size) {
  size_t max_compressed = ZSTD_compressBound(size);
  std::vector<u8> result(max_compressed);

  size_t compressed_size = ZSTD_compress(result.data(), max_compressed, data, size, 1);
  if (ZSTD_isError(compressed_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(compressed_size)));
  }

  result.resize(compressed_size);
  return result;
}
}  // namespace compression
