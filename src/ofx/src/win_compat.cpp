// Windows port: see win_compat.h.
#include "win_compat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <io.h>
#include <process.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <random>

namespace ltxag {

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
  return out;
}

std::string narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::string forwardSlashes(std::string path) {
  for (auto& c : path)
    if (c == '\\') c = '/';
  return path;
}

std::string backSlashes(std::string path) {
  for (auto& c : path)
    if (c == '/') c = '\\';
  return path;
}

std::string homeDirectory() {
  wchar_t buf[MAX_PATH + 1] = {0};
  DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) n = GetTempPathW(MAX_PATH, buf);
  std::string home = forwardSlashes(narrow(std::wstring(buf, n < MAX_PATH ? n : 0)));
  while (home.size() > 3 && home.back() == '/') home.pop_back();
  return home.empty() ? "C:/" : home;
}

int renameOver(const char* from, const char* to) {
  const std::wstring a = widen(from), b = widen(to);
  for (int attempt = 0; attempt < 40; ++attempt) {
    if (MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING)) return 0;
    const DWORD e = GetLastError();
    if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) break;
    Sleep(25);  // a reader has the target open: it is done in a moment
  }
  errno = EACCES;
  return -1;
}

bool processAlive(int pid) {
  if (pid <= 0) return false;
  const HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;  // exists, but another user's
  const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
}

std::string modulePath(const void* address) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &mod))
    return "";
  std::wstring buf(32768, L'\0');
  const DWORD n = GetModuleFileNameW(mod, &buf[0], DWORD(buf.size()));
  if (n == 0 || n >= buf.size()) return "";
  buf.resize(n);
  return forwardSlashes(narrow(buf));
}

bool isAbsolutePath(const std::string& p) {
  if (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
    return true;
  return p.size() >= 2 && (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\');
}

bool driveMissing(const std::string& path) {
  if (!isAbsolutePath(path)) return false;
  std::wstring root;
  if (path[1] == ':') {
    root = widen(path.substr(0, 2)) + L"\\";
  } else {  // \\server\share\...
    const std::string p = backSlashes(path);
    const size_t server = p.find('\\', 2);
    const size_t share = server == std::string::npos ? server : p.find('\\', server + 1);
    root = widen(p.substr(0, share)) + L"\\";
  }
  return GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
}

}  // namespace ltxag

using ltxag::widen;

namespace {

time_t toTimeT(const FILETIME& ft) {
  ULARGE_INTEGER v;
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  return time_t((v.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

int errnoFromWin(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
      return ENOENT;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return EEXIST;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return ENOSPC;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      return EACCES;
    default:
      return EIO;
  }
}

HANDLE handleOf(int fd) { return reinterpret_cast<HANDLE>(_get_osfhandle(fd)); }

}  // namespace

int stat(const char* path, struct stat* st) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) {
    errno = errnoFromWin(GetLastError());
    return -1;
  }
  st->st_mode = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : 0x8000;
  st->st_size = (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  st->st_mtime = toTimeT(d.ftLastWriteTime);
  return 0;
}

int access(const char* path, int) {
  if (GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES) return 0;
  errno = ENOENT;
  return -1;
}

int unlink(const char* path) {
  if (DeleteFileW(widen(path).c_str())) return 0;
  errno = errnoFromWin(GetLastError());
  return -1;
}

int mkdir(const char* path, int) {
  if (CreateDirectoryW(widen(path).c_str(), nullptr)) return 0;
  errno = errnoFromWin(GetLastError());
  return -1;
}

int getpid() { return _getpid(); }

struct DIR {
  HANDLE find = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data{};
  bool first = true;
  dirent entry{};
};

DIR* opendir(const char* path) {
  std::string pattern = path;
  if (!pattern.empty() && pattern.back() != '/' && pattern.back() != '\\') pattern += '/';
  pattern += '*';
  DIR* d = new DIR;
  d->find = FindFirstFileW(widen(pattern).c_str(), &d->data);
  if (d->find == INVALID_HANDLE_VALUE) {
    delete d;
    errno = ENOENT;
    return nullptr;
  }
  return d;
}

dirent* readdir(DIR* d) {
  if (!d) return nullptr;
  if (!d->first && !FindNextFileW(d->find, &d->data)) return nullptr;
  d->first = false;
  const std::string name = ltxag::narrow(d->data.cFileName);
  std::strncpy(d->entry.d_name, name.c_str(), sizeof d->entry.d_name - 1);
  d->entry.d_name[sizeof d->entry.d_name - 1] = 0;
  return &d->entry;
}

int closedir(DIR* d) {
  if (!d) return -1;
  FindClose(d->find);
  delete d;
  return 0;
}

int open(const char* path, int flags, int) {
  const int rw = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
  DWORD accessMode = GENERIC_READ;
  if (flags & _O_APPEND)
    accessMode = FILE_APPEND_DATA;  // every write goes to the end, atomically
  else if (rw == _O_WRONLY)
    accessMode = GENERIC_WRITE;
  else if (rw == _O_RDWR)
    accessMode = GENERIC_READ | GENERIC_WRITE;
  DWORD create = OPEN_EXISTING;
  if ((flags & _O_CREAT) && (flags & _O_EXCL))
    create = CREATE_NEW;
  else if ((flags & _O_CREAT) && (flags & _O_TRUNC))
    create = CREATE_ALWAYS;
  else if (flags & _O_CREAT)
    create = OPEN_ALWAYS;
  else if (flags & _O_TRUNC)
    create = TRUNCATE_EXISTING;
  const HANDLE h = CreateFileW(widen(path).c_str(), accessMode,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    errno = errnoFromWin(GetLastError());
    return -1;
  }
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h),
                                 _O_BINARY | _O_NOINHERIT | (flags & _O_APPEND) | (rw == _O_RDONLY ? _O_RDONLY : 0));
  if (fd < 0) {
    CloseHandle(h);
    errno = EMFILE;
  }
  return fd;
}

