"""Generate standalone evaluation graphs for complete-round classifiers."""
import os

import numpy as np

NAMES = {'majority': 'Majority baseline', 'logistic': 'Logistic regression',
         'random_forest': 'Random forest', 'rbf_svm': 'RBF SVM', 'mlp': 'Neural network'}


def plot_evaluation(output, report, y_test, scores, history):
    os.environ.setdefault('MPLCONFIGDIR', str((output / '.mplconfig').resolve()))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from sklearn.metrics import ConfusionMatrixDisplay, roc_curve

    plt.rcParams.update({'font.size': 11, 'axes.spines.top': False, 'axes.spines.right': False})
    names = list(report['models'])
    positions = np.arange(len(names))
    fig, ax = plt.subplots(figsize=(12, 5.5))
    for offset, part, color, label in [(-0.19, 'validation', '#7da8c2', 'Validation'),
                                       (0.19, 'test', '#245e83', 'Held-out test')]:
        values = [report['models'][name][part]['accuracy'] for name in names]
        bars = ax.bar(positions + offset, values, width=0.36, color=color, label=label)
        ax.bar_label(bars, labels=[f'{v:.1%}' for v in values], padding=4, fontsize=9)
    ax.set_xticks(positions, [NAMES[name] for name in names])
    ax.set(ylim=(0, 1.14), ylabel='Accuracy',
           title=f"Raw complete rounds: 52 CSI pairs x 6 directions x 2 components\n"
                 f"{len(y_test):,} held-out test rounds; selected by validation: {NAMES[report['selected_model']]}")
    ax.legend(loc='lower right')
    fig.tight_layout()
    fig.savefig(output / 'model_comparison.png', dpi=180)
    plt.close(fig)

    trained = [name for name in names if name != 'majority']
    fig, axes = plt.subplots(2, 2, figsize=(10, 9))
    for name, ax in zip(trained, axes.flat):
        matrix = np.array(report['models'][name]['test']['confusion_matrix'])
        ConfusionMatrixDisplay(matrix, display_labels=['Absent', 'Moving']).plot(
            ax=ax, colorbar=False, cmap='Blues', values_format='d')
        ax.set_title(f"{NAMES[name]}: {report['models'][name]['test']['accuracy']:.2%}")
    fig.suptitle('Held-out test confusion matrices (counts)', fontsize=15)
    fig.tight_layout()
    fig.savefig(output / 'confusion_matrices.png', dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(7, 6))
    for name in trained:
        fpr, tpr, _ = roc_curve(y_test, scores[name])
        auc = report['models'][name]['test']['roc_auc']
        ax.plot(fpr, tpr, linewidth=2, label=f'{NAMES[name]} (AUC {auc:.4f})')
    ax.plot([0, 1], [0, 1], '--', color='#999999', label='Chance')
    ax.set(xlim=(0, 1), ylim=(0, 1.02), xlabel='False positive rate',
           ylabel='True positive rate', title='Held-out test ROC curves: moving is positive')
    ax.legend(loc='lower right')
    fig.tight_layout()
    fig.savefig(output / 'roc_curves.png', dpi=180)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    epochs = [row['epoch'] for row in history]
    for part, color in [('train', '#245e83'), ('validation', '#d07c3c')]:
        axes[0].plot(epochs, [row[f'{part}_log_loss'] for row in history], color=color, label=part.title())
        axes[1].plot(epochs, [row[f'{part}_accuracy'] for row in history], color=color, label=part.title())
    for ax in axes:
        ax.axvline(report['mlp']['best_epoch'], linestyle='--', color='#777777', label='Saved checkpoint')
        ax.set_xlabel('Epoch')
        ax.legend()
    axes[0].set(ylabel='Log loss', title='Neural network loss')
    axes[1].set(ylabel='Accuracy', ylim=(0, 1.02), title='Neural network accuracy')
    fig.tight_layout()
    fig.savefig(output / 'neural_network_learning_curve.png', dpi=180)
    plt.close(fig)
