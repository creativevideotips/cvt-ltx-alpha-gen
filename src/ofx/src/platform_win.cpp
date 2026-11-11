// Windows port: the Windows side of platform.h (decision 64). GPU memory comes from NVML, as on
// Linux upstream (platform_linux_nvml.cpp, the same calls and structs), loaded at run time so the
// plugin has no link-time dependency on the NVIDIA driver. nvml.dll is in System32 with every
// current driver (older ones: Program Files\NVIDIA Corporation\NVSMI). Without an NVIDIA GPU the
// queries report "not available", and the real model can't run anyway (the runtime is CUDA).
#include "platform.h"
#include "platform_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "win_compat.h"

namespace ltxag {

namespace {

std::string gb(double bytes) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
  return b;
}

std::string registryString(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[512] = {0};
  DWORD size = sizeof buf - sizeof(wchar_t);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return "";
  return narrow(buf);
}

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};
struct NvmlProcessInfo {
  unsigned int pid;
  unsigned long long usedGpuMemory;
  unsigned int gpuInstanceId;
  unsigned int computeInstanceId;
};
using InitFn = int (*)(unsigned int);
using HandleFn = int (*)(unsigned int, void**);
using MemFn = int (*)(void*, NvmlMemory*);
using ShutdownFn = int (*)();
using NameFn = int (*)(void*, char*, unsigned int);
using DriverFn = int (*)(char*, unsigned int);
using ProcFn = int (*)(void*, unsigned int*, NvmlProcessInfo*);
constexpr unsigned long long kNotAvailable = ~0ULL;  // NVML_VALUE_NOT_AVAILABLE_ulonglong
constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

HMODULE loadNvml() {
  if (HMODULE lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return lib;
  wchar_t pf[MAX_PATH] = {0};
  const DWORD n = GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return nullptr;
  const std::wstring old = std::wstring(pf, n) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
  return LoadLibraryExW(old.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

template <class Fn>
Fn sym(HMODULE lib, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(lib, name)));
}

std::string moduleFolder(const std::string& resources) {
  // .../Contents/Resources -> .../Contents/Win64
  return resources.substr(0, resources.find_last_of("/\\")) + "/Win64";
}

}  // namespace

double totalRamBytes() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? double(m.ullTotalPhys) : 0.0;
}

double availableRamBytes() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? double(m.ullAvailPhys) : 0.0;
}

// The user's temp folder (%LOCALAPPDATA%\Temp), on disk like the Mac's. Forward slashes, no
// trailing one; the long form of the path (the short "NAME~1" form would not match what Python
// reports for the same folder).
std::string stagingDir() {
  wchar_t buf[MAX_PATH + 1] = {0};
  const DWORD n = GetTempPathW(MAX_PATH, buf);
  std::wstring dir = n > 0 && n < MAX_PATH ? std::wstring(buf, n) : L"C:\\Windows\\Temp";
  wchar_t longer[1024] = {0};
  const DWORD m = GetLongPathNameW(dir.c_str(), longer, 1023);
  if (m > 0 && m < 1023) dir.assign(longer, m);
  std::string out = forwardSlashes(narrow(dir));
  while (out.size() > 3 && out.back() == '/') out.pop_back();
  return out;
}

VramInfo queryVram() {
  VramInfo info;
  const HMODULE lib = loadNvml();
  if (!lib) return info;
  auto init = sym<InitFn>(lib, "nvmlInitWithFlags");
  auto handle = sym<HandleFn>(lib, "nvmlDeviceGetHandleByIndex_v2");
  auto mem = sym<MemFn>(lib, "nvmlDeviceGetMemoryInfo");
  auto shutdown = sym<ShutdownFn>(lib, "nvmlShutdown");
  auto name = sym<NameFn>(lib, "nvmlDeviceGetName");
  auto driver = sym<DriverFn>(lib, "nvmlSystemGetDriverVersion");
  if (init && handle && mem && shutdown && init(0) == 0) {
    void* dev = nullptr;
    NvmlMemory m{};
    if (handle(0, &dev) == 0 && mem(dev, &m) == 0) {
      info.ok = true;
      info.freeGB = double(m.free) / kGB;
      info.totalGB = double(m.total) / kGB;
      char buf[128] = {0};
      if (name && name(dev, buf, sizeof buf) == 0) info.name = buf;
      buf[0] = 0;
      if (driver && driver(buf, sizeof buf) == 0) info.driver = buf;
    }
    shutdown();
  }
  FreeLibrary(lib);
  return info;
}

