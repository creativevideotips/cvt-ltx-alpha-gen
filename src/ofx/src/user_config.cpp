#include "user_config.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "oscompat.h"

namespace ltxag {

std::string userConfigPath() {
  return homeDirectory() + "/.config/ltx-alphagen/config.conf";
}

UserConfig loadUserConfig() {
  UserConfig cfg;
  std::ifstream in(fsPath(userConfigPath()));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // a file edited on Windows
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    cfg[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return cfg;
}

bool saveUserConfig(const UserConfig& cfg) {
  const std::string path = userConfigPath();
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(fsPath(tmp), std::ios::trunc | std::ios::binary);  // binary: the same bytes as on the Mac
    if (!out) return false;
    out << "# LTX Alpha Gen OFX user settings. Written by the plugin.\n";
    for (const auto& kv : cfg) out << kv.first << '=' << kv.second << '\n';
    if (!out) return false;
  }
  chmod(tmp.c_str(), 0644);  // cvt1.43: written inside Resolve, whose umask is 000
  return renameOver(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace ltxag
