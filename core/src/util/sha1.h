// SHA-1 for the BytecodeFileFooter. Hermes uses llvh::SHA1 for the same
// purpose; this is a self-contained one-shot copy to avoid depending on LLVM.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hbc {

// SHA-1 of `len` bytes starting at `data`. Returns the 20-byte digest.
std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len);

}  // namespace hbc
