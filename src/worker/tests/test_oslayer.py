"""The OS layer (decision 64, macOS only since cvt1.17): process names and the window commands."""

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
        p = subprocess.Popen(["/bin/sleep", "5"])
        try:
            self.assertEqual(oslayer.process_name(p.pid), "sleep")
        finally:
            p.kill()
            p.wait()
        self.assertEqual(oslayer.process_name(p.pid), "")
        self.assertEqual(oslayer.process_name(0), "")


class WindowCommandTest(unittest.TestCase):
    def test_disabled_is_a_no_op(self):
        win = oslayer.ProgressWindow("t", 500, False)
        self.assertFalse(win.shown)
        self.assertFalse(win.gone())
        win.close()

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


# cvt1.17: opt-in (LTXAG_GUI_TESTS=1): it opens a real window on the user's screen for a second.
@unittest.skipUnless(os.environ.get("LTXAG_GUI_TESTS") == "1", "opens a window: set LTXAG_GUI_TESTS=1")
class MacProgressHelperTest(unittest.TestCase):
    def test_build_and_protocol(self):
        # Shows a window for about a second: the helper needs the logged-in user's GUI session.
        win = oslayer.ProgressWindow("LTX Alpha Gen test", 520, True)
        self.assertTrue(win.shown, "swiftc build failed: see ~/.cache/ltx-alphagen/logs/mac_progress_build.log")
        win.show(0.4, "Chunk 1/2 · frames 1–145 of 200")
        self.assertFalse(win.gone())
        win.close(wait=10)
        self.assertEqual(win.proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
