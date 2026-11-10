import unittest
from types import SimpleNamespace

from pose_coordinates import pose_rows
from run_pose import timestamp_ms


class CoordinateTests(unittest.TestCase):
    def landmarks(self, width, height, scale=1, dx=0, dy=0):
        pixels = [(400, 300)] * 33
        pixels[11], pixels[12] = (350, 200), (450, 200)
        pixels[23], pixels[24] = (350, 400), (450, 400)
        pixels[15] = (600, 200)
        return [SimpleNamespace(x=(x * scale + dx) / width, y=(y * scale + dy) / height,
                                visibility=1., presence=1.) for x, y in pixels]

    def test_relative_coordinates_use_pixels_and_y_up(self):
        rows, info = pose_rows(self.landmarks(1920, 1080), 1920, 1080)
        self.assertEqual(info['hip_center_pixels'], (400, 400))
        self.assertAlmostEqual(info['torso_length_pixels'], 200)
        self.assertAlmostEqual(rows[15]['x_relative'], 1)
        self.assertAlmostEqual(rows[15]['y_relative'], 1)

    def test_translation_and_uniform_scale_do_not_change_relative_coordinates(self):
        first, _ = pose_rows(self.landmarks(1920, 1080), 1920, 1080)
        second, _ = pose_rows(self.landmarks(1920, 1080, 1.5, 100, -50), 1920, 1080)
        for a, b in zip(first, second):
            self.assertAlmostEqual(a['x_relative'], b['x_relative'])
            self.assertAlmostEqual(a['y_relative'], b['y_relative'])

    def test_unreliable_anchor_disables_relative_coordinates(self):
        landmarks = self.landmarks(1920, 1080)
        landmarks[23].visibility = .1
        rows, info = pose_rows(landmarks, 1920, 1080)
        self.assertFalse(info['relative_available'])
        self.assertTrue(all(row['x_relative'] == '' for row in rows))

    def test_unreliable_joint_does_not_get_relative_values(self):
        landmarks = self.landmarks(1920, 1080)
        landmarks[15].presence = .1
        rows, info = pose_rows(landmarks, 1920, 1080)
        self.assertTrue(info['relative_available'])
        self.assertEqual(rows[15]['relative_valid'], 0)
        self.assertEqual(rows[15]['x_relative'], '')

    def test_video_timestamps_use_source_clock_with_fallback(self):
        self.assertEqual(timestamp_ms(0, 0, 30, -1), 0)
        self.assertEqual(timestamp_ms(1, 42, 30, 0), 42)
        self.assertEqual(timestamp_ms(2, 0, 30, 42), 67)
        with self.assertRaises(ValueError):
            timestamp_ms(1, 0, 0, 0)


if __name__ == '__main__':
    unittest.main()
