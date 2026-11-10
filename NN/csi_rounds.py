"""Build raw, complete CSI rounds without time averaging or amplitude conversion."""
from __future__ import annotations

import csv
import hashlib
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

import numpy as np

LINKS = ('A->B', 'A->C', 'B->A', 'B->C', 'C->A', 'C->B')
PAIRS = tuple(range(1, 27)) + tuple(range(38, 64))
EXCLUDED_PAIRS = (0,) + tuple(range(27, 38))  # DC and unused LLTF guard bins.
SUBCARRIER_SOURCE = 'https://github.com/espressif/esp-csi/issues/45'
CLASSES = ('absent', 'moving')
COMPONENTS = ('imag', 'real')
FEATURE_NAMES = tuple(f'{link}:{component}_{pair:02d}'
                      for link in LINKS for pair in PAIRS for component in COMPONENTS)
SOURCE_ROW_FIELDS = tuple('source_row_' + link.replace('->', '_to_') for link in LINKS)


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def partition(start, end, duration, gap=2.0):
    """Assign a whole round using all its timestamps, never split its packets."""
    if end <= duration * 0.6:
        return 'train'
    if start >= duration * 0.6 + gap and end <= duration * 0.8:
        return 'validation'
    if start >= duration * 0.8 + gap and end <= duration:
        return 'test'
    return 'purged'


def load_rounds(path, expected_label):
    path = Path(path)
    digest = file_hash(path)
    groups = defaultdict(list)
    times = []
    row_count = 0
    with path.open(newline='', encoding='utf-8-sig') as stream:
        for row_number, row in enumerate(csv.DictReader(stream), start=2):
            row_count += 1
            if row['label'] != expected_label or row['type'] != 'CSI':
                raise ValueError(f'{path}:{row_number}: unexpected label or record type')
            link = row['tx_board'] + '->' + row['rx_board']
            if link not in LINKS:
                raise ValueError(f'{path}:{row_number}: unexpected direction {link}')
            timestamp = datetime.fromisoformat(row['host_time_utc']).timestamp()
            values = [int(row[f'{component}_{pair:02d}'])
                      for pair in PAIRS for component in COMPONENTS]
            if any(value < -128 or value > 127 for value in values):
                raise ValueError(f'{path}:{row_number}: CSI outside signed-byte range')
            packet = {'link': link, 'time': timestamp, 'row': row_number,
                      'values': values, 'rx': row['rx_board'], 'boot': row['rx_boot'],
                      'invalid': bool(int(row['first_word_invalid'])),
                      'length': int(row['len']), 'channel': row['channel']}
            groups[(row['session'], int(row['sequence']))].append(packet)
            times.append(timestamp)
    if not times:
        raise ValueError(f'{path}: empty recording')
    origin, end = min(times), max(times)
    complete, rejected = [], Counter()
    for (session, sequence), packets in groups.items():
        directions = Counter(packet['link'] for packet in packets)
        if any(count > 1 for count in directions.values()):
            rejected['duplicate_direction'] += 1
            continue
        if set(directions) != set(LINKS):
            rejected['missing_direction'] += 1
            continue
        if any(packet['invalid'] or packet['length'] != 128 for packet in packets):
            rejected['invalid_csi'] += 1
            continue
        if len({packet['channel'] for packet in packets}) != 1:
            rejected['mixed_channels'] += 1
            continue
        boots = defaultdict(set)
        for packet in packets:
            boots[packet['rx']].add(packet['boot'])
        if any(len(values) > 1 for values in boots.values()):
            rejected['receiver_reboot_inside_round'] += 1
            continue
        by_link = {packet['link']: packet for packet in packets}
        first = min(packet['time'] for packet in packets) - origin
        last = max(packet['time'] for packet in packets) - origin
        metadata = {'recording': path.name, 'session': session, 'sequence': sequence,
                    'start_seconds': first, 'end_seconds': last,
                    'label': 'absent' if expected_label == 'absent' else 'moving'}
        metadata.update({field: by_link[link]['row']
                         for field, link in zip(SOURCE_ROW_FIELDS, LINKS)})
        vector = [value for link in LINKS for value in by_link[link]['values']]
        complete.append((metadata, vector))
    complete.sort(key=lambda item: (item[0]['start_seconds'], item[0]['sequence']))
    if not complete:
        raise ValueError(f'{path}: no complete valid six-direction rounds')
    summary = {'file': path.name, 'sha256': digest, 'raw_packet_rows': row_count,
               'duration_seconds': end - origin, 'sequence_groups': len(groups),
               'complete_rounds': len(complete), 'packets_in_complete_rounds': 6 * len(complete),
               'complete_packet_fraction': 6 * len(complete) / row_count,
               'rejected_groups': dict(rejected)}
    return complete, summary


def build_dataset(absent, moving, gap=2.0):
    vectors, targets, metadata, recordings = [], [], [], []
    for target, (path, label) in enumerate([(absent, 'absent'), (moving, 'present_moving')]):
        complete, summary = load_rounds(path, label)
        recordings.append(summary)
        for row, vector in complete:
            row['split'] = partition(row['start_seconds'], row['end_seconds'],
                                     summary['duration_seconds'], gap)
            row['round_id'] = len(metadata)
            metadata.append(row)
            vectors.append(vector)
            targets.append(target)
    x = np.array(vectors, dtype=np.float32)
    y = np.array(targets, dtype=np.int8)
    splits = np.array([row['split'] for row in metadata])
    audit = audit_partitions(metadata, gap)
    return x, y, splits, metadata, recordings, audit


def audit_partitions(metadata, gap):
    seen = set()
    per_file = defaultdict(lambda: defaultdict(list))
    for row in metadata:
        for field in SOURCE_ROW_FIELDS:
            identity = (row['recording'], row[field])
            if identity in seen:
                raise ValueError('Raw packet appears in multiple rounds')
            seen.add(identity)
        per_file[row['recording']][row['split']].append(row)
    gaps = {}
    for recording, parts in per_file.items():
        for left, right in [('train', 'validation'), ('validation', 'test')]:
            if not parts[left] or not parts[right]:
                raise ValueError(f'{recording}: empty {left} or {right} partition')
            actual = min(row['start_seconds'] for row in parts[right]) - max(
                row['end_seconds'] for row in parts[left])
            if actual < gap:
                raise ValueError('Chronological partitions violate requested gap')
            gaps[f'{recording}:{left}->{right}'] = actual
    return {'no_reused_raw_packets': True, 'raw_rows_in_complete_rounds': len(seen),
            'minimum_partition_gap_seconds': gap, 'observed_gaps_seconds': gaps}
