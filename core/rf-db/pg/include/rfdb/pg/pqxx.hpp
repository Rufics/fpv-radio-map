#pragma once

// The distribution's libpqxx 7.8 is built as C++17, so its exception constructors take no
// std::source_location. Compiled as C++20, its headers would declare ones that take it and
// the link fails; hide the feature so the headers match the library's ABI.
#include <version>
#undef __cpp_lib_source_location  // NOSONAR(cpp:S959): a library feature macro <version> defines; see above
#include <pqxx/pqxx>
