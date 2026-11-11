"""Everything in the Python side that is specific to the operating system (decision 64): macOS,
and since the Windows port, Windows. CVT port (cvt1.17): upstream's Linux branches (zenity
windows, /proc) are gone. Standard library only (setup_helper.py and job.py run under the system
python3 on a Mac).

The progress window is mac_progress.swift, a small AppKit program that reads zenity's stdin
protocol, compiled once with swiftc (Command Line Tools, which Setup requires) into
~/.cache/ltx-alphagen/bin. Chosen over the alternatives on a stock macOS 15: AppleScript's
"progress" properties draw nothing when run by osascript (only in applets/Script Editor); the
system python3's tkinter (Tk 8.5) aborts at Tk() ("macOS 15 (1507) or later required"); a JXA
(osascript -l JavaScript) Cocoa window works but needs the untyped ObjC bridge for the button
target and the stdin reader; `swift script.swift` would recompile on every job. Messages are
osascript `display dialog`; ps gives the process name.

Windows: the progress window and the messages are ltxag_ui.exe (ofx/ui/ltxag_ui.cpp), built with the
plugin and placed beside the .ofx; it reads the same stdin protocol. Windows also has no process
groups, no signals, no flock and (in Python) no Unix sockets, so the calls job.py, the worker and
Setup need for those are here in both forms: pid_alive, kill_tree, NEW_SESSION, lock_file, listen /
connect (the resident worker: a Unix socket on the Mac, a loopback TCP port with a shared secret on
Windows), and replace / remove, which retry while another process has the file open.
"""

from __future__ import annotations

import hashlib
import os
import signal
import socket
import subprocess
import sys
import time

WINDOWS = sys.platform == "win32"
# subprocess.Popen arguments for a child that must not be ended with us: its own session on the
# Mac; on Windows its own process group and no console window of its own.
NEW_SESSION = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW} if WINDOWS
               else {"start_new_session": True})


class ProgressWindow:
    """A progress window with a Cancel button. Without a UI every call is a no-op."""

    def __init__(self, title: str, width: int, enabled: bool) -> None:
        self.proc = None
        helper = (_windows_ui_helper() if WINDOWS else _mac_progress_helper()) if enabled else ""
        if helper:  # zenity's stdin protocol; exits 1 on Cancel or when closed
            self.proc = subprocess.Popen([helper, "progress", title, str(width)] if WINDOWS else [helper, title, str(width)],
                                         stdin=subprocess.PIPE, text=True, encoding="utf-8")

    @property
    def shown(self) -> bool:
        return self.proc is not None

    def gone(self) -> bool:
        """True once the window has closed (the user pressed Cancel, or it was closed)."""
        return bool(self.proc) and self.proc.poll() is not None

    def show(self, frac: float, text: str) -> None:
        """Raises BrokenPipeError if the window has gone."""
        pct = max(0, min(99, int(frac * 100)))  # 100 auto-closes; sent only by close()
        self.proc.stdin.write(f"# {text}\n{pct}\n")
        self.proc.stdin.flush()

    def close(self, wait: float = 0.0) -> None:
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.write("100\n")
                self.proc.stdin.close()
            except OSError:  # BrokenPipeError; on Windows a closed pipe can also be EINVAL
                pass
            if wait:
                self.proc.wait(timeout=wait)


def _windows_ui_helper() -> str:
    """ltxag_ui.exe, beside the .ofx: this file is <bundle>/Contents/Resources/worker/ltxag_worker/.
    "" when it isn't there (run from a source folder): then there is no window."""
    here = os.path.dirname(os.path.abspath(__file__))
    exe = os.path.normpath(os.path.join(here, "..", "..", "..", "Win64", "ltxag_ui.exe"))
    return exe if os.path.exists(exe) else ""


