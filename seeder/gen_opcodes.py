#!/usr/bin/env python3
"""
gen_opcodes.py - generates C++ opcode tables for hermit from a Hermes BytecodeList.def file.

Usage:
    python seeder/gen_opcodes.py <BytecodeList.def> <version> > core/src/format/opcodes_vNN.cpp

Example (from repo root):
    python seeder/gen_opcodes.py \\
        /path/to/hermes/include/hermes/BCGen/HBC/BytecodeList.def 96 \\
        > core/src/format/opcodes_v96.cpp

The generated file defines a static OPCODES[256] array in the hbc::vNN namespace.
Each entry describes the opcode: mnemonic, operand count, physical type of each
operand, and a bitmask indicating which operands are string table indices.

The generated files are committed to the repo (not regenerated on every build).
"""

import re
import sys

# Mapping between the operand types in Hermes' .def file and the C++ enum
# constants in opcodes.h. "StringID" is NOT in this map: it is handled via
# stringRefMask (a bit set on OPERAND_STRING_ID), not as a separate physical
# type. The physical type stays UInt8/16/32 and determines the size read
# from the bytecode.
OPERAND_TYPE_MAP = {
    'Reg8':   'OperandType::Reg8',
    'Reg32':  'OperandType::Reg32',
    'UInt8':  'OperandType::UInt8',
    'UInt16': 'OperandType::UInt16',
    'UInt32': 'OperandType::UInt32',
    'Imm32':  'OperandType::Imm32',
    'Double': 'OperandType::Double',
    'Addr8':  'OperandType::Addr8',
    'Addr32': 'OperandType::Addr32',
}

def parse_def(path):
    """
    Reads and parses a BytecodeList.def, returning:
      - opcodes: list of (name: str, operands: [str]) in opcode-byte order
      - string_refs: dict of {name: set of int} (0-indexed operand positions)
    """
    raw_text = open(path, encoding='utf-8').read()

    # --- Step 1: collect all OPERAND_STRING_ID annotations before doing any
    #   other transformation (they appear both before and after the DEFINE_OPCODE
    #   lines). OPERAND_STRING_ID(Name, N) -> operand N (1-indexed) of Name is a
    #   string index.
    string_refs = {}  # name -> set of 0-indexed positions
    for m in re.finditer(r'\bOPERAND_STRING_ID\(\s*(\w+)\s*,\s*(\d+)\s*\)', raw_text):
        name = m.group(1)
        pos  = int(m.group(2)) - 1   # convert to 0-indexed
        string_refs.setdefault(name, set()).add(pos)

    # --- Step 2: merge backslash-continuation lines (e.g. multi-line #define).
    raw_lines = raw_text.splitlines()
    joined = []
    buf = ''
    for line in raw_lines:
        stripped = line.rstrip()
        if stripped.endswith('\\'):
            buf += stripped[:-1] + ' '
        else:
            buf += stripped
            joined.append(buf)
            buf = ''
    if buf:
        joined.append(buf)

    # --- Step 3: (removed) the #ifdef HERMES_RUN_WASM block used to be stripped
    #   out under the assumption that RN bundles didn't use those opcodes. The
    #   assumption was wrong: Meta's Hermes compiler for real RN apps is built
    #   with HERMES_RUN_WASM, so Add32/Sub32/…/Store32 (0xC0–0xCD) do exist in
    #   the binary format. The .def file is now passed through unchanged to
    #   step 4.
    filtered = joined

    # --- Step 4: expand the DEFINE_JUMP_1/2/3 macros into their Short+Long pairs.
    #   DEFINE_JUMP_1(X)  ->  DEFINE_OPCODE_1(X, Addr8) + DEFINE_OPCODE_1(XLong, Addr32)
    #   DEFINE_JUMP_2(X)  ->  DEFINE_OPCODE_2(X, Addr8, Reg8) + DEFINE_OPCODE_2(XLong, Addr32, Reg8)
    #   DEFINE_JUMP_3(X)  ->  DEFINE_OPCODE_3(X, Addr8, Reg8, Reg8) + DEFINE_OPCODE_3(XLong, Addr32, Reg8, Reg8)
    jump_patterns = [
        (re.compile(r'\bDEFINE_JUMP_1\(\s*(\w+)\s*\)'), 1),
        (re.compile(r'\bDEFINE_JUMP_2\(\s*(\w+)\s*\)'), 2),
        (re.compile(r'\bDEFINE_JUMP_3\(\s*(\w+)\s*\)'), 3),
    ]
    expanded  = []
    jump_names: set = set()   # nomi opcode (Short e Long) generati da DEFINE_JUMP_*
    for line in filtered:
        # Skip #define lines (macro definitions, not invocations):
        # "#define DEFINE_JUMP_1(name) ..." becomes a single line after joining;
        # without this check the regex would capture "name" as a mnemonic -> 6 spurious entries.
        if line.strip().startswith('#'):
            expanded.append(line)
            continue
        matched = False
        for pat, n in jump_patterns:
            m = pat.search(line)
            if m:
                name = m.group(1)
                jump_names.add(name)
                jump_names.add(name + 'Long')
                if n == 1:
                    expanded.append(f'DEFINE_OPCODE_1({name}, Addr8)')
                    expanded.append(f'DEFINE_OPCODE_1({name}Long, Addr32)')
                elif n == 2:
                    expanded.append(f'DEFINE_OPCODE_2({name}, Addr8, Reg8)')
                    expanded.append(f'DEFINE_OPCODE_2({name}Long, Addr32, Reg8)')
                elif n == 3:
                    expanded.append(f'DEFINE_OPCODE_3({name}, Addr8, Reg8, Reg8)')
                    expanded.append(f'DEFINE_OPCODE_3({name}Long, Addr32, Reg8, Reg8)')
                matched = True
                break
        if not matched:
            expanded.append(line)

    # --- Step 5: extract DEFINE_OPCODE_N in order (= opcode byte index).
    #   Skip lines starting with '#' (preprocessor directives: #define, #undef,
    #   #ifndef, #ifdef, #endif) to avoid matching the macro definitions themselves.
    opcode_re = re.compile(r'\bDEFINE_OPCODE_\d+\(\s*(\w+)\s*(?:,([^)]*))?\)')
    opcodes = []
    for line in expanded:
        if line.strip().startswith('#'):
            continue
        m = opcode_re.search(line)
        if m:
            name = m.group(1)
            args_str = m.group(2) or ''
            args = [a.strip() for a in args_str.split(',') if a.strip()]
            opcodes.append((name, args))

    return opcodes, string_refs, jump_names


