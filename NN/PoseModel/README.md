# MediaPipe Pose: pictures, video and webcam

This folder runs Google's pretrained **Pose Landmarker Full** model. It detects
33 body landmarks for one person, draws the skeleton, and exports 2D coordinates.
There is no training step or posture classifier in this initial version.

The camera model has its own `.venv`, dependencies and outputs. It does not read
or modify the CSI datasets or classifiers.

## Run from the project root

The environment and model are already installed in this workspace.

**A picture, including 1920 x 1080:**

```powershell
.\PoseModel\.venv\Scripts\python.exe PoseModel/run_pose.py --input "C:\path\to\picture.jpg"
```

**The default webcam:**

```powershell
.\PoseModel\.venv\Scripts\python.exe PoseModel/run_pose.py --camera 0
```

Use `--camera 1` for a different device. The preview opens when this command
runs. Press **Q** or **Esc** to close it. For a picture, any key also lets the
single-image run finish.

**A video:**

```powershell
.\PoseModel\.venv\Scripts\python.exe PoseModel/run_pose.py --input "C:\path\to\video.mp4"
```

**A picture without a preview window:**

```powershell
.\PoseModel\.venv\Scripts\python.exe PoseModel/run_pose.py --input "C:\path\to\picture.jpg" --no-display
```

Other options: `--max-frames 100`, `--confidence 0.5`, `--model path/to/model.task`,
and `--output path/to/new_results`. Without `--input` or `--camera`, camera 0 is
used. A headless webcam run requires `--max-frames` so it will terminate.

File inputs are processed locally. Default runs create a new UTC-timestamped
directory under `PoseModel/outputs/`; an existing result directory is never
silently overwritten. Full-resolution video recordings are not saved. For
videos/webcams, the annotated PNG is the last processed frame.

## What 1080p means here

Input pictures and video frames can be 1920 x 1080. This implementation passes
their original dimensions to MediaPipe and saves the annotated PNG at that
resolution. The preview window fits the screen without changing exported
coordinates. MediaPipe internally resizes/crops input for its fixed-size neural
models, so its neural network itself does not operate at 1920 x 1080. Higher
source resolution does not guarantee better landmark accuracy.

Keep the full body visible and reasonably large in the frame. Low visibility,
occlusion, unusual viewpoints or poor lighting can reduce landmark reliability.

## Outputs and coordinate systems

| File in a run directory | Contents |
| --- | --- |
| `annotated.png` | Original-resolution picture with landmarks, connections and hip center |
| `landmarks.csv` | 33 rows per detected pose/frame; coordinates and reliability scores |
| `frames.csv` | One row per processed frame, including frames with no detected pose |
| `run.json` | Source, model, frame dimensions/counts and coordinate conventions |

`landmarks.csv` includes:

- `frame_index`, `timestamp_ms`, `landmark_index`, `landmark`.
- `x_image`, `y_image`: original MediaPipe image-normalized predictions; x is
  rightward and y downward. These can lie outside the image for inferred points.
- `x_pixels`, `y_pixels`: predictions multiplied by original width and height.
- `visibility`, `presence`: MediaPipe reliability estimates.
- `valid`: both reliability scores pass the chosen threshold and values are finite.
- `x_relative`, `y_relative`: coordinates centered on the midpoint of the hips,
  scaled by projected torso length. Here x is rightward and y upward.
- `relative_valid`: whether that landmark has usable relative coordinates.

The relative-coordinate transform is:

```text
hip_center = average(left_hip_pixels, right_hip_pixels)
shoulder_center = average(left_shoulder_pixels, right_shoulder_pixels)
torso_length = Euclidean distance(hip_center, shoulder_center)
x_relative = (x_pixels - hip_center_x) / torso_length
y_relative = (hip_center_y - y_pixels) / torso_length
```

Pixel coordinates are used before scaling so a rectangular picture does not
distort distances. The transform preserves pose shape and removes image
translation and uniform size changes. It does not remove perspective changes.
Axes remain aligned with the camera image; they are not rotated to the person's
facing direction. These are projected 2D distances in torso-length units, not
world coordinates in meters, joint velocities, or Widar body-coordinate velocity
profiles.

Both shoulders and hips must pass the confidence threshold and have nonzero
torso length. Otherwise relative fields are blank. Individual unreliable joints
also receive blank relative coordinates. No-pose frames produce no landmark
rows, but remain in `frames.csv`. Coordinates are not carried forward from prior
frames. For video inputs, CSV timestamps use the decoder's source timestamps,
with an FPS fallback. Webcam timestamps use a monotonic clock.

## Installation on another machine

Python 3.11 on Windows was used here:

```powershell
python -m venv PoseModel/.venv
.\PoseModel\.venv\Scripts\python.exe -m pip install -r PoseModel/requirements.txt
.\PoseModel\.venv\Scripts\python.exe PoseModel/download_model.py
```

Only install the OpenCV contrib package listed in the requirements; installing
`opencv-python` alongside it duplicates the same `cv2` module.

The model is Google's Full variant, float16 bundle version 1, downloaded from
the official MediaPipe model storage. `models/model_source.json` records its URL,
size and SHA-256 checksum. `download_model.py --sample` also downloads Google's
sample photograph into `sample/`.

## Verification

```powershell
.\PoseModel\.venv\Scripts\python.exe -m unittest discover -s PoseModel -p test_pose_coordinates.py -v
```

Five unit tests passed: rectangular-image geometry, translation/scale invariance,
missing anchors, unreliable joints, and video timestamps. Actual pretrained-model
inference also passed on:

- Google's original 1000 x 667 sample photograph: 33 landmarks.
- A resized and padded 1920 x 1080 version: 33 landmarks, all with usable relative
  coordinates, with the annotated output remaining 1920 x 1080.
- A blank 1920 x 1080 image: no pose and no invented landmark rows.
- A three-frame 1920 x 1080 test video: 99 landmark rows and increasing timestamps.

The 1080p fixture verifies input/output handling; it is not a new accuracy
benchmark because it was made from the smaller sample image. These results are
under `outputs/smoke_*`. Live webcam hardware and the interactive preview have
not been exercised in this session.

## Sources

- [Official Pose Landmarker documentation](https://developers.google.com/edge/mediapipe/solutions/vision/pose_landmarker)
- [Official Python API implementation](https://github.com/google-ai-edge/mediapipe/blob/master/mediapipe/tasks/python/vision/pose_landmarker.py)
- [Full model bundle](https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/1/pose_landmarker_full.task)
- [Google sample photograph](https://storage.googleapis.com/mediapipe-assets/pose.jpg)
