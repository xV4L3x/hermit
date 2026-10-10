"""Interactive full-screen editors: instructions (`edit func:`), strings
(`edit string:`) and array/object literals (`edit array:`)."""

from cli.editor.arrays import ArrayEditState, _SLP_TAG_ORDER, run_array_editor
from cli.editor.instructions import (
    EditorState, assemble_committed_function, encode_committed_lines,
    new_function_seed_instrs, new_function_stub, run_editor,
)
from cli.editor.strings import (
    StringEditState, _StrEditError, encode_new_string, encode_string_commit,
    encode_string_new_value, encode_string_relocation_commit, encode_string_value,
    run_string_editor,
)

__all__ = [
    "ArrayEditState", "EditorState", "StringEditState",
    "assemble_committed_function", "encode_committed_lines", "encode_new_string",
    "encode_string_commit", "encode_string_new_value",
    "encode_string_relocation_commit", "encode_string_value",
    "new_function_seed_instrs", "new_function_stub",
    "run_array_editor", "run_editor", "run_string_editor",
]