// Under Windows' display driver model NVML lists the processes but mostly not their memory
// (usedGB < 0), so the preflight message names who is on the GPU without the amounts.
std::vector<GpuProcess> queryGpuProcesses() {
  std::vector<GpuProcess> out;
  const HMODULE lib = loadNvml();
  if (!lib) return out;
  auto init = sym<InitFn>(lib, "nvmlInitWithFlags");
  auto handle = sym<HandleFn>(lib, "nvmlDeviceGetHandleByIndex_v2");
  auto shutdown = sym<ShutdownFn>(lib, "nvmlShutdown");
  ProcFn lists[] = {sym<ProcFn>(lib, "nvmlDeviceGetComputeRunningProcesses_v3"),
                    sym<ProcFn>(lib, "nvmlDeviceGetGraphicsRunningProcesses_v3")};
  if (init && handle && shutdown && init(0) == 0) {
    void* dev = nullptr;
    std::map<unsigned, double> used;
    if (handle(0, &dev) == 0) {
      for (ProcFn list : lists) {
        if (!list) continue;
        std::vector<NvmlProcessInfo> infos(256);
        unsigned int count = unsigned(infos.size());
        if (list(dev, &count, infos.data()) != 0) continue;
        for (unsigned i = 0; i < count && i < infos.size(); ++i) {
          const double g = infos[i].usedGpuMemory == kNotAvailable ? -1.0 : double(infos[i].usedGpuMemory) / kGB;
          auto it = used.find(infos[i].pid);
          if (it == used.end() || g > it->second) used[infos[i].pid] = g;
        }
      }
    }
    shutdown();
    for (const auto& u : used) {
      GpuProcess p;
      p.pid = u.first;
      p.usedGB = u.second;
      p.name = processName(u.first);
      if (p.name.empty()) p.name = "pid " + std::to_string(u.first);
      out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const GpuProcess& a, const GpuProcess& b) { return a.usedGB > b.usedGB; });
  }
  FreeLibrary(lib);
  return out;
}

// "Resolve" for ...\Resolve.exe; empty if the process is gone.
std::string processName(unsigned pid) {
  const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";
  wchar_t buf[1024] = {0};
  DWORD size = 1023;
  std::string name;
  if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
    name = narrow(std::wstring(buf, size));
    name = name.substr(name.find_last_of("\\/") + 1);
    if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".exe") == 0) name.resize(name.size() - 4);
  }
  CloseHandle(h);
  return name;
}

std::vector<std::string> platformSnapshot() {
  std::vector<std::string> out;
  const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  // ProductName still says "Windows 10" on Windows 11: the build number tells them apart.
  const std::string build = registryString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild");
  std::string product = registryString(HKEY_LOCAL_MACHINE, nt, L"ProductName");
  if (std::atoi(build.c_str()) >= 22000 && product.rfind("Windows 10", 0) == 0) product.replace(0, 10, "Windows 11");
  out.push_back("os: " + (product.empty() ? std::string("Windows") : product) + " " +
                registryString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion") + " (build " + build + ")");
  std::string cpu = registryString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                   L"ProcessorNameString");
  while (!cpu.empty() && cpu.back() == ' ') cpu.pop_back();
  out.push_back("cpu: " + (cpu.empty() ? std::string("unknown") : cpu) + ", " +
                std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + " threads");
  out.push_back("ram: " + gb(totalRamBytes()) + " total, " + gb(availableRamBytes()) + " available");
  ULARGE_INTEGER freeBytes{}, totalBytes{};
  const std::string staging = stagingDir();
  if (GetDiskFreeSpaceExW(widen(staging).c_str(), &freeBytes, &totalBytes, nullptr))
    out.push_back("staging: " + staging + ", " + gb(double(totalBytes.QuadPart)) + " total, " +
                  gb(double(freeBytes.QuadPart)) + " free");
  return out;
}

std::string systemTool(const char* name) {
  wchar_t dir[MAX_PATH + 1] = {0};
  const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  return forwardSlashes(narrow(std::wstring(dir, n < MAX_PATH ? n : 0))) + "/" + name;
}

// Notepad has no title argument, so title is unused here (as with TextEdit on the Mac).
bool showTextFile(const std::string& /*title*/, const std::string& path, const Env& env, std::string& err) {
  return launchDetached({systemTool("notepad.exe"), backSlashes(path)}, env, err);
}

bool openUrl(const std::string& url, std::string& err) {
  // Only web addresses reach the shell, never a file or a program.
  if (url.rfind("https://", 0) != 0) {
    err = "not a web address";
    return false;
  }
  const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (rc > 32) return true;
  err = "the browser could not be opened (ShellExecute " + std::to_string(int(rc)) + ")";
  return false;
}

std::string uiHelper(const std::string& resources) { return moduleFolder(resources) + "/ltxag_ui.exe"; }

std::string bundledUv(const std::string& resources) {
  const std::string uv = moduleFolder(resources) + "/uv.exe";
  return GetFileAttributesW(widen(uv).c_str()) != INVALID_FILE_ATTRIBUTES ? uv : "";
}

}  // namespace ltxag
