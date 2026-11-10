"""Independently compare every saved input component with its original CSV row."""
import csv
import hashlib
import json
from pathlib import Path

import numpy as np


def main():
    output = Path('results')
    report = json.loads((output / 'metrics.json').read_text())
    schema = json.loads((output / 'input_schema.json').read_text())
    data = np.load(output / 'round_features.npz')
    x, y, splits = data['x'], data['y'], data['splits']
    with (output / 'round_splits.csv').open(newline='') as stream:
        rounds = list(csv.DictReader(stream))
    keep = list(range(1, 27)) + list(range(38, 64))
    excluded = [0] + list(range(27, 38))
    assert schema['kept_pair_indices'] == keep
    assert schema['excluded_pair_indices'] == excluded
    assert x.shape == (len(rounds), 624)
    references = {}
    for index, row in enumerate(rounds):
        assert row['split'] == splits[index]
        assert row['label'] == ['absent', 'moving'][int(y[index])]
        for link_index, link in enumerate(schema['links']):
            key = (row['recording'], int(row['source_row_' + link.replace('->', '_to_')]))
            assert key not in references, 'Raw packet reused'
            references[key] = (index, link_index)
    inspected, raw_count, nonzero_excluded = 0, 0, 0
    for source in report['recordings']:
        path = Path(source['file'])
        assert hashlib.sha256(path.read_bytes()).hexdigest() == source['sha256']
        with path.open(newline='') as stream:
            for line, raw in enumerate(csv.DictReader(stream), start=2):
                raw_count += 1
                nonzero_excluded += sum(int(raw[f'{component}_{k:02d}']) != 0
                                        for k in excluded for component in ['imag', 'real'])
                key = (path.name, line)
                if key not in references:
                    continue
                index, link_index = references[key]
                info = rounds[index]
                assert raw['session'] == info['session']
                assert int(raw['sequence']) == int(info['sequence'])
                assert raw['tx_board'] + '->' + raw['rx_board'] == schema['links'][link_index]
                assert raw['label'] == ('absent' if y[index] == 0 else 'present_moving')
                original = np.array([int(raw[f'{component}_{k:02d}'])
                                     for k in keep for component in ['imag', 'real']], dtype=np.float32)
                np.testing.assert_array_equal(x[index, link_index * 104:(link_index + 1) * 104], original)
                inspected += 1
    assert inspected == len(references) == len(rounds) * 6
    assert nonzero_excluded == 0
    result = {
        'verified': True, 'raw_rows_read': raw_count, 'raw_packets_in_complete_rounds': inspected,
        'complete_rounds': len(rounds), 'raw_feature_count': x.shape[1],
        'every_component_matches_original_signed_value_exactly': True,
        'correct_session_sequence_and_direction_membership': True,
        'no_packet_reuse': True, 'excluded_pairs': excluded,
        'excluded_components_nonzero_count_in_raw_files': nonzero_excluded,
        'source_hashes_match_training_run': True,
        'complete_packet_fraction': inspected / raw_count,
        'packet_fraction_used_after_purging': int((splits != 'purged').sum()) * 6 / raw_count,
    }
    (output / 'raw_verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
