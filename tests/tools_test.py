#!/usr/bin/env python3
"""Host tests of the tools: the TOML reader, the board files' checks, what `gen` writes.

They run against the stand-in phone of tests/devices and the made-up chip of tests/soc, whose addresses
are no real phone's, so a test cannot pass by a value being hard-wired somewhere.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import minitoml  # noqa: E402

BOARDS = [sys.executable, str(ROOT / 'tools/boards.py'), '--alone', '--devices', str(ROOT / 'tests/devices'),
          '--soc-dir', str(ROOT / 'tests/soc')]


def boards(*arguments, devices=None):
    command = list(BOARDS)
    if devices is not None:
        command[4] = str(devices)
    return subprocess.run(command + list(arguments), capture_output=True, text=True)


class TOML(unittest.TestCase):
    def test_every_file_of_the_repository(self):
        files = [p for p in ROOT.rglob('*.toml') if 'build' not in p.parts and '.git' not in p.parts]
        self.assertGreater(len(files), 3)
        for path in files:
            own = minitoml.loads_own(path.read_text())
            self.assertIsInstance(own, dict, path)
            try:
                import tomllib
            except ImportError:
                continue
            self.assertEqual(own, tomllib.loads(path.read_text()), path)

    def test_values(self):
        data = minitoml.loads_own('a = 0x10 # c\nb = "x\\ty"\nc = [1, 2,\n 3]\n[t]\nd = true\n[[r]]\ne = \'q\'\n[[r]]\ne = "w"\n')
        self.assertEqual(data, {'a': 16, 'b': 'x\ty', 'c': [1, 2, 3], 't': {'d': True}, 'r': [{'e': 'q'}, {'e': 'w'}]})

    def test_refuses_what_it_does_not_read(self):
        for text in ('a = 1.5\n', 'a = {b = 1}\n', 'a.b = 1\n', 'a = """x"""\n', 'a = 1\na = 2\n', 'a = [1\n'):
            with self.assertRaises(ValueError, msg=text):
                minitoml.loads_own(text)


class Boards(unittest.TestCase):
    def test_the_stand_in_is_consistent(self):
        done = boards('check')
        self.assertEqual(done.returncode, 0, done.stderr)

    def changed(self, old, new):
        """A copy of the stand-in's package with one piece of text replaced; the check's answer."""
        with tempfile.TemporaryDirectory() as directory:
            shutil.copytree(ROOT / 'tests/devices/phone-a', Path(directory) / 'phone-a')
            path = Path(directory) / 'phone-a/board.toml'
            text = path.read_text()
            self.assertIn(old, text)
            path.write_text(text.replace(old, new))
            return boards('check', devices=directory)

    def test_refusals(self):
        for old, new, why in (
                ('"touch-td4160"]', '"touch-td4160", "no-such-part"]', 'knows no "no-such-part"'),
                ('[battery]\ndesign_uah = 4000000\n', '', 'needs [battery]'),
                ('core_order = [0, 6, 7, 1, 2, 3, 4, 5]', 'core_order = [0, 6, 7, 1, 2, 3, 4]', 'every core of the chip once'),
                ('core_order = [0, 6, 7, 1, 2, 3, 4, 5]', 'core_order = [6, 0, 7, 1, 2, 3, 4, 5]', 'begins with the core'),
                ('marker = 0x81000000', 'marker = 0xd0000000', 'overlap'),
                ('fb_pitch = 4352', 'fb_pitch = 1080', 'four a pixel'),
                ('soc = "mt0001"', 'soc = "mt9999"', 'there is no soc/mt9999.toml'),
                ('cpus = 4', 'cpus = 8', 'cpus: 1 to 7'),
                ('bootargs = ["touchscreen=2"]', 'bootargs = ["touch screen"]', 'not NAME or NAME=VALUE'),
                ('pmsg_size = 0x10000', 'pmsg_size = "big"', 'a number of 32 bits'),
                ('width = 1080', 'width = 1080\nwidht = 1', 'unknown key "widht"')):
            done = self.changed(old, new)
            self.assertNotEqual(done.returncode, 0, new)
            self.assertIn(why, done.stderr, new)

    def test_generated(self):
        with tempfile.TemporaryDirectory() as out:
            done = boards('gen', '--out', out)
            self.assertEqual(done.returncode, 0, done.stderr)
            table = (Path(out) / 'hakios_board_table.h').read_text()
            header = (Path(out) / 'phone-a/hakios_platform.h').read_text()
            again = boards('gen', '--out', out)
            self.assertIn('nothing new', again.stdout)
        self.assertIn('.name = "phone-a"', table)
        self.assertIn('.features = HAKIOS_HAS_EMMC_MSDC | HAKIOS_HAS_BATTERY_MT6358 | HAKIOS_HAS_TOUCH_TD4160,', table)
        self.assertIn('.system_partition = "sysa",', table)
        self.assertIn('.msdc0 = 0x11330000ULL,', table)            # (the made-up chip's, not the MT6768's 0x11230000)
        self.assertIn('.design_uah = 4000000u,', table)
        self.assertNotIn('.connsys', table)
        self.assertNotIn('x6826b', table)
        for line in ('#define HAKIOS_BOARD_NAME               "phone-a"', 'HAKIOS_LOADER_KERNEL_LOAD           0x41080000ULL',
                     'HAKIOS_LOADER_CORE_ORDER            { 0, 6, 7, 1, 2, 3, 4, 5 }', 'HAKIOS_HAS_TOUCH_TD4160             1',
                     'HAKIOS_HAS_CONNSYS_MT6631           0', 'HAKIOS_RAMOOPS_PMSG_SIZE            0x10000u',
                     'HAKIOS_DISPLAY_FB_PITCH             4352u', 'HAKIOS_SOC_FIRST_BIG_CORE           6'):
            self.assertIn(line, header)
        self.assertNotIn('HAKIOS_LOADER_BIG_CORES_CLOCK', header)

    def test_status_and_get(self):
        status = boards('status', 'phone-a').stdout
        self.assertIn('emmc-msdc          on', status)
        self.assertIn('connsys-mt6631     needs [connsys] in the board file', status)
        self.assertEqual(boards('get', 'phone-a', 'boot.android_device').stdout.strip(), 'stand-in-a')
        self.assertEqual(boards('get', 'phone-a', 'loader.kernel_load').stdout.strip(), '0x41080000')
        self.assertEqual(boards('which').stdout.strip(), 'phone-a')

    def test_the_template_explains_every_key(self):
        import boards as tool
        board = (ROOT / 'devices/_template/board.toml').read_text()
        for section, keys in tool.BOARD_SCHEMA.items():
            for key in keys:
                self.assertRegex(board, r'(?m)^#? ?%s = ' % key, f'devices/_template/board.toml does not explain {section or "top"}.{key}')
        soc = (ROOT / 'devices/_template/soc.toml').read_text()
        for section in ('platform', 'cpufreq'):
            for key in tool.SOC_SCHEMA[section]:
                self.assertRegex(soc, r'(?m)^#? ?%s = ' % key, f'devices/_template/soc.toml does not explain {section}.{key}')
        for key in tool.SOC_SCHEMA['blocks']:
            self.assertIn(key, soc)


if __name__ == '__main__':
    unittest.main()
