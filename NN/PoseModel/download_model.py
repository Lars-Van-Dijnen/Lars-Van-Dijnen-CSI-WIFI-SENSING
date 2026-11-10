"""Download Google's versioned MediaPipe model bundle and an optional test photo."""
import argparse
import hashlib
import json
import urllib.request
import zipfile
from pathlib import Path

BASE = Path(__file__).resolve().parent
MODEL_URL = ('https://storage.googleapis.com/mediapipe-models/pose_landmarker/'
             'pose_landmarker_full/float16/1/pose_landmarker_full.task')
SAMPLE_URL = 'https://storage.googleapis.com/mediapipe-assets/pose.jpg'


def download(url, destination):
    if destination.exists():
        print(f'Already present: {destination}')
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + '.part')
    try:
        with urllib.request.urlopen(url, timeout=60) as source, temporary.open('wb') as target:
            while chunk := source.read(1024 * 1024):
                target.write(chunk)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    print(f'Downloaded: {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', action='store_true', help='Also fetch Google\'s sample pose photo')
    args = parser.parse_args()
    model = BASE / 'models' / 'pose_landmarker_full.task'
    download(MODEL_URL, model)
    if not zipfile.is_zipfile(model):
        raise ValueError('The downloaded model is not a valid .task bundle')
    with zipfile.ZipFile(model) as bundle:
        if bundle.testzip() is not None:
            raise ValueError('The model bundle is corrupt')
    manifest = {'source_url': MODEL_URL, 'variant': 'full', 'model_version': 1,
                'sha256': hashlib.sha256(model.read_bytes()).hexdigest(),
                'size_bytes': model.stat().st_size,
                'documentation': 'https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker'}
    (BASE / 'models' / 'model_source.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    if args.sample:
        download(SAMPLE_URL, BASE / 'sample' / 'pose.jpg')


if __name__ == '__main__':
    main()