def _mac_progress_helper() -> str:
    """Path of the compiled mac_progress.swift, built on first use (one binary per source version;
    the first build takes ~1 min while swiftc fills its module cache). "" if it can't be built:
    then there is no window. Build errors go to the log folder."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mac_progress.swift")
    try:
        with open(src, "rb") as f:
            exe = os.path.expanduser(f"~/.cache/ltx-alphagen/bin/ltxag_progress-"
                                     f"{hashlib.sha256(f.read()).hexdigest()[:12]}")
        if os.path.exists(exe):
            return exe
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        tmp = f"{exe}.{os.getpid()}.tmp"  # two jobs may build at once; os.replace is atomic
        r = subprocess.run(["/usr/bin/xcrun", "swiftc", "-O", "-o", tmp, src], capture_output=True,
                           text=True, timeout=600)
        if r.returncode != 0:
            logs = os.path.expanduser("~/.cache/ltx-alphagen/logs")
            os.makedirs(logs, exist_ok=True)
            with open(os.path.join(logs, "mac_progress_build.log"), "w") as f:
                f.write(r.stdout + r.stderr)
            return ""
        os.replace(tmp, exe)
        return exe
    except (OSError, subprocess.SubprocessError):
        return ""


def message(kind: str, title: str, text: str, width: int, enabled: bool) -> None:
    """A detached message window; kind is "info" or "error". `width` is unused: the dialog sizes
    itself (kept so callers need not change)."""
    # CVT port (cvt1.10): the system test runs under its own HOME and leaves this marker there,
    # so its simulated failures don't pop up dialogs on the user's screen. A real HOME never has it.
    if os.path.exists(os.path.expanduser("~/.ltxag-no-dialogs")) or not enabled:
        return
    if WINDOWS:
        helper = _windows_ui_helper()
        if helper:
            subprocess.Popen([helper, "message", kind, title, text], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **NEW_SESSION)
        return
    icon = "stop" if kind == "error" else "note"
    subprocess.Popen(["/usr/bin/osascript", "-e", "on run argv", "-e",
                      f'display dialog (item 2 of argv) with title (item 1 of argv) '
                      f'buttons {{"OK"}} default button "OK" with icon {icon}',
                      "-e", "end run", title, text],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def process_name(pid: int) -> str:
    """Short name of a live process, "" if it is gone. ps's "comm" is the executable's path
    (man ps); pid 0 is the kernel."""
    if WINDOWS:
        return _windows_process_name(pid)
    try:
        if int(pid) <= 0:
            return ""
        r = subprocess.run(["/bin/ps", "-p", str(int(pid)), "-o", "comm="], capture_output=True,
                           text=True, timeout=10)
        return os.path.basename(r.stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        return ""


# --------------------------------------------------------------------------- Windows port
# What job.py, the worker and Setup did with POSIX calls directly. On the Mac each is that same call.

_SYNCHRONIZE = 0x00100000
_QUERY_LIMITED = 0x1000  # PROCESS_QUERY_LIMITED_INFORMATION


def _kernel32():
    import ctypes
    from ctypes import wintypes

    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.restype = wintypes.HANDLE
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                             ctypes.POINTER(wintypes.DWORD)]
    return k


def _windows_process_name(pid: int) -> str:
    """"Resolve" for ...\\Resolve.exe, "" if the process is gone."""
    import ctypes
    from ctypes import wintypes

    try:
        if int(pid) <= 0:
            return ""
        k = _kernel32()
        h = k.OpenProcess(_QUERY_LIMITED, False, int(pid))
        if not h:
            return ""
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(1023)
            if not k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                return ""
            name = os.path.basename(buf.value)
            return name[:-4] if name.lower().endswith(".exe") else name
        finally:
            k.CloseHandle(h)
    except (OSError, ValueError):
        return ""


def pid_alive(pid: int) -> bool:
    """Is a process with this id running? (On Windows os.kill(pid, 0) is not a question: it sends
    Ctrl+C.)"""
    if int(pid) <= 0:
        return False
    if WINDOWS:
        import ctypes

        k = _kernel32()
        h = k.OpenProcess(_SYNCHRONIZE | _QUERY_LIMITED, False, int(pid))
        if not h:
            return ctypes.get_last_error() == 5  # access denied: it exists, as another user's
        try:
            return k.WaitForSingleObject(h, 0) == 0x102  # WAIT_TIMEOUT: still running
        finally:
            k.CloseHandle(h)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def kill_tree(pid: int) -> None:
    """Ends a process we started with NEW_SESSION and everything it started: the process group on
    the Mac (ProcessLookupError if it is gone, as os.killpg raises), taskkill /T on Windows."""
    if WINDOWS:
        subprocess.run([os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "taskkill.exe"),
                        "/PID", str(int(pid)), "/T", "/F"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW, check=False)
        return
    os.killpg(pid, signal.SIGKILL)


def lock_file(f) -> None:  # noqa: ANN001 - an open file
    """An exclusive lock on an open file, held until it is closed; waits for another holder."""
    if WINDOWS:
        import msvcrt

        while True:
            try:
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                return
            except OSError:
                time.sleep(0.1)
    import fcntl

    fcntl.flock(f, fcntl.LOCK_EX)


def replace(src: str, dst: str) -> None:
    """os.replace. Windows refuses to replace a file another process has open (the plugin reading
    a status file, a render reading a matte): those readers are done in milliseconds, so retry."""
    for attempt in range(80):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if not WINDOWS or attempt == 79:
                raise
            time.sleep(0.025)


def remove(path: str) -> None:
    """Deletes a file if it is there. On Windows a file still open elsewhere can't be deleted:
    retry for a moment, then leave it (the plugin sweeps stale staging files when it loads)."""
    for _attempt in range(40 if WINDOWS else 1):
        try:
            os.unlink(path)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            if not WINDOWS:
                raise
            time.sleep(0.05)


def listen(sock_path: str) -> tuple[socket.socket, dict]:
    """The resident worker's listening socket, and what its info file must say for a client to
    reach it. Mac: the Unix socket at sock_path, for this user only. Windows (Python has no Unix
    sockets there): a loopback TCP port chosen by the system, plus a random secret every request
    must carry, since any local program can connect to a port; the info file is in the user's
    folder."""
    if WINDOWS:
        import secrets

        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.bind(("127.0.0.1", 0))
        return srv, {"port": srv.getsockname()[1], "secret": secrets.token_hex(16)}
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    os.chmod(sock_path, 0o600)
    return srv, {}


def connect(sock_path: str, info: dict) -> socket.socket:
    """Connects to the resident worker (info = its info file). FileNotFoundError or
    ConnectionRefusedError while it isn't listening yet."""
    if WINDOWS:
        if not info.get("port"):
            raise FileNotFoundError(sock_path)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.connect(("127.0.0.1", int(info["port"])))
        except OSError as exc:
            s.close()
            raise ConnectionRefusedError(str(exc)) from exc
        return s
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock_path)
    except (FileNotFoundError, ConnectionRefusedError):
        s.close()
        raise
    return s


def wait_stdin_closed() -> None:
    """Returns when whoever started us has closed our stdin (it ended, or cancelled). On the Mac a
    read to the end. On Windows a thread blocked in a read on a pipe makes the main thread hang
    when it next loads a native library (the loader asks about the standard handles, and that
    waits for the read): seen with the real worker, which hung at `import scipy` with no GPU
    work started. So there the pipe is asked, twice a second, whether its other end is still
    open."""
    if WINDOWS:
        import ctypes
        import msvcrt
        from ctypes import wintypes

        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.GetFileType.argtypes = [wintypes.HANDLE]
        k.PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                    ctypes.c_void_p, ctypes.c_void_p]
        try:
            handle = msvcrt.get_osfhandle(sys.stdin.fileno())
        except (OSError, ValueError, AttributeError):
            return  # no stdin at all
        if k.GetFileType(handle) == 3:  # FILE_TYPE_PIPE
            while k.PeekNamedPipe(handle, None, 0, None, None, None):
                time.sleep(0.5)
            return
    sys.stdin.read()
