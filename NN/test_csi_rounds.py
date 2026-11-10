"""Validate raw component order, grouping integrity, and time separation."""
import csv
import tempfile
import unittest
from pathlib import Path

import numpy as np

from csi_rounds import (FEATURE_NAMES, LINKS, PAIRS, SOURCE_ROW_FIELDS,
                        audit_partitions, load_rounds, partition)


class CompleteRoundTests(unittest.TestCase):
    def packets(self, sequence=1):
        rows = []
        for index, link in enumerate(LINKS):
            tx, rx = link.split('->')
            row = {'host_time_utc': f'2026-10-30T12:00:00.{index * 1000:06d}+00:00',
                   'label': 'absent', 'type': 'CSI', 'tx_board': tx, 'rx_board': rx,
                   'session': 'session1', 'sequence': sequence, 'rx_boot': f'boot{rx}',
                   'first_word_invalid': 0, 'len': 128, 'channel': 6}
            for k in range(64):
                row[f'imag_{k:02d}'] = -k
                row[f'real_{k:02d}'] = k + index
            rows.append(row)
        return rows

    def load(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.csv'
            with path.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            return load_rounds(path, 'absent')

    def test_preserves_52_raw_pairs_excluding_dc_and_guards(self):
        complete, summary = self.load(list(reversed(self.packets())))
        self.assertEqual(len(complete), 1)
        self.assertEqual(PAIRS, tuple(range(1, 27)) + tuple(range(38, 64)))
        self.assertEqual(len(FEATURE_NAMES), 624)
        self.assertFalse(any(name.endswith('_00') for name in FEATURE_NAMES))
        self.assertTrue(set(PAIRS).isdisjoint({0, *range(27, 38)}))
        values = np.array(complete[0][1]).reshape(6, 52, 2)
        for link_index in range(6):
            np.testing.assert_array_equal(values[link_index, :, 0], -np.array(PAIRS))
            np.testing.assert_array_equal(values[link_index, :, 1], np.array(PAIRS) + link_index)
        self.assertEqual(summary['packets_in_complete_rounds'], 6)

    def test_rejects_incomplete_and_duplicate_groups(self):
        rows = self.packets(1) + self.packets(2)[:-1] + self.packets(3)
        rows.append(dict(self.packets(3)[0]))
        complete, summary = self.load(rows)
        self.assertEqual([item[0]['sequence'] for item in complete], [1])
        self.assertEqual(summary['rejected_groups'], {'missing_direction': 1, 'duplicate_direction': 1})

    def test_rejects_invalid_group_instead_of_filling_missing_values(self):
        rows = self.packets(1) + self.packets(2)
        rows[-1]['first_word_invalid'] = 1
        complete, summary = self.load(rows)
        self.assertEqual(len(complete), 1)
        self.assertEqual(summary['rejected_groups']['invalid_csi'], 1)

    def test_session_is_part_of_group_identity(self):
        first, second = self.packets(1), self.packets(1)
        for row in second:
            row['session'] = 'session2'
        complete, _ = self.load(first + second)
        self.assertEqual(len(complete), 2)

    def test_whole_round_must_fit_partition(self):
        self.assertEqual(partition(179, 180, 300), 'train')
        self.assertEqual(partition(179, 181, 300), 'purged')
        self.assertEqual(partition(181, 183, 300), 'purged')
        self.assertEqual(partition(182, 183, 300), 'validation')
        self.assertEqual(partition(239, 241, 300), 'purged')
        self.assertEqual(partition(242, 243, 300), 'test')

    def test_audit_rejects_reused_packet_rows(self):
        metadata = []
        for index, (split, start, end) in enumerate([
                ('train', 0, 1), ('validation', 4, 5), ('test', 8, 9)]):
            row = {'recording': 'file.csv', 'split': split,
                   'start_seconds': start, 'end_seconds': end}
            row.update({field: 6 * index + k + 2 for k, field in enumerate(SOURCE_ROW_FIELDS)})
            metadata.append(row)
        self.assertTrue(audit_partitions(metadata, 2)['no_reused_raw_packets'])
        metadata[1][SOURCE_ROW_FIELDS[0]] = metadata[0][SOURCE_ROW_FIELDS[0]]
        with self.assertRaisesRegex(ValueError, 'multiple rounds'):
            audit_partitions(metadata, 2)


if __name__ == '__main__':
    unittest.main()