int close(int fd) { return _close(fd); }

ssize_t write(int fd, const void* data, size_t len) {
  // The handle, not _write: with FILE_APPEND_DATA the system appends by itself, and _write's own
  // seek to the end is not allowed on such a handle.
  const DWORD part = DWORD(len > (1u << 30) ? (1u << 30) : len);
  DWORD done = 0;
  if (!WriteFile(handleOf(fd), data, part, &done, nullptr)) {
    errno = errnoFromWin(GetLastError());
    return -1;
  }
  return ssize_t(done);
}

ssize_t read(int fd, void* data, size_t len) {
  const DWORD part = DWORD(len > (1u << 30) ? (1u << 30) : len);
  DWORD done = 0;
  if (!ReadFile(handleOf(fd), data, part, &done, nullptr)) {
    if (GetLastError() == ERROR_HANDLE_EOF || GetLastError() == ERROR_BROKEN_PIPE) return 0;
    errno = EIO;
    return -1;
  }
  return ssize_t(done);
}

ssize_t pread(int fd, void* data, size_t len, off_t offset) {
  OVERLAPPED ov{};
  ov.Offset = DWORD(static_cast<unsigned long long>(offset) & 0xffffffffu);
  ov.OffsetHigh = DWORD(static_cast<unsigned long long>(offset) >> 32);
  const DWORD part = DWORD(len > (1u << 30) ? (1u << 30) : len);
  DWORD done = 0;
  if (!ReadFile(handleOf(fd), data, part, &done, &ov)) {
    if (GetLastError() == ERROR_HANDLE_EOF) return 0;
    errno = EIO;
    return -1;
  }
  return ssize_t(done);
}

int posix_fallocate(int fd, off_t offset, off_t len) {
  // Sets the size, which reserves the space on NTFS; then back to the start for the writes.
  const HANDLE h = handleOf(fd);
  LARGE_INTEGER end, start;
  end.QuadPart = offset + len;
  start.QuadPart = 0;
  if (!SetFilePointerEx(h, end, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) return errnoFromWin(GetLastError());
  if (!SetFilePointerEx(h, start, nullptr, FILE_BEGIN)) return EIO;
  return 0;
}

int utimensat(int, const char* path, const timespec* times, int) {
  const HANDLE h = CreateFileW(widen(path).c_str(), FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return -1;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  if (times) {
    ULARGE_INTEGER v;
    v.QuadPart = static_cast<unsigned long long>(times[1].tv_sec) * 10000000ULL + 116444736000000000ULL;
    now.dwLowDateTime = v.LowPart;
    now.dwHighDateTime = v.HighPart;
  }
  const BOOL ok = SetFileTime(h, nullptr, &now, &now);
  CloseHandle(h);
  return ok ? 0 : -1;
}

std::tm* localtime_r(const std::time_t* t, std::tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }

int setenv(const char* name, const char* value, int) {
  // Both the system's and the C runtime's copy: the plugin reads either.
  SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
  return _wputenv_s(widen(name).c_str(), widen(value).c_str());
}

int unsetenv(const char* name) {
  SetEnvironmentVariableW(widen(name).c_str(), nullptr);
  return _wputenv_s(widen(name).c_str(), L"");
}

char* mkdtemp(char* tmpl) {
  // The trailing XXXXXX becomes random; a leading "/tmp/" becomes the user's temp folder, so
  // the buffer must be the caller's std::string-sized copy (see the tests).
  const size_t len = std::strlen(tmpl);
  if (len < 6) return nullptr;
  std::random_device rd;
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (size_t i = len - 6; i < len; ++i) tmpl[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[rd() % 36];
    if (CreateDirectoryW(widen(tmpl).c_str(), nullptr)) return tmpl;
  }
  return nullptr;
}
