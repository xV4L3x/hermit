#pragma once

// Opcode category predicates, by mnemonic. Single source of truth for the
// classifications that several analysis passes need, so the mnemonic lists live
// in one place instead of being re-spelled (and silently diverging) per file.
//
// Closures come in three sets that different passes deliberately use:
//   isPlainClosure      - user closures only (CreateClosure[LongIndex])
//   isClosureCreation   - plain + generator-closure + async-closure (captures an
//                         environment), but NOT a bare generator
//   isClosureOrGenerator- all of the above plus bare CreateGenerator[LongIndex]
// Pick the one that matches the pass; they are NOT interchangeable.

#include <string>

namespace hbc {

inline bool isPlainClosure(const std::string& mn) {
    return mn == "CreateClosure" || mn == "CreateClosureLongIndex";
}

inline bool isClosureCreation(const std::string& mn) {
    return isPlainClosure(mn) ||
           mn == "CreateGeneratorClosure" || mn == "CreateGeneratorClosureLongIndex" ||
           mn == "CreateAsyncClosure" || mn == "CreateAsyncClosureLongIndex";
}

inline bool isClosureOrGenerator(const std::string& mn) {
    return isClosureCreation(mn) ||
           mn == "CreateGenerator" || mn == "CreateGeneratorLongIndex";
}

inline bool isEnvLoad(const std::string& mn) {
    return mn == "LoadFromEnvironment" || mn == "LoadFromEnvironmentL";
}

inline bool isEnvStore(const std::string& mn) {
    return mn == "StoreToEnvironment" || mn == "StoreToEnvironmentL" ||
           mn == "StoreNPToEnvironment" || mn == "StoreNPToEnvironmentL";
}

inline bool isGetById(const std::string& mn) {
    return mn == "GetById" || mn == "GetByIdShort" || mn == "GetByIdLong" ||
           mn == "TryGetById" || mn == "TryGetByIdLong";
}

} // namespace hbc
