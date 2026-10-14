#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "hbc/hbc.h"
#include "hbc/module.h"
#include <limits>

namespace py = pybind11;

namespace {

py::bytes toBytes(const std::vector<uint8_t>& b) {
    return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
}

py::bytes toBytes(const std::string& s) {
    return py::bytes(s.data(), s.size());
}

// Decodes with errors="replace" instead of relying on pybind11's strict UTF-8
// conversion. The C++ side already converts strings (ASCII and UTF-16LE) to
// UTF-8, but this is a reverse-engineering tool and its input is not
// necessarily well-formed: a mangled string beats a crash.
py::str decodeUtf8(const std::string& s) {
    return toBytes(s).attr("decode")("utf-8", "replace");
}

py::list decodeUtf8List(const std::vector<std::string>& v) {
    py::list result;
    for (const auto& s : v) result.append(decodeUtf8(s));
    return result;
}

// IR from Python: (opcode, operands, jump_target_idx, addr_operand_pos[, orig_byte_offset]).
std::vector<hbc::AsmInstr> toAsmInstrs(const std::vector<py::tuple>& ir, bool withOrigOffset) {
    std::vector<hbc::AsmInstr> instrs;
    instrs.reserve(ir.size());
    for (const py::tuple& t : ir) {
        hbc::AsmInstr a;
        a.opcode         = t[0].cast<uint8_t>();
        a.operands       = t[1].cast<std::vector<double>>();
        a.jumpTargetIdx  = t[2].cast<int32_t>();
        a.addrOperandPos = t[3].cast<int8_t>();
        if (withOrigOffset)
            a.origByteOffset = t.size() > 4 ? t[4].cast<int32_t>() : -1;
        instrs.push_back(std::move(a));
    }
    return instrs;
}

} // namespace

