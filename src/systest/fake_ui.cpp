// Windows port: the system test's stand-in for ltxag_ui.exe, as fake_ui.py is for zenity and the
// Mac's progress helper (systest/README.md). run_systest.ps1 puts it in a copy of the bundle as
// Contents/Win64/ltxag_ui.exe, so job.py opens it like the real window.
//   progress <title> <width>: appends every stdin line to %LTXAG_FAKE_UI_DIR%\progress.log, exits
//     0 at "100" or the end of input, and exits 1 (= the user pressed Cancel) as soon as the file
//     %LTXAG_FAKE_UI_DIR%\cancel exists.
//   message / folder ...: appends the arguments to messages.log and exits 0.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

static std::string dir() {
  const char* d = std::getenv("LTXAG_FAKE_UI_DIR");
  return d ? d : ".";
}

static void log(const char* name, const std::string& text) {
  std::ofstream f(dir() + "\\" + name, std::ios::app);
  f << text << "\n";
}

int main(int argc, char** argv) {
  std::string args;
  for (int i = 1; i < argc; ++i) args += std::string(i > 1 ? " " : "") + argv[i];
  const std::string pid = std::to_string(GetCurrentProcessId());
  log("windows.log", "start pid=" + pid + " args=" + args);
  if (argc < 2 || std::string(argv[1]) != "progress") {
    log("messages.log", args);
    return 0;
  }
  std::thread([pid] {
    while (GetFileAttributesA((dir() + "\\cancel").c_str()) == INVALID_FILE_ATTRIBUTES) Sleep(200);
    log("windows.log", "cancel pid=" + pid);
    ExitProcess(1);
  }).detach();
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    log("progress.log", line);
    if (line == "100") break;
  }
  log("windows.log", "end pid=" + pid);
  return 0;
}
