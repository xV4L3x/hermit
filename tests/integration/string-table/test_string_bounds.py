"""String entries whose byte range leaves the string storage are rejected.

The range [offset, offset + length * unit) of every string must lie inside
`stringStorageSize`; otherwise decoding reads past the end of the file.
Regression for a fuzzer file whose overflow string 0 had length 524288
UTF-16 units at an offset beyond the file end (segfault in `strings()`).
"""

import struct

import _libhbc
import pytest

from testutil import corpus_path

STRING_STORAGE_SIZE_OFFSET = 0x3C  # HBCFileHeader::stringStorageSize


def _with_storage_size(name: str, size: int) -> "_libhbc.HBCFile":
    data = bytearray(corpus_path(name).read_bytes())
    struct.pack_into("<I", data, STRING_STORAGE_SIZE_OFFSET, size)
    return _libhbc.HBCFile.from_bytes(bytes(data))


def test_intact_file_decodes():
    hbc = _libhbc.HBCFile.open(str(corpus_path("hello.hbc")))
    assert len(hbc.strings()) == hbc.header().string_count


@pytest.mark.parametrize("call", ["strings", "string_entries"])
def test_range_outside_storage_raises(call):
    hbc = _with_storage_size("hello.hbc", 1)
    with pytest.raises(RuntimeError, match="string storage"):
        getattr(hbc, call)()
