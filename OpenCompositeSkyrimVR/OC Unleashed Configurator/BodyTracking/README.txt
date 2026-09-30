OCU body tracking runs locally. Camera frames are not uploaded to Google or
any other service.

Camera FBT uses one pose engine: the bundled MediaPipe Pose Landmarker under
MediaPipe\v0.10.35. Its metric 33-point skeleton owns waist/feet/knee trackers,
arm-and-leg gait, and kick-vs-step classification.

Legacy RTMW/2D tracking and pose_model.onnx are not part of this build. A
MediaPipe dropout pauses tracker output; it cannot switch coordinate systems.

BodyTracking\world3d_diag.csv is replaced on each Start and records fresh
MediaPipe landmarks, converted tracker positions, classifier states, and frame
age at 10 Hz. Calibration takes are written under BodyTracking\Captures.
