// Windows port: what the plugin needs from Windows besides platform.h (platform_win.cpp).
#pragma once

#include <string>
#include <vector>

namespace ltxag {

double totalRamBytes();
double availableRamBytes();  // what can be handed out without paging
// Opens a web page in the default browser.
bool openUrl(const std::string& url, std::string& err);
// ltxag_ui.exe, the bundle's own small window program (progress, messages, folder picker),
// beside the .ofx in Contents/Win64. `resources` is the bundle's Contents/Resources.
std::string uiHelper(const std::string& resources);
// uv.exe, put beside it by build.ps1; "" if this bundle has none.
std::string bundledUv(const std::string& resources);
// A file in the Windows system folder ("tar.exe", "notepad.exe"), by full path.
std::string systemTool(const char* name);

}  // namespace ltxag
