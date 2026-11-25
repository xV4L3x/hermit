// Runtime dispatch to the per-version format module. Mirrors getOpcodeTable():
// each version family's module is defined in its own translation unit and
// registered here. Adding a version means adding a module and one case.

#include "hbc/format_version.h"

namespace hbc {

const FormatVersion* formatLegacy();  // format_legacy.cpp (v94/95/96)
const FormatVersion* formatV98();     // format_v98.cpp    (v97/98, Static Hermes)
const FormatVersion* formatV99();     // format_v99.cpp    (v99,   Static Hermes)

const FormatVersion* formatForVersion(uint32_t version) {
    switch (version) {
        case 99: return formatV99();
        case 98:
        case 97: return formatV98();
        case 96:
        case 95:
        case 94: return formatLegacy();
        default: return nullptr;
    }
}

} // namespace hbc
