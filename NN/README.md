# Absent vs moving from raw, complete ESP32-S3 CSI rounds

This project starts directly from `absent_02.csv` and `moving_01.csv`. Each model
input is one complete measurement round identified by `(recording, session,
sequence)`, with exactly one valid packet for each of six directions:
`A->B`, `A->C`, `B->A`, `B->C`, `C->A`, `C->B`.

The original datasets are unchanged. Previous time-window experiments have been
removed. No previous model, statistical feature vector, or amplitude feature is
used in this pipeline.

## Input: 52 raw complex pairs per direction

For the 64-entry LLTF layout in these files:

| Array pair indices | Action | Meaning |
| --- | --- | --- |
| `00` | Remove | DC |
| `01` through `26` | Keep | Positive active subcarriers |
| `27` through `37` | Remove | Unused guard bins |
| `38` through `63` | Keep | Negative active subcarriers |

This leaves **52 complex pairs per direction**, including the pilot
subcarriers. Espressif's collaborator provides exactly these index ranges in
[its CSI selection guidance](https://github.com/espressif/esp-csi/issues/45).
The [ESP32-S3 driver documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.2/esp32s3/api-guides/wifi.html#wi-fi-channel-state-information)
documents LLTF ordering `0..31, -32..-1` and imaginary followed by real.
The dropped components are also verified to be zero throughout both CSVs.

The retained values remain their original signed integers. There is **no
averaging, amplitude calculation, phase conversion, interpolation, or filling
of missing directions**. Concatenation gives:

```text
6 directions x 52 pairs x 2 components = 624 values per round
```

Feature order is direction as listed above, then pair index in CSV order
`01..26,38..63`, then `imag,real`. `results/input_schema.json` lists all 624
names explicitly. Timestamps, sequence IDs, session IDs, file names, labels,
RSSI, and source-row references remain metadata and never become input features.

For logistic regression, SVM, and the neural network, a standardizer learns
feature means and scales from the training partition only. Random forest uses
the original unscaled components.

## Dataset coverage and split

Incomplete rounds, duplicated directions, invalid CSI, mixed channels, or a
receiver reboot inside a round cause that whole round to be rejected.

| Recording | Original packets | Complete rounds | Packets in complete rounds |
| --- | ---: | ---: | ---: |
| Absent | 29,176 | 4,844 | 29,064 |
| Moving | 29,191 | 4,847 | 29,082 |
| Total | 58,367 | 9,691 | 58,146 |

Complete rounds retain **99.62% of the raw packets**. These are measurement
rounds, not fixed-duration bins. A direction repeats about every 60 ms, with
some timing variation.

Each recording is split chronologically: first 60% of recording time for
training, next 20% for validation, final 20% for testing. A two-second gap follows
each boundary. Every timestamp in a round must fit its partition. Crossing a
boundary or falling in a gap purges that round rather than splitting it.

| Partition | Absent rounds | Moving rounds | Total |
| --- | ---: | ---: | ---: |
| Training | 2,902 | 2,904 | 5,806 |
| Validation | 936 | 937 | 1,873 |
| Test | 939 | 940 | 1,879 |
| Purged at boundaries/gaps | 67 | 66 | 133 |

After purging, the three active partitions use **98.25% of original packets**.
No raw packet is reused in another round or partition. Actual gaps exceed two
seconds and are recorded in `results/metrics.json`.

## Models and measured evaluation

All parameters were set before this test evaluation, using seed 42.

| Model | Validation accuracy | Test accuracy | Moving F1 | Test ROC AUC |
| --- | ---: | ---: | ---: | ---: |
| Majority baseline | 50.03% | 50.03% | 0.6669 | 0.5000 |
| Logistic regression | 46.72% | 48.91% | 0.4877 | 0.4904 |
| Random forest | 97.38% | **96.97%** | 0.9690 | 0.9971 |
| RBF SVM | 75.87% | 75.41% | 0.7475 | 0.8449 |
| MLP neural network | 94.66% | **94.73%** | 0.9467 | 0.9852 |

Random forest was selected using validation balanced accuracy, with validation
ROC AUC as the tie breaker. Test labels do not train models, select checkpoints,
or choose the saved model. The test set was evaluated after selection.

- Logistic regression: standardization, `C=0.1`, at most 2,000 iterations.
- Random forest: 300 trees, minimum leaf size 2, other defaults.
- RBF SVM: standardization, `C=3`, `gamma='scale'`. ROC curves use decision
  margins; no probability calibration is fitted.
- MLP: standardization, hidden layers `(128,64)`, ReLU, Adam, batch size 128,
  learning rate 0.001, L2 strength 0.01, 88,321 parameters. It trained 55 epochs
  and stopped after 25 epochs without improved validation log loss. The saved
  network is its epoch-30 checkpoint, selected on validation loss.

Random forest correctly classified **1,822 of 1,879 test rounds**: 930 absent
and 892 moving. It misclassified 9 absent rounds as moving and 48 moving rounds
as absent. The neural network correctly classified **1,780 of 1,879 rounds**.

Raw signed components perform differently from averaged amplitude summaries.
These scores are an evaluation of this particular representation and these
fixed model configurations, not a claim that any model family is optimal.

![Validation and held-out test accuracy](results/model_comparison.png)

![Held-out confusion matrices](results/confusion_matrices.png)

![Held-out ROC curves](results/roc_curves.png)

![Neural network learning curves](results/neural_network_learning_curve.png)

The two CSVs are still one recording per class. The test rounds are later
segments of those same recordings; class remains confounded with recording
identity, and adjacent rounds can be correlated. These scores establish
performance on this split, not accuracy on independent sessions or a stationary
occupant. No additional recording is needed to reproduce this evaluation.

## Run and verify

The existing `.venv` contains the required packages. From this folder:

```powershell
.\.venv\Scripts\python.exe train_rounds.py
.\.venv\Scripts\python.exe -m unittest -v test_csi_rounds
.\.venv\Scripts\python.exe verify_rounds.py
```

For a fresh installation with Python 3.11:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe train_rounds.py
```

Training replaces files in `results/`. To keep another run separately, use
`--output results/another_run`. Other options are `--absent`, `--moving`,
`--gap-seconds`, `--max-epochs`, and `--seed`. `verify_rounds.py` checks the
default `results/` folder.

Six tests check the exact mask and raw component order, missing/duplicated
directions, invalid CSI, session identity, boundary purging, and packet reuse.
The separate verifier streams the original CSVs and independently checks every
saved raw component and its session/sequence/direction membership. All 58,146
packet references matched their original values exactly. Source-file hashes,
training-only standardizers, and saved-model predictions were also checked.

## Saved files

| File in `results/` | Purpose |
| --- | --- |
| `raw_complete_rounds.csv` | Unscaled raw component table, labels, split and six source CSV row numbers |
| `round_splits.csv` | Metadata and partition membership of every complete round |
| `round_features.npz` | Raw feature matrix `x`, labels `y`, and `splits`, including purged rounds |
| `input_schema.json` | Exact direction/pair/component order and mask reference |
| `metrics.json` | Per-model scores, reports, confusion matrices, split and audit details |
| `test_predictions.csv` | Per-round held-out predictions and model scores |
| `best_model.joblib` | Selected random forest with its input schema |
| `majority.joblib`, `logistic.joblib`, `random_forest.joblib`, `rbf_svm.joblib`, `mlp.joblib` | Fitted classifiers; scalers are included where applicable |
| `model_comparison.png` | Validation/test accuracy comparison |
| `confusion_matrices.png` | Test confusion matrices for four trained classifiers |
| `roc_curves.png` | Test ROC curves |
| `neural_network_learning_curve.png`, `mlp_history.json` | Training/validation history and chosen checkpoint |
| `raw_verification.json` | Independent raw-value and provenance checks |

`test_predictions.csv` uses `_moving_score` columns. Scores are probabilities
for all models except RBF SVM, where they are decision margins, as recorded in
`metrics.json`. Rows of a confusion matrix are true classes and columns are
predicted classes, in `absent,moving` order.

To reproduce predictions from the selected saved model:

```python
import joblib
import numpy as np

bundle = joblib.load('results/best_model.joblib')
data = np.load('results/round_features.npz')
test = data['splits'] == 'test'
prediction = bundle['model'].predict(data['x'][test])
print([bundle['schema']['class_names'][int(value)] for value in prediction])
```
