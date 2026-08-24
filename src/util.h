#pragma once

#include <string>

namespace bt {

// Full path of the running executable, empty on failure.
std::wstring module_file_path();

// Directory holding the running executable, without a trailing backslash.
std::wstring module_directory();

} // namespace bt
