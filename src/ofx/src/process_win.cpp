// Windows port: process.h for Windows (process.cpp is the POSIX one; CMake picks one).
// - Worker: its own job object (kill = end the job, so its children go too), stdout and stderr
//   piped back, a stdin pipe held open so the worker sees the end of input if the host dies.
// - Detached: no console window, no handle of the host, not in the host's job object where the
//   host allows it, so it outlives the host.
// Children inherit only the handles named here (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), never the
// host's other files or sockets.
#include "process.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace ltxag {

namespace {

const wchar_t* const kStripped[] = {L"PYTHONHOME",     L"PYTHONPATH", L"PYTHONSTARTUP",
                                    L"PYTHONUSERBASE", L"HF_TOKEN"};

std::wstring upper(std::wstring s) {
  for (auto& c : s) c = wchar_t(std::towupper(c));
  return s;
}

std::wstring nameOf(const std::wstring& entry) {
  // "=C:=C:\folder" entries (per-drive current folders) start with '=': the name ends at the
  // next one.
  const size_t eq = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
  return upper(entry.substr(0, eq));
}

std::string winError(const char* what, DWORD code) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::string text = msg ? narrow(msg) : "";
  if (msg) LocalFree(msg);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return std::string(what) + ": " + (text.empty() ? "error " + std::to_string(code) : text);
}

// One argument as CommandLineToArgvW (and the C runtime) will read it back.
std::wstring quoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') ++i, ++slashes;
    if (i == arg.size()) {
      out.append(slashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"')
      out.append(slashes * 2 + 1, L'\\');
    else
      out.append(slashes, L'\\');
    out.push_back(arg[i]);
  }
  out.push_back(L'"');
  return out;
}

std::wstring commandLine(const std::vector<std::string>& argv) {
  std::wstring line;
  for (const auto& a : argv) line += (line.empty() ? L"" : L" ") + quoteArg(widen(a));
  return line;
}

// The program: a path as given, or a bare name ("python.exe") found the way the system finds it.
std::wstring programPath(const std::string& argv0) {
  const std::wstring given = widen(backSlashes(argv0));
  if (given.find_first_of(L"\\:") != std::wstring::npos) return given;
  wchar_t found[MAX_PATH + 1] = {0};
  const DWORD n = SearchPathW(nullptr, given.c_str(), L".exe", MAX_PATH, found, nullptr);
  return n > 0 && n < MAX_PATH ? std::wstring(found, n) : given;
}

// "KEY=value" strings as the double-null-terminated UTF-16 block CreateProcess wants. Windows
// expects it sorted by name without regard to case.
std::vector<wchar_t> environmentBlock(const Env& env) {
  std::vector<std::wstring> wide;
  for (const auto& e : env) wide.push_back(widen(e));
  std::stable_sort(wide.begin(), wide.end(),
                   [](const std::wstring& a, const std::wstring& b) { return nameOf(a) < nameOf(b); });
  std::vector<wchar_t> block;
  for (const auto& e : wide) {
    block.insert(block.end(), e.begin(), e.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

HANDLE inheritable(HANDLE h) {
  SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  return h;
}

// CreateProcess with exactly `handles` inherited, as the child's stdin, stdout and stderr.
bool spawn(const std::vector<std::string>& argv, const Env& env, HANDLE in, HANDLE out, DWORD flags,
           PROCESS_INFORMATION& pi, std::string& err) {
  HANDLE handles[2] = {in, out};
  const DWORD count = in == out ? 1 : 2;
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> attrs(size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
      !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, count * sizeof(HANDLE), nullptr,
                                 nullptr)) {
    err = winError("process attributes", GetLastError());
    return false;
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in;
  si.StartupInfo.hStdOutput = out;
  si.StartupInfo.hStdError = out;
  si.lpAttributeList = list;
  const std::wstring program = programPath(argv[0]);
  std::wstring line = commandLine(argv);
  std::vector<wchar_t> block = environmentBlock(env);
  flags |= CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
  BOOL ok = CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, TRUE, flags, block.data(), nullptr,
                           &si.StartupInfo, &pi);
  if (!ok && (flags & CREATE_BREAKAWAY_FROM_JOB) && GetLastError() == ERROR_ACCESS_DENIED) {
    // The host's job object does not let processes leave it: stay in it.
    ok = CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, TRUE, flags & ~DWORD(CREATE_BREAKAWAY_FROM_JOB),
                        block.data(), nullptr, &si.StartupInfo, &pi);
  }
  const DWORD code = GetLastError();
  DeleteProcThreadAttributeList(list);
  if (!ok) err = winError(("cannot start " + argv[0]).c_str(), code);
  return ok != FALSE;
}

}  // namespace