PYBIND11_MODULE(_libhbc, m) {
    m.doc() = "hermit HBC core library";

    py::enum_<hbc::StringKind>(m, "StringKind")
        .value("String",     hbc::StringKind::String)
        .value("Identifier", hbc::StringKind::Identifier)
    ;

    // Raw per-string metadata - parallel to strings()/string_kinds().
    py::class_<hbc::StringEntry>(m, "StringEntry")
        .def_readonly("storage_offset", &hbc::StringEntry::storageOffset)
        .def_readonly("byte_length",    &hbc::StringEntry::byteLength)
        .def_readonly("char_length",    &hbc::StringEntry::charLength)
        .def_readonly("is_utf16",       &hbc::StringEntry::isUTF16)
        .def_readonly("is_overflow",    &hbc::StringEntry::isOverflow)
        .def_readonly("kind",           &hbc::StringEntry::kind)
    ;

    // NewArrayWithBuffer[Long]/NewObjectWithBuffer[Long] site.
    py::class_<hbc::LiteralSite>(m, "LiteralSite")
        .def_readonly("function_index",      &hbc::LiteralSite::functionIndex)
        .def_readonly("instruction_offset",  &hbc::LiteralSite::instructionOffset)
        .def_readonly("mnemonic",            &hbc::LiteralSite::mnemonic)
        .def_readonly("is_object",           &hbc::LiteralSite::isObject)
        .def_readonly("count",               &hbc::LiteralSite::count)
        .def_readonly("buffer_idx",          &hbc::LiteralSite::bufferIdx)
        .def_readonly("key_buffer_idx",      &hbc::LiteralSite::keyBufferIdx)
    ;

    py::enum_<hbc::Operand::Type>(m, "OperandType")
        .value("Reg8",     hbc::Operand::Type::Reg8)
        .value("Reg32",    hbc::Operand::Type::Reg32)
        .value("UInt8",    hbc::Operand::Type::UInt8)
        .value("UInt16",   hbc::Operand::Type::UInt16)
        .value("UInt32",   hbc::Operand::Type::UInt32)
        .value("Imm32",    hbc::Operand::Type::Imm32)
        .value("Double",   hbc::Operand::Type::Double)
        .value("Addr8",    hbc::Operand::Type::Addr8)
        .value("Addr32",   hbc::Operand::Type::Addr32)
        .value("StringID", hbc::Operand::Type::StringID)
    ;

    py::class_<hbc::Operand>(m, "Operand")
        .def_readonly("type", &hbc::Operand::type)
        .def_property_readonly("u32", [](const hbc::Operand& op) -> uint32_t { return op.u32; })
        .def_property_readonly("i32", [](const hbc::Operand& op) -> int32_t  { return op.i32; })
        .def_property_readonly("f64", [](const hbc::Operand& op) -> double   { return op.f64; })
        .def_property_readonly("resolved_string", [](const hbc::Operand& op) {
            return decodeUtf8(op.resolvedString);
        })
    ;

    py::class_<hbc::Instruction>(m, "Instruction")
        .def_readonly("offset",         &hbc::Instruction::offset)
        .def_readonly("opcode",         &hbc::Instruction::opcode)
        .def_readonly("mnemonic",       &hbc::Instruction::mnemonic)
        .def_readonly("operands",       &hbc::Instruction::operands)
        .def_readonly("switch_targets", &hbc::Instruction::switchTargets)
    ;

    // Opcode table for the bytecode editor
    py::class_<hbc::OpcodeDesc>(m, "OpcodeDesc")
        .def_readonly("opcode",          &hbc::OpcodeDesc::opcode)
        .def_readonly("mnemonic",        &hbc::OpcodeDesc::mnemonic)
        .def_readonly("operands",        &hbc::OpcodeDesc::operands)
        .def_readonly("string_ref_mask", &hbc::OpcodeDesc::stringRefMask)
        .def_readonly("jump_kind",       &hbc::OpcodeDesc::jumpKind)
    ;
    m.def("opcode_table", &hbc::opcode_table,
          "Complete opcode table (list[OpcodeDesc]) for an HBC version");
    m.def("operand_size", &hbc::operand_size,
          "Size in bytes of a physical operand type");
    m.def("encode_instruction",
          [](uint32_t version, uint8_t opcode, const std::vector<double>& operands) {
              return toBytes(hbc::encode_instruction(version, opcode, operands));
          },
          py::arg("version"), py::arg("opcode"), py::arg("operands"),
          "Encodes an instruction (opcode byte + operand values) into raw HBC bytecode. "
          "Raises ValueError if the opcode/signature is invalid or a value is out of range.");

    // Structural serializer
    m.def("assemble_function",
          [](uint32_t version, const std::vector<py::tuple>& ir) {
              hbc::AssembledFunction r = hbc::assemble_function(version, toAsmInstrs(ir, false));
              return py::make_tuple(toBytes(r.bytecode), r.offsets);
          },
          py::arg("version"), py::arg("ir"),
          "Assembles a list of (opcode, operands, jump_target_idx, addr_operand_pos) "
          "into (bytecode: bytes, offsets: list[int]).");

    py::class_<hbc::Module>(m, "Module")
        .def_static("from_hbcfile", &hbc::Module::from, py::arg("file"),
                    "Snapshot of an HBCFile for structural patching. Supports "
                    "files with overflowed functions and real bundles.")
        .def_property_readonly("version", &hbc::Module::version)
        .def_property_readonly("function_count", &hbc::Module::function_count)
        .def("set_function_body",
             [](hbc::Module& self, uint32_t funcIdx, const std::vector<py::tuple>& ir,
                uint32_t frameSize, bool editAll) {
                 self.set_function_body(funcIdx, toAsmInstrs(ir, true), frameSize, editAll);
             },
             py::arg("func_idx"), py::arg("ir"), py::arg("frame_size"),
             py::arg("edit_all") = false,
             "ir = list of (opcode, operands, jump_target_idx, addr_operand_pos, orig_byte_offset). "
             "orig_byte_offset = the instruction's offset in the original body, -1 if inserted. "
             "edit_all: if the function has shared bytecode, applies to every function in the group.")
        .def("add_string", &hbc::Module::add_string, py::arg("value"),
             "Appends a new String-kind string; returns the index it will have "
             "in the serialized file. ValueError if empty or non-ASCII. "
             "Cannot be combined with other edits in the same Module.")
        .def("set_string_value",
             [](hbc::Module& self, uint32_t idx, const py::bytes& newBytes) {
                 self.set_string_value(idx, static_cast<std::string>(newBytes));
             },
             py::arg("idx"), py::arg("new_bytes"),
             "Repoints entry idx to new_bytes (already encoded: 8-bit ASCII or "
             "UTF-16LE), appended at the end of the string storage (a shared "
             "entry is detached implicitly). RuntimeError on promotion to overflow "
             "or storage > 8 MB. Cannot be combined with other edits in the same "
             "Module; only one set_string_value per Module.")
        .def("set_literal_value",
             [](hbc::Module& self, uint32_t funcIdx, uint32_t instrOffset,
                bool isObject, const py::bytes& newBufferBytes) {
                 self.set_literal_value(funcIdx, instrOffset, isObject,
                                        static_cast<std::string>(newBufferBytes));
             },
             py::arg("func_idx"), py::arg("instr_offset"), py::arg("is_object"),
             py::arg("new_buffer_bytes"),
             "Replaces the complete SLP payload of a NewArrayWithBuffer"
             "[Long]/NewObjectWithBuffer[Long] site: appends new_buffer_bytes at "
             "the end of 'array buffer' or 'object value buffer' and repoints only that "
             "instruction's bufferIdx (implicit detach for shared "
             "buffers). RuntimeError if the function has shared bytecode "
             "or the offset doesn't fit the operand's width. "
             "Cannot be combined with other edits in the same Module; only one "
             "set_literal_value per Module.")
        .def("resize_literal",
             [](hbc::Module& self, uint32_t funcIdx, uint32_t instrOffset,
                bool isObject, bool isAdd, const py::bytes& newBufferBytes,
                const py::bytes& newKeyBufferBytes) {
                 self.resize_literal(funcIdx, instrOffset, isObject, isAdd,
                                     static_cast<std::string>(newBufferBytes),
                                     static_cast<std::string>(newKeyBufferBytes));
             },
             py::arg("func_idx"), py::arg("instr_offset"), py::arg("is_object"),
             py::arg("is_add"), py::arg("new_buffer_bytes"),
             py::arg("new_key_buffer_bytes") = py::bytes(),
             "Adds (is_add=True) or removes (is_add=False) the LAST element "
             "(array) or the LAST key-value pair (object) of a site. new_buffer_bytes/new_key_buffer_bytes = complete SLP "
             "payload (count ± 1 elements). Rewrites count, sizeHint (if it "
             "matched count), bufferIdx and - for objects - "
             "keyBufferIdx. RuntimeError if the function has shared bytecode, "
             "sizeHint != count in the original (mixed literal), "
             "count exceeds 65535 (impossible), or an offset doesn't fit the "
             "width of its operand. Cannot be combined with other "
             "edits in the same Module.")
        .def("add_function",
             [](hbc::Module& self, const std::vector<py::tuple>& ir, uint32_t frameSize) {
                 return self.add_function(toAsmInstrs(ir, true), frameSize);
             },
             py::arg("ir"), py::arg("frame_size"),
             "Adds a NEW function at the end of the function table; "
             "ir = same shape as set_function_body. Returns the index it will have "
             "in the serialized file. Fixed metadata: paramCount=1, functionName=0, "
             "strictMode=True, never an exception handler / debug info. "
             "RuntimeError if ir is empty/SwitchImm/unknown "
             "opcode, frameSize > 127, resulting bytecode >= 32768 B, or another "
             "structural operation is already queued for this Module.")
        .def("serialize",
             [](hbc::Module& self) { return toBytes(self.serialize()); })
        .def("warnings", &hbc::Module::warnings);

    py::class_<hbc::HBCFileHeader>(m, "HBCFileHeader")
        .def_readonly("version",               &hbc::HBCFileHeader::version)
        .def_readonly("file_length",           &hbc::HBCFileHeader::fileLength)
        .def_readonly("global_code_index",     &hbc::HBCFileHeader::globalCodeIndex)
        .def_readonly("function_count",        &hbc::HBCFileHeader::functionCount)
        .def_readonly("string_kind_count",     &hbc::HBCFileHeader::stringKindCount)
        .def_readonly("identifier_count",      &hbc::HBCFileHeader::identifierCount)
        .def_readonly("string_count",          &hbc::HBCFileHeader::stringCount)
        .def_readonly("overflow_string_count", &hbc::HBCFileHeader::overflowStringCount)
        .def_readonly("string_storage_size",   &hbc::HBCFileHeader::stringStorageSize)
        .def_readonly("array_buffer_size",     &hbc::HBCFileHeader::arrayBufferSize)
        .def_readonly("obj_key_buffer_size",   &hbc::HBCFileHeader::objKeyBufferSize)
        .def_readonly("obj_value_buffer_size", &hbc::HBCFileHeader::objValueBufferSize)
        .def_readonly("reg_exp_count",         &hbc::HBCFileHeader::regExpCount)
        .def_readonly("debug_info_offset",     &hbc::HBCFileHeader::debugInfoOffset)
        .def_property_readonly("source_hash", [](const hbc::HBCFileHeader& h) {
            return py::bytes(reinterpret_cast<const char*>(h.sourceHash), sizeof(h.sourceHash));
        })
    ;

    py::class_<hbc::HBCFile>(m, "HBCFile")
        .def_static("open",        &hbc::HBCFile::open)
        .def_static("from_bytes",  [](const py::bytes& b) {
            return hbc::HBCFile::from_bytes(static_cast<std::string>(b));
        }, py::arg("data"),
           "Like open() but from an in-memory buffer (bytes). Used by the patcher.")
        .def("version",            &hbc::HBCFile::version)
        .def("header",             &hbc::HBCFile::header,
             py::return_value_policy::reference_internal)
        .def("strings", [](const hbc::HBCFile& self) { return decodeUtf8List(self.strings()); })
        .def("string_kinds",       &hbc::HBCFile::stringKinds)
        .def("string_entries",     &hbc::HBCFile::string_entries)
        .def("literal_sites",      &hbc::HBCFile::literal_sites)
        .def("array_buffer", [](const hbc::HBCFile& self) { return toBytes(self.array_buffer()); })
        .def("obj_key_buffer", [](const hbc::HBCFile& self) { return toBytes(self.obj_key_buffer()); })
        .def("obj_value_buffer", [](const hbc::HBCFile& self) { return toBytes(self.obj_value_buffer()); })
        .def("array_buffer_offset",     &hbc::HBCFile::array_buffer_offset)
        .def("obj_key_buffer_offset",   &hbc::HBCFile::obj_key_buffer_offset)
        .def("obj_value_buffer_offset", &hbc::HBCFile::obj_value_buffer_offset)
        .def("functions",          &hbc::HBCFile::functions)
        .def("disasm",             &hbc::HBCFile::disasm)
        .def("cfg",                &hbc::HBCFile::cfg)
        .def("string_xrefs",       &hbc::HBCFile::string_xrefs)
        .def("call_graph",         &hbc::HBCFile::call_graph)
        .def("recover_modules",    &hbc::HBCFile::recover_modules)
        .def("cjs_modules",        &hbc::HBCFile::cjs_modules)
        .def("source_files", [](const hbc::HBCFile& self) { return decodeUtf8List(self.source_files()); })
        .def("source_locations", &hbc::HBCFile::source_locations)
        .def("decompile",        &hbc::HBCFile::decompile)
        .def("debug_info_header",   &hbc::HBCFile::debug_info_header)
        .def("scope_variables",     &hbc::HBCFile::scope_variables)
        .def("textified_callees",   &hbc::HBCFile::textified_callees)
        .def("debug_strings", [](const hbc::HBCFile& self) { return decodeUtf8List(self.debug_strings()); })
    ;

    py::class_<hbc::ExceptionHandlerInfo>(m, "ExceptionHandlerInfo")
        .def_readonly("start",  &hbc::ExceptionHandlerInfo::start)
        .def_readonly("end",    &hbc::ExceptionHandlerInfo::end)
        .def_readonly("target", &hbc::ExceptionHandlerInfo::target)
    ;

    py::class_<hbc::FunctionInfo>(m, "FunctionInfo")
        .def_readonly("index",                &hbc::FunctionInfo::index)
        .def_readonly("name",                 &hbc::FunctionInfo::name)
        .def_readonly("offset",               &hbc::FunctionInfo::offset)
        .def_readonly("bytecodeSizeInBytes",  &hbc::FunctionInfo::bytecodeSizeInBytes)
        .def_readonly("paramCount",           &hbc::FunctionInfo::paramCount)
        .def_readonly("frameSize",            &hbc::FunctionInfo::frameSize)
        .def_readonly("infoOffset",           &hbc::FunctionInfo::infoOffset)
        .def_readonly("isStrictMode",         &hbc::FunctionInfo::isStrictMode)
        .def_readonly("isAsync",              &hbc::FunctionInfo::isAsync)
        .def_readonly("isGenerator",          &hbc::FunctionInfo::isGenerator)
        .def_readonly("hasExceptionHandler",  &hbc::FunctionInfo::hasExceptionHandler)
        .def_readonly("hasDebugInfo",         &hbc::FunctionInfo::hasDebugInfo)
        .def_readonly("prohibitCall",         &hbc::FunctionInfo::prohibitCall)
        .def_readonly("prohibitConstruct",    &hbc::FunctionInfo::prohibitConstruct)
        .def_readonly("overflowed",           &hbc::FunctionInfo::overflowed)
        .def_readonly("shared",               &hbc::FunctionInfo::shared)
        .def_readonly("shareCount",           &hbc::FunctionInfo::shareCount)
        .def_readonly("debugOffset",             &hbc::FunctionInfo::debugOffset)
        .def_readonly("scopeDescOffset",         &hbc::FunctionInfo::scopeDescOffset)
        .def_readonly("textifiedCalleesOffset",  &hbc::FunctionInfo::textifiedCalleesOffset)
        .def_readonly("hasPreservedSource",      &hbc::FunctionInfo::hasPreservedSource)
        .def_readonly("sourceIsHidden",          &hbc::FunctionInfo::sourceIsHidden)
        .def_readonly("preservedSource",         &hbc::FunctionInfo::preservedSource)
        .def_readonly("exceptionHandlers",       &hbc::FunctionInfo::exceptionHandlers)
    ;

    // ------------------------------------------------------------------
    // CFG types
    // ------------------------------------------------------------------

    py::enum_<hbc::EdgeKind>(m, "EdgeKind")
        .value("FallThrough",   hbc::EdgeKind::FallThrough)
        .value("Jump",          hbc::EdgeKind::Jump)
        .value("CondTaken",     hbc::EdgeKind::CondTaken)
        .value("CondFallThru",  hbc::EdgeKind::CondFallThru)
        .value("SwitchDefault",     hbc::EdgeKind::SwitchDefault)
        .value("SwitchCase",        hbc::EdgeKind::SwitchCase)
        .value("BackEdge",          hbc::EdgeKind::BackEdge)
        .value("ExceptionHandler",  hbc::EdgeKind::ExceptionHandler)
    ;

    py::class_<hbc::CFGEdge>(m, "CFGEdge")
        .def_readonly("target_id", &hbc::CFGEdge::targetId)
        .def_readonly("kind",      &hbc::CFGEdge::kind)
    ;

    py::class_<hbc::BasicBlock>(m, "BasicBlock")
        .def_readonly("id",           &hbc::BasicBlock::id)
        .def_readonly("start_offset", &hbc::BasicBlock::startOffset)
        .def_readonly("end_offset",   &hbc::BasicBlock::endOffset)
        .def_readonly("instructions", &hbc::BasicBlock::instructions)
        .def_readonly("successors",   &hbc::BasicBlock::successors)
        .def_readonly("predecessors", &hbc::BasicBlock::predecessors)
        .def_readonly("is_loop_header", &hbc::BasicBlock::isLoopHeader)
    ;

    py::class_<hbc::CFGData>(m, "CFGData")
        .def_readonly("blocks",     &hbc::CFGData::blocks)
        .def_readonly("entry_id",   &hbc::CFGData::entryId)
        .def_readonly("has_loops",  &hbc::CFGData::hasLoops)
    ;

    // ------------------------------------------------------------------
    // Analysis types
    // ------------------------------------------------------------------

    py::class_<hbc::XRefSite>(m, "XRefSite")
        .def_readonly("function_index",      &hbc::XRefSite::functionIndex)
        .def_readonly("instruction_offset",  &hbc::XRefSite::instructionOffset)
        .def_readonly("mnemonic",            &hbc::XRefSite::mnemonic)
    ;

    py::class_<hbc::StringXRef>(m, "StringXRef")
        .def_readonly("string_index", &hbc::StringXRef::stringIndex)
        .def_property_readonly("string_value", [](const hbc::StringXRef& x) {
            return decodeUtf8(x.stringValue);
        })
        .def_readonly("sites", &hbc::StringXRef::sites)
    ;

    // Call graph
    py::class_<hbc::CallSite>(m, "CallSite")
        .def_readonly("caller_index", &hbc::CallSite::callerIndex)
        .def_readonly("callee_index", &hbc::CallSite::calleeIndex)  // 0xFFFFFFFF = unresolved
        .def_readonly("offset",       &hbc::CallSite::offset)
        .def_readonly("is_direct",    &hbc::CallSite::isDirect)
        .def_readonly("mnemonic",     &hbc::CallSite::mnemonic)
        .def_property_readonly("is_resolved", [](const hbc::CallSite& cs) {
            return cs.calleeIndex != std::numeric_limits<uint32_t>::max();
        })
    ;

    // Module ID recovery
    py::class_<hbc::ModuleInfo>(m, "ModuleInfo")
        .def_readonly("function_index", &hbc::ModuleInfo::functionIndex)
        .def_readonly("module_id",      &hbc::ModuleInfo::moduleId)
    ;

    // CJS Module Table (native `hermes -commonjs` mode, never present in a
    // Metro-bundled bundle -- see ModuleInfo/recover_modules() for that case).
    py::class_<hbc::CJSModuleInfo>(m, "CJSModuleInfo")
        .def_readonly("function_index",         &hbc::CJSModuleInfo::functionIndex)
        .def_readonly("is_statically_resolved",  &hbc::CJSModuleInfo::isStaticallyResolved)
        .def_readonly("module_id",               &hbc::CJSModuleInfo::moduleId)
        .def_property_readonly("module_path", [](const hbc::CJSModuleInfo& cm) {
            return decodeUtf8(cm.modulePath);
        })
    ;

    // Source location data
    py::class_<hbc::SourceLocation>(m, "SourceLocation")
        .def_readonly("address", &hbc::SourceLocation::address)
        .def_readonly("line",    &hbc::SourceLocation::line)
        .def_readonly("column",  &hbc::SourceLocation::column)
    ;

    // Debug info section (scope descriptor, textified callees)
    py::class_<hbc::DebugInfoHeader>(m, "DebugInfoHeader")
        .def_readonly("filename_count",         &hbc::DebugInfoHeader::filenameCount)
        .def_readonly("filename_storage_size",  &hbc::DebugInfoHeader::filenameStorageSize)
        .def_readonly("file_region_count",      &hbc::DebugInfoHeader::fileRegionCount)
        .def_readonly("source_loc_data_end",    &hbc::DebugInfoHeader::sourceLocDataEnd)
        .def_readonly("scope_desc_data_end",    &hbc::DebugInfoHeader::scopeDescDataEnd)
        .def_readonly("textified_callees_end",  &hbc::DebugInfoHeader::textifiedCalleesEnd)
        .def_readonly("string_table_end",       &hbc::DebugInfoHeader::stringTableEnd)
    ;

    py::class_<hbc::ScopeVariable>(m, "ScopeVariable")
        .def_property_readonly("name", [](const hbc::ScopeVariable& v) { return decodeUtf8(v.name); })
    ;

    py::class_<hbc::TextifiedCallee>(m, "TextifiedCallee")
        .def_readonly("bytecode_offset", &hbc::TextifiedCallee::bytecodeOffset)
        .def_property_readonly("name", [](const hbc::TextifiedCallee& c) { return decodeUtf8(c.name); })
    ;

    py::register_exception<hbc::InvalidMagicError>(m,     "InvalidMagicError");
    py::register_exception<hbc::UnsupportedVersionError>(m, "UnsupportedVersionError");
    py::register_exception<hbc::FileTooSmallError>(m,      "FileTooSmallError");
}
