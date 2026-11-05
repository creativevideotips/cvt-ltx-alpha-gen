"""Flow-warped dissolve (ltxag_worker/morph.py)."""

import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from ltxag_worker import morph  # noqa: E402


def disc(h, w, cx, r, soft=3.0):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((x - cx) ** 2 + (y - h / 2) ** 2)
    return np.clip((r - d) / soft + 0.5, 0, 1).astype(np.float32)


class MorphTest(unittest.TestCase):
    def test_endpoints_are_exact(self):
        a, b = disc(135, 240, 110, 30), disc(135, 240, 120, 30)
        np.testing.assert_allclose(morph.morph(a, b, 0.0), a, atol=1e-6)
        np.testing.assert_allclose(morph.morph(a, b, 1.0), b, atol=1e-6)

    def test_middle_is_much_closer_than_a_dissolve(self):
        for dx in (6, 24):
            a, b = disc(270, 480, 200, 60), disc(270, 480, 200 + dx, 60)
            truth = disc(270, 480, 200 + dx / 2, 60)
            got = np.abs(morph.morph(a, b, 0.5) - truth).mean()
            dissolve = np.abs(0.5 * (a + b) - truth).mean()
            self.assertLess(got, 0.25 * dissolve, f"dx={dx}")

    def test_agreed_detail_is_left_alone(self):
        # cvt1.30: fine texture both mattes share, far from the moving edge, is not resampled.
        rng = np.random.default_rng(1)
        tex = (rng.random((270, 480)) > 0.5).astype(np.float32) * 0.6
        a, b = disc(270, 480, 120, 40), disc(270, 480, 150, 40)
        a[:, 360:], b[:, 360:] = tex[:, 360:], tex[:, 360:]
        out = morph.morph(a, b, 0.5)
        np.testing.assert_allclose(out[:, 420:], tex[:, 420:], atol=1e-6)

    def test_seam_ends_are_close_to_a_dissolve(self):
        # cvt1.30: near the ends of a seam the warp fades out (sin(pi t)).
        a, b = disc(270, 480, 200, 60), disc(270, 480, 224, 60)
        for t in (0.03, 0.97):
            dissolve = (1 - t) * a + t * b
            self.assertLess(np.abs(morph.morph(a, b, t) - dissolve).max(), 0.12, f"t={t}")
        st = {}
        morph.morph(a, b, 0.5, st)
        self.assertTrue(st["warped"] and st["strength"] > 0.99 and 0 < st["area"] < 0.5, st)

    def test_identical_mattes_pass_through(self):
        a = disc(64, 64, 32, 10)
        np.testing.assert_allclose(morph.morph(a, a.copy(), 0.3), a, atol=1e-6)

    def test_output_stays_in_range_for_unrelated_mattes(self):
        rng = np.random.default_rng(0)
        a, b = rng.random((50, 70)).astype(np.float32), rng.random((50, 70)).astype(np.float32)
        out = morph.morph(a, b, 0.5)
        self.assertEqual(out.shape, a.shape)
        self.assertTrue(np.isfinite(out).all() and out.min() >= 0 and out.max() <= 1)


if __name__ == "__main__":
    unittest.main()
