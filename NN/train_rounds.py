"""Train and evaluate classifiers on complete rounds of raw ESP32-S3 CSI."""
from __future__ import annotations

import argparse
import copy
import csv
import json
import time
from collections import Counter
from pathlib import Path

import joblib
import numpy as np
from sklearn.dummy import DummyClassifier
from sklearn.ensemble import RandomForestClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import (accuracy_score, balanced_accuracy_score,
                             classification_report, confusion_matrix, f1_score,
                             log_loss, roc_auc_score)
from sklearn.neural_network import MLPClassifier
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC
from threadpoolctl import threadpool_limits

from csi_rounds import (CLASSES, EXCLUDED_PAIRS, FEATURE_NAMES, LINKS, PAIRS,
                        SUBCARRIER_SOURCE, build_dataset, file_hash)


def evaluate(model, x, y):
    prediction = model.predict(x)
    probability = model.predict_proba(x) if hasattr(model, 'predict_proba') else None
    score = probability[:, 1] if probability is not None else model.decision_function(x)
    metrics = {
        'accuracy': float(accuracy_score(y, prediction)),
        'balanced_accuracy': float(balanced_accuracy_score(y, prediction)),
        'moving_f1': float(f1_score(y, prediction, zero_division=0)),
        'roc_auc': float(roc_auc_score(y, score)),
        'log_loss': float(log_loss(y, probability, labels=[0, 1])) if probability is not None else None,
        'score_kind': 'moving_probability' if probability is not None else 'decision_margin',
        'confusion_matrix': confusion_matrix(y, prediction, labels=[0, 1]).tolist(),
        'classification_report': classification_report(
            y, prediction, labels=[0, 1], target_names=CLASSES,
            output_dict=True, zero_division=0),
    }
    return metrics, prediction, score


def fit_neural_network(x_train, y_train, x_val, y_val, seed, epochs):
    scaler = StandardScaler().fit(x_train)
    train, validation = scaler.transform(x_train), scaler.transform(x_val)
    network = MLPClassifier(hidden_layer_sizes=(128, 64), activation='relu', solver='adam',
                            alpha=0.01, batch_size=128, learning_rate_init=0.001,
                            random_state=seed, early_stopping=False)
    best, best_loss, best_epoch, stale = None, float('inf'), 0, 0
    history = []
    for epoch in range(1, epochs + 1):
        network.partial_fit(train, y_train, classes=np.array([0, 1]))
        train_probability = network.predict_proba(train)
        val_probability = network.predict_proba(validation)
        val_loss = float(log_loss(y_val, val_probability, labels=[0, 1]))
        row = {'epoch': epoch,
               'train_log_loss': float(log_loss(y_train, train_probability, labels=[0, 1])),
               'validation_log_loss': val_loss,
               'train_accuracy': float(accuracy_score(y_train, train_probability.argmax(axis=1))),
               'validation_accuracy': float(accuracy_score(y_val, val_probability.argmax(axis=1)))}
        history.append(row)
        if val_loss < best_loss - 1e-5:
            best, best_loss, best_epoch, stale = copy.deepcopy(network), val_loss, epoch, 0
        else:
            stale += 1
        if epoch == 1 or epoch % 10 == 0:
            print(f"  Epoch {epoch}: train accuracy={row['train_accuracy']:.2%}; "
                  f"validation accuracy={row['validation_accuracy']:.2%}; "
                  f"validation loss={val_loss:.4f}", flush=True)
        if stale >= 25:
            break
    return make_pipeline(scaler, best), history, best_epoch


