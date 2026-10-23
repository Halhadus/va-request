#!/usr/bin/env python3
import unittest
from mpeg2_reference import align_sequence_height


def sequence(width=1920, height=1080):
    return b'\x00\x00\x01\xb3' + ((width << 12) | height).to_bytes(3, 'big') + b'\x13\x00\x00\x20\x00'


def extension(bits=(1 << 20) | (0x48 << 12) | (1 << 11) | (1 << 9)):
    return b'\x00\x00\x01\xb5' + bits.to_bytes(3, 'big') + b'\x00\x01\x00'


class ReferenceTests(unittest.TestCase):
    def test_repeated_headers_preserve_other_bytes(self):
        payload = b'\x00\x00\x01\x00picture\x00\x00\x01\x01slice'
        original = (sequence() + extension() + payload) * 2
        expected = (sequence(height=1088) + extension() + payload) * 2
        self.assertEqual(align_sequence_height(original, 1920, 1080), expected)

    def test_aligned_height_is_unchanged(self):
        data = sequence(720, 480) + extension()
        self.assertEqual(align_sequence_height(data, 720, 480), data)

    def test_invalid_headers_and_dimensions(self):
        for data, width, height in [(b'no header', 1920, 1080), (sequence()[:8], 1920, 1080),
                                    (sequence(), 1280, 720), (sequence(), 1920, 4095),
                                    (sequence(), 0, 1080), (sequence(), 1920, -1)]:
            with self.subTest(data=data, width=width, height=height), self.assertRaises(ValueError):
                align_sequence_height(data, width, height)

    def test_interlaced_and_extended_dimensions_are_rejected(self):
        for bits in [(1 << 20) | (0x48 << 12), (1 << 20) | (1 << 11) | (1 << 5)]:
            with self.assertRaises(ValueError):
                align_sequence_height(sequence() + extension(bits), 1920, 1080)


if __name__ == '__main__':
    unittest.main()
