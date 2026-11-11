"""The resident worker's server and a client, with the model faked: the listening socket (a Unix
socket on the Mac, a loopback port with a secret on Windows; oslayer.listen / connect), the info
file, one job, a request for other settings ("restart"), and on Windows a caller without the secret."""

import argparse
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

import ltx_worker  # noqa: E402  (torch and the LTX packages are only imported inside Engine)
from ltxag_worker import oslayer  # noqa: E402

HOME_VAR = "USERPROFILE" if sys.platform == "win32" else "HOME"


class FakeEngine:
    def __init__(self, *args, **kwargs) -> None:  # noqa: ANN002, ANN003
        self.jobs = []

    def vram_gb(self) -> float:
        return 1.5

    def run(self, inp, chunks, out_prefix, conv, emit, seed, holds) -> None:  # noqa: ANN001
        self.jobs.append((inp, chunks))
        emit("info", "fake engine")
        emit("chunk_done", 0)

    def drop_transformer(self) -> None:
        pass


class ResidentServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        env = mock.patch.dict(os.environ, {HOME_VAR: self.tmp.name, "LTXAG_IDLE_TIMEOUT": "30"})
        env.start()
        self.addCleanup(env.stop)
        self.sock = os.path.join(self.tmp.name, "w.sock")
        args = argparse.Namespace(serve=self.sock, precision="bf16", offload="none", model_dir="M", embeddings="E",
                                  flame_pid=0, idle_timeout=30.0, rebuild_transformer=True, frames_on_gpu=False)
        engine = mock.patch.object(ltx_worker, "Engine", FakeEngine)
        engine.start()
        self.addCleanup(engine.stop)
        self.server = ltx_worker.Server(args)
        self.thread = threading.Thread(target=self.server.serve, daemon=True)
        self.thread.start()
        deadline = time.time() + 10
        while self.info().get("state") != "ready" and time.time() < deadline:
            time.sleep(0.05)
        self.assertEqual(self.info().get("state"), "ready")

    def info(self) -> dict:
        try:
            with open(self.sock + ".json") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def ask(self, request: dict, with_secret: bool = True) -> list:
        info = self.info()
        conn = oslayer.connect(self.sock, info)
        if with_secret:
            request = dict(request, secret=info.get("secret", ""))
        conn.sendall((json.dumps(request) + "\n").encode())
        with conn, conn.makefile("rb") as f:
            return [line.decode().rstrip("\n") for line in f]

    def request(self, **changes) -> dict:  # noqa: ANN003
        return dict({"job_id": "j", "in": "ltxag_x", "chunks": [[0, 3]], "out_prefix": "o", "precision": "bf16",
                     "model_dir": "M", "offload": "none", "unload": False}, **changes)

    def test_job_then_restart_for_other_settings(self):
        info = self.info()
        self.assertEqual((info["pid"], info["offload"], info["vram_gb"]), (os.getpid(), "none", 1.5))
        self.assertEqual(self.ask({"ping": True}), ["pong bf16 M"])
        if sys.platform == "win32":  # any local program can reach a port: without the secret, nothing
            self.assertEqual(self.ask(self.request(), with_secret=False), [])
            self.assertEqual(self.server.engine.jobs, [])
        self.assertEqual(self.ask(self.request()), ["info fake engine", "chunk_done 0", "done"])
        self.assertEqual(self.server.engine.jobs, [("ltxag_x", [(0, 3)])])
        # Windows port: weights kept elsewhere is another worker, like another precision.
        self.assertEqual(self.ask(self.request(offload="cpu")), ["restart"])
        self.thread.join(timeout=10)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.info(), {}, "the info file goes with the server")


if __name__ == "__main__":
    unittest.main()
