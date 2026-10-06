// Annota - ffi.cpp : the one place the FFI registry lives.
//
// It must be a single object across the interpreter and every plugin: `ffiRegistry()` is
// therefore declared in ffi.hpp and defined here (not inline), and the interpreter exports it
// (build.ps1 passes -Wl,--export-all-symbols and writes an import library) so a plugin's
// registration lands in the host's registry.
#include "ffi.hpp"

namespace annota {

FfiRegistry& ffiRegistry() {
    static FfiRegistry registry;
    return registry;
}

} // namespace annota
