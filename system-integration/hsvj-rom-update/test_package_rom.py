import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import package_rom as rom


class ImageValidationTests(unittest.TestCase):
    def fixture(self, directory):
        # A compact RKFW/RKAF with the actual production partition offsets.
        names = ["parameter", *rom.PARTITIONS]
        rkaf = bytearray(140 + 112 * len(names))
        rkaf[:4] = b"RKAF"
        struct.pack_into("<I", rkaf, 136, len(names))
        offset = len(rkaf)
        for i, name in enumerate(names):
            filename = "Image/parameter.txt" if name == "parameter" else "Image/" + name + ".img"
            flash = 0 if name == "parameter" else rom.PARTITIONS[name][0]
            struct.pack_into("<32s60sIIIII", rkaf, 140 + i * 112, name.encode(), filename.encode(), 0, offset, flash, 0, 16)
            rkaf.extend(b"PARM" + struct.pack("<I", 4) + b"map!" + bytes(4) if name == "parameter" else bytes([i]) * 16)
            offset += 16
        header = bytearray(102)
        header[:4] = b"RKFW"
        struct.pack_into("<H", header, 4, 102)
        struct.pack_into("<II", header, 33, 102, len(rkaf))
        path = Path(directory) / "update.img"
        path.write_bytes(header + rkaf)
        return path

    def test_ranges_and_partition_allowlist(self):
        with tempfile.TemporaryDirectory() as directory:
            image = self.fixture(directory)
            parameter_hash = rom.digest(image, 102 + 140 + 112 * 9 + 8, 4)
            with patch.object(rom, "PARAMETER_SHA256", parameter_hash):
                self.assertEqual(len(rom.parse_image(image)), 9)
                original = image.read_bytes()
                for offset, value in [(102 + 140 + 112, b"huoshan\0"),
                                      (102 + 140 + 112 + 96, struct.pack("<I", 0)),
                                      (102 + 140 + 112 + 100, struct.pack("<I", 0xc37000)),
                                      (33, struct.pack("<I", 0xfffffff0))]:
                    damaged = bytearray(original)
                    damaged[offset:offset + len(value)] = value
                    image.write_bytes(damaged)
                    with self.assertRaises(ValueError):
                        rom.parse_image(image)

    @unittest.skipUnless(os.environ.get("HSVJ_TEST_IMAGE"), "optional real build input")
    def test_real_production_image(self):
        entries = rom.parse_image(Path(os.environ["HSVJ_TEST_IMAGE"]))
        self.assertEqual(set(entries), {"parameter", *rom.PARTITIONS})


if __name__ == "__main__":
    unittest.main()
