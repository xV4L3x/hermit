"""
hermit HBC core library
"""
from __future__ import annotations
import typing
__all__: list[str] = ['AbstractValue', 'BasicBlock', 'BigIntInfo', 'CFGData', 'CFGEdge', 'CJSModuleInfo', 'CallSite', 'CallerSite', 'DebugInfoHeader', 'EdgeKind', 'EnvAccess', 'EnvKey', 'EnvSlotUse', 'EnvironmentInfo', 'ExceptionHandlerInfo', 'FileTooSmallError', 'FunctionInfo', 'HBCFile', 'HBCFileHeader', 'InstrValues', 'Instruction', 'InvalidMagicError', 'InvalidStringEntryError', 'LiteralSite', 'MetroModule', 'Module', 'ModuleExport', 'ModuleExports', 'OpcodeDesc', 'Operand', 'OperandType', 'ResolvedCall', 'ScopeVariable', 'Section', 'SourceLocation', 'StringEntry', 'StringKind', 'StringVerdict', 'StringXRef', 'TextifiedCallee', 'UnsupportedVersionError', 'ValidationMode', 'XRefKind', 'XRefSite', 'assemble_function', 'encode_instruction', 'identifier_hash', 'object_literal_uses_shape_table', 'opcode_table', 'operand_size']
class StringKind:
    """
    Members:
    
      String
    
      Identifier
    """
    Identifier: typing.ClassVar[StringKind]
    String: typing.ClassVar[StringKind]
    __members__: typing.ClassVar[dict[str, StringKind]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class ValidationMode:
    """
    Members:
    
      Strict
    
      Permissive
    """
    Permissive: typing.ClassVar[ValidationMode]
    Strict: typing.ClassVar[ValidationMode]
    __members__: typing.ClassVar[dict[str, ValidationMode]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class StringVerdict:
    """
    Members:
    
      InsideStorage
    
      InsideFile
    
      OutsideFile
    
      BadOverflowIndex
    """
    BadOverflowIndex: typing.ClassVar[StringVerdict]
    InsideFile: typing.ClassVar[StringVerdict]
    InsideStorage: typing.ClassVar[StringVerdict]
    OutsideFile: typing.ClassVar[StringVerdict]
    __members__: typing.ClassVar[dict[str, StringVerdict]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class StringEntry:
    @property
    def byte_length(self) -> int:
        ...
    @property
    def char_length(self) -> int:
        ...
    @property
    def is_overflow(self) -> bool:
        ...
    @property
    def is_utf16(self) -> bool:
        ...
    @property
    def kind(self) -> StringKind:
        ...
    @property
    def misaligned(self) -> bool:
        ...
    @property
    def storage_offset(self) -> int:
        ...
    @property
    def verdict(self) -> StringVerdict:
        ...
class LiteralSite:
    @property
    def buffer_idx(self) -> int:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def function_index(self) -> int:
        ...
    @property
    def instruction_offset(self) -> int:
        ...
    @property
    def is_object(self) -> bool:
        ...
    @property
    def key_buffer_idx(self) -> int:
        ...
    @property
    def mnemonic(self) -> str:
        ...
class OperandType:
    """
    Members:
    
      Reg8
    
      Reg32
    
      UInt8
    
      UInt16
    
      UInt32
    
      Imm32
    
      Double
    
      Addr8
    
      Addr32
    
      StringID
    """
    Addr32: typing.ClassVar[OperandType]
    Addr8: typing.ClassVar[OperandType]
    Double: typing.ClassVar[OperandType]
    Imm32: typing.ClassVar[OperandType]
    Reg32: typing.ClassVar[OperandType]
    Reg8: typing.ClassVar[OperandType]
    StringID: typing.ClassVar[OperandType]
    UInt16: typing.ClassVar[OperandType]
    UInt32: typing.ClassVar[OperandType]
    UInt8: typing.ClassVar[OperandType]
    __members__: typing.ClassVar[dict[str, OperandType]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class Operand:
    @property
    def f64(self) -> float:
        ...
    @property
    def i32(self) -> int:
        ...
    @property
    def resolved_string(self) -> str:
        ...
    @property
    def type(self) -> OperandType:
        ...
    @property
    def u32(self) -> int:
        ...
class Instruction:
    @property
    def mnemonic(self) -> str:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def opcode(self) -> int:
        ...
    @property
    def operands(self) -> list[Operand]:
        ...
    @property
    def switch_targets(self) -> list[int]:
        ...
class OpcodeDesc:
    @property
    def jump_kind(self) -> str:
        ...
    @property
    def mnemonic(self) -> str:
        ...
    @property
    def opcode(self) -> int:
        ...
    @property
    def operands(self) -> list[OperandType]:
        ...
    @property
    def string_ref_mask(self) -> int:
        ...
class Module:
    @staticmethod
    def from_hbcfile(file: ...) -> Module:
        """
        Snapshot of an HBCFile for structural patching. Supports files with overflowed functions and real bundles.
        """
    def add_function(self, ir: list[tuple], frame_size: int) -> int:
        """
        Adds a NEW function at the end of the function table; ir = same shape as set_function_body. Returns the index it will have in the serialized file. Fixed metadata: paramCount=1, functionName=0, strictMode=True, never an exception handler / debug info. RuntimeError if ir is empty/SwitchImm/unknown opcode, frameSize > 127, resulting bytecode >= 32768 B, or another structural operation is already queued for this Module.
        """
    def add_identifier(self, value: str) -> int:
        """
        Appends a new Identifier-kind string (and its hash); returns the index it will have in the serialized file. ValueError if empty or non-ASCII. Can share a Module only with add_string.
        """
    def add_string(self, value: str) -> int:
        """
        Appends a new String-kind string; returns the index it will have in the serialized file. ValueError if empty or non-ASCII. Cannot be combined with other edits in the same Module.
        """
    def add_string_raw(self, bytes: bytes, utf16: bool = False, kind: StringKind = ...) -> int:
        """
        Appends a new string from raw, already-encoded bytes (8-bit if utf16=False, UTF-16LE if utf16=True); returns the index it will have in the serialized file. Unlike add_string, bytes are never validated as ASCII/UTF-8 - this can build a malformed-UTF-16 string (e.g. unpaired surrogates), which add_string/add_identifier reject and which a Python str with unpaired surrogates can't even reach (pybind11's UTF-8 caster rejects it before add_string runs). ValueError if bytes is empty, or odd-length with utf16=True. Cannot be combined with other edits in the same Module.
        """
    def resize_literal(self, func_idx: int, instr_offset: int, is_object: bool, is_add: bool, new_buffer_bytes: bytes, new_key_buffer_bytes: bytes = b'') -> None:
        """
        Adds (is_add=True) or removes (is_add=False) the LAST element (array) or the LAST key-value pair (object) of a site. new_buffer_bytes/new_key_buffer_bytes = complete SLP payload (count ± 1 elements). Rewrites count, sizeHint (if it matched count), bufferIdx and - for objects - keyBufferIdx. RuntimeError if the function has shared bytecode, sizeHint != count in the original (mixed literal), count exceeds 65535 (impossible), or an offset doesn't fit the width of its operand. Cannot be combined with other edits in the same Module.
        """
    def serialize(self) -> bytes:
        ...
    def set_function_body(self, func_idx: int, ir: list[tuple], frame_size: int, edit_all: bool = False) -> None:
        """
        ir = list of (opcode, operands, jump_target_idx, addr_operand_pos, orig_byte_offset). orig_byte_offset = the instruction's offset in the original body, -1 if inserted. edit_all: if the function has shared bytecode, applies to every function in the group.
        """
    def set_literal_value(self, func_idx: int, instr_offset: int, is_object: bool, new_buffer_bytes: bytes) -> None:
        """
        Replaces the complete SLP payload of a NewArrayWithBuffer[Long]/NewObjectWithBuffer[Long] site: appends new_buffer_bytes at the end of 'array buffer' or 'object value buffer' and repoints only that instruction's bufferIdx (implicit detach for shared buffers). RuntimeError if the function has shared bytecode or the offset doesn't fit the operand's width. Cannot be combined with other edits in the same Module; only one set_literal_value per Module.
        """
    def set_string_value(self, idx: int, new_bytes: bytes) -> None:
        """
        Repoints entry idx to new_bytes (already encoded: 8-bit ASCII or UTF-16LE), appended at the end of the string storage (a shared entry is detached implicitly). RuntimeError on promotion to overflow or storage > 8 MB. Cannot be combined with other edits in the same Module; only one set_string_value per Module.
        """
    def warnings(self) -> list[str]:
        ...
    @property
    def function_count(self) -> int:
        ...
    @property
    def version(self) -> int:
        ...
class HBCFileHeader:
    @property
    def array_buffer_size(self) -> int:
        ...
    @property
    def bigint_count(self) -> int:
        ...
    @property
    def bigint_storage_size(self) -> int:
        ...
    @property
    def cjs_module_count(self) -> int:
        ...
    @property
    def debug_info_offset(self) -> int:
        ...
    @property
    def file_length(self) -> int:
        ...
    @property
    def function_count(self) -> int:
        ...
    @property
    def function_source_count(self) -> int:
        ...
    @property
    def global_code_index(self) -> int:
        ...
    @property
    def identifier_count(self) -> int:
        ...
    @property
    def obj_key_buffer_size(self) -> int:
        ...
    @property
    def obj_value_buffer_size(self) -> int:
        ...
    @property
    def options(self) -> int:
        ...
    @property
    def overflow_string_count(self) -> int:
        ...
    @property
    def reg_exp_count(self) -> int:
        ...
    @property
    def reg_exp_storage_size(self) -> int:
        ...
    @property
    def segment_id(self) -> int:
        ...
    @property
    def source_hash(self) -> bytes:
        ...
    @property
    def string_count(self) -> int:
        ...
    @property
    def string_kind_count(self) -> int:
        ...
    @property
    def string_storage_size(self) -> int:
        ...
    @property
    def version(self) -> int:
        ...
class Section:
    @property
    def align(self) -> int:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def size(self) -> int:
        ...
class HBCFile:
    @staticmethod
    def from_bytes(data: bytes, mode: ValidationMode = ...) -> HBCFile:
        """
        Like open() but from an in-memory buffer (bytes). Used by the patcher.
        """
    @staticmethod
    def open(path: str, mode: ValidationMode = ...) -> HBCFile:
        ...
    def array_buffer(self) -> bytes:
        ...
    def array_buffer_offset(self) -> int:
        ...
    def bigints(self) -> list[BigIntInfo]:
        ...
    def build_caller_index(self, progress: typing.Any = None) -> None:
        """
        Builds the reverse call index that function_callers() uses, up front. This is the one-time heavy pass (it resolves calls across every function); afterwards function_callers() is O(1). progress, if given, is called as progress(done, total) while functions are scanned. No-op if the index is already built.
        """
    def call_graph(self) -> list[CallSite]:
        ...
    def captured_environment(self, function_index: int) -> EnvKey | None:
        ...
    def cfg(self, arg0: int) -> CFGData:
        ...
    def cjs_modules(self) -> list[CJSModuleInfo]:
        ...
    def debug_info_header(self) -> DebugInfoHeader:
        ...
    def debug_strings(self) -> list[str]:
        ...
    def decompile(self, arg0: int) -> str:
        ...
    def disasm(self, arg0: int) -> list[Instruction]:
        ...
    def env_accesses(self, function_index: int) -> list[EnvAccess]:
        ...
    def env_slot_uses(self, env: EnvKey, slot: int) -> list[EnvSlotUse]:
        ...
    def env_slot_value(self, env: EnvKey, slot: int) -> AbstractValue | None:
        ...
    def environments(self, function_index: int) -> list[EnvironmentInfo]:
        ...
    def function_callers(self, function_index: int) -> list[CallerSite]:
        ...
    def function_module(self, function_index: int) -> int | None:
        ...
    def function_modules(self) -> list[int | None]:
        """
        The module id of every function at once, index-aligned to functions() (None for a function outside every module). One call instead of function_module() per function.
        """
    def functions(self) -> list[FunctionInfo]:
        ...
    def header(self) -> HBCFileHeader:
        ...
    def identifier_hashes(self) -> list[int]:
        ...
    def literal_sites(self) -> list[LiteralSite]:
        ...
    def metro_entry_modules(self) -> list[int]:
        ...
    def module_exports(self, module_id: int) -> ModuleExports:
        ...
    def obj_key_buffer(self) -> bytes:
        ...
    def obj_key_buffer_offset(self) -> int:
        ...
    def obj_value_buffer(self) -> bytes:
        ...
    def obj_value_buffer_offset(self) -> int:
        ...
    def recover_modules(self) -> list[MetroModule]:
        ...
    def resolve_value(self, value: AbstractValue) -> AbstractValue:
        ...
    def resolved_calls(self, function_index: int) -> list[ResolvedCall]:
        ...
    def scope_variables(self, arg0: int) -> list[ScopeVariable]:
        ...
    def section_map(self) -> list[Section]:
        ...
    def source_files(self) -> list[str]:
        ...
    def source_locations(self, arg0: int) -> list[SourceLocation]:
        ...
    def string_entries(self) -> list[StringEntry]:
        ...
    def string_kinds(self) -> list[StringKind]:
        ...
    def string_xrefs(self, string_index: int | None = None) -> list[StringXRef] | StringXRef | None:
        """
        Without string_index: every referenced string and its use sites (StringID operands + array/object literal elements), in index order. With string_index=N: just that string's StringXRef, or None if it is never referenced. Both share one memoized computation, so the indexed form is an O(1) lookup after the first call.
        """
    def strings(self) -> list[str]:
        ...
    def textified_callees(self, arg0: int) -> list[TextifiedCallee]:
        ...
    def validation_mode(self) -> ValidationMode:
        ...
    def value_flow(self, function_index: int) -> list[InstrValues]:
        ...
    def version(self) -> int:
        ...
class ExceptionHandlerInfo:
    @property
    def end(self) -> int:
        ...
    @property
    def start(self) -> int:
        ...
    @property
    def target(self) -> int:
        ...
class FunctionInfo:
    @property
    def bytecodeSizeInBytes(self) -> int:
        ...
    @property
    def debugOffset(self) -> int:
        ...
    @property
    def exceptionHandlers(self) -> list[ExceptionHandlerInfo]:
        ...
    @property
    def frameSize(self) -> int:
        ...
    @property
    def hasDebugInfo(self) -> bool:
        ...
    @property
    def hasExceptionHandler(self) -> bool:
        ...
    @property
    def hasPreservedSource(self) -> bool:
        ...
    @property
    def index(self) -> int:
        ...
    @property
    def infoOffset(self) -> int:
        ...
    @property
    def isAsync(self) -> bool:
        ...
    @property
    def isGenerator(self) -> bool:
        ...
    @property
    def isStrictMode(self) -> bool:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def overflowed(self) -> bool:
        ...
    @property
    def paramCount(self) -> int:
        ...
    @property
    def preservedSource(self) -> str:
        ...
    @property
    def prohibitCall(self) -> bool:
        ...
    @property
    def prohibitConstruct(self) -> bool:
        ...
    @property
    def scopeDescOffset(self) -> int:
        ...
    @property
    def shareCount(self) -> int:
        ...
    @property
    def shared(self) -> bool:
        ...
    @property
    def sourceIsHidden(self) -> bool:
        ...
    @property
    def textifiedCalleesOffset(self) -> int:
        ...
class EdgeKind:
    """
    Members:
    
      FallThrough
    
      Jump
    
      CondTaken
    
      CondFallThru
    
      SwitchDefault
    
      SwitchCase
    
      BackEdge
    
      ExceptionHandler
    
      GeneratorResume
    """
    BackEdge: typing.ClassVar[EdgeKind]
    CondFallThru: typing.ClassVar[EdgeKind]
    CondTaken: typing.ClassVar[EdgeKind]
    ExceptionHandler: typing.ClassVar[EdgeKind]
    FallThrough: typing.ClassVar[EdgeKind]
    GeneratorResume: typing.ClassVar[EdgeKind]
    Jump: typing.ClassVar[EdgeKind]
    SwitchCase: typing.ClassVar[EdgeKind]
    SwitchDefault: typing.ClassVar[EdgeKind]
    __members__: typing.ClassVar[dict[str, EdgeKind]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class CFGEdge:
    @property
    def kind(self) -> EdgeKind:
        ...
    @property
    def target_id(self) -> int:
        ...
class BasicBlock:
    @property
    def end_offset(self) -> int:
        ...
    @property
    def id(self) -> int:
        ...
    @property
    def instructions(self) -> list[Instruction]:
        ...
    @property
    def is_loop_header(self) -> bool:
        ...
    @property
    def predecessors(self) -> list[int]:
        ...
    @property
    def start_offset(self) -> int:
        ...
    @property
    def successors(self) -> list[CFGEdge]:
        ...
class CFGData:
    @property
    def blocks(self) -> list[BasicBlock]:
        ...
    @property
    def entry_id(self) -> int:
        ...
    @property
    def has_loops(self) -> bool:
        ...
class XRefKind:
    """
    Members:
    
      Operand
    
      ArrayElement
    
      ObjectKey
    
      ObjectValue
    """
    ArrayElement: typing.ClassVar[XRefKind]
    ObjectKey: typing.ClassVar[XRefKind]
    ObjectValue: typing.ClassVar[XRefKind]
    Operand: typing.ClassVar[XRefKind]
    __members__: typing.ClassVar[dict[str, XRefKind]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class XRefSite:
    def __repr__(self) -> str:
        ...
    @property
    def element_count(self) -> int:
        ...
    @property
    def element_index(self) -> int:
        ...
    @property
    def function_index(self) -> int:
        ...
    @property
    def instruction_offset(self) -> int:
        ...
    @property
    def kind(self) -> XRefKind:
        ...
    @property
    def mnemonic(self) -> str:
        ...
class StringXRef:
    def __repr__(self) -> str:
        ...
    @property
    def sites(self) -> list[XRefSite]:
        ...
    @property
    def string_index(self) -> int:
        ...
    @property
    def string_value(self) -> str:
        ...
class CallSite:
    def __repr__(self) -> str:
        ...
    @property
    def callee_index(self) -> int:
        ...
    @property
    def caller_index(self) -> int:
        ...
    @property
    def is_direct(self) -> bool:
        ...
    @property
    def is_resolved(self) -> bool:
        ...
    @property
    def mnemonic(self) -> str:
        ...
    @property
    def offset(self) -> int:
        ...
class EnvKey:
    def __eq__(self, arg0: EnvKey) -> bool:
        ...
    def __hash__(self) -> int:
        ...
    def __init__(self, function: int, offset: int) -> None:
        ...
    def __repr__(self) -> str:
        ...
    @property
    def function(self) -> int:
        ...
    @property
    def offset(self) -> int:
        ...
class EnvAccess:
    @property
    def env(self) -> EnvKey:
        ...
    @property
    def is_store(self) -> bool:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def resolved(self) -> bool:
        ...
    @property
    def slot(self) -> int:
        ...
class EnvironmentInfo:
    @property
    def env(self) -> EnvKey:
        ...
    @property
    def has_parent(self) -> bool:
        ...
    @property
    def is_inner(self) -> bool:
        ...
    @property
    def parent(self) -> EnvKey:
        ...
    @property
    def parent_known(self) -> bool:
        ...
class EnvSlotUse:
    @property
    def function_index(self) -> int:
        ...
    @property
    def is_store(self) -> bool:
        ...
    @property
    def offset(self) -> int:
        ...
class AbstractValue:
    class Kind:
        """
        Members:
        
          GlobalObject
        
          Global
        
          FactoryParam
        
          Closure
        
          Module
        
          ImportDefault
        
          DepIndex
        
          Prop
        
          Object
        
          Number
        
          String
        
          Undefined
        
          Null
        
          Boolean
        """
        Boolean: typing.ClassVar[AbstractValue.Kind]
        Closure: typing.ClassVar[AbstractValue.Kind]
        DepIndex: typing.ClassVar[AbstractValue.Kind]
        FactoryParam: typing.ClassVar[AbstractValue.Kind]
        Global: typing.ClassVar[AbstractValue.Kind]
        GlobalObject: typing.ClassVar[AbstractValue.Kind]
        ImportDefault: typing.ClassVar[AbstractValue.Kind]
        Module: typing.ClassVar[AbstractValue.Kind]
        Null: typing.ClassVar[AbstractValue.Kind]
        Number: typing.ClassVar[AbstractValue.Kind]
        Object: typing.ClassVar[AbstractValue.Kind]
        Prop: typing.ClassVar[AbstractValue.Kind]
        String: typing.ClassVar[AbstractValue.Kind]
        Undefined: typing.ClassVar[AbstractValue.Kind]
        __members__: typing.ClassVar[dict[str, AbstractValue.Kind]]
        def __eq__(self, other: typing.Any) -> bool:
            ...
        def __getstate__(self) -> int:
            ...
        def __hash__(self) -> int:
            ...
        def __index__(self) -> int:
            ...
        def __init__(self, value: int) -> None:
            ...
        def __int__(self) -> int:
            ...
        def __ne__(self, other: typing.Any) -> bool:
            ...
        def __repr__(self) -> str:
            ...
        def __setstate__(self, state: int) -> None:
            ...
        def __str__(self) -> str:
            ...
        @property
        def name(self) -> str:
            ...
        @property
        def value(self) -> int:
            ...
    __hash__: typing.ClassVar[None] = None
    def __eq__(self, arg0: AbstractValue) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __str__(self) -> str:
        ...
    @property
    def base(self) -> AbstractValue | None:
        ...
    @property
    def func_index(self) -> int | None:
        """
        The function index when this value is a closure (kind == Kind.Closure), else None. The plain way to turn a resolved value into a function index without parsing repr().
        """
    @property
    def index(self) -> int:
        ...
    @property
    def kind(self) -> AbstractValue.Kind:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def number(self) -> float:
        ...
class ModuleExport:
    def __repr__(self) -> str:
        ...
    @property
    def function_index(self) -> int | None:
        """
        The exported function's index when the export resolves to a closure, else None. Shortcut for value.func_index.
        """
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> AbstractValue | None:
        """
        The exported value as an AbstractValue, or None if unknown. When it is a closure, function_index is its function index (no index scan needed).
        """
    @property
    def via_getter(self) -> bool:
        ...
class ModuleExports:
    @property
    def dynamic_names(self) -> bool:
        ...
    @property
    def es_module(self) -> bool:
        ...
    @property
    def names(self) -> list[ModuleExport]:
        ...
    @property
    def reassigned(self) -> bool:
        ...
    @property
    def value(self) -> AbstractValue | None:
        ...
class ResolvedCall:
    class Via:
        """
        Members:
        
          Closure
        
          Export
        """
        Closure: typing.ClassVar[ResolvedCall.Via]
        Export: typing.ClassVar[ResolvedCall.Via]
        __members__: typing.ClassVar[dict[str, ResolvedCall.Via]]
        def __eq__(self, other: typing.Any) -> bool:
            ...
        def __getstate__(self) -> int:
            ...
        def __hash__(self) -> int:
            ...
        def __index__(self) -> int:
            ...
        def __init__(self, value: int) -> None:
            ...
        def __int__(self) -> int:
            ...
        def __ne__(self, other: typing.Any) -> bool:
            ...
        def __repr__(self) -> str:
            ...
        def __setstate__(self, state: int) -> None:
            ...
        def __str__(self) -> str:
            ...
        @property
        def name(self) -> str:
            ...
        @property
        def value(self) -> int:
            ...
    def __repr__(self) -> str:
        ...
    @property
    def callee(self) -> int:
        ...
    @property
    def callee_value(self) -> AbstractValue:
        ...
    @property
    def is_construct(self) -> bool:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def via(self) -> ResolvedCall.Via:
        ...
class CallerSite:
    def __repr__(self) -> str:
        ...
    @property
    def caller(self) -> int:
        ...
    @property
    def is_construct(self) -> bool:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def via(self) -> ResolvedCall.Via:
        ...
class InstrValues:
    @property
    def offset(self) -> int:
        ...
    @property
    def operands(self) -> list[AbstractValue | None]:
        ...
    @property
    def result(self) -> AbstractValue | None:
        ...
class MetroModule:
    def __repr__(self) -> str:
        ...
    @property
    def define_offset(self) -> int:
        ...
    @property
    def dependencies(self) -> list[int | None]:
        ...
    @property
    def dependencies_known(self) -> bool:
        ...
    @property
    def function_index(self) -> int:
        ...
    @property
    def module_id(self) -> int:
        ...
class BigIntInfo:
    @property
    def bytes(self) -> bytes:
        ...
    @property
    def decimal(self) -> str:
        ...
    @property
    def error(self) -> str:
        ...
    @property
    def index(self) -> int:
        ...
    @property
    def is_negative(self) -> bool:
        ...
    @property
    def length(self) -> int:
        ...
    @property
    def offset(self) -> int:
        ...
    @property
    def ok(self) -> bool:
        ...
class CJSModuleInfo:
    @property
    def function_index(self) -> int:
        ...
    @property
    def is_statically_resolved(self) -> bool:
        ...
    @property
    def module_id(self) -> int:
        ...
    @property
    def module_path(self) -> str:
        ...
class SourceLocation:
    @property
    def address(self) -> int:
        ...
    @property
    def column(self) -> int:
        ...
    @property
    def line(self) -> int:
        ...
class DebugInfoHeader:
    @property
    def file_region_count(self) -> int:
        ...
    @property
    def filename_count(self) -> int:
        ...
    @property
    def filename_storage_size(self) -> int:
        ...
    @property
    def scope_desc_data_end(self) -> int:
        ...
    @property
    def source_loc_data_end(self) -> int:
        ...
    @property
    def string_table_end(self) -> int:
        ...
    @property
    def textified_callees_end(self) -> int:
        ...
class ScopeVariable:
    @property
    def name(self) -> str:
        ...
class TextifiedCallee:
    @property
    def bytecode_offset(self) -> int:
        ...
    @property
    def name(self) -> str:
        ...
class InvalidMagicError(Exception):
    pass
class UnsupportedVersionError(Exception):
    pass
class FileTooSmallError(Exception):
    pass
class InvalidStringEntryError(RuntimeError):
    pass
def assemble_function(version: int, ir: list[tuple]) -> tuple:
    """
    Assembles a list of (opcode, operands, jump_target_idx, addr_operand_pos) into (bytecode: bytes, offsets: list[int]).
    """
def encode_instruction(version: int, opcode: int, operands: list[float]) -> bytes:
    """
    Encodes an instruction (opcode byte + operand values) into raw HBC bytecode. Raises ValueError if the opcode/signature is invalid or a value is out of range.
    """
def identifier_hash(data: bytes, is_utf16: bool = False) -> int:
    """
    Hermes's Identifier Hash Table hash of an encoded string (ASCII bytes, or UTF-16LE when is_utf16). ValueError on an odd UTF-16 length.
    """
def object_literal_uses_shape_table(version: int) -> bool:
    """
    True if object literals reference their keys/count through the object shape table (v98) rather than inline NewObjectWithBuffer operands (v94-96).
    """
def opcode_table(arg0: int) -> list[OpcodeDesc]:
    """
    Complete opcode table (list[OpcodeDesc]) for an HBC version
    """
def operand_size(arg0: OperandType) -> int:
    """
    Size in bytes of a physical operand type
    """
