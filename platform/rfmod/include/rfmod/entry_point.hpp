#pragma once

#include "rfmod/abi.h"

#include <dlfcn.h>

namespace rfmod {

/// Looks up a loaded module library's entry function (RFMOD_ENTRY_SYMBOL).
class EntryPoint {
public:
    /// The entry function of the library behind `handle` (from dlopen), or nullptr when the
    /// library does not export it.
    [[nodiscard]] static rfmod_get_module_fn find(void* handle) {
        // POSIX guarantees a function symbol's address round-trips through dlsym's void*, and
        // reinterpret_cast is the one C++ cast for it: static_cast does not compile, and
        // clang-tidy rejects std::bit_cast and memcpy (bugprone-bitwise-pointer-cast).
        return reinterpret_cast<rfmod_get_module_fn>(dlsym(handle, RFMOD_ENTRY_SYMBOL));  // NOSONAR(cpp:S3630)
    }
};

}  // namespace rfmod