def save_csv(path, rows):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def export_dataset(output, x, y, splits, metadata, schema):
    np.savez_compressed(output / 'round_features.npz', x=x, y=y, splits=splits)
    save_csv(output / 'round_splits.csv', metadata)
    # Keep an inspectable, unscaled table: every input is an original signed CSI component.
    with (output / 'raw_complete_rounds.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream)
        columns = list(metadata[0])
        writer.writerow(columns + list(FEATURE_NAMES))
        for row, vector in zip(metadata, x):
            writer.writerow([row[column] for column in columns] + vector.astype(int).tolist())
    (output / 'input_schema.json').write_text(json.dumps(schema, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--absent', type=Path, default=Path('absent_02.csv'))
    parser.add_argument('--moving', type=Path, default=Path('moving_01.csv'))
    parser.add_argument('--output', type=Path, default=Path('results'))
    parser.add_argument('--gap-seconds', type=float, default=2.0)
    parser.add_argument('--max-epochs', type=int, default=200)
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()
    if args.gap_seconds < 0 or args.max_epochs < 1:
        parser.error('Require gap-seconds >= 0 and max-epochs >= 1')
    args.output.mkdir(parents=True, exist_ok=True)
    x, y, splits, metadata, recordings, audit = build_dataset(
        args.absent, args.moving, args.gap_seconds)
    masks = {part: splits == part for part in ['train', 'validation', 'test']}
    for part, mask in masks.items():
        if set(y[mask]) != {0, 1}:
            raise ValueError(f'{part} needs both classes')
    train, validation, test = (masks[part] for part in ['train', 'validation', 'test'])
    schema = {
        'representation': 'Raw signed imaginary/real CSI components from one complete round',
        'grouping': '(recording, session, sequence); exactly one packet per direction',
        'links': LINKS, 'kept_pair_indices': PAIRS, 'excluded_pair_indices': EXCLUDED_PAIRS,
        'components_per_pair': ['imag', 'real'], 'feature_count': len(FEATURE_NAMES),
        'feature_names': FEATURE_NAMES, 'class_names': CLASSES,
        'mask_reference': SUBCARRIER_SOURCE,
        'temporal_averaging': False, 'amplitude_conversion': False,
    }
    export_dataset(args.output, x, y, splits, metadata, schema)
    print(f'{len(x)} complete rounds; {len(PAIRS)} pairs per direction; '
          f'{x.shape[1]} raw features per round.', flush=True)
    print('Split counts: ' + ', '.join(f'{part}={int(mask.sum())}'
                                     for part, mask in masks.items())
          + f", purged={int((splits == 'purged').sum())}", flush=True)
    models = {
        'majority': DummyClassifier(strategy='most_frequent'),
        'logistic': make_pipeline(StandardScaler(), LogisticRegression(C=0.1, max_iter=2000)),
        'random_forest': RandomForestClassifier(n_estimators=300, min_samples_leaf=2,
                                                random_state=args.seed, n_jobs=-1),
        'rbf_svm': make_pipeline(StandardScaler(), SVC(C=3.0, kernel='rbf', gamma='scale',
                                                       probability=False, cache_size=512)),
    }
    times = {}
    for name, model in models.items():
        print(f'Training {name}...', flush=True)
        start = time.perf_counter()
        model.fit(x[train], y[train])
        times[name] = time.perf_counter() - start
    print('Training MLP neural network (128, 64)...', flush=True)
    start = time.perf_counter()
    models['mlp'], history, best_epoch = fit_neural_network(
        x[train], y[train], x[validation], y[validation], args.seed, args.max_epochs)
    times['mlp'] = time.perf_counter() - start
    results = {name: {'validation': evaluate(model, x[validation], y[validation])[0],
                     'fit_seconds': times[name]} for name, model in models.items()}
    selected = max(results, key=lambda name: (
        results[name]['validation']['balanced_accuracy'], results[name]['validation']['roc_auc']))
    print(f'Selected {selected} using validation only; now evaluating the test partition.', flush=True)
    predictions, scores = {}, {}
    for name, model in models.items():
        results[name]['test'], predictions[name], scores[name] = evaluate(model, x[test], y[test])
        joblib.dump(model, args.output / f'{name}.joblib')
        restored = joblib.load(args.output / f'{name}.joblib')
        np.testing.assert_array_equal(restored.predict(x[test]), predictions[name])
        if name in ['logistic', 'rbf_svm', 'mlp']:
            scaler = model.steps[0][1]
            np.testing.assert_allclose(scaler.mean_, x[train].astype(np.float64).mean(axis=0), atol=1e-12)
            assert scaler.n_samples_seen_ == int(train.sum())
        print(f"{name:14s} validation accuracy={results[name]['validation']['accuracy']:.2%}; "
              f"test accuracy={results[name]['test']['accuracy']:.2%}; "
              f"test balanced accuracy={results[name]['test']['balanced_accuracy']:.2%}", flush=True)
    audit.update({'scalers_fit_training_only': True, 'saved_models_reproduce_predictions': True})
    for path, summary in zip([args.absent, args.moving], recordings):
        if file_hash(path) != summary['sha256']:
            raise ValueError('Source CSV changed during training')
    audit['source_csvs_unchanged_during_run'] = True
    joblib.dump({'model': models[selected], 'schema': schema, 'model_name': selected},
                args.output / 'best_model.joblib')
    test_rows = [dict(row) for row, keep in zip(metadata, test) if keep]
    for index, row in enumerate(test_rows):
        for name in models:
            row[f'{name}_prediction'] = CLASSES[int(predictions[name][index])]
            row[f'{name}_moving_score'] = float(scores[name][index])
    save_csv(args.output / 'test_predictions.csv', test_rows)
    report = {
        'representation': schema['representation'], 'feature_count': x.shape[1],
        'kept_pairs': PAIRS, 'excluded_pairs': EXCLUDED_PAIRS,
        'seed': args.seed, 'recordings': recordings,
        'split': {'fractions': [0.6, 0.2, 0.2], 'gap_seconds': args.gap_seconds,
                  'method': 'Chronological per recording; rounds crossing boundaries are purged'},
        'split_counts': {part: dict(Counter(CLASSES[int(v)] for v in y[splits == part]))
                         for part in ['train', 'validation', 'test', 'purged']},
        'selected_model': selected,
        'selection_rule': 'Highest validation balanced accuracy, then validation ROC AUC',
        'mlp': {'hidden_layers': [128, 64], 'best_epoch': best_epoch,
                'epochs_run': len(history), 'checkpoint_rule': 'Minimum validation log loss',
                'parameter_count': int(sum(a.size for a in models['mlp'].steps[-1][1].coefs_)
                                       + sum(a.size for a in models['mlp'].steps[-1][1].intercepts_))},
        'models': results, 'audit': audit,
        'limitation': 'Test rounds come from later parts of the same two recordings. '
                      'One recording per class confounds class with recording identity. '
                      'Temporally adjacent rounds can remain correlated.',
    }
    (args.output / 'metrics.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (args.output / 'mlp_history.json').write_text(json.dumps(history, indent=2), encoding='utf-8')
    from plot_evaluation import plot_evaluation
    plot_evaluation(args.output, report, y[test], scores, history)
    print(f'Graphs, trained models, raw rounds and metrics saved to {args.output.resolve()}', flush=True)


if __name__ == '__main__':
    with threadpool_limits(limits=2):
        main()
