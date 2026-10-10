// Private header. The free functions are declared here so the unit tests can
// call them without going through HBCFile::decompile().
#pragma once

#include "hbc/hbc.h"

#include <string>
#include <vector>

namespace hbc {

// Everything needed to turn raw operands (register, string and function
// indices) into readable JS identifiers.
struct DecompileContext {
    // Display name per register: rN, `this` or param_N.
    const std::vector<std::string>&  regNames;
    // The function table, to name the target of CreateClosure*/CallDirect*.
    const std::vector<FunctionInfo>& functions;
    // Raw strings. Never use Operand::resolvedString: it carries the
    // " [identifier]" / " [string]" suffix.
    const std::vector<std::string>&  strings;
    // Variables of this function's OWN scope (scope_variables()), indexed by
    // environment slot (slot N is position N).
    const std::vector<std::string>&  envSlotNames;
    // Register holding the environment created by this function (its first
    // CreateEnvironment), or -1. Store/LoadFromEnvironment on that register use
    // envSlotNames. On any other register (an outer function's environment, from
    // GetEnvironment) they fall back to envSlot_N: resolving those would mean
    // walking the closure chain, and a wrong name is worse than a placeholder.
    int64_t ownEnvReg = -1;
    // File-wide literal buffers (HBCFile::array_buffer() etc.).
    // NewArrayWithBuffer/NewObjectWithBuffer carry an offset into them, decoded
    // to render a real `[...]` / `{...}` literal.
    const std::string& arrayBuffer;
    const std::string& objKeyBuffer;
    const std::string& objValueBuffer;
};

// One leaf (non control-flow) instruction as a line of pseudo-JS; also Ret and
// Throw. Jumps are consumed by structureCFG. Returns "" for VM bookkeeping with
// no JS equivalent (CreateEnvironment, ProfilePoint, ...): the caller skips it
// without emitting an empty line.
std::string emitExpression(const Instruction& instr, const DecompileContext& ctx);

// Rebuilds sequences, if/else, while, do-while and try/catch from the CFG and
// returns the function body (no signature). Anything else (switch, irregular
// CFG) is dumped per block under a "// unstructured region: B<n>" comment.
std::string structureCFG(
    const CFGData&                            cfg,
    const std::vector<ExceptionHandlerInfo>&  exceptionHandlers,
    const std::vector<SourceLocation>&        srcLocs,
    const DecompileContext&                   ctx);

} // namespace hbc
