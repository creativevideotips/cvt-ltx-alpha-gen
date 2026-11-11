"""The OS layer (decision 64; macOS, and Windows since the Windows port): process names, the window
commands, and on both the calls that stand in for POSIX ones (pid_alive, replace, listen/connect)."""

import os
import subprocess
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

from ltxag_worker import oslayer  # noqa: E402


class ProcessNameTest(unittest.TestCase):
    def test_live_and_gone(self):
        if sys.platform == "win32":
            p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(5)"])
            want = os.path.splitext(os.path.basename(sys.executable))[0]
        else:
            p = subprocess.Popen(["/bin/sleep", "5"])
            want = "sleep"
        try:
            self.assertEqual(oslayer.process_name(p.pid).lower(), want.lower())
            self.assertTrue(oslayer.pid_alive(p.pid))
        finally:
            p.kill()
            p.wait()
        self.assertEqual(oslayer.process_name(p.pid), "")
        self.assertEqual(oslayer.process_name(0), "")
        self.assertFalse(oslayer.pid_alive(p.pid))
        self.assertFalse(oslayer.pid_alive(0))


class WindowCommandTest(unittest.TestCase):
    def test_disabled_is_a_no_op(self):
        win = oslayer.ProgressWindow("t", 500, False)
        self.assertFalse(win.shown)
        self.assertFalse(win.gone())
        win.close()

    @unittest.skipUnless(sys.platform == "win32", "the Windows commands")
    def test_windows_calls(self):
        with mock.patch.object(oslayer, "_windows_ui_helper", return_value="C:\\x\\ltxag_ui.exe"), \
                mock.patch.object(oslayer.subprocess, "Popen") as popen:
            oslayer.ProgressWindow("T", 560, True)
            oslayer.message("error", "T", 'say "hi"', 420, True)
        self.assertEqual(popen.call_args_list[0][0][0], ["C:\\x\\ltxag_ui.exe", "progress", "T", "560"])
        # the text travels as its own argument, never through a shell
        self.assertEqual(popen.call_args_list[1][0][0], ["C:\\x\\ltxag_ui.exe", "message", "error", "T", 'say "hi"'])

    @unittest.skipIf(sys.platform == "win32", "the Mac commands")
    def test_darwin_calls(self):  # cvt1.17: Mac only, no platform switch left to patch
        with mock.patch.object(oslayer, "_mac_progress_helper", return_value="/x/ltxag_progress"), \
                mock.patch.object(oslayer.subprocess, "Popen") as popen:
            oslayer.ProgressWindow("T", 560, True)
            oslayer.message("info", "T", 'say "hi"', 420, True)
        self.assertEqual(popen.call_args_list[0][0][0], ["/x/ltxag_progress", "T", "560"])
        cmd = popen.call_args_list[1][0][0]
        self.assertEqual(cmd[0], "/usr/bin/osascript")
        self.assertIn("with icon note", cmd[4])
        self.assertEqual(cmd[-2:], ["T", 'say "hi"'])  # text travels as argv, never inside the script


class StandInTest(unittest.TestCase):
    """Windows port: the calls that are a POSIX call on the Mac and something else on Windows."""

    def test_replace_and_remove(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            a, b = os.path.join(d, "a"), os.path.join(d, "b")
            for path, text in ((a, "new"), (b, "old")):
                with open(path, "w") as f:
                    f.write(text)
            oslayer.replace(a, b)
            with open(b) as f:
                self.assertEqual(f.read(), "new")
            self.assertFalse(os.path.exists(a))
            oslayer.remove(b)
            oslayer.remove(b)  # gone already: not an error
            self.assertFalse(os.path.exists(b))

    def test_listen_and_connect(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "w.sock")
            with self.assertRaises((FileNotFoundError, ConnectionRefusedError)):
                oslayer.connect(path, {})  # nobody listens yet
            srv, endpoint = oslayer.listen(path)
            srv.listen(1)
            self.assertEqual(bool(endpoint.get("secret")), sys.platform == "win32")
            client = oslayer.connect(path, endpoint)
            conn, _ = srv.accept()
            client.sendall(b"hello\n")
            self.assertEqual(conn.recv(16), b"hello\n")
            for s in (client, conn, srv):
                s.close()


# cvt1.17: opt-in (LTXAG_GUI_TESTS=1): it opens a real window on the user's screen for a second.
@unittest.skipUnless(os.environ.get("LTXAG_GUI_TESTS") == "1", "opens a window: set LTXAG_GUI_TESTS=1")
class MacProgressHelperTest(unittest.TestCase):
    def test_build_and_protocol(self):
        # Shows a window for about a second: the helper needs the logged-in user's GUI session.
        win = oslayer.ProgressWindow("LTX Alpha Gen test", 520, True)
        self.assertTrue(win.shown, "no window: on a Mac the swiftc build failed (see ~/.cache/ltx-alphagen/logs/"
                                   "mac_progress_build.log); on Windows run it from a built bundle")
        win.show(0.4, "Chunk 1/2 · frames 1–145 of 200")
        self.assertFalse(win.gone())
        win.close(wait=10)
        self.assertEqual(win.proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
