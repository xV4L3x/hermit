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

// Call1..Call4: the only Call-family opcodes whose top-of-frame call-setup
// window (StackFrameLayout::CallerExtraRegistersAtEnd + numArgsIncludingThis -
// include/hermes/BCGen/HBC/StackFrameLayout.h) is NEVER visible as a register
// operand anywhere: their own operands name arbitrary SOURCE registers
// (wherever the callee/this/args already live), and the interpreter copies
// those values into implicit frame-top slots at call time (lib/VM/
// Interpreter.cpp's `doCall`), so nothing forces the register allocator to
// expose the window as a Reg8/Reg32 operand the way it does elsewhere.
//
// The other Call-family opcodes (Call, Construct, CallLong, ConstructLong,
// CallBuiltin, CallBuiltinLong, CallDirect, CallDirectLongIndex) read their
// args from an implicit top-of-frame window too, but hermesc materializes
// that window with ordinary Mov instructions into real register operands
// before the call - so a plain max-register scan over the body already
// accounts for it; adding the same padding again on top of an already-
// compiled function's frameSize over-counts and false-positives the
// exception-handler frameSize-growth guard (regression caught by
// tests/integration/patcher/test_multiversion.py on try/catch functions that
// call `new Error(...)` via Construct). Deliberately NOT covered here.
struct CallArgInfo {
    bool     isCall        = false;
    uint32_t fixedArgs     = 0;   // numArgsIncludingThis
};

inline CallArgInfo callArgInfo(const std::string& mn) {
    if (mn == "Call1") return {true, 1};
    if (mn == "Call2") return {true, 2};
    if (mn == "Call3") return {true, 3};
    if (mn == "Call4") return {true, 4};
    return {};
}

} // namespace hbc