Env cleanEnvironment(const Env& extras) {
  std::vector<std::wstring> drop(std::begin(kStripped), std::end(kStripped));
  for (const auto& e : extras) drop.push_back(nameOf(widen(e)));  // an extra replaces the host's value
  Env env;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
      const std::wstring entry = p;
      if (std::find(drop.begin(), drop.end(), nameOf(entry)) == drop.end()) env.push_back(narrow(entry));
    }
    FreeEnvironmentStringsW(block);
  }
  env.insert(env.end(), extras.begin(), extras.end());
  return env;
}

WorkerProcess::~WorkerProcess() {
  if (process_) {
    kill();
    wait();
  }
  if (outRead_) CloseHandle(outRead_);
  if (inWrite_) CloseHandle(inWrite_);
  if (job_) CloseHandle(job_);
}

bool WorkerProcess::start(const std::vector<std::string>& argv, const Env& env, std::string& err) {
  HANDLE outRd = nullptr, outWr = nullptr, inRd = nullptr, inWr = nullptr;
  if (!CreatePipe(&outRd, &outWr, nullptr, 1 << 16) || !CreatePipe(&inRd, &inWr, nullptr, 0)) {
    err = winError("pipe", GetLastError());
    if (outRd) CloseHandle(outRd), CloseHandle(outWr);
    return false;
  }
  inheritable(outWr);
  inheritable(inRd);
  job_ = CreateJobObjectW(nullptr, nullptr);
  if (job_) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);
  }
  PROCESS_INFORMATION pi{};
  // Suspended until it is in the job, so a child it starts at once is in the job too.
  const bool ok = spawn(argv, env, inRd, outWr, CREATE_SUSPENDED, pi, err);
  CloseHandle(outWr);
  CloseHandle(inRd);
  outRead_ = outRd;
  inWrite_ = inWr;
  if (!ok) return false;
  if (job_) AssignProcessToJobObject(job_, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  pid_ = pid_t(pi.dwProcessId);
  return true;
}

bool WorkerProcess::readLines(int timeoutMs, std::vector<std::string>& lines) {
  if (!outRead_) return false;
  const ULONGLONG deadline = GetTickCount64() + ULONGLONG(std::max(timeoutMs, 0));
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(outRead_, nullptr, 0, nullptr, &avail, nullptr)) {  // the worker closed its end
      if (!buf_.empty()) lines.push_back(buf_);
      buf_.clear();
      CloseHandle(outRead_);
      outRead_ = nullptr;
      break;
    }
    if (avail > 0) {
      char chunk[4096];
      DWORD got = 0;
      if (ReadFile(outRead_, chunk, DWORD(std::min<size_t>(avail, sizeof chunk)), &got, nullptr) && got > 0)
        buf_.append(chunk, got);
      break;
    }
    if (GetTickCount64() >= deadline) break;
    Sleep(10);
  }
  size_t nl;
  while ((nl = buf_.find('\n')) != std::string::npos) {
    std::string line = buf_.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
    buf_.erase(0, nl + 1);
  }
  return outRead_ != nullptr;
}

void WorkerProcess::kill() {
  if (job_) TerminateJobObject(job_, 9);
  if (process_) TerminateProcess(process_, 9);
}

int WorkerProcess::wait() {
  DWORD code = 0;
  if (process_) {
    WaitForSingleObject(process_, INFINITE);
    GetExitCodeProcess(process_, &code);
    CloseHandle(process_);
    process_ = nullptr;
    pid_ = -1;
  }
  return int(code);
}

bool launchDetached(const std::vector<std::string>& argv, const Env& env, std::string& err) {
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
  if (nul == INVALID_HANDLE_VALUE) {
    err = winError("cannot start helper", GetLastError());
    return false;
  }
  PROCESS_INFORMATION pi{};
  const bool ok = spawn(argv, env, nul, nul, CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB, pi, err);
  CloseHandle(nul);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace ltxag
