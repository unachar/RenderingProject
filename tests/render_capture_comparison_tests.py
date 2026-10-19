import importlib.util
from pathlib import Path
import sys
import unittest

import numpy as np


script = Path(__file__).resolve().parents[1] / "scripts/compare-render-captures.py"
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("capture_comparison", script)
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class CaptureMetricsTests(unittest.TestCase):
    def test_identical_nonuniform_capture(self):
        pixels = np.arange(13 * 12 * 3).reshape(13, 12, 3) % 256
        metrics = comparison.rgb_metrics(pixels, pixels)
        self.assertEqual(metrics["rgbRootMeanSquareError"], 0)
        self.assertAlmostEqual(metrics["rgbSsim"], 1)
        self.assertEqual(metrics["ssimWindowSize"], 11)

    def test_constant_black_and_white(self):
        first = np.zeros((12, 12, 3))
        metrics = comparison.rgb_metrics(first, first + 255)
        self.assertEqual(metrics["rgbRootMeanSquareError"], 255)
        self.assertAlmostEqual(metrics["rgbSsim"], 0.01**2 / (1 + 0.01**2))

    def test_small_capture_and_symmetry(self):
        first = np.array([[[10, 20, 30]]])
        second = first + 10
        forward = comparison.rgb_metrics(first, second)
        backward = comparison.rgb_metrics(second, first)
        self.assertEqual(forward, backward)
        self.assertEqual(forward["rgbRootMeanSquareError"], 10)
        self.assertEqual(forward["ssimWindowSize"], 1)

    def test_local_statistics_against_direct_windows(self):
        first = (np.arange(13 * 12 * 3).reshape(13, 12, 3) % 256).astype(float)
        second = first[::-1, ::-1]
        expected = []
        c1, c2 = (0.01 * 255)**2, (0.03 * 255)**2
        for y in range(3):
            for x in range(2):
                for channel in range(3):
                    a = first[y:y + 11, x:x + 11, channel]
                    b = second[y:y + 11, x:x + 11, channel]
                    covariance = np.mean((a - a.mean()) * (b - b.mean()))
                    expected.append(((2 * a.mean() * b.mean() + c1) * (2 * covariance + c2))
                                    / ((a.mean()**2 + b.mean()**2 + c1)
                                       * (a.var() + b.var() + c2)))
        self.assertAlmostEqual(comparison.rgb_metrics(first, second)["rgbSsim"], np.mean(expected))

    def test_invalid_inputs(self):
        valid = np.zeros((2, 2, 3))
        for invalid in (np.zeros((0, 2, 3)), np.zeros((2, 2, 4)),
                        np.full((2, 2, 3), np.nan), np.full((2, 2, 3), 256)):
            with self.subTest(shape=invalid.shape):
                with self.assertRaises(ValueError):
                    comparison.rgb_metrics(valid, invalid)


if __name__ == "__main__":
    unittest.main()
