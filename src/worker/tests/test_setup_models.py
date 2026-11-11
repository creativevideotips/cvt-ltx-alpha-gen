"""Setup's model step with an Existing Models folder: copy (decision 51) or link (decision 66)."""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "helper"))

import setup_helper  # noqa: E402

DATA = b"model weights" * 1000
# Windows port: the variable that names the user's folder (where ~ points, so the config, cache
# and log of a test stay out of the real ones).
HOME_VAR = "USERPROFILE" if sys.platform == "win32" else "HOME"


def manifest(sha: str) -> dict:
    return {"files": [{"path": "loras/m.safetensors", "size": len(DATA), "sha256": sha, "repo": "r",
                       "revision": "v", "hf_path": "m.safetensors"}]}


class ExistingModelsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        self.home = os.environ.get(HOME_VAR)
        os.environ[HOME_VAR] = root  # the helper logs to ~/.cache: keep it out of the real log
        self.existing = os.path.join(root, "existing", "sub")
        os.makedirs(self.existing)
        self.src = os.path.join(self.existing, "m.safetensors")
        with open(self.src, "wb") as f:
            f.write(DATA)
        self.models = os.path.join(root, "models")
        self.dst = os.path.join(self.models, "loras", "m.safetensors")
        self.man = manifest(hashlib.sha256(DATA).hexdigest())
        self.prog = setup_helper.Progress(False)

    def tearDown(self) -> None:
        if self.home is not None:
            os.environ[HOME_VAR] = self.home
        self.tmp.cleanup()

    def linked(self) -> bool:
        """A symlink; on Windows also a hard link (setup_helper.link_to: a symlink needs Developer
        Mode or admin rights there)."""
        return os.path.islink(self.dst) or (sys.platform == "win32" and os.path.samefile(self.dst, self.src))

    def gather(self, link: bool, man: dict | None = None) -> dict:
        return setup_helper.gather_models(man or self.man, self.models, os.path.dirname(self.existing), "",
                                          self.prog, link=link)

    def test_link(self):
        s = self.gather(link=True)
        self.assertEqual(len(s["linked"]), 1)
        self.assertTrue(self.linked())
        self.assertTrue(os.path.samefile(self.dst, self.src))
        self.assertTrue(os.path.exists(self.dst + ".verified"))
        again = self.gather(link=True)  # a re-run keeps the verified link
        self.assertEqual(len(again["kept"]), 1)
        self.assertTrue(self.linked())

    def test_copy(self):
        s = self.gather(link=False)
        self.assertEqual(len(s["copied"]), 1)
        self.assertFalse(os.path.islink(self.dst))
        with open(self.dst, "rb") as f:
            self.assertEqual(f.read(), DATA)

    def test_link_rejects_other_version(self):
        with self.assertRaises(setup_helper.SetupError):  # no token to download instead
            self.gather(link=True, man=manifest("0" * 64))
        self.assertFalse(os.path.lexists(self.dst))

    def test_link_replaces_dangling_link(self):
        os.makedirs(os.path.dirname(self.dst))
        try:
            os.symlink(os.path.join(self.tmp.name, "gone.safetensors"), self.dst)
        except OSError:
            self.skipTest("this Windows user may not make symlinks (no Developer Mode)")
        self.gather(link=True)
        self.assertEqual(os.path.realpath(self.dst), os.path.realpath(self.src))


if __name__ == "__main__":
    unittest.main()


class TokenRedirectTest(unittest.TestCase):
    """cvt1.43: the Hugging Face token must not follow a redirect to another host."""

    def redirected(self, new_url: str):
        req = urllib.request.Request("https://huggingface.co/r/resolve/v/m",
                                     headers={"Authorization": "Bearer secret"})
        return setup_helper._KeepTokenOnHost().redirect_request(req, None, 302, "Found", {}, new_url)

    def test_other_host_loses_the_token(self):
        self.assertFalse(self.redirected("https://cdn.example.org/signed").has_header("Authorization"))

    def test_same_host_keeps_the_token(self):
        self.assertTrue(self.redirected("https://huggingface.co/other").has_header("Authorization"))


class Fp16CopyTest(unittest.TestCase):
    """Decision 68: Setup's fp16 step, with the probe and the conversion faked."""

    def setUp(self) -> None:
        from unittest import mock

        self.mock = mock
        self.tmp = tempfile.TemporaryDirectory()
        self.home = os.environ.get(HOME_VAR)
        os.environ[HOME_VAR] = self.tmp.name
        self.models = os.path.join(self.tmp.name, "models")
        self.src = os.path.join(self.models, "diffusion_models", "t-bf16.safetensors")
        self.dst = self.src.replace("-bf16", "-fp16")
        # Never the real settings of whoever runs the tests.
        self.choices = setup_helper.FP16_CHOICE_FILES
        setup_helper.FP16_CHOICE_FILES = ()
        self.addCleanup(setattr, setup_helper, "FP16_CHOICE_FILES", self.choices)
        os.makedirs(os.path.dirname(self.src))
        with open(self.src, "wb") as f:
            f.write(DATA)
        self.man = {"files": [{"key": "transformer", "path": "diffusion_models/t-bf16.safetensors",
                               "size": len(DATA), "sha256": "a" * 64}]}
        self.prog = setup_helper.Progress(False)
        self.converts = 0

    def tearDown(self) -> None:
        if self.home is not None:
            os.environ[HOME_VAR] = self.home
        self.tmp.cleanup()

    def fake_run(self, cmd, *args, **kwargs):  # noqa: ANN001, ANN002, ANN003
        self.converts += 1
        with open(cmd[-1], "wb") as f:
            f.write(b"fp16")

    def go(self, probe_out: str) -> str:
        done = setup_helper.subprocess.CompletedProcess([], 0, stdout=probe_out)
        with self.mock.patch.object(setup_helper.subprocess, "run", return_value=done), \
                self.mock.patch.object(setup_helper, "run", side_effect=self.fake_run):
            return setup_helper.make_fp16_copy("py", self.man, self.models, self.prog)

    def test_an_earlier_choice_holds(self):
        choice = os.path.join(os.path.dirname(self.src), "other.conf")
        setup_helper.FP16_CHOICE_FILES = (os.path.join(os.path.dirname(self.src), "missing.conf"), choice)
        with open(choice, "w") as f:
            f.write("model_dir=x\ntransformer_fp16=0\n")
        self.assertEqual(self.go("fp16_probe 80.0 50.0\n"), "0")  # faster, but already decided
        self.assertFalse(os.path.exists(self.dst))

    def test_a_borderline_result_makes_no_copy(self):
        self.assertEqual(self.go("fp16_probe 2.9 2.4\n"), "0")  # 1.21x, as an M5 Max can measure

    def test_faster_makes_copy_once(self):
        self.assertEqual(self.go("fp16_probe 80.0 50.0\n"), "1")
        self.assertTrue(os.path.exists(self.dst) and os.path.exists(self.dst + ".verified"))
        self.assertEqual(self.go("fp16_probe 80.0 50.0\n"), "1")  # re-run keeps the verified copy
        self.assertEqual(self.converts, 1)

    def test_not_faster_no_copy(self):
        self.assertEqual(self.go("fp16_probe 50.0 48.0\n"), "0")
        self.assertFalse(os.path.exists(self.dst))

    def test_probe_failure_is_not_fatal(self):
        self.assertEqual(self.go("Traceback: no MPS\n"), "0")
