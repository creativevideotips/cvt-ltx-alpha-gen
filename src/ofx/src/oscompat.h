// Windows port: the few file calls that need another form on Windows. On macOS they are what the
// code called before (the HOME folder, std::rename, a plain path for file streams); on Windows
// win_compat.h supplies them, with the POSIX calls the shared code uses.
#pragma once

#include <string>

#ifdef _WIN32
#include "win_compat.h"
#else
#include <cstdio>
#include <cstdlib>

namespace ltxag {

inline const std::string& fsPath(const std::string& path) { return path; }
inline std::string homeDirectory() {
  const char* home = std::getenv("HOME");
  return home ? home : "/tmp";
}
inline int renameOver(const char* from, const char* to) { return std::rename(from, to); }

}  // namespace ltxag
#endif
