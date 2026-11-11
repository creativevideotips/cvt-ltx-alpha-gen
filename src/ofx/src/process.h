// Child processes launched from inside Flame.
// - Worker: own process group (cancel = SIGKILL the group), stdout/stderr piped
//   back line by line, stdin pipe held open so the worker sees EOF if Flame dies.
// - Detached: fire-and-forget (Model Page helper), own session, never a zombie.
// Both get a cleaned environment: Flame's PYTHONHOME/PYTHONPATH/LD_LIBRARY_PATH
// must not leak into our Python.
#pragma once

#ifdef _WIN32
// Windows port: the C runtime has a process.h of its own, which <thread> includes, and this
// folder comes first on the include path: so this file passes the runtime's one on.
#include <../ucrt/process.h>

#include "win_compat.h"  // pid_t
#else
#include <sys/types.h>
#endif

#include <string>
#include <vector>

namespace ltxag {

using Env = std::vector<std::string>;  // "KEY=value"

// Current environment minus variables that would hijack our Python, plus extras.
Env cleanEnvironment(const Env& extras = {});

class WorkerProcess {
 public:
  ~WorkerProcess();
  bool start(const std::vector<std::string>& argv, const Env& env, std::string& err);
  // Waits up to timeoutMs for output; appends complete lines. Returns false once
  // the worker has closed stdout (it exited) and all lines were delivered.
  bool readLines(int timeoutMs, std::vector<std::string>& lines);
  void kill();  // SIGKILL the whole process group
  int wait();   // blocking; returns the raw waitpid status
  pid_t pid() const { return pid_; }

 private:
  pid_t pid_ = -1;
#ifdef _WIN32
  // Windows port (process_win.cpp): handles. The worker and everything it starts are in a job
  // object, which kill() ends; wait() returns the exit code.
  void* process_ = nullptr;
  void* job_ = nullptr;
  void* outRead_ = nullptr;
  void* inWrite_ = nullptr;
#else
  int outFd_ = -1;
  int inFd_ = -1;
#endif
  std::string buf_;
};

// Runs argv detached from Flame (new session, reparented to init).
bool launchDetached(const std::vector<std::string>& argv, const Env& env, std::string& err);

}  // namespace ltxag
