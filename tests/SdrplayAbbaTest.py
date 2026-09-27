#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plan validation and metric parsing; never contacts a receiver or systemd."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(pathlib.Path(__file__).parents[1]/'scripts'))
import sdrplay_abba as abba
from sdrplay_capture_set import copy_durable

class TestAbba(unittest.TestCase):
    def test_rate_output_pairs(self):
        self.assertEqual(abba.normalize({'rate':9})['output'],12)
        self.assertEqual(abba.normalize({'rate':4})['if_gr'],40)
        for settings in ({'rate':2,'output':12},{'rate':10,'output':8},{'rate':4,'if_gr':19},
                         {'rate':4,'lna':9},{'rate':4,'if_gr':30.5}):
            with self.assertRaises(ValueError):abba.normalize(settings)

    def test_metrics_labels_and_exponents(self):
        result=abba.metric_values('# comment\nstream1090_messages_total{df="17"} 1.2e3\nstream1090_device_up 1\n')
        self.assertEqual(result,{'stream1090_messages_total{df="17"}':1200.,'stream1090_device_up':1.})

    def test_capture_copy_never_overwrites(self):
        with tempfile.TemporaryDirectory() as folder:
            source=pathlib.Path(folder)/'source';destination=pathlib.Path(folder)/'destination'
            source.write_bytes(bytes(range(256))*4096)
            copy_durable(source,destination)
            self.assertEqual(source.read_bytes(),destination.read_bytes())
            source.write_bytes(b'changed')
            with self.assertRaises(FileExistsError):copy_durable(source,destination)
            self.assertEqual(destination.stat().st_size,256*4096)

    def test_restore_only_our_override(self):
        with tempfile.TemporaryDirectory() as folder:
            path=pathlib.Path(folder)/'override.conf'
            with patch.object(abba,'OVERRIDE',path),patch.object(abba,'systemctl') as systemctl:
                path.write_text('unrelated configuration')
                with self.assertRaises(RuntimeError):abba.restore()
                systemctl.assert_not_called()
                path.write_text(abba.MARKER+'[Service]\n')
                abba.restore()
                self.assertFalse(path.exists())
                self.assertEqual([c.args for c in systemctl.call_args_list],[('daemon-reload',),('restart',abba.UNIT)])
                systemctl.reset_mock();abba.restore();systemctl.assert_not_called()

if __name__=='__main__':unittest.main()
