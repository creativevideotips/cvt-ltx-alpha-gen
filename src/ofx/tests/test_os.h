// Windows port: the few things the unit tests ask of the system, in both forms.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include "win_compat.h"
#else
#include <unistd.h>
#endif

// A new empty folder in the temp folder; `name` starts its name.
inline std::string testTempDir(const char* name) {
#ifdef _WIN32
  const char* base = std::getenv("TEMP");
  std::string t = ltxag::forwardSlashes(base ? base : "C:/Windows/Temp") + "/" + name + "_XXXXXX";
#else
  std::string t = std::string("/tmp/") + name + "_XXXXXX";
#endif
  const char* made = mkdtemp(&t[0]);
  return made ? made : "";
}

inline void testRemoveTree(const std::string& dir) {
#ifdef _WIN32
  const std::string cmd = "rmdir /s /q \"" + ltxag::backSlashes(dir) + "\"";
#else
  const std::string cmd = "rm -rf " + dir;
#endif
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
}

// The folder the plugin takes as the user's home (config, cache and logs live under it).
inline void testSetHome(const std::string& dir) {
#ifdef _WIN32
  setenv("USERPROFILE", ltxag::backSlashes(dir).c_str(), 1);
#else
  setenv("HOME", dir.c_str(), 1);
#endif
}
