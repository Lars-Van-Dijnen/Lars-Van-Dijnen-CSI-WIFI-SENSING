"""Convert MediaPipe image landmarks to 2D, hip-relative coordinates."""
import math

LANDMARK_NAMES = (
    'nose', 'left_eye_inner', 'left_eye', 'left_eye_outer', 'right_eye_inner',
    'right_eye', 'right_eye_outer', 'left_ear', 'right_ear', 'mouth_left',
    'mouth_right', 'left_shoulder', 'right_shoulder', 'left_elbow', 'right_elbow',
    'left_wrist', 'right_wrist', 'left_pinky', 'right_pinky', 'left_index',
    'right_index', 'left_thumb', 'right_thumb', 'left_hip', 'right_hip',
    'left_knee', 'right_knee', 'left_ankle', 'right_ankle', 'left_heel',
    'right_heel', 'left_foot_index', 'right_foot_index',
)


def pose_rows(landmarks, width, height, threshold=0.5):
    """Use pixels before scaling so non-square images don't distort distances."""
    if len(landmarks) != 33:
        raise ValueError('Expected 33 MediaPipe pose landmarks')
    if width <= 0 or height <= 0:
        raise ValueError('Image dimensions must be positive')
    points, confidence, valid = [], [], []
    for landmark in landmarks:
        point = (float(landmark.x) * width, float(landmark.y) * height)
        visibility = float(getattr(landmark, 'visibility', 0) or 0)
        presence = float(getattr(landmark, 'presence', 0) or 0)
        points.append(point)
        confidence.append((visibility, presence))
        valid.append(all(math.isfinite(v) for v in (*point, visibility, presence))
                     and min(visibility, presence) >= threshold)
    root, scale = None, None
    if all(valid[index] for index in (11, 12, 23, 24)):
        root = tuple((points[23][axis] + points[24][axis]) / 2 for axis in (0, 1))
        shoulders = tuple((points[11][axis] + points[12][axis]) / 2 for axis in (0, 1))
        scale = math.dist(root, shoulders)
        if scale <= 1e-6:
            root, scale = None, None
    rows = []
    for index, (landmark, name) in enumerate(zip(landmarks, LANDMARK_NAMES)):
        relative_valid = valid[index] and scale is not None
        rows.append({
            'landmark_index': index, 'landmark': name,
            'x_image': float(landmark.x), 'y_image': float(landmark.y),
            'x_pixels': points[index][0], 'y_pixels': points[index][1],
            'x_relative': (points[index][0] - root[0]) / scale if relative_valid else '',
            'y_relative': (root[1] - points[index][1]) / scale if relative_valid else '',
            'visibility': confidence[index][0], 'presence': confidence[index][1],
            'valid': int(valid[index]), 'relative_valid': int(relative_valid),
        })
    return rows, {'hip_center_pixels': root, 'torso_length_pixels': scale,
                  'relative_available': scale is not None}