def _jump_kind(name: str, args: list, jump_names: set) -> str:
    """Returns the C++ JumpKind constant for the given opcode."""
    if name in ('Unreachable', 'Throw'):
        return 'JumpKind::Throw'
    if name == 'Ret':
        return 'JumpKind::Return'
    if name == 'SwitchImm':
        return 'JumpKind::Switch'
    if name in jump_names:
        # 1 operand (Addr8/Addr32 only) -> unconditional jump (Jmp, JmpLong)
        # 2+ operands -> conditional jump (condition + address)
        return 'JumpKind::Unconditional' if len(args) == 1 else 'JumpKind::Conditional'
    return 'JumpKind::None'


def generate_cpp(opcodes, string_refs, version, jump_names=None):
    """
    Generates the C++ code for the OPCODES[256] array for the given version.
    Prints directly to stdout so it can be redirected to a file.
    """
    if jump_names is None:
        jump_names = set()
    lines = []
    lines.append(f'// auto-generated by seeder/gen_opcodes.py - do NOT edit by hand')
    lines.append(f'// source: BytecodeList.def (Hermes HBC v{version})')
    lines.append(f'// to regenerate: python seeder/gen_opcodes.py <BytecodeList.def> {version}')
    lines.append(f'//')
    lines.append(f'// stringRefMask: bit N set  ⟹  operand N (0-indexed) is a string table index.')
    lines.append(f'// The physical type stays UInt8/UInt16/UInt32 and determines the size in bytes;')
    lines.append(f'// the disassembler resolves the string when the corresponding bit is set.')
    lines.append(f'')
    lines.append(f'#include "format/opcodes.h"')
    lines.append(f'')
    lines.append(f'namespace hbc {{')
    lines.append(f'namespace v{version} {{')
    lines.append(f'')
    # "extern const" is required because in C++ a const at namespace scope has
    # internal linkage by default (like static). The extern keyword forces external
    # linkage so opcodes.cpp can reference the array via "extern const".
    lines.append(f'// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)')
    lines.append(f'extern const OpcodeInfo OPCODES[256] = {{')

    n_total = len(opcodes)
    n_unknown = 0

    for idx in range(256):
        if idx < n_total:
            name, args = opcodes[idx]

            # Map the operand types to C++ constants
            operand_cpp = []
            for arg in args:
                mapped = OPERAND_TYPE_MAP.get(arg)
                if mapped is None:
                    print(f'// WARNING: unknown operand type "{arg}" for {name}',
                          file=sys.stderr)
                    mapped = f'OperandType::UInt8 /* UNKNOWN: {arg} */'
                operand_cpp.append(mapped)

            # Compute stringRefMask: one bit per operand that is a string ID
            refs = string_refs.get(name, set())
            mask = 0
            for r in refs:
                mask |= (1 << r)

            # Format the operand initializer (partial init: the rest is zero)
            if operand_cpp:
                ops_str = '{' + ', '.join(operand_cpp) + '}'
            else:
                ops_str = '{}'

            n_ops  = len(args)
            jkind  = _jump_kind(name, args, jump_names)
            if jkind == 'JumpKind::None':
                lines.append(
                    f'    /* 0x{idx:02X} */ {{ "{name}", {n_ops}, {ops_str}, 0x{mask:02X} }},'
                )
            else:
                lines.append(
                    f'    /* 0x{idx:02X} */ {{ "{name}", {n_ops}, {ops_str}, 0x{mask:02X}, {jkind} }},'
                )
        else:
            # Unassigned slot: unknown/reserved opcode
            lines.append(f'    /* 0x{idx:02X} */ {{ nullptr, 0, {{}}, 0x00 }},')
            n_unknown += 1

    lines.append(f'}};')
    lines.append(f'')
    lines.append(f'}} // namespace v{version}')
    lines.append(f'}} // namespace hbc')

    print('\n'.join(lines))

    # Report statistics to stderr
    print(f'Generated {n_total} opcodes (0x00–0x{n_total-1:02X}), '
          f'{n_unknown} reserved slots (0x{n_total:02X}–0xFF).', file=sys.stderr)
    n_with_strings = sum(1 for name, _ in opcodes if name in string_refs)
    sample = list(string_refs.keys())[:5]
    print(f'Opcodes with string refs: {n_with_strings} (e.g. {sample}...)',
          file=sys.stderr)


def main():
    if len(sys.argv) < 3:
        print(f'Usage: {sys.argv[0]} <BytecodeList.def> <version>', file=sys.stderr)
        print(f'Output is written to stdout - redirect it to the desired .cpp file.',
              file=sys.stderr)
        sys.exit(1)

    def_path = sys.argv[1]
    version  = int(sys.argv[2])

    opcodes, string_refs, jump_names = parse_def(def_path)
    generate_cpp(opcodes, string_refs, version, jump_names)


if __name__ == '__main__':
    main()
