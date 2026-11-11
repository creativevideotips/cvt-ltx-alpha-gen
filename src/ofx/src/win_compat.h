// Windows port: the POSIX calls the plugin's shared code uses, so that code stays as it is on
// macOS. Every path is UTF-8 (OFX strings are; a user folder may hold non-ASCII letters) and is
// converted to UTF-16 here, which the C runtime's own narrow versions don't do (they use the ANSI
// code page). CMake defines _CRT_DECLARE_NONSTDC_NAMES=0 for our sources, so the C runtime does
// not declare its versions of these names (unlink, mkdir, stat, open, getpid ...).
// Include it after the standard headers. Implemented in win_compat.cpp.
#pragma once
#ifdef _WIN32

#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

using ssize_t = intptr_t;
using pid_t = int;
using off_t = long long;

#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_EXCL _O_EXCL
#define O_TRUNC _O_TRUNC
#define O_APPEND _O_APPEND
#define O_CLOEXEC _O_NOINHERIT
#define F_OK 0
#define X_OK 0  // "exists" is all that can be asked here
#define AT_FDCWD (-100)
#define S_IFDIR 0x4000
#define S_ISDIR(m) (((m) & S_IFDIR) != 0)
// WorkerProcess::wait() returns the exit code on Windows (there are no signals).
#define WIFEXITED(status) (true)
#define WEXITSTATUS(status) (status)

struct stat {
  int st_mode = 0;
  long long st_size = 0;
  time_t st_mtime = 0;
};
struct dirent {
  char d_name[1024];
};
struct DIR;

int stat(const char* path, struct stat* st);
int access(const char* path, int mode);
int unlink(const char* path);
int mkdir(const char* path, int mode);
inline int chmod(const char*, int) { return 0; }  // files get the folder's access list
int getpid();
DIR* opendir(const char* path);
dirent* readdir(DIR* dir);
int closedir(DIR* dir);
// Binary, never inherited by child processes. O_APPEND writes are atomic between processes
// (FILE_APPEND_DATA), as the shared log needs.
int open(const char* path, int flags, int mode = 0);
int close(int fd);
ssize_t write(int fd, const void* data, size_t len);
ssize_t read(int fd, void* data, size_t len);
ssize_t pread(int fd, void* data, size_t len, off_t offset);
int posix_fallocate(int fd, off_t offset, off_t len);  // 0 or an errno
int utimensat(int, const char* path, const timespec* times, int);  // times: {access, modified}; null = now
std::tm* localtime_r(const std::time_t* t, std::tm* out);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
char* mkdtemp(char* tmpl);  // tests

namespace ltxag {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
// For std::ifstream / std::ofstream, which take a wide path in the Microsoft library.
inline std::wstring fsPath(const std::string& utf8) { return widen(utf8); }
// %USERPROFILE% with forward slashes ("C:/Users/name"), so "/sub/folder" can be appended as on
// macOS. Python's os.path.expanduser("~") is the same folder.
std::string homeDirectory();
// rename() that replaces an existing file, as POSIX does. Retries for a moment when a reader has
// the target open (Windows refuses to replace an open file).
int renameOver(const char* from, const char* to);
bool processAlive(int pid);
// Full path of the module (the .ofx) that holds this address, forward slashes.
std::string modulePath(const void* address);
std::string forwardSlashes(std::string path);
std::string backSlashes(std::string path);
// "C:/x", "C:\x" or "\\server\share".
bool isAbsolutePath(const std::string& path);
// True if the path's drive (or share) is not there: an unplugged or unmapped drive.
bool driveMissing(const std::string& path);

}  // namespace ltxag

#endif  // _WIN32
