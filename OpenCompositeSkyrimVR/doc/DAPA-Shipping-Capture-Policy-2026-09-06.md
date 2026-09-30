# Shipping capture policy — 6 September 2026

`OCU_DAPA_CAPTURE` is a CMake option, OFF by default. Production builds skip
capture input polling/haptics and make Request, ToggleSession and Poll inert.
This disables the two-grip gesture, Ctrl+Shift+F10 and desktop control files without
changing gameplay grip handling, DAPA motion math or player/held-object masks.

Developers can explicitly configure `-DOCU_DAPA_CAPTURE=ON` to retain diagnostic
recording. The standalone capture regression target enables it independently;
the shipping-policy target compiles the production-disabled implementation.
Do not package DAPA Capture.exe or capture-only instructions in Nexus releases.

Validation: RelWithDebInfo OCOVR build, OCUDapaCaptureShippingTest,
OCUDapaCaptureTest, OCUDapaMotionTest and OCUDapaTimingTest passed. Timing covers
60, 72, 80, 90, 96, 100, 120 and 144 Hz. This is automated verification, not a new
headset test. MO2 retains the user's existing developer test runtime.

Camera audit: MediaPipe World3D sources, models, calibration recorder and Vive
tracker runtime support remain. BodyTrackingTab.cs still has the previous removal
of the dormant ONNX/RTMW Legacy2D solver/sender. It was not restored by this task;
the prior implementation remains in local Git history, including
878dc2b4d12b02755891063ed6e3d830decbca4f. Preserve that history if the old prototype
will be revisited. No camera source, model or settings were changed here.
