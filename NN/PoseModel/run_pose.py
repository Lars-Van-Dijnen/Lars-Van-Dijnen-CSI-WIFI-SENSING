"""Run pretrained MediaPipe Pose on a picture, video, or webcam and export 2D joints."""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from pose_coordinates import pose_rows

BASE = Path(__file__).resolve().parent
IMAGE_SUFFIXES = {'.jpg', '.jpeg', '.png', '.bmp', '.webp', '.tif', '.tiff'}


def draw_pose(image, rows, info, connections, inference_ms):
    import cv2
    height, width = image.shape[:2]
    radius = max(3, round(height / 240))
    for edge in (connections if rows else ()):
        start, end = rows[edge.start], rows[edge.end]
        if start['valid'] and end['valid']:
            cv2.line(image, (round(start['x_pixels']), round(start['y_pixels'])),
                     (round(end['x_pixels']), round(end['y_pixels'])),
                     (70, 220, 80), max(2, radius // 2), cv2.LINE_AA)
    for row in rows:
        if row['valid'] and 0 <= row['x_pixels'] < width and 0 <= row['y_pixels'] < height:
            cv2.circle(image, (round(row['x_pixels']), round(row['y_pixels'])),
                       radius, (0, 180, 255), -1, cv2.LINE_AA)
    if info['hip_center_pixels'] is not None:
        center = tuple(round(value) for value in info['hip_center_pixels'])
        cv2.drawMarker(image, center, (255, 100, 0), cv2.MARKER_CROSS, radius * 5, 2)
    message = f'Pose detected | {inference_ms:.0f} ms'
    if not rows:
        message = 'No pose detected'
    elif not info['relative_available']:
        message += ' | Keep shoulders and hips in view'
    scale = max(0.5, height / 1080)
    cv2.putText(image, message, (15, max(30, round(40 * scale))),
                cv2.FONT_HERSHEY_SIMPLEX, scale, (255, 255, 255), max(1, round(scale * 2)), cv2.LINE_AA)
    return image


def timestamp_ms(index, video_position_ms, fps, previous):
    """Prefer container timestamps; fall back to FPS when the decoder has none."""
    candidate = video_position_ms
    if not math.isfinite(candidate) or (index > 0 and candidate <= previous):
        if not math.isfinite(fps) or fps <= 0:
            raise ValueError('Video has no usable timestamps/FPS')
        candidate = index * 1000 / fps
    return max(previous + 1, round(candidate))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--input', type=Path, help='Picture or video file, including 1080p inputs')
    source.add_argument('--camera', type=int, help='Webcam index (default: 0 when no input file is given)')
    parser.add_argument('--model', type=Path, default=BASE / 'models' / 'pose_landmarker_full.task')
    parser.add_argument('--output', type=Path, help='New output directory; existing result files are not overwritten')
    parser.add_argument('--no-display', action='store_true', help='Process files without opening a preview window')
    parser.add_argument('--max-frames', type=int, help='Stop after this many frames')
    parser.add_argument('--confidence', type=float, default=0.5, help='Landmark visibility/presence threshold')
    args = parser.parse_args()
    if not 0 <= args.confidence <= 1 or (args.max_frames is not None and args.max_frames < 1):
        parser.error('Require confidence in [0,1] and max-frames >= 1')
    if not args.model.is_file():
        parser.error('Model missing. Run: python PoseModel/download_model.py')
    if args.input and not args.input.is_file():
        parser.error(f'Input file not found: {args.input}')
    camera = args.input is None
    if camera and args.no_display and args.max_frames is None:
        parser.error('Headless webcam capture requires --max-frames')

    import cv2
    import mediapipe as mp
    from mediapipe.tasks import python
    from mediapipe.tasks.python import vision
    from mediapipe.tasks.python.vision import pose_landmarker

    still = args.input is not None and args.input.suffix.lower() in IMAGE_SUFFIXES
    capture = None
    if still:
        # imdecode handles Unicode Windows paths and preserves the source dimensions.
        import numpy as np
        picture = cv2.imdecode(np.fromfile(args.input, dtype=np.uint8), cv2.IMREAD_COLOR)
        if picture is None:
            raise ValueError('Could not decode input picture')
    else:
        target = (args.camera if args.camera is not None else 0) if camera else str(args.input)
        capture = cv2.VideoCapture(target)
        if not capture.isOpened():
            capture.release()
            raise RuntimeError(f'Could not open {"webcam" if camera else "video"}: {target}')

    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S_%fZ')
    output = args.output or BASE / 'outputs' / stamp
    if any((output / name).exists() for name in ['landmarks.csv', 'frames.csv', 'run.json', 'annotated.png']):
        if capture is not None:
            capture.release()
        raise ValueError('Output already contains results; choose another --output directory')
    output.mkdir(parents=True, exist_ok=True)
    options = vision.PoseLandmarkerOptions(
        base_options=python.BaseOptions(model_asset_path=str(args.model.resolve())),
        running_mode=vision.RunningMode.IMAGE if still else vision.RunningMode.VIDEO,
        num_poses=1, min_pose_detection_confidence=0.5,
        min_pose_presence_confidence=0.5, min_tracking_confidence=0.5,
        output_segmentation_masks=False,
    )
    joint_columns = ['frame_index', 'timestamp_ms', 'landmark_index', 'landmark',
                     'x_image', 'y_image', 'x_pixels', 'y_pixels', 'x_relative', 'y_relative',
                     'visibility', 'presence', 'valid', 'relative_valid']
    frame_columns = ['frame_index', 'timestamp_ms', 'width', 'height', 'pose_detected',
                     'relative_available', 'inference_ms']
    count, detected_count, previous, annotated = 0, 0, -1, None
    started = time.perf_counter()
    try:
        with vision.PoseLandmarker.create_from_options(options) as detector, \
                (output / 'landmarks.csv').open('w', newline='', encoding='utf-8') as joints_file, \
                (output / 'frames.csv').open('w', newline='', encoding='utf-8') as frames_file:
            joint_writer = csv.DictWriter(joints_file, fieldnames=joint_columns)
            frame_writer = csv.DictWriter(frames_file, fieldnames=frame_columns)
            joint_writer.writeheader()
            frame_writer.writeheader()
            if not args.no_display:
                cv2.namedWindow('MediaPipe Pose - Q or Esc to close', cv2.WINDOW_NORMAL)
            while args.max_frames is None or count < args.max_frames:
                if still:
                    image = picture.copy()
                    current_ms = 0
                else:
                    ok, image = capture.read()
                    if not ok:
                        break
                    if camera:
                        current_ms = max(previous + 1, round((time.perf_counter() - started) * 1000))
                    else:
                        current_ms = timestamp_ms(count, capture.get(cv2.CAP_PROP_POS_MSEC),
                                                  capture.get(cv2.CAP_PROP_FPS), previous)
                previous = current_ms
                height, width = image.shape[:2]
                rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
                mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
                inference_start = time.perf_counter()
                result = detector.detect(mp_image) if still else detector.detect_for_video(mp_image, current_ms)
                elapsed_ms = (time.perf_counter() - inference_start) * 1000
                rows, info = [], {'hip_center_pixels': None, 'relative_available': False}
                if result.pose_landmarks:
                    rows, info = pose_rows(result.pose_landmarks[0], width, height, args.confidence)
                    detected_count += 1
                    for row in rows:
                        joint_writer.writerow({'frame_index': count, 'timestamp_ms': current_ms, **row})
                frame_writer.writerow({'frame_index': count, 'timestamp_ms': current_ms,
                                       'width': width, 'height': height,
                                       'pose_detected': int(bool(rows)),
                                       'relative_available': int(info['relative_available']),
                                       'inference_ms': elapsed_ms})
                annotated = draw_pose(image, rows, info,
                                      pose_landmarker.PoseLandmarksConnections.POSE_LANDMARKS, elapsed_ms)
                count += 1
                if not args.no_display:
                    factor = min(1, 1280 / width, 800 / height)
                    cv2.resizeWindow('MediaPipe Pose - Q or Esc to close', round(width * factor), round(height * factor))
                    cv2.imshow('MediaPipe Pose - Q or Esc to close', annotated)
                    key = cv2.waitKey(0 if still else 1) & 0xFF
                    if key in (ord('q'), 27):
                        break
                if still:
                    break
                if count % 100 == 0:
                    joints_file.flush()
                    frames_file.flush()
                    print(f'Processed {count} frames; pose detected in {detected_count}', flush=True)
    finally:
        if capture is not None:
            capture.release()
        if not args.no_display:
            cv2.destroyAllWindows()
    if count == 0:
        raise RuntimeError('Input opened but yielded no readable frames')
    if annotated is not None:
        ok, encoded = cv2.imencode('.png', annotated)
        if not ok:
            raise RuntimeError('Could not encode annotated preview')
        encoded.tofile(output / 'annotated.png')
    report = {
        'model': str(args.model.resolve()), 'source': str(args.input.resolve()) if args.input else f'camera:{args.camera or 0}',
        'frames_processed': count, 'frames_with_pose': detected_count,
        'last_frame_dimensions': {'width': width, 'height': height},
        'landmarks_per_detected_pose': 33, 'people_per_frame': 1,
        'image_coordinates': 'x right, y down; x/width and y/height',
        'relative_coordinates': 'Subtract midpoint of hips in pixels, divide by projected torso length; x right, y up',
        'relative_axes': 'Image plane; not rotated to body orientation and not real-world meters',
        'relative_anchors': 'Both hips and both shoulders must pass visibility and presence thresholds',
        'confidence_threshold': args.confidence,
        'missing_data': 'No-pose frames appear in frames.csv only; unavailable relative values are blank',
        'annotated_preview': 'Original resolution; last processed frame for video/webcam',
    }
    (output / 'run.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Processed {count} frame(s); pose detected in {detected_count}. Outputs: {output.resolve()}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f'Error: {error}', file=sys.stderr)
        raise SystemExit(1)
