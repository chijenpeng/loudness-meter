"""Oracle/fixture tests only. Passing these does not test the product implementation."""
import math
import unittest

from reference_math import (
    OFFSET, RATES, anchors, integrated, loudness_range, order_statistic,
    section_short_term, to_lufs, to_power,
)


class ReferenceTests(unittest.TestCase):
    def test_known_filter_and_level_anchors(self):
        values = anchors()
        self.assertAlmostEqual(values['k_gain_1k_db'], 0.6977043961, places=8)
        self.assertAlmostEqual(values['mono_peak1_1k_lufs'], -3.0035955606, places=8)
        self.assertAlmostEqual(values['stereo_peak01_1k_lufs'], -19.993295604, places=8)

    def test_two_stage_gating_is_not_running_lufs_mean(self):
        self.assertAlmostEqual(integrated([1, 0.01]), -0.691, places=12)
        self.assertAlmostEqual(integrated([1, 0.01] * 3), -0.691, places=12)
        self.assertEqual(integrated([0, 0, 0]), -math.inf)
        self.assertEqual(integrated([to_power(-71)]), -math.inf)
        self.assertAlmostEqual(integrated([to_power(-69)]), -69, places=10)
        self.assertNotAlmostEqual(integrated([1, 0.01]), (to_lufs(1)+to_lufs(0.01))/2)

    def test_quantile_order_not_linear_interpolation(self):
        self.assertEqual(order_statistic([0, 10, 20, 30], 0.1), 0)
        self.assertEqual(order_statistic([0, 10, 20, 30], 0.95), 30)
        self.assertEqual(order_statistic([0, 10], 0.5), 10)
        self.assertTrue(math.isnan(order_statistic([], 0.1)))

    def test_lra_nominal_sections_and_no_file_padding(self):
        for levels, expected in [([-20, -30], 10), ([-20, -15], 5),
                                 ([-40, -20], 20), ([-50, -35, -20, -35, -50], 15)]:
            energies = section_short_term(levels)
            self.assertEqual(len(energies), len(levels) * 200 - 29)
            self.assertLessEqual(abs(loudness_range(energies) - expected), 1.0)
        self.assertTrue(math.isnan(loudness_range([0, 0])))
        self.assertAlmostEqual(loudness_range([to_power(-20)] * 20), 0, places=10)

    def test_histogram_boundary_fixture_exposes_large_error(self):
        # Exact per-bin sums alone do not fix assigning a threshold-cut bin as a whole.
        energies = [90.9991] + [1.0001] * 9
        exact = integrated(energies)
        relative = math.fsum(energies) / len(energies) / 10
        bins = {}
        for energy in energies:
            index = math.floor(to_lufs(energy) / 0.01)
            bins.setdefault(index, []).append(energy)
        selected = [e for index, items in bins.items()
                    if to_power((index + 0.5) * 0.01) > relative for e in items]
        centre_cut_estimate = to_lufs(math.fsum(selected) / len(selected))
        self.assertAlmostEqual(exact, 9.309, places=10)
        self.assertGreater(abs(centre_cut_estimate - exact), 5.0)
        self.assertTrue(all(e > relative for e in energies))

    def test_sample_grid_and_complete_window_counts(self):
        for rate in RATES:
            hop, momentary, short_term = rate // 10, rate * 4 // 10, rate * 3
            self.assertEqual(hop * 10, rate)
            self.assertEqual(momentary, hop * 4)
            self.assertEqual(short_term, hop * 30)
            for duration in [momentary - 1, momentary, momentary + hop - 1, momentary + hop]:
                count = max(0, (duration - momentary) // hop + 1)
                expected = sum(1 for end in range(momentary, duration + 1, hop))
                self.assertEqual(count, expected)

    def test_tp19_samples_and_analytical_peak_are_distinct(self):
        samples = [1.41 * math.sin(math.pi / 2 * n + math.pi / 4) for n in range(4)]
        sample_peak = max(abs(value) for value in samples)
        sample_db = 20 * math.log10(sample_peak)
        true_db = 20 * math.log10(1.41)
        self.assertLess(sample_db, 0)
        self.assertAlmostEqual(true_db, 2.9843822531, places=9)
        self.assertTrue(3 - 0.4 <= true_db <= 3 + 0.2)
        self.assertGreater(true_db - sample_db, 3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
