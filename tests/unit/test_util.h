// Helpers shared by the unit tests.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include "hbc/hbc.h"

#include <fstream>
#include <string>

// HBC_CORPUS_DIR is injected by CMake so tests find the corpus from any working directory.
#ifndef HBC_CORPUS_DIR
#  error "HBC_CORPUS_DIR must be set by CMake"
#endif

namespace testutil {

// Opens a corpus file; skips the test if it isn't there. Several corpus files
// (`*_g0.hbc`, ridemovi.hbc, ...) are gitignored and generated with
//   .venv/bin/python -m seeder.compile --g0 seeder/samples/<sample>
inline hbc::HBCFile openOrSkip(const std::string& path) {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.good())
        SKIP("missing corpus: " + path + "  (regenerate with `seeder.compile --g0`)");
    return hbc::HBCFile::open(path);
}

// Numeric value of an operand: Imm32/Addr8/Addr32 live in i32, Double in f64,
// everything else (Reg*, UInt*, string index) in u32.
inline double opValue(const hbc::Operand& op) {
    using T = hbc::Operand::Type;
    switch (op.type) {
        case T::Imm32:
        case T::Addr8:
        case T::Addr32: return static_cast<double>(op.i32);
        case T::Double: return op.f64;
        default:        return static_cast<double>(op.u32);
    }
}

} // namespace testutil
