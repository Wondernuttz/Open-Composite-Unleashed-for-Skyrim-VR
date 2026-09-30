using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
using OpenCompositeConfigurator.BodyTracking;

namespace OpenCompositeConfigurator
{
    // ═══════════════════════════════════════════════════════════════════════
    // BODY TRACKING TAB
    // Webcam / phone camera -> local MediaPipe World3D -> OSC trackers into
    // the OCU DLL's network tracker receiver (127.0.0.1, networkTrackerPort).
    // Left: body silhouette with tracker points lighting up as they track
    // (KAT treadmill shows as a puck under the feet). Right: live video with
    // the detected skeleton drawn over it.
    //
    // MediaPipe's 33-point metric skeleton is the only active camera source.
    // Retired ONNX/RTMW inference and its Legacy2D sender have been removed.
    // ═══════════════════════════════════════════════════════════════════════
    public partial class MainForm
    {
        private Button _btnTabBody = null!;
        private Panel _tabBody = null!;

        private CheckBox _chkNetTrackersEnabled = null!;
        private CheckBox _chkCameraLegCalibration = null!;
        private CheckBox _chkWalkInPlace = null!;
        private ComboBox _cmbWalkActivation = null!;
        private ComboBox _cmbBodyDevice = null!;
        private ComboBox _cmbBodyPoseSource = null!;
        // Only MediaPipe World3D is selectable, including when loading old ui.json files.
        private bool _bodyRequestedWorld3D => true;
        private bool _bodyUseWorld3D => true;
        private int _bodyActiveTrackerSource;
        private bool _bodyUiInitialized;
        private Label _lblBodyPoseSourceStatus = null!;
        private NumericUpDown _nudBodyOffX = null!;
        private NumericUpDown _nudBodyOffY = null!;
        private volatile float _bodyOffX; // percent of frame, applied to every keypoint
        private volatile float _bodyOffY; // positive = down
        private ComboBox _cmbBodyCamera = null!;
        private TextBox _txtBodyCamUrl = null!;
        private Button _btnBodyStartStop = null!;
        private CheckBox _chkBodyMirror = null!;
        private CheckBox _chkBodyStream = null!;
        private CheckBox _chkBodyPreview = null!;
        private CheckBox _chkBodySkeletonOnly = null!;
        private NumericUpDown _nudBodyHeightCm = null!;
        private Label _lblBodyStatus = null!;
        private PictureBox _picBodyVideo = null!;
        private BodySilhouettePanel _pnlBodySilhouette = null!;
        private ComboBox _cmbBodyCaptureView = null!;
        private ComboBox _cmbBodyCaptureAction = null!;
        private ComboBox _cmbBodyCaptureLeg = null!;
        private NumericUpDown _nudBodyCaptureTake = null!;
        private Button _btnBodyCaptureRecord = null!;
        private Label _lblBodyCaptureStatus = null!;

        // Video is only a setup aid. Tracking and OSC continue when the preview
        // is disabled, on another tab, or the Configurator is minimized.
        private volatile bool _bodyPreviewActive = true;
        private volatile bool _bodyPreviewMirror = true;
        private volatile bool _bodyPreviewSkeletonOnly;
        private volatile bool _bodyStreamEnabled = true;
        private int _bodyPreviewUiPending;
        private int _bodyPreviewModeVersion;
        private long _bodyPreviewLastFrameMs;
        private const int BodyPreviewIntervalMs = 100; // 10 FPS is plenty for camera setup
        private const int BodyPreviewMaxEdge = 960;
        private const int BodyInferenceIntervalMs = 33; // cameras are 30 FPS; don't burn cycles beyond their data rate

        private Thread? _bodyCapThread;
        private volatile bool _bodyCapRunning;
        private int _bodyCaptureGeneration;
        private volatile bool _bodyTrackingStreamReady;
        private volatile bool _bodyCalibrationRecording;
        private volatile string _bodyCalibrationView = "0";
        private volatile string _bodyCalibrationAction = "neutral";
        private volatile string _bodyCalibrationLeg = "both";
        private volatile string _bodyCalibrationFileName = "";
        private int _bodyCalibrationTake = 1;
        private int _bodyCalibrationSegment;
        private UdpClient? _bodyOsc;
        private int _bodyOscPort = 9000;
        private readonly object _bodyOscGate = new object();
        private readonly Dictionary<string, byte[]> _bodyOscPackets = new(StringComparer.Ordinal);

        private const int BodyFootKeypointCount = 23;
        private const int TrackedKeypointCount = 133;
        // RTMW's ankle/foot scores fall sharply when a leg points toward the
        // camera even though the reported landmark still follows the limb.
        // Treat those lower-body points as weak measurements instead of
        // freezing them at the exact moment a knee extends into a kick.
        // Camera skeleton coordinates are deliberately expressed against one
        // fixed reference body size. The game performs the real player-height
        // scale from the HMD, so a stale hidden HeightCm setting must never
        // alter kick reach, lift thresholds, or the calibration dataset.
        private const float BodyReferenceHeightM = 1.75f;
        private const int LeftHandStart = 91;
        private const int RightHandStart = 112;
        private const int HandKeypointCount = 21;

        // Latest keypoints (normalized 0..1 image coords + confidence),
        // Full COCO-WholeBody order (body, feet, face, both hands). Written by
        // capture, read by the UI; face remains hidden under the VR headset.
        private readonly float[,] _bodyKp = new float[TrackedKeypointCount, 3];
        private readonly object _bodyKpLock = new object();
        private volatile bool _bodyPoseValid;
        private volatile bool _bodyKatDetected;

        private const string FbtModUrl = "https://www.nexusmods.com/skyrimspecialedition/mods/185070";

        private static readonly (string label, string id)[] BodyCaptureViews =
        {
            ("Front (0 deg)", "0"),
            ("Turn left (-45 deg)", "-45"),
            ("Turn right (+45 deg)", "+45"),
            ("Left profile (-90 deg)", "-90"),
            ("Right profile (+90 deg)", "+90"),
        };

        private static readonly (string label, string id)[] BodyCaptureActions =
        {
            ("Neutral stance", "neutral"),
            ("Slow walk", "walk_slow"),
            ("Normal walk", "walk_normal"),
            ("Run in place", "run"),
            ("Knee raise", "knee_raise"),
            ("Front kick", "front_kick"),
            ("High march (not a kick)", "high_march"),
        };

        private static readonly (string label, string id)[] BodyCaptureLegs =
        {
            ("Both / N/A", "both"),
            ("Left", "left"),
            ("Right", "right"),
        };

        // Indices 5..22, in COCO-WholeBody order. Face landmarks are omitted;
        // shoulders through toes are the geometry needed by the lower-body
        // classifier and keep the capture files reasonably small.
        private static readonly string[] BodyCaptureJointNames =
        {
            "lshoulder", "rshoulder", "lelbow", "relbow", "lwrist", "rwrist",
            "lhip", "rhip", "lknee", "rknee", "lankle", "rankle",
            "lbigtoe", "lsmalltoe", "lheel", "rbigtoe", "rsmalltoe", "rheel",
        };

        // COCO-WholeBody indices. 0..16 are COCO body, 17..22 are the six
        // RTMW foot landmarks that the previous decoder silently discarded.
        private const int KpNose = 0, KpLSho = 5, KpRSho = 6, KpLElb = 7, KpRElb = 8,
            KpLWri = 9, KpRWri = 10, KpLHip = 11, KpRHip = 12, KpLKnee = 13, KpRKnee = 14,
            KpLAnk = 15, KpRAnk = 16,
            KpLBigToe = 17, KpLSmallToe = 18, KpLHeel = 19,
            KpRBigToe = 20, KpRSmallToe = 21, KpRHeel = 22;

        // MediaPipe Pose's normalized 33-point topology. This is used only for
        // the preview when the continuous World 3D frame actually owns output;
        // emitted positions still come from metric WorldPosition landmarks.
        private static readonly (MediaPipe33LandmarkIndex a, MediaPipe33LandmarkIndex b)[] MediaPipePoseBones =
        {
            (MediaPipe33LandmarkIndex.LeftShoulder, MediaPipe33LandmarkIndex.RightShoulder),
            (MediaPipe33LandmarkIndex.LeftShoulder, MediaPipe33LandmarkIndex.LeftElbow),
            (MediaPipe33LandmarkIndex.LeftElbow, MediaPipe33LandmarkIndex.LeftWrist),
            (MediaPipe33LandmarkIndex.LeftWrist, MediaPipe33LandmarkIndex.LeftPinky),
            (MediaPipe33LandmarkIndex.LeftWrist, MediaPipe33LandmarkIndex.LeftIndex),
            (MediaPipe33LandmarkIndex.LeftWrist, MediaPipe33LandmarkIndex.LeftThumb),
            (MediaPipe33LandmarkIndex.RightShoulder, MediaPipe33LandmarkIndex.RightElbow),
            (MediaPipe33LandmarkIndex.RightElbow, MediaPipe33LandmarkIndex.RightWrist),
            (MediaPipe33LandmarkIndex.RightWrist, MediaPipe33LandmarkIndex.RightPinky),
            (MediaPipe33LandmarkIndex.RightWrist, MediaPipe33LandmarkIndex.RightIndex),
            (MediaPipe33LandmarkIndex.RightWrist, MediaPipe33LandmarkIndex.RightThumb),
            (MediaPipe33LandmarkIndex.LeftShoulder, MediaPipe33LandmarkIndex.LeftHip),
            (MediaPipe33LandmarkIndex.RightShoulder, MediaPipe33LandmarkIndex.RightHip),
            (MediaPipe33LandmarkIndex.LeftHip, MediaPipe33LandmarkIndex.RightHip),
            (MediaPipe33LandmarkIndex.LeftHip, MediaPipe33LandmarkIndex.LeftKnee),
            (MediaPipe33LandmarkIndex.LeftKnee, MediaPipe33LandmarkIndex.LeftAnkle),
            (MediaPipe33LandmarkIndex.LeftAnkle, MediaPipe33LandmarkIndex.LeftHeel),
            (MediaPipe33LandmarkIndex.LeftHeel, MediaPipe33LandmarkIndex.LeftFootIndex),
            (MediaPipe33LandmarkIndex.LeftAnkle, MediaPipe33LandmarkIndex.LeftFootIndex),
            (MediaPipe33LandmarkIndex.RightHip, MediaPipe33LandmarkIndex.RightKnee),
            (MediaPipe33LandmarkIndex.RightKnee, MediaPipe33LandmarkIndex.RightAnkle),
            (MediaPipe33LandmarkIndex.RightAnkle, MediaPipe33LandmarkIndex.RightHeel),
            (MediaPipe33LandmarkIndex.RightHeel, MediaPipe33LandmarkIndex.RightFootIndex),
            (MediaPipe33LandmarkIndex.RightAnkle, MediaPipe33LandmarkIndex.RightFootIndex),
        };

        private void BuildBodyTrackingTab()
        {
            var container = _tabBody;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 20;
            int y = 10;

            var btnBodySave = MakeButton("Save opencomposite.ini", rightEdge - 200, y - 4, 200, 30);
            btnBodySave.BackColor = Color.FromArgb(40, 120, 40);
            btnBodySave.ForeColor = Color.White;
            btnBodySave.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            btnBodySave.Click += BtnSave_Click;
            container.Controls.Add(btnBodySave);

            // ── Setup instructions (compact — details live in the panels below) ──
            var lblSteps = new Label
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin - 210, 66),
                Text = "1. Camera 2-4 m away, full body in frame (webcam below, or a phone IP-camera app's URL).\n"
                     + "2. Face the camera and press Start - local MediaPipe Lite tracks you; no camera data leaves this PC.\n"
                     + "3. Tick \"Send trackers to the game\" + Save. In-game body size auto-calibrates to your headset.\n"
                     + "4. In the game, hold both grips 2s to calibrate the FBT mod (download link below).",
                ForeColor = Color.FromArgb(190, 190, 195),
                Font = new Font("Segoe UI", 8.5f),
                AutoSize = false
            };
            container.Controls.Add(lblSteps);
            y += 70;

            // ── Source row ──
            container.Controls.Add(MakeLabel("Camera:", leftMargin, y + 3, 60));
            _cmbBodyCamera = new ComboBox
            {
                Location = new Point(leftMargin + 60, y), Width = 170,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _cmbBodyCamera.Items.AddRange(new object[]
                { "Webcam", "Phone / IP camera (URL)" });
            _cmbBodyCamera.SelectedIndex = 0;
            _cmbBodyCamera.SelectedIndexChanged += (s, e) =>
                _txtBodyCamUrl.Enabled = _cmbBodyCamera.SelectedIndex == 1;
            container.Controls.Add(_cmbBodyCamera);

            _txtBodyCamUrl = new TextBox
            {
                Location = new Point(leftMargin + 238, y), Width = 260,
                Text = "http://192.168.1.100:8080/video",
                Enabled = false,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_txtBodyCamUrl);

            _btnBodyStartStop = new ModernPillButton
            {
                Location = new Point(leftMargin + 508, y - 2), Size = new Size(90, 26),
                Text = "Start",
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                ForeColor = Color.White, BackColor = Color.FromArgb(40, 120, 40), // match the Save button green
                Cursor = Cursors.Hand,
            };
            _btnBodyStartStop.Click += (s, e) => ToggleBodyCapture();
            container.Controls.Add(_btnBodyStartStop);
            y += 30;

            // ── Options row ──
            _chkBodyMirror = MakeCheckBox("Mirror", leftMargin, y);
            _chkBodyMirror.Checked = true;
            _chkBodyMirror.CheckedChanged += (s, e) => _bodyPreviewMirror = _chkBodyMirror.Checked;
            container.Controls.Add(_chkBodyMirror);

            _chkBodyStream = MakeCheckBox("Stream to OCU (OSC)", leftMargin + 80, y);
            _chkBodyStream.Checked = true;
            _chkBodyStream.CheckedChanged += (s, e) => _bodyStreamEnabled = _chkBodyStream.Checked;
            container.Controls.Add(_chkBodyStream);

            _chkBodyPreview = MakeCheckBox("Show preview", leftMargin + 245, y);
            _chkBodyPreview.Checked = true;
            _chkBodyPreview.CheckedChanged += (s, e) =>
            {
                _chkBodySkeletonOnly.Enabled = _chkBodyPreview.Checked;
                Interlocked.Increment(ref _bodyPreviewModeVersion);
                UpdateBodyPreviewActivity();
            };
            container.Controls.Add(_chkBodyPreview);
            new ToolTip { AutoPopDelay = 12000, InitialDelay = 350 }.SetToolTip(_chkBodyPreview,
                "Display only. Turn this off to skip video conversion, skeleton drawing, and UI repainting while tracking and OSC keep running. Preview also pauses on other pages and when minimized.");

            _chkBodySkeletonOnly = MakeCheckBox("Skeleton only", leftMargin + 365, y);
            _chkBodySkeletonOnly.Checked = true;
            _chkBodySkeletonOnly.CheckedChanged += (s, e) =>
            {
                _bodyPreviewSkeletonOnly = _chkBodySkeletonOnly.Checked;
                Interlocked.Increment(ref _bodyPreviewModeVersion);
                Interlocked.Exchange(ref _bodyPreviewLastFrameMs, 0);
                var old = _picBodyVideo?.Image;
                if (_picBodyVideo != null)
                    _picBodyVideo.Image = null;
                old?.Dispose();
            };
            container.Controls.Add(_chkBodySkeletonOnly);
            new ToolTip { AutoPopDelay = 12000, InitialDelay = 350 }.SetToolTip(_chkBodySkeletonOnly,
                "Hide the camera image and draw only the detected skeleton on black. Camera capture, pose AI, feet/finger tracking, and OSC continue normally. This skips video-to-bitmap conversion but not pose inference.");

            // Height input retired from the UI: the OCU DLL auto-scales the
            // skeleton against the real HMD height in-game, so a manual value
            // is redundant. The hidden control keeps the plumbing satisfied.
            _nudBodyHeightCm = new NumericUpDown
            {
                Minimum = 120, Maximum = 220, Value = 175, Visible = false
            };

            _lblBodyStatus = MakeLabel("Idle", leftMargin + 475, y + 2, rightEdge - leftMargin - 475);
            _lblBodyStatus.ForeColor = Color.FromArgb(160, 160, 160);
            container.Controls.Add(_lblBodyStatus);
            y += 26;

            _chkNetTrackersEnabled = MakeCheckBox("Send trackers to the game (networkTrackersEnabled, needs game restart)", leftMargin, y);
            container.Controls.Add(_chkNetTrackersEnabled);
            // MediaPipe's native World3D task is the sole pose engine and runs
            // locally on CPU. The disabled field makes that runtime contract
            // explicit instead of exposing the dormant RTMW device selector.
            container.Controls.Add(MakeLabel("AI device:", leftMargin + 480, y + 2, 70));
            _cmbBodyDevice = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Location = new Point(leftMargin + 552, y - 1), Width = 300,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _cmbBodyDevice.Items.Add("CPU (MediaPipe World3D Lite, local)");
            _cmbBodyDevice.SelectedIndex = 0;
            _cmbBodyDevice.Enabled = false;
            new ToolTip().SetToolTip(_cmbBodyDevice,
                "MediaPipe World3D Lite runs locally on CPU. RTMW/Legacy2D is not loaded.");
            container.Controls.Add(_cmbBodyDevice);
            y += 26;

            // One immutable source owns trackers, gait, recording and preview.
            // No secondary 2D solver is kept warm and no fallback can change
            // coordinate systems during a MediaPipe dropout.
            container.Controls.Add(MakeLabel("FBT tracker pose:", leftMargin, y + 2, 112));
            _cmbBodyPoseSource = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Location = new Point(leftMargin + 112, y - 1), Width = 330,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            _cmbBodyPoseSource.Items.Add("World 3D landmarks (MediaPipe, local)");
            _cmbBodyPoseSource.SelectedIndex = 0;
            _cmbBodyPoseSource.Enabled = false;
            new ToolTip { AutoPopDelay = 15000, InitialDelay = 350 }.SetToolTip(_cmbBodyPoseSource,
                "World 3D uses MediaPipe's continuous hip-centered 3D skeleton for waist, knees and feet.\n" +
                "It bypasses the old gait-state depth guesses, so a straight kick stays a straight leg.\n" +
                "MediaPipe shoulders, elbows and wrists also drive walking rhythm.\n" +
                "Everything is processed locally; no RTMW fallback or Google metrics uploader runs.");
            container.Controls.Add(_cmbBodyPoseSource);
            _lblBodyPoseSourceStatus = MakeLabel(
                "Mode: World 3D (MediaPipe) | Active: stopped",
                leftMargin + 452,
                y + 2,
                Math.Max(210, rightEdge - leftMargin - 452));
            _lblBodyPoseSourceStatus.ForeColor = Color.FromArgb(145, 165, 175);
            _lblBodyPoseSourceStatus.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(_lblBodyPoseSourceStatus);
            _cmbBodyPoseSource.SelectedIndexChanged += (s, e) => OnBodyPoseSourceChanged();
            y += 26;

            // Display-only alignment for the camera overlay. It never changes
            // metric tracker geometry, classifier input or recorded landmarks.
            container.Controls.Add(MakeLabel(
                "Preview skeleton nudge:  right +",
                leftMargin, y + 2, 185));
            _nudBodyOffX = new NumericUpDown
            {
                Location = new Point(leftMargin + 185, y), Width = 60,
                DecimalPlaces = 1, Increment = 0.5m, Minimum = -20, Maximum = 20, Value = 0,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            _nudBodyOffX.ValueChanged += (s, e) => _bodyOffX = (float)_nudBodyOffX.Value;
            container.Controls.Add(_nudBodyOffX);
            container.Controls.Add(MakeLabel("down +", leftMargin + 255, y + 2, 55));
            _nudBodyOffY = new NumericUpDown
            {
                Location = new Point(leftMargin + 310, y), Width = 60,
                DecimalPlaces = 1, Increment = 0.5m, Minimum = -20, Maximum = 20, Value = 0,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            _nudBodyOffY.ValueChanged += (s, e) => _bodyOffY = (float)_nudBodyOffY.Value;
            container.Controls.Add(_nudBodyOffY);
            container.Controls.Add(MakeLabel("(% of frame, preview only)", leftMargin + 378, y + 2, 160));
            y += 26;

            // ── Left: silhouette / Right: video ──
            int panelH = 330;
            int silW = 240;
            _pnlBodySilhouette = new BodySilhouettePanel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(silW, panelH),
                BackColor = Color.FromArgb(24, 24, 28),
                BorderStyle = BorderStyle.FixedSingle,
            };
            container.Controls.Add(_pnlBodySilhouette);

            _picBodyVideo = new PictureBox
            {
                Location = new Point(leftMargin + silW + 8, y),
                Size = new Size(rightEdge - leftMargin - silW - 8, panelH),
                BackColor = Color.Black,
                SizeMode = PictureBoxSizeMode.Zoom,
                BorderStyle = BorderStyle.FixedSingle,
            };
            container.Controls.Add(_picBodyVideo);
            y += panelH + 8;

            // Labeled motion capture for tuning the camera FBT classifier.
            // This is diagnostics only: it records landmarks and derived leg
            // geometry, never camera frames, and does not alter tracker output.
            var pnlCapture = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 94),
                BackColor = Color.FromArgb(48, 42, 60),
                BorderStyle = BorderStyle.FixedSingle,
            };
            var lblCaptureTitle = MakeLabel("FBT motion calibration capture", 8, 4, pnlCapture.Width - 16);
            lblCaptureTitle.ForeColor = Color.FromArgb(205, 175, 245);
            lblCaptureTitle.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            pnlCapture.Controls.Add(lblCaptureTitle);

            pnlCapture.Controls.Add(MakeLabel("View:", 8, 31, 38));
            _cmbBodyCaptureView = new ComboBox
            {
                Location = new Point(46, 27), Width = 155,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            foreach (var option in BodyCaptureViews) _cmbBodyCaptureView.Items.Add(option.label);
            _cmbBodyCaptureView.SelectedIndex = 0;
            pnlCapture.Controls.Add(_cmbBodyCaptureView);

            pnlCapture.Controls.Add(MakeLabel("Action:", 211, 31, 45));
            _cmbBodyCaptureAction = new ComboBox
            {
                Location = new Point(256, 27), Width = 170,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            foreach (var option in BodyCaptureActions) _cmbBodyCaptureAction.Items.Add(option.label);
            _cmbBodyCaptureAction.SelectedIndex = 0;
            pnlCapture.Controls.Add(_cmbBodyCaptureAction);

            pnlCapture.Controls.Add(MakeLabel("Leg:", 436, 31, 32));
            _cmbBodyCaptureLeg = new ComboBox
            {
                Location = new Point(468, 27), Width = 100,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            foreach (var option in BodyCaptureLegs) _cmbBodyCaptureLeg.Items.Add(option.label);
            _cmbBodyCaptureLeg.SelectedIndex = 0;
            pnlCapture.Controls.Add(_cmbBodyCaptureLeg);

            pnlCapture.Controls.Add(MakeLabel("Take:", 578, 31, 35));
            _nudBodyCaptureTake = new NumericUpDown
            {
                Location = new Point(613, 27), Width = 48,
                Minimum = 1, Maximum = 99, Value = 1,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
            };
            pnlCapture.Controls.Add(_nudBodyCaptureTake);

            _btnBodyCaptureRecord = new ModernPillButton
            {
                Location = new Point(674, 25), Size = new Size(132, 27),
                Text = "Record sample",
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
                ForeColor = Color.White, BackColor = Color.FromArgb(95, 55, 135),
                Cursor = Cursors.Hand,
            };
            _btnBodyCaptureRecord.Click += (s, e) => ToggleBodyCalibrationRecord();
            pnlCapture.Controls.Add(_btnBodyCaptureRecord);

            _lblBodyCaptureStatus = MakeLabel(
                "Start tracking, choose labels, Record, perform one sample, then Stop recording.",
                8, 60, pnlCapture.Width - 16);
            _lblBodyCaptureStatus.ForeColor = Color.FromArgb(175, 165, 190);
            _lblBodyCaptureStatus.Font = new Font("Segoe UI", 8f);
            pnlCapture.Controls.Add(_lblBodyCaptureStatus);
            // Dev-only: the controls stay constructed (other code references
            // them) but the panel is only shown with devtools.on present.
            if (ShowDevTools)
            {
                container.Controls.Add(pnlCapture);
                y += 102;
            }

            // ── FBT mod callout ──
            var pnlFbt = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 46),
                BackColor = Color.FromArgb(45, 55, 40),
                BorderStyle = BorderStyle.FixedSingle,
            };
            var lnkFbt = new LinkLabel
            {
                Location = new Point(8, 4),
                Size = new Size(pnlFbt.Width - 16, 18),
                Text = "Required in-game mod: SkyrimVR FBT - Full Body Tracking with Physics (Nexus) — click to open",
                LinkColor = Color.FromArgb(140, 210, 140),
                ActiveLinkColor = Color.White,
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
            };
            lnkFbt.LinkClicked += (s, e) =>
            {
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(FbtModUrl) { UseShellExecute = true }); }
                catch { }
            };
            pnlFbt.Controls.Add(lnkFbt);
            var lblFbtNote = new Label
            {
                Location = new Point(8, 23),
                Size = new Size(pnlFbt.Width - 16, 18),
                Text = "Needs VRIK + HIGGS + PLANCK. Without it, trackers exist but nothing in-game moves.",
                ForeColor = Color.FromArgb(150, 150, 150),
                Font = new Font("Segoe UI", 8f),
            };
            pnlFbt.Controls.Add(lblFbtNote);
            container.Controls.Add(pnlFbt);
            y += 54;

            // Live controller trim for the public camera foot targets. Physical
            // Vive/Tundra trackers bypass this feature in the native runtime.
            var pnlLegCal = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 118),
                BackColor = Color.FromArgb(50, 43, 58),
                BorderStyle = BorderStyle.FixedSingle,
            };
            var lblLegCalTitle = MakeLabel("Live Camera Foot Placement + Lift Range (in game)", 8, 4, pnlLegCal.Width - 16);
            lblLegCalTitle.ForeColor = Color.FromArgb(205, 175, 245);
            lblLegCalTitle.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            pnlLegCal.Controls.Add(lblLegCalTitle);

            var lblLegCalBody = new Label
            {
                Location = new Point(8, 24),
                Size = new Size(pnlLegCal.Width - 16, 52),
                Text = "TRIPLE-TAP RIGHT A to enter/exit; OCU blocks game input while editing and auto-saves. Stick-clicks are never used.\n"
                     + "LEFT STICK controls the LEFT foot; RIGHT STICK controls the RIGHT foot. Normal stick moves left/right and forward/back.\n"
                     + "Hold that side's GRIP + stick to ROTATE its foot (X turns, Y pivots). LEFT X or either TRIGGER + stick Y adjusts LIFT RANGE. Modifier + triple RIGHT A resets both legs.",
                ForeColor = Color.FromArgb(195, 185, 205),
                Font = new Font("Segoe UI", 8.25f),
                AutoSize = false,
            };
            pnlLegCal.Controls.Add(lblLegCalBody);

            _chkCameraLegCalibration = MakeCheckBox(
                "Arm camera-leg calibration controls (Save + game restart required)", 8, 84);
            _chkCameraLegCalibration.Checked = ShowDevTools;
            _chkCameraLegCalibration.ForeColor = Color.FromArgb(190, 160, 235);
            pnlLegCal.Controls.Add(_chkCameraLegCalibration);

            var btnResetLegCal = MakeButton("Reset saved trim", pnlLegCal.Width - 152, 80, 142, 28);
            btnResetLegCal.BackColor = Color.FromArgb(90, 55, 105);
            btnResetLegCal.ForeColor = Color.White;
            btnResetLegCal.Click += (s, e) => ResetSavedCameraLegCalibration();
            pnlLegCal.Controls.Add(btnResetLegCal);
            if (ShowDevTools)
            {
                container.Controls.Add(pnlLegCal);
                y += 126;
            }

            // ── Full-body walking (walk-in-place locomotion) ──
            var pnlWalk = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 142),
                BackColor = Color.FromArgb(40, 52, 60),
                BorderStyle = BorderStyle.FixedSingle,
            };
            var lblWalkTitle = new Label
            {
                Location = new Point(8, 4),
                Size = new Size(pnlWalk.Width - 16, 18),
                Text = "Full-Body Walking (walk in place to move)",
                ForeColor = Color.FromArgb(140, 200, 240),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
            };
            pnlWalk.Controls.Add(lblWalkTitle);
            var lblWalkBody = new Label
            {
                Location = new Point(8, 23),
                Size = new Size(pnlWalk.Width - 16, 52),
                Text = "FORWARD - Skeleton ankles/knees + opposite shoulder-elbow-wrist swings drive gait; a selected hold button lowers the first-step gate.\n"
                     + "BACKWARD - Keep both skeleton hands near chin height while marching; lower them to go forward again.\n"
                     + "TURN - Point the right controller's head right, or the left controller's head left (palm toward the camera). Keep the other hand normal.",
                ForeColor = Color.FromArgb(175, 190, 200),
                Font = new Font("Segoe UI", 8.5f),
                AutoSize = false
            };
            pnlWalk.Controls.Add(lblWalkBody);
            _chkWalkInPlace = MakeCheckBox("Enable Full-Body Walking (Save + game restart required)", 8, 76);
            _chkWalkInPlace.ForeColor = Color.FromArgb(140, 210, 240);
            _chkWalkInPlace.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            pnlWalk.Controls.Add(_chkWalkInPlace);

            pnlWalk.Controls.Add(MakeLabel("Hold to activate:", 8, 106, 102));
            _cmbWalkActivation = new ComboBox
            {
                DropDownStyle = ComboBoxStyle.DropDownList,
                Location = new Point(112, 102),
                Width = 300,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
            };
            RefreshWalkActivationOptions();
            new ToolTip().SetToolTip(_cmbWalkActivation,
                "The physical buttons follow the Meta Touch or Index model selected on the Bindings page.\n" +
                "Hold this exact button to enable walking and get a quicker first step.\n" +
                "None requires no button and uses the normal rhythm gate.");
            pnlWalk.Controls.Add(_cmbWalkActivation);
            var lblWalkActivation = MakeLabel("Each hand/button is separate; controller mappings still resolve through the active OpenXR profile.", 424, 105, pnlWalk.Width - 432);
            lblWalkActivation.ForeColor = Color.FromArgb(145, 165, 175);
            lblWalkActivation.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            pnlWalk.Controls.Add(lblWalkActivation);
            container.Controls.Add(pnlWalk);
            y += 150;

            // ── Lighting guidance (field-tested: front light = night and day) ──
            var pnlLight = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 64),
                BackColor = Color.FromArgb(55, 50, 38),
                BorderStyle = BorderStyle.FixedSingle,
            };
            var lblLightTitle = new Label
            {
                Location = new Point(8, 4),
                Size = new Size(pnlLight.Width - 16, 18),
                Text = "Lighting makes or breaks tracking quality",
                ForeColor = Color.FromArgb(235, 200, 120),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
            };
            pnlLight.Controls.Add(lblLightTitle);
            var lblLightBody = new Label
            {
                Location = new Point(8, 23),
                Size = new Size(pnlLight.Width - 16, 36),
                Text = "Light yourself EVENLY and DIRECTLY from the front - a lamp near the camera, pointed at you. Side lighting and "
                     + "backlighting (a window or lamp behind you) confuse the AI and cause jitter; remove them from the equation. "
                     + "Don't shine light into the camera lens itself.",
                ForeColor = Color.FromArgb(200, 190, 165),
                Font = new Font("Segoe UI", 8.5f),
                AutoSize = false
            };
            pnlLight.Controls.Add(lblLightBody);
            container.Controls.Add(pnlLight);
            y += 72;

            container.Size = new Size(container.Width, y + 6);

            // Silhouette repaint + KAT presence poll while the tab is alive
            var uiTimer = new System.Windows.Forms.Timer { Interval = 100 };
            int katPollCounter = 0;
            uiTimer.Tick += (s, e) =>
            {
                if (!_tabBody.Visible) return;
                if (++katPollCounter >= 50) // every 5s
                {
                    katPollCounter = 0;
                    try
                    {
                        _bodyKatDetected =
                            System.Diagnostics.Process.GetProcessesByName("KAT Gateway").Length > 0
                            || System.Diagnostics.Process.GetProcessesByName("KATGateway").Length > 0
                            || System.Diagnostics.Process.GetProcessesByName("KAT Gateway Core").Length > 0;
                    }
                    catch { _bodyKatDetected = false; }
                }
                lock (_bodyKpLock)
                {
                    _pnlBodySilhouette.UpdateState(_bodyKp, _bodyPoseValid, _bodyKatDetected);
                }
                _pnlBodySilhouette.Invalidate();
                UpdateBodyPoseSourceStatus();
            };
            uiTimer.Start();

            _tabBody.VisibleChanged += (s, e) => UpdateBodyPreviewActivity();
            Resize += (s, e) => UpdateBodyPreviewActivity();
            FormClosing += (s, e) => SaveBodyUi();
            FormClosed += (s, e) => StopBodyCapture();

            LoadBodyUi();
            _bodyUiInitialized = true;
            UpdateBodyPoseSourceStatus();
            UpdateBodyPreviewActivity();
        }

        private void ResetSavedCameraLegCalibration()
        {
            try
            {
                var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                if (!string.IsNullOrWhiteSpace(_gameDir) && Directory.Exists(_gameDir))
                    paths.Add(Path.Combine(_gameDir, "camera_leg_calibration.ini"));

                // Under MO2 the runtime writes this game-root file through
                // USVFS, so its physical home is <instance>\overwrite\Root.
                // Reset both locations; deleting only the real game path leaves
                // the active overwrite value untouched and makes Reset appear
                // to work when it did nothing.
                string modDir = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\', '/');
                string modsDir = Path.GetDirectoryName(modDir) ?? "";
                if (string.Equals(Path.GetFileName(modsDir), "mods", StringComparison.OrdinalIgnoreCase))
                {
                    string instanceDir = Path.GetDirectoryName(modsDir) ?? "";
                    if (instanceDir.Length > 0)
                        paths.Add(Path.Combine(instanceDir, "overwrite", "Root", "camera_leg_calibration.ini"));
                }

                if (paths.Count == 0)
                    throw new DirectoryNotFoundException("Select the Skyrim VR game folder first.");
                foreach (string path in paths)
                    if (File.Exists(path))
                        File.Delete(path);
                _lblBodyStatus.Text = "Saved camera leg trim reset (restart game).";
                _lblBodyStatus.ForeColor = Color.FromArgb(135, 220, 150);
            }
            catch (Exception ex)
            {
                _lblBodyStatus.Text = $"Leg trim reset failed: {ex.Message}";
                _lblBodyStatus.ForeColor = Color.FromArgb(255, 115, 115);
            }
        }

        private string WalkActivationKey()
        {
            if (_cmbWalkActivation == null || _cmbWalkActivation.SelectedIndex < 0)
                return "none";
            var opts = CurrentHoldOptions;
            int i = _cmbWalkActivation.SelectedIndex;
            return i < opts.Length && opts[i].id.Length > 0 ? opts[i].id : "none";
        }

        // Keep walking on the exact same per-controller/per-hand button model
        // used by the Gestures and Bindings pages instead of maintaining a
        // second generic button vocabulary.
        private void RefreshWalkActivationOptions()
        {
            if (_cmbWalkActivation == null) return;
            string keep = WalkActivationKey();
            _cmbWalkActivation.Items.Clear();
            foreach (var opt in CurrentHoldOptions)
                _cmbWalkActivation.Items.Add(opt.id.Length == 0 ? "None (walking is always active)" : opt.label);
            int idx = Array.FindIndex(CurrentHoldOptions, o => o.id == keep);
            _cmbWalkActivation.SelectedIndex = idx >= 0 ? idx : 0;
        }

        private void SelectWalkActivation(string key)
        {
            string selected = key.Trim().ToLowerInvariant() switch
            {
                // Migrate the old hand-agnostic choices without breaking an
                // existing config. New saves always carry an exact hand.
                "grip" => "r_grip",
                "trigger" => "r_trigger",
                "primary" => "r_a",
                "secondary" => "r_b",
                "thumbstick" => "r_stick",
                "trackpad" => "r_trackpad",
                "none" => "",
                _ => key.Trim().ToLowerInvariant(),
            };
            int idx = Array.FindIndex(CurrentHoldOptions, o => o.id == selected);
            _cmbWalkActivation.SelectedIndex = idx >= 0 ? idx : 0;
        }

        // ── Tab-state persistence (BodyTracking\ui.json beside the exe) ──
        // Camera choice/URL and tuning survive relaunches; saved on Start and
        // on close so a fresh session comes up ready to go.

        private sealed class BodyUiState
        {
            public int CameraChoicesVersion { get; set; } = 0;
            public int CamIndex { get; set; } = 0;
            public string CamUrl { get; set; } = "";
            public int HeightCm { get; set; } = 175;
            public bool Mirror { get; set; } = true;
            public bool Stream { get; set; } = true;
            public bool Preview { get; set; } = true;
            public bool SkeletonOnly { get; set; } = false;
            public bool Gpu { get; set; } = true; // legacy; superseded by Device
            public string Device { get; set; } = ""; // "auto" | "cpu" | "gpu<N>"
            public string PoseSource { get; set; } = "world3d"; // Legacy values are ignored on load.
            public float OffX { get; set; }
            public float OffY { get; set; }
        }

        private string BodyUiPath => Path.Combine(AppContext.BaseDirectory, "BodyTracking", "ui.json");

        private void OnBodyPoseSourceChanged()
        {
            if (_cmbBodyPoseSource == null || _cmbBodyPoseSource.SelectedIndex < 0)
                return;

            if (_bodyUiInitialized)
                SaveBodyUi();
            UpdateBodyPoseSourceStatus();
        }

        private void UpdateBodyPoseSourceStatus()
        {
            if (_lblBodyPoseSourceStatus == null || _cmbBodyPoseSource == null)
                return;

            if (!_bodyCapRunning)
            {
                _lblBodyPoseSourceStatus.Text = "Mode: World 3D (MediaPipe) | Active: stopped";
                _lblBodyPoseSourceStatus.ForeColor = Color.FromArgb(145, 165, 175);
                return;
            }

            BodyTrackerSource active = (BodyTrackerSource)Volatile.Read(ref _bodyActiveTrackerSource);
            string activeText = active switch
            {
                BodyTrackerSource.Continuous3D => "tracking",
                _ => "waiting for pose",
            };
            _lblBodyPoseSourceStatus.Text = $"Mode: World 3D (MediaPipe) | Active: {activeText}";
            _lblBodyPoseSourceStatus.ForeColor = active switch
            {
                BodyTrackerSource.Continuous3D => Color.FromArgb(105, 215, 255),
                _ => Color.FromArgb(175, 165, 190),
            };
        }

        private void SaveBodyUi()
        {
            if (!ShowDevTools) return;
            try
            {
                var st = new BodyUiState
                {
                    CameraChoicesVersion = 2,
                    CamIndex = _cmbBodyCamera.SelectedIndex,
                    CamUrl = _txtBodyCamUrl.Text,
                    HeightCm = (int)(BodyReferenceHeightM * 100f),
                    Mirror = _chkBodyMirror.Checked,
                    Stream = _chkBodyStream.Checked,
                    Preview = _chkBodyPreview.Checked,
                    SkeletonOnly = _chkBodySkeletonOnly.Checked,
                    Gpu = false,
                    Device = "cpu",
                    PoseSource = "world3d",
                    OffX = (float)_nudBodyOffX.Value,
                    OffY = (float)_nudBodyOffY.Value,
                };
                Directory.CreateDirectory(Path.GetDirectoryName(BodyUiPath)!);
                File.WriteAllText(BodyUiPath, System.Text.Json.JsonSerializer.Serialize(st));
            }
            catch { }
        }

        private void LoadBodyUi()
        {
            try
            {
                if (!File.Exists(BodyUiPath)) return;
                var st = System.Text.Json.JsonSerializer.Deserialize<BodyUiState>(File.ReadAllText(BodyUiPath));
                if (st == null) return;
                // Old builds stored 0..3 for four separate webcams and 4 for
                // IP camera. Collapse all legacy webcam indices to Webcam;
                // version 2 stores the new two-choice 0/1 layout directly.
                _cmbBodyCamera.SelectedIndex = st.CameraChoicesVersion >= 2
                    ? (st.CamIndex == 1 ? 1 : 0)
                    : (st.CamIndex == 4 ? 1 : 0);
                if (!string.IsNullOrWhiteSpace(st.CamUrl))
                    _txtBodyCamUrl.Text = st.CamUrl;
                // Height is now an in-game HMD calibration. Ignore legacy
                // hidden values here; older ui.json files could silently force
                // the camera solver as low as 120 cm.
                _nudBodyHeightCm.Value = (decimal)(BodyReferenceHeightM * 100f);
                _chkBodyMirror.Checked = st.Mirror;
                _chkBodyStream.Checked = st.Stream;
                _chkBodyPreview.Checked = st.Preview;
                _chkBodySkeletonOnly.Checked = st.SkeletonOnly;
                // World3D-only builds ignore the old RTMW device/source choice,
                // but preserve the user's display-only skeleton alignment.
                _cmbBodyDevice.SelectedIndex = 0;
                _cmbBodyPoseSource.SelectedIndex = 0;
                _nudBodyOffX.Value = (decimal)Math.Clamp(st.OffX, -20f, 20f);
                _nudBodyOffY.Value = (decimal)Math.Clamp(st.OffY, -20f, 20f);
                _bodyOffX = (float)_nudBodyOffX.Value;
                _bodyOffY = (float)_nudBodyOffY.Value;
            }
            catch { }
        }

        private void UpdateBodyPreviewActivity()
        {
            if (_chkBodyPreview == null || _tabBody == null || _picBodyVideo == null)
                return;

            bool active = _chkBodyPreview.Checked
                && _tabBody.Visible
                && WindowState != FormWindowState.Minimized;
            _bodyPreviewActive = active;

            if (!active)
            {
                Interlocked.Exchange(ref _bodyPreviewLastFrameMs, 0);
                var old = _picBodyVideo.Image;
                _picBodyVideo.Image = null;
                old?.Dispose();
            }
        }

        private void ToggleBodyCapture()
        {
            if (_bodyCapRunning) { StopBodyCapture(); return; }

            _bodyOscPort = 9000;
            if (int.TryParse(_ini.Get("input", "networkTrackerPort", _ini.Get("", "networkTrackerPort", "9000")), out int p))
                _bodyOscPort = p;

            int camIndex = _cmbBodyCamera.SelectedIndex;
            string source = camIndex == 1 ? _txtBodyCamUrl.Text.Trim() : "0";
            Volatile.Write(ref _bodyActiveTrackerSource, (int)BodyTrackerSource.None);
            SaveBodyUi(); // a successful Start config is worth remembering

            _bodyCalibrationFileName = "";
            _bodyCapRunning = true;
            int captureGeneration = Interlocked.Increment(ref _bodyCaptureGeneration);
            _btnBodyStartStop.Text = "Stop";
            _btnBodyStartStop.BackColor = Color.FromArgb(150, 45, 45);
            _lblBodyStatus.Text = "Opening camera...";
            _lblBodyStatus.ForeColor = Color.FromArgb(220, 200, 120);
            UpdateBodyPoseSourceStatus();

            _bodyCapThread = new Thread(() => BodyCaptureLoop(source, captureGeneration)) { IsBackground = true };
            _bodyCapThread.Start();
        }

        private void ToggleBodyCalibrationRecord()
        {
            if (_bodyCalibrationRecording)
            {
                _bodyCalibrationRecording = false;
                _btnBodyCaptureRecord.Text = "Record sample";
                _btnBodyCaptureRecord.BackColor = Color.FromArgb(95, 55, 135);
                _cmbBodyCaptureView.Enabled = true;
                _cmbBodyCaptureAction.Enabled = true;
                _cmbBodyCaptureLeg.Enabled = true;
                _nudBodyCaptureTake.Enabled = true;
                _lblBodyCaptureStatus.Text = string.IsNullOrEmpty(_bodyCalibrationFileName)
                    ? "Sample stopped."
                    : $"Sample saved in BodyTracking\\Captures\\{_bodyCalibrationFileName}";
                _lblBodyCaptureStatus.ForeColor = Color.FromArgb(170, 210, 170);
                return;
            }

            BodyTrackerSource activeSource =
                (BodyTrackerSource)Volatile.Read(ref _bodyActiveTrackerSource);
            if (!_bodyCapRunning
                || !_bodyTrackingStreamReady
                || activeSource != BodyTrackerSource.Continuous3D)
            {
                _lblBodyCaptureStatus.Text = "Wait for MediaPipe World3D: the full body and both feet must be locked before recording.";
                _lblBodyCaptureStatus.ForeColor = Color.FromArgb(245, 175, 105);
                return;
            }

            int view = Math.Clamp(_cmbBodyCaptureView.SelectedIndex, 0, BodyCaptureViews.Length - 1);
            int action = Math.Clamp(_cmbBodyCaptureAction.SelectedIndex, 0, BodyCaptureActions.Length - 1);
            int leg = Math.Clamp(_cmbBodyCaptureLeg.SelectedIndex, 0, BodyCaptureLegs.Length - 1);
            _bodyCalibrationView = BodyCaptureViews[view].id;
            _bodyCalibrationAction = BodyCaptureActions[action].id;
            _bodyCalibrationLeg = BodyCaptureLegs[leg].id;
            Volatile.Write(ref _bodyCalibrationTake, (int)_nudBodyCaptureTake.Value);
            Interlocked.Increment(ref _bodyCalibrationSegment);
            _bodyCalibrationRecording = true;

            _cmbBodyCaptureView.Enabled = false;
            _cmbBodyCaptureAction.Enabled = false;
            _cmbBodyCaptureLeg.Enabled = false;
            _nudBodyCaptureTake.Enabled = false;
            _btnBodyCaptureRecord.Text = "Stop recording";
            _btnBodyCaptureRecord.BackColor = Color.FromArgb(155, 45, 65);
            _lblBodyCaptureStatus.Text = $"RECORDING: {BodyCaptureViews[view].label} / {BodyCaptureActions[action].label} / {BodyCaptureLegs[leg].label}";
            _lblBodyCaptureStatus.ForeColor = Color.FromArgb(255, 135, 155);
        }

        private void StopBodyCalibrationRecord(string status)
        {
            _bodyCalibrationRecording = false;
            if (_btnBodyCaptureRecord == null || IsDisposed) return;
            _btnBodyCaptureRecord.Text = "Record sample";
            _btnBodyCaptureRecord.BackColor = Color.FromArgb(95, 55, 135);
            _cmbBodyCaptureView.Enabled = true;
            _cmbBodyCaptureAction.Enabled = true;
            _cmbBodyCaptureLeg.Enabled = true;
            _nudBodyCaptureTake.Enabled = true;
            _lblBodyCaptureStatus.Text = status;
            _lblBodyCaptureStatus.ForeColor = Color.FromArgb(175, 165, 190);
        }

        private void BodySetCaptureStatus(string text, Color color)
        {
            try
            {
                BeginInvoke(() =>
                {
                    if (_lblBodyCaptureStatus == null || IsDisposed) return;
                    _lblBodyCaptureStatus.Text = text;
                    _lblBodyCaptureStatus.ForeColor = color;
                });
            }
            catch { }
        }

        private void StopBodyCapture()
        {
            _bodyCapRunning = false;
            Interlocked.Increment(ref _bodyCaptureGeneration);
            _bodyTrackingStreamReady = false;
            Volatile.Write(ref _bodyActiveTrackerSource, (int)BodyTrackerSource.None);
            StopBodyCalibrationRecord("Tracking stopped. Calibration file was closed safely.");
            Thread? stoppingThread = _bodyCapThread;
            try { stoppingThread?.Join(1500); } catch { }
            if (ReferenceEquals(_bodyCapThread, stoppingThread))
                _bodyCapThread = null;
            // A normally exiting capture thread clears and detaches its own
            // socket in finally. If it is still blocked after the bounded join,
            // claim that current socket here, clear it, and prevent the delayed
            // old session from clearing any replacement session later.
            UdpClient? stoppingOsc = ClearBodyOscSession(expectedSession: null);
            try { stoppingOsc?.Dispose(); } catch { }
            _bodyPoseValid = false;
            var oldPreview = _picBodyVideo?.Image;
            if (_picBodyVideo != null)
                _picBodyVideo.Image = null;
            oldPreview?.Dispose();
            if (_btnBodyStartStop != null && !IsDisposed)
            {
                _btnBodyStartStop.Text = "Start";
                _btnBodyStartStop.BackColor = Color.FromArgb(40, 120, 40);
                _lblBodyStatus.Text = "Idle";
                _lblBodyStatus.ForeColor = Color.FromArgb(160, 160, 160);
            }
            UpdateBodyPoseSourceStatus();
        }

        private bool IsBodyCaptureSessionActive(int captureGeneration) =>
            _bodyCapRunning
            && Volatile.Read(ref _bodyCaptureGeneration) == captureGeneration;

        private void BodySetStatus(string text, Color color)
        {
            try
            {
                BeginInvoke(() => { _lblBodyStatus.Text = text; _lblBodyStatus.ForeColor = color; });
            }
            catch { }
        }

        // ── Capture worker ────────────────────────────────────────────────

        private void BodyCaptureLoop(string source, int captureGeneration)
        {
            OpenCvSharp.VideoCapture? cap = null;
            StreamWriter? diag = null;
            World3DCalibrationRecorder? world3dCalibrationRecorder = null;
            Thread? grabberThread = null;
            MediaPipePoseWorker? world3dWorker = null;
            UdpClient? sessionOsc = null;
            string trackingStatusText = "Tracking";

            try
            {
                cap = int.TryParse(source, out int idx)
                    ? new OpenCvSharp.VideoCapture(idx, OpenCvSharp.VideoCaptureAPIs.DSHOW)
                    : new OpenCvSharp.VideoCapture(source);
                string cameraFormatNote = "";
                if (cap.IsOpened() && int.TryParse(source, out _))
                {
                    // Without format hints DSHOW negotiates uncompressed YUY2 at
                    // the camera's maximum resolution, which USB bandwidth caps
                    // at 5-7.5 fps (measured live: 141 ms frame gaps with only
                    // ~47 ms pipeline age). MJPG at 720p keeps webcams at their
                    // full frame rate; MediaPipe downscales internally, so
                    // capture resolution above 720p adds nothing but latency.
                    try
                    {
                        cap.Set(OpenCvSharp.VideoCaptureProperties.FourCC,
                            OpenCvSharp.VideoWriter.FourCC('M', 'J', 'P', 'G'));
                        cap.Set(OpenCvSharp.VideoCaptureProperties.FrameWidth, 1280);
                        cap.Set(OpenCvSharp.VideoCaptureProperties.FrameHeight, 720);
                        cap.Set(OpenCvSharp.VideoCaptureProperties.Fps, 30);
                        double negotiated = cap.Get(OpenCvSharp.VideoCaptureProperties.Fps);
                        if (negotiated > 0 && negotiated < 15)
                        {
                            // The camera refused 30 fps at 720p; USB bandwidth
                            // or driver caps often lift at a lower resolution.
                            cap.Set(OpenCvSharp.VideoCaptureProperties.FrameWidth, 640);
                            cap.Set(OpenCvSharp.VideoCaptureProperties.FrameHeight, 480);
                            cap.Set(OpenCvSharp.VideoCaptureProperties.Fps, 30);
                        }
                        int fcc = (int)cap.Get(OpenCvSharp.VideoCaptureProperties.FourCC);
                        string fourcc = new string(new[]
                        {
                            (char)(fcc & 255), (char)((fcc >> 8) & 255),
                            (char)((fcc >> 16) & 255), (char)((fcc >> 24) & 255),
                        });
                        cameraFormatNote = string.Format(" | cam {0:F0}x{1:F0}@{2:F0} {3}",
                            cap.Get(OpenCvSharp.VideoCaptureProperties.FrameWidth),
                            cap.Get(OpenCvSharp.VideoCaptureProperties.FrameHeight),
                            cap.Get(OpenCvSharp.VideoCaptureProperties.Fps),
                            fourcc);
                    }
                    catch { }
                }
                if (!cap.IsOpened())
                {
                    if (IsBodyCaptureSessionActive(captureGeneration))
                    {
                        BodySetStatus("Camera failed to open: " + source, Color.FromArgb(255, 120, 120));
                        _bodyCapRunning = false;
                        BeginInvoke(() =>
                        {
                            if (Volatile.Read(ref _bodyCaptureGeneration) == captureGeneration)
                                StopBodyCapture();
                        });
                    }
                    return;
                }

                uint captureEpoch = (uint)(Environment.TickCount64
                    % OscTrackerFrameContract.MaximumExactFloatInteger);
                if (captureEpoch == 0) captureEpoch = 1;
                uint world3dSourceEpoch = captureEpoch == OscTrackerFrameContract.MaximumExactFloatInteger
                    ? 1u : captureEpoch + 1u;
                var trackerMux = new BodyTrackerFrameMux();
                trackerMux.Reset(captureEpoch);
                var world3dLegClassifier = new World3DLegMotionClassifier();
                var world3dCaptureDeriver = new World3DLegCaptureDeriver(
                    new World3DLegCaptureDeriverOptions
                    {
                        MinimumLandmarkConfidence = 0.25f,
                    });
                MediaPipePoseWorkerFrameSource? world3dSource = null;
                MediaPipe33TrackerConverter? world3dConverter = null;
                WorldLandmarkFrame? latestWorldLandmarkFrame = null;
                BodyTrackerFrame? latestWorld3dFrame = null;
                CameraLowerBodyClassification latestWorld3dLegClassification = default;
                World3DLegCapturePair latestWorld3dCaptureLegs = default;
                long lastConvertedWorld3dSequence = -1;
                long lastPublishedWorld3dSequence = -1;
                long lastRecordedWorld3dSequence = -1;
                BodyTrackerSource lastPublishedSource = BodyTrackerSource.None;
                bool world3dFaultReported = false;
                bool world3dArmCenterValid = false;
                float world3dArmCenter = 0f;

                if (_bodyUseWorld3D)
                {
                    try
                    {
                        world3dWorker = new MediaPipePoseWorker(
                            new MediaPipePoseLandmarkerOptions
                            {
                                MinimumPoseDetectionConfidence = 0.40f,
                                MinimumPosePresenceConfidence = 0.30f,
                                MinimumTrackingConfidence = 0.30f,
                            });
                        world3dSource = new MediaPipePoseWorkerFrameSource(
                            world3dWorker, world3dSourceEpoch);
                        world3dConverter = new MediaPipe33TrackerConverter(
                            new MediaPipe33TrackerConverterOptions
                            {
                                // Feet lose visibility while aimed into the
                                // camera. Presence and direct 3D geometry stay
                                // useful below the old 2D confidence gate.
                                MinimumLandmarkConfidence = 0.25f,
                                IncludeAuxiliaryTrackers = true,
                            });
                        trackingStatusText = "Tracking (World 3D Lite, MediaPipe CPU)" + cameraFormatNote;
                        BodySetStatus(trackingStatusText, Color.FromArgb(120, 220, 120));
                    }
                    catch (Exception ex)
                    {
                        try { world3dWorker?.Dispose(); } catch { }
                        world3dWorker = null;
                        world3dSource = null;
                        world3dConverter = null;
                        trackingStatusText += " - World 3D unavailable; tracker output paused";
                        BodySetStatus(trackingStatusText + ": " + ex.Message,
                            Color.FromArgb(255, 175, 90));
                    }
                }

                sessionOsc = new UdpClient();
                sessionOsc.Connect(IPAddress.Loopback, _bodyOscPort);
                lock (_bodyOscGate)
                {
                    if (!IsBodyCaptureSessionActive(captureGeneration))
                        return;
                    _bodyOsc = sessionOsc;
                }

                // Anti-lag capture: a dedicated grabber thread keeps ONLY the
                // newest frame. Without this, the capture buffer queues frames
                // whenever inference runs slower than the camera, and the
                // preview drifts seconds behind reality.
                try { cap.Set(OpenCvSharp.VideoCaptureProperties.BufferSize, 1); } catch { }
                OpenCvSharp.Mat? latestFrame = null;
                object latestLock = new object();
                OpenCvSharp.VideoCapture grabberCapture = cap;
                var grabber = new Thread(() =>
                {
                    try
                    {
                        using var g = new OpenCvSharp.Mat();
                        long fpsProbeStartMs = Environment.TickCount64;
                        int fpsProbeFrames = 0;
                        bool exposureOverrideDecided = false;
                        while (IsBodyCaptureSessionActive(captureGeneration))
                        {
                            if (!grabberCapture.Read(g) || g.Empty())
                            {
                                Thread.Sleep(5);
                                continue;
                            }
                            fpsProbeFrames++;
                            long probeElapsed = Environment.TickCount64 - fpsProbeStartMs;
                            if (!exposureOverrideDecided && probeElapsed > 3000)
                            {
                                exposureOverrideDecided = true;
                                double measuredFps = fpsProbeFrames * 1000.0 / probeElapsed;
                                if (measuredFps < 15.0)
                                {
                                    // Delivery is slow despite the negotiated
                                    // format: the usual culprit is low-light
                                    // auto-exposure holding the shutter open
                                    // for 130-200 ms. Cap it at ~1/32 s; the
                                    // image gets darker but arrives on time.
                                    // More room light beats this override.
                                    try
                                    {
                                        grabberCapture.Set(OpenCvSharp.VideoCaptureProperties.AutoExposure, 0.25);
                                        grabberCapture.Set(OpenCvSharp.VideoCaptureProperties.Exposure, -5);
                                        BodySetStatus(string.Format(
                                            "Camera delivered {0:F0} fps - forced 1/32s shutter (low light?). Add room light for best tracking.",
                                            measuredFps), Color.FromArgb(255, 190, 90));
                                    }
                                    catch { }
                                }
                            }
                            var clone = g.Clone();
                            lock (latestLock)
                            {
                                latestFrame?.Dispose();
                                latestFrame = clone;
                            }
                        }
                    }
                    catch (Exception ex)
                    {
                        if (IsBodyCaptureSessionActive(captureGeneration))
                            BodySetStatus("Camera read failed: " + ex.Message, Color.FromArgb(255, 120, 120));
                    }
                    finally
                    {
                        lock (latestLock)
                        {
                            latestFrame?.Dispose();
                            latestFrame = null;
                        }
                        try { grabberCapture.Release(); grabberCapture.Dispose(); } catch { }
                    }
                })
                { IsBackground = true };
                grabberThread = grabber;
                grabber.Start();
                cap = null; // ownership moved to the grabber thread

                using var world3dScaled = new OpenCvSharp.Mat();
                using var world3dRgb = new OpenCvSharp.Mat();
                long world3dDiagLastWriteMs = 0;
                long world3dDiagLastFlushMs = 0;
                bool calibrationWasRecording = false;
                long lastInferenceStartMs = 0;
                try
                {
                    string diagnosticDir = Path.Combine(AppContext.BaseDirectory, "BodyTracking");
                    Directory.CreateDirectory(diagnosticDir);
                    diag = new StreamWriter(
                        Path.Combine(diagnosticDir, "world3d_diag.csv"), false);
                    diag.WriteLine(
                        "harvest_now_ms;sequence;frame_timestamp_ms;harvest_age_ms;source_epoch;active;core_valid;" +
                        "l_state;l_confidence;r_state;r_confidence;" +
                        "waist_x;waist_y;waist_z;lfoot_x;lfoot_y;lfoot_z;rfoot_x;rfoot_y;rfoot_z;" +
                        "lhip_x;lhip_y;lhip_z;lhip_conf;lknee_x;lknee_y;lknee_z;lknee_conf;" +
                        "lankle_x;lankle_y;lankle_z;lankle_conf;" +
                        "rhip_x;rhip_y;rhip_z;rhip_conf;rknee_x;rknee_y;rknee_z;rknee_conf;" +
                        "rankle_x;rankle_y;rankle_z;rankle_conf");
                    diag.Flush();
                }
                catch { }

                while (IsBodyCaptureSessionActive(captureGeneration))
                {
                    // The source is at most 30 useful frames per second. Let the
                    // grabber keep replacing its one-frame slot while we wait,
                    // then infer the newest image instead of processing duplicates.
                    long inferenceNow = Environment.TickCount64;
                    int inferenceWait = (int)(BodyInferenceIntervalMs - (inferenceNow - lastInferenceStartMs));
                    if (lastInferenceStartMs != 0 && inferenceWait > 0)
                    {
                        Thread.Sleep(inferenceWait);
                        continue;
                    }

                    // Take the newest frame the grabber has; never a queued one
                    OpenCvSharp.Mat? take = null;
                    lock (latestLock)
                    {
                        if (latestFrame != null) { take = latestFrame; latestFrame = null; }
                    }
                    if (take == null) { Thread.Sleep(5); continue; }
                    using var frame = take;
                    if (frame.Empty()) { Thread.Sleep(5); continue; }
                    lastInferenceStartMs = Environment.TickCount64;
                    // NOTE: inference always runs on the RAW frame; the Mirror
                    // checkbox only flips the final preview bitmap. (Inferring
                    // on a pre-mirrored frame shifted the model's placement.)

                    if (world3dWorker != null && world3dSource != null)
                    {
                        try
                        {
                            const int maxWorld3dInputEdge = 960;
                            OpenCvSharp.Mat worldInput = frame;
                            int longestEdge = Math.Max(frame.Width, frame.Height);
                            if (longestEdge > maxWorld3dInputEdge)
                            {
                                float scale = maxWorld3dInputEdge / (float)longestEdge;
                                int width = Math.Max(1, (int)MathF.Round(frame.Width * scale));
                                int height = Math.Max(1, (int)MathF.Round(frame.Height * scale));
                                OpenCvSharp.Cv2.Resize(frame, world3dScaled,
                                    new OpenCvSharp.Size(width, height),
                                    0, 0, OpenCvSharp.InterpolationFlags.Area);
                                worldInput = world3dScaled;
                            }

                            OpenCvSharp.ColorConversionCodes conversion = worldInput.Channels() switch
                            {
                                1 => OpenCvSharp.ColorConversionCodes.GRAY2RGB,
                                4 => OpenCvSharp.ColorConversionCodes.BGRA2RGB,
                                _ => OpenCvSharp.ColorConversionCodes.BGR2RGB,
                            };
                            OpenCvSharp.Cv2.CvtColor(worldInput, world3dRgb, conversion);
                            world3dSource.UpdateInputMetadata(
                                world3dRgb.Width, world3dRgb.Height, inputMirrored: false);
                            world3dWorker.TrySubmitRgb24(
                                world3dRgb.Data,
                                world3dRgb.Width,
                                world3dRgb.Height,
                                checked((int)world3dRgb.Step()),
                                lastInferenceStartMs);
                        }
                        catch (Exception ex)
                        {
                            if (!world3dFaultReported)
                            {
                                world3dFaultReported = true;
                                BodySetStatus("World 3D input failed; tracker output paused: " + ex.Message,
                                    Color.FromArgb(255, 175, 90));
                            }
                        }
                    }

                    // Stop/restart may happen while native inference is blocked.
                    // Never let an obsolete session publish into the new one.
                    if (!IsBodyCaptureSessionActive(captureGeneration))
                        break;

                    long trackerNowMs = Environment.TickCount64;
                    if (world3dSource != null && world3dConverter != null
                        && world3dSource.TryGetLatestFrame(out WorldLandmarkFrame worldLandmarks)
                        && worldLandmarks.Sequence != lastConvertedWorld3dSequence)
                    {
                        try
                        {
                            latestWorldLandmarkFrame = worldLandmarks;
                            latestWorld3dFrame = world3dConverter.Convert(worldLandmarks);
                            latestWorld3dLegClassification = world3dLegClassifier.Update(
                                worldLandmarks,
                                latestWorld3dFrame);
                            latestWorld3dCaptureLegs = world3dCaptureDeriver.Derive(
                                worldLandmarks,
                                latestWorld3dLegClassification,
                                "world3d");
                            lastConvertedWorld3dSequence = worldLandmarks.Sequence;
                        }
                        catch (Exception ex)
                        {
                            latestWorldLandmarkFrame = null;
                            latestWorld3dFrame = null;
                            latestWorld3dLegClassification = default;
                            latestWorld3dCaptureLegs = default;
                            world3dLegClassifier.Reset();
                            world3dCaptureDeriver.Reset();
                            if (!world3dFaultReported)
                            {
                                world3dFaultReported = true;
                                BodySetStatus("World 3D conversion failed; tracker output paused: " + ex.Message,
                                    Color.FromArgb(255, 175, 90));
                            }
                        }
                    }
                    if (world3dWorker?.LastError is Exception workerError
                        && !world3dFaultReported)
                    {
                        world3dFaultReported = true;
                        BodySetStatus("World 3D worker failed; tracker output paused: " + workerError.Message,
                            Color.FromArgb(255, 175, 90));
                    }

                    // MediaPipe is the only selectable source. A dropout is an
                    // explicit tracker loss; Legacy2D never receives a marker
                    // and can never enter the mux as fallback.
                    bool haveTrackerSelection = trackerMux.TrySelect(
                        trackerNowMs,
                        latestWorld3dFrame,
                        null,
                        out BodyTrackerMuxSelection trackerSelection);
                    Volatile.Write(ref _bodyActiveTrackerSource,
                        (int)(haveTrackerSelection
                            ? trackerSelection.Source
                            : BodyTrackerSource.None));
                    bool continuousSelected = haveTrackerSelection
                        && trackerSelection.Source == BodyTrackerSource.Continuous3D;
                    Vector3 continuousGaitArms = Vector3.Zero;
                    bool continuousArmsAvailable = continuousSelected
                        && latestWorldLandmarkFrame != null
                        && latestWorldLandmarkFrame.Sequence == trackerSelection.Frame.Sequence
                        && TryBuildContinuousGaitArms(
                            latestWorldLandmarkFrame,
                            trackerSelection.Frame,
                            out continuousGaitArms);
                    bool continuousLegSemanticsAvailable = continuousSelected
                        && latestWorldLandmarkFrame != null
                        && latestWorldLandmarkFrame.Sequence == trackerSelection.Frame.Sequence
                        && latestWorldLandmarkFrame.SourceEpoch == trackerSelection.Frame.SourceEpoch
                        && latestWorld3dLegClassification.TimestampMs
                            == trackerSelection.Frame.TimestampMs;
                    CameraLowerBodyClassification selectedWorld3dLegClassification =
                        continuousLegSemanticsAvailable
                            ? latestWorld3dLegClassification
                            : default;

                    _bodyTrackingStreamReady = continuousSelected;

                    if (_bodyStreamEnabled)
                    {
                        if (continuousSelected)
                        {
                            bool publishWorldFrame = trackerSelection.SourceChanged
                                || trackerSelection.Frame.Sequence != lastPublishedWorld3dSequence;
                            if (publishWorldFrame)
                            {
                                if (continuousArmsAvailable)
                                {
                                    if (!world3dArmCenterValid || trackerSelection.SourceChanged)
                                    {
                                        world3dArmCenter = continuousGaitArms.X;
                                        world3dArmCenterValid = true;
                                    }
                                    else
                                    {
                                        world3dArmCenter += 0.004f
                                            * (continuousGaitArms.X - world3dArmCenter);
                                    }
                                    continuousGaitArms.X = Math.Clamp(
                                        continuousGaitArms.X - world3dArmCenter,
                                        -0.40f,
                                        0.40f);
                                }
                                SendRawMediaPipeFramePacket(
                                    latestWorldLandmarkFrame!,
                                    trackerSelection.OutputSourceEpoch,
                                    continuousArmsAvailable ? continuousGaitArms : null,
                                    selectedWorld3dLegClassification);
                                lastPublishedWorld3dSequence = trackerSelection.Frame.Sequence;
                            }
                            lastPublishedSource = BodyTrackerSource.Continuous3D;
                        }
                        else
                        {
                            if (lastPublishedSource != BodyTrackerSource.None)
                                SendEmptyBodyTrackerFrame(trackerMux.OutputSourceEpoch);
                            lastPublishedSource = BodyTrackerSource.None;
                            lastPublishedWorld3dSequence = -1;
                            world3dArmCenterValid = false;
                        }
                    }
                    else
                    {
                        if (lastPublishedSource != BodyTrackerSource.None)
                            SendEmptyBodyTrackerFrame(trackerMux.OutputSourceEpoch);
                        // A later re-enable must republish even if the worker's
                        // latest sequence has not changed yet.
                        lastPublishedSource = BodyTrackerSource.None;
                        lastPublishedWorld3dSequence = -1;
                    }

                    UpdateMediaPipeSilhouetteState(
                        continuousSelected
                            && latestWorldLandmarkFrame != null
                            && latestWorldLandmarkFrame.Sequence == trackerSelection.Frame.Sequence
                                ? latestWorldLandmarkFrame
                                : null,
                        continuousSelected);

                    if (diag != null
                        && trackerNowMs - world3dDiagLastWriteMs >= 100)
                    {
                        try
                        {
                            world3dDiagLastWriteMs = trackerNowMs;
                            WriteWorld3dDiagnosticRow(
                                diag,
                                trackerNowMs,
                                continuousSelected,
                                latestWorldLandmarkFrame,
                                latestWorld3dFrame,
                                latestWorld3dLegClassification);
                            if (trackerNowMs - world3dDiagLastFlushMs >= 1000)
                            {
                                world3dDiagLastFlushMs = trackerNowMs;
                                diag.Flush();
                            }
                        }
                        catch { }
                    }

                    long calibrationNowMs = Environment.TickCount64;
                    if (_bodyCalibrationRecording)
                    {
                        try
                        {
                            if (_bodyUseWorld3D)
                            {
                                WorldLandmarkFrame? captureLandmarks = latestWorldLandmarkFrame;
                                BodyTrackerFrame? captureTrackers = latestWorld3dFrame;
                                if (continuousSelected
                                    && captureLandmarks != null
                                    && captureTrackers != null
                                    && captureTrackers.CorePoseValid
                                    && trackerSelection.Frame.Sequence == captureLandmarks.Sequence
                                    && trackerSelection.Frame.SourceEpoch == captureLandmarks.SourceEpoch
                                    && trackerSelection.Frame.TimestampMs == captureLandmarks.TimestampMs
                                    && captureLandmarks.Sequence != lastRecordedWorld3dSequence
                                    && lastConvertedWorld3dSequence == captureLandmarks.Sequence)
                                {
                                    if (world3dCalibrationRecorder == null)
                                    {
                                        string captureDir = Path.Combine(
                                            AppContext.BaseDirectory,
                                            "BodyTracking",
                                            "Captures");
                                        string fileName =
                                            $"fbt_world3d_capture_{DateTime.Now:yyyyMMdd_HHmmss_fff}_{Guid.NewGuid():N}.csv";
                                        world3dCalibrationRecorder = new World3DCalibrationRecorder();
                                        world3dCalibrationRecorder.Start(
                                            Path.Combine(captureDir, fileName));
                                        _bodyCalibrationFileName = fileName;
                                        BodySetCaptureStatus(
                                            $"RECORDING World3D to BodyTracking\\Captures\\{fileName}",
                                            Color.FromArgb(255, 135, 155));
                                    }

                                    var labels = new World3DCalibrationLabels(
                                        Volatile.Read(ref _bodyCalibrationSegment),
                                        _bodyCalibrationView,
                                        _bodyCalibrationAction,
                                        _bodyCalibrationLeg,
                                        Volatile.Read(ref _bodyCalibrationTake));
                                    BodyTrackerFrame? synchronizedTrackers = captureTrackers != null
                                        && captureTrackers.Source == BodyTrackerSource.Continuous3D
                                        && captureTrackers.Sequence == captureLandmarks.Sequence
                                        && captureTrackers.SourceEpoch == captureLandmarks.SourceEpoch
                                        && captureTrackers.TimestampMs == captureLandmarks.TimestampMs
                                        ? captureTrackers
                                        : null;
                                    world3dCalibrationRecorder.Append(
                                        labels,
                                        captureLandmarks,
                                        synchronizedTrackers,
                                        latestWorld3dCaptureLegs.Left,
                                        latestWorld3dCaptureLegs.Right);
                                    lastRecordedWorld3dSequence = captureLandmarks.Sequence;
                                }
                            }
                            calibrationWasRecording = true;
                        }
                        catch (Exception ex)
                        {
                            _bodyCalibrationRecording = false;
                            try { world3dCalibrationRecorder?.Dispose(); } catch { }
                            world3dCalibrationRecorder = null;
                            string error = "Calibration capture failed: " + ex.Message;
                            try { BeginInvoke(() => StopBodyCalibrationRecord(error)); }
                            catch
                            {
                                BodySetCaptureStatus(error, Color.FromArgb(255, 120, 120));
                            }
                        }
                    }
                    else if (calibrationWasRecording)
                    {
                        calibrationWasRecording = false;
                    }

                    // Preview is display-only. Skip all bitmap work when the user
                    // turns it off, leaves this page, or minimizes the window.
                    // A single-flight gate also prevents UI callbacks piling up.
                    long previewNow = Environment.TickCount64;
                    long previewLast = Interlocked.Read(ref _bodyPreviewLastFrameMs);
                    if (_bodyPreviewActive
                        && previewNow - previewLast >= BodyPreviewIntervalMs
                        && Interlocked.CompareExchange(ref _bodyPreviewUiPending, 1, 0) == 0)
                    {
                        Interlocked.Exchange(ref _bodyPreviewLastFrameMs, previewNow);
                        int previewModeVersion = Volatile.Read(ref _bodyPreviewModeVersion);
                        bool skeletonOnly = _bodyPreviewSkeletonOnly;
                        Bitmap? bmp = null;
                        try
                        {
                            int longestEdge = Math.Max(frame.Width, frame.Height);
                            float previewScale = longestEdge > BodyPreviewMaxEdge
                                ? BodyPreviewMaxEdge / (float)longestEdge
                                : 1.0f;
                            int previewWidth = Math.Max(1, (int)MathF.Round(frame.Width * previewScale));
                            int previewHeight = Math.Max(1, (int)MathF.Round(frame.Height * previewScale));

                            if (skeletonOnly)
                            {
                                // Inference still consumes the raw camera frame above.
                                // This branch changes only what WinForms displays and
                                // avoids the camera resize/copy entirely.
                                bmp = new Bitmap(previewWidth, previewHeight, PixelFormat.Format24bppRgb);
                                using (var black = Graphics.FromImage(bmp))
                                    black.Clear(Color.Black);
                            }
                            else
                            {
                                // Drawing a 1080p/4K camera image into a much smaller
                                // WinForms box wastes CPU and memory bandwidth. Scale
                                // once before bitmap conversion; normalized landmarks
                                // remain correct at any preview resolution.
                                OpenCvSharp.Mat previewFrame = frame;
                                OpenCvSharp.Mat? scaledPreview = null;
                                if (previewScale < 1.0f)
                                {
                                    scaledPreview = new OpenCvSharp.Mat();
                                    OpenCvSharp.Cv2.Resize(frame, scaledPreview,
                                        new OpenCvSharp.Size(previewWidth, previewHeight),
                                        0, 0, OpenCvSharp.InterpolationFlags.Area);
                                    previewFrame = scaledPreview;
                                }
                                try
                                {
                                    bmp = MatToBitmap(previewFrame);
                                }
                                finally
                                {
                                    scaledPreview?.Dispose();
                                }
                            }
                            bool worldPreviewActive = latestWorldLandmarkFrame != null;
                            if (worldPreviewActive)
                                DrawMediaPipeSkeleton(
                                    bmp,
                                    latestWorldLandmarkFrame!,
                                    _bodyOffX / 100f,
                                    _bodyOffY / 100f);
                            if (_bodyPreviewMirror)
                                bmp.RotateFlip(RotateFlipType.RotateNoneFlipX);
                            DrawTrackerSourceBadge(
                                bmp,
                                haveTrackerSelection
                                    ? trackerSelection.Source
                                    : BodyTrackerSource.None,
                                worldPreviewActive);

                            Bitmap queuedBitmap = bmp;
                            BeginInvoke(() =>
                            {
                                try
                                {
                                    if (_bodyPreviewActive
                                        && IsBodyCaptureSessionActive(captureGeneration)
                                        && !IsDisposed
                                        && previewModeVersion == Volatile.Read(ref _bodyPreviewModeVersion))
                                    {
                                        var old = _picBodyVideo.Image;
                                        _picBodyVideo.Image = queuedBitmap;
                                        old?.Dispose();
                                    }
                                    else
                                    {
                                        queuedBitmap.Dispose();
                                    }
                                }
                                finally
                                {
                                    Interlocked.Exchange(ref _bodyPreviewUiPending, 0);
                                }
                            });
                            bmp = null; // ownership moved to the UI callback
                        }
                        catch
                        {
                            bmp?.Dispose();
                            Interlocked.Exchange(ref _bodyPreviewUiPending, 0);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                if (IsBodyCaptureSessionActive(captureGeneration))
                    BodySetStatus("Capture error: " + ex.Message, Color.FromArgb(255, 120, 120));
            }
            finally
            {
                // Clear while this session still owns its socket. The ownership
                // check and send are serialized with new-session registration,
                // so an obsolete delayed finally can never clear a newer feed.
                if (sessionOsc != null)
                    ClearBodyOscSession(sessionOsc);
                try { sessionOsc?.Dispose(); } catch { }
                try { grabberThread?.Join(1000); } catch { }
                try { diag?.Flush(); diag?.Dispose(); } catch { }
                try { world3dCalibrationRecorder?.Dispose(); } catch { }
                try { cap?.Release(); cap?.Dispose(); } catch { }
                try { world3dWorker?.Dispose(); } catch { }
                if (Volatile.Read(ref _bodyCaptureGeneration) == captureGeneration)
                {
                    _bodyTrackingStreamReady = false;
                    _bodyPoseValid = false;
                    Volatile.Write(ref _bodyActiveTrackerSource, (int)BodyTrackerSource.None);
                }
            }
        }

        // COCO-WholeBody left/right partners. The last six preserve the foot
        // landmark type while swapping anatomical sides.
        private static Bitmap MatToBitmap(OpenCvSharp.Mat mat)
        {
            int w = mat.Width, h = mat.Height;
            var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
            var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            try
            {
                long srcStep = mat.Step();
                int rowBytes = w * 3;
                var row = new byte[rowBytes];
                for (int r = 0; r < h; r++)
                {
                    Marshal.Copy(mat.Data + (nint)(r * srcStep), row, 0, rowBytes);
                    Marshal.Copy(row, 0, data.Scan0 + r * data.Stride, rowBytes);
                }
            }
            finally { bmp.UnlockBits(data); }
            return bmp;
        }

        private static void DrawMediaPipeSkeleton(
            Bitmap bmp,
            WorldLandmarkFrame frame,
            float previewOffsetX,
            float previewOffsetY)
        {
            const float minConfidence = 0.25f;
            using var g = Graphics.FromImage(bmp);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using var bonePen = new Pen(Color.FromArgb(235, 70, 200, 255), 3f);
            using var jointBrush = new SolidBrush(Color.FromArgb(255, 120, 225, 255));

            bool TryPoint(MediaPipe33LandmarkIndex index, out PointF point)
            {
                WorldLandmark landmark = frame[(int)index];
                Vector3 p = landmark.NormalizedPosition;
                if (!landmark.IsValid
                    || landmark.Confidence < minConfidence
                    || !WorldLandmark.IsFinite(p))
                {
                    point = default;
                    return false;
                }
                point = new PointF(
                    (p.X + previewOffsetX) * bmp.Width,
                    (p.Y + previewOffsetY) * bmp.Height);
                return true;
            }

            foreach (var (a, b) in MediaPipePoseBones)
            {
                if (TryPoint(a, out PointF pa) && TryPoint(b, out PointF pb))
                    g.DrawLine(bonePen, pa, pb);
            }

            for (int i = (int)MediaPipe33LandmarkIndex.LeftShoulder;
                 i <= (int)MediaPipe33LandmarkIndex.RightFootIndex;
                 i++)
            {
                if (!TryPoint((MediaPipe33LandmarkIndex)i, out PointF p))
                    continue;
                float radius = i >= (int)MediaPipe33LandmarkIndex.LeftAnkle ? 3.5f : 4f;
                g.FillEllipse(jointBrush,
                    p.X - radius, p.Y - radius, radius * 2f, radius * 2f);
            }
        }

        private void UpdateMediaPipeSilhouetteState(
            WorldLandmarkFrame? landmarks,
            bool continuousSelected)
        {
            lock (_bodyKpLock)
            {
                Array.Clear(_bodyKp);
                _bodyPoseValid = continuousSelected && landmarks != null;
                if (!_bodyPoseValid)
                    return;

                void Copy(MediaPipe33LandmarkIndex source, params int[] targets)
                {
                    WorldLandmark point = landmarks![(int)source];
                    if (!point.IsValid)
                        return;
                    foreach (int target in targets)
                    {
                        _bodyKp[target, 0] = point.NormalizedPosition.X;
                        _bodyKp[target, 1] = point.NormalizedPosition.Y;
                        _bodyKp[target, 2] = point.Confidence;
                    }
                }

                Copy(MediaPipe33LandmarkIndex.Nose, KpNose);
                Copy(MediaPipe33LandmarkIndex.LeftShoulder, KpLSho);
                Copy(MediaPipe33LandmarkIndex.RightShoulder, KpRSho);
                Copy(MediaPipe33LandmarkIndex.LeftElbow, KpLElb);
                Copy(MediaPipe33LandmarkIndex.RightElbow, KpRElb);
                Copy(MediaPipe33LandmarkIndex.LeftWrist, KpLWri);
                Copy(MediaPipe33LandmarkIndex.RightWrist, KpRWri);
                Copy(MediaPipe33LandmarkIndex.LeftHip, KpLHip);
                Copy(MediaPipe33LandmarkIndex.RightHip, KpRHip);
                Copy(MediaPipe33LandmarkIndex.LeftKnee, KpLKnee);
                Copy(MediaPipe33LandmarkIndex.RightKnee, KpRKnee);
                Copy(MediaPipe33LandmarkIndex.LeftAnkle, KpLAnk);
                Copy(MediaPipe33LandmarkIndex.RightAnkle, KpRAnk);
                Copy(MediaPipe33LandmarkIndex.LeftHeel, KpLHeel);
                Copy(MediaPipe33LandmarkIndex.RightHeel, KpRHeel);
                // MediaPipe has one forefoot point per side; mirror it into the
                // two old display-only toe slots used by the silhouette panel.
                Copy(MediaPipe33LandmarkIndex.LeftFootIndex, KpLBigToe, KpLSmallToe);
                Copy(MediaPipe33LandmarkIndex.RightFootIndex, KpRBigToe, KpRSmallToe);
            }
        }

        private static void WriteWorld3dDiagnosticRow(
            StreamWriter writer,
            long harvestNowMs,
            bool active,
            WorldLandmarkFrame? landmarks,
            BodyTrackerFrame? converted,
            CameraLowerBodyClassification classification)
        {
            var line = new System.Text.StringBuilder(640);
            void AddText(string value)
            {
                if (line.Length != 0) line.Append(';');
                line.Append(value);
            }
            void AddLong(long value) => AddText(value.ToString(
                System.Globalization.CultureInfo.InvariantCulture));
            void AddUInt(uint value) => AddText(value.ToString(
                System.Globalization.CultureInfo.InvariantCulture));
            void AddFloat(float value) => AddText(float.IsFinite(value)
                ? value.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture)
                : string.Empty);
            void AddBool(bool value) => AddText(value ? "1" : "0");
            void AddPose(BodyTrackerPose pose)
            {
                if (pose.PositionValid && WorldLandmark.IsFinite(pose.Position))
                {
                    AddFloat(pose.Position.X); AddFloat(pose.Position.Y); AddFloat(pose.Position.Z);
                }
                else
                {
                    AddText(string.Empty); AddText(string.Empty); AddText(string.Empty);
                }
            }
            void AddLandmark(MediaPipe33LandmarkIndex index)
            {
                if (landmarks == null)
                {
                    AddText(string.Empty); AddText(string.Empty); AddText(string.Empty); AddFloat(0f);
                    return;
                }
                WorldLandmark point = landmarks[(int)index];
                if (point.IsValid && WorldLandmark.IsFinite(point.WorldPosition))
                {
                    AddFloat(point.WorldPosition.X);
                    AddFloat(point.WorldPosition.Y);
                    AddFloat(point.WorldPosition.Z);
                }
                else
                {
                    AddText(string.Empty); AddText(string.Empty); AddText(string.Empty);
                }
                AddFloat(point.Confidence);
            }

            AddLong(harvestNowMs);
            AddLong(landmarks?.Sequence ?? -1L);
            AddLong(landmarks?.TimestampMs ?? -1L);
            AddLong(landmarks != null ? harvestNowMs - landmarks.TimestampMs : -1L);
            AddUInt(landmarks?.SourceEpoch ?? 0u);
            AddBool(active);
            AddBool(converted?.CorePoseValid == true);
            AddText(((int)classification.Left.TransportState).ToString(
                System.Globalization.CultureInfo.InvariantCulture));
            AddFloat(classification.Left.Confidence);
            AddText(((int)classification.Right.TransportState).ToString(
                System.Globalization.CultureInfo.InvariantCulture));
            AddFloat(classification.Right.Confidence);
            AddPose(converted?.GetTracker(1) ?? default);
            AddPose(converted?.GetTracker(2) ?? default);
            AddPose(converted?.GetTracker(3) ?? default);
            AddLandmark(MediaPipe33LandmarkIndex.LeftHip);
            AddLandmark(MediaPipe33LandmarkIndex.LeftKnee);
            AddLandmark(MediaPipe33LandmarkIndex.LeftAnkle);
            AddLandmark(MediaPipe33LandmarkIndex.RightHip);
            AddLandmark(MediaPipe33LandmarkIndex.RightKnee);
            AddLandmark(MediaPipe33LandmarkIndex.RightAnkle);
            writer.WriteLine(line.ToString());
        }

        private static void DrawTrackerSourceBadge(
            Bitmap bmp,
            BodyTrackerSource activeSource,
            bool worldPreviewActive)
        {
            string active = activeSource switch
            {
                BodyTrackerSource.Continuous3D => "WORLD 3D",
                _ => "WAITING",
            };
            string headline = $"MEDIAPIPE WORLD 3D  |  ACTIVE: {active}";
            string detail = worldPreviewActive
                ? "Preview joints: MediaPipe 33"
                : "Waiting for MediaPipe pose";

            Color accent = activeSource switch
            {
                BodyTrackerSource.Continuous3D => Color.FromArgb(90, 215, 255),
                _ => Color.FromArgb(185, 175, 195),
            };

            using var g = Graphics.FromImage(bmp);
            using var headlineFont = new Font("Segoe UI", 9f, FontStyle.Bold);
            using var detailFont = new Font("Segoe UI", 8f, FontStyle.Regular);
            SizeF headlineSize = g.MeasureString(headline, headlineFont);
            SizeF detailSize = g.MeasureString(detail, detailFont);
            float width = Math.Max(headlineSize.Width, detailSize.Width) + 16f;
            float height = headlineSize.Height + detailSize.Height + 10f;
            using var background = new SolidBrush(Color.FromArgb(205, 12, 14, 18));
            using var accentBrush = new SolidBrush(accent);
            using var detailBrush = new SolidBrush(Color.FromArgb(225, 225, 230, 235));
            g.FillRectangle(background, 8f, 8f, width, height);
            g.FillRectangle(accentBrush, 8f, 8f, 4f, height);
            g.DrawString(headline, headlineFont, accentBrush, 16f, 11f);
            g.DrawString(detail, detailFont, detailBrush,
                16f, 11f + headlineSize.Height);
        }

        private static bool TryBuildContinuousGaitArms(
            WorldLandmarkFrame landmarks,
            BodyTrackerFrame trackerFrame,
            out Vector3 gaitArms)
        {
            const float minimumConfidence = 0.25f;

            bool TryPoint(MediaPipe33LandmarkIndex index, out Vector3 point)
            {
                WorldLandmark landmark = landmarks[(int)index];
                if (!landmark.IsWorldUsable(minimumConfidence))
                {
                    point = Vector3.Zero;
                    return false;
                }
                Vector3 raw = landmark.WorldPosition;
                point = new Vector3(
                    (landmarks.InputMirrored ? -1f : 1f) * raw.X,
                    -raw.Y,
                    -raw.Z);
                return WorldLandmark.IsFinite(point);
            }

            if (!TryPoint(MediaPipe33LandmarkIndex.LeftShoulder, out Vector3 leftShoulder)
                || !TryPoint(MediaPipe33LandmarkIndex.RightShoulder, out Vector3 rightShoulder)
                || !TryPoint(MediaPipe33LandmarkIndex.LeftElbow, out Vector3 leftElbow)
                || !TryPoint(MediaPipe33LandmarkIndex.RightElbow, out Vector3 rightElbow)
                || !TryPoint(MediaPipe33LandmarkIndex.LeftWrist, out Vector3 leftWrist)
                || !TryPoint(MediaPipe33LandmarkIndex.RightWrist, out Vector3 rightWrist))
            {
                gaitArms = Vector3.Zero;
                return false;
            }

            float floorY = float.PositiveInfinity;
            void ConsiderFloor(MediaPipe33LandmarkIndex index)
            {
                if (TryPoint(index, out Vector3 point))
                    floorY = MathF.Min(floorY, point.Y);
            }
            ConsiderFloor(MediaPipe33LandmarkIndex.LeftAnkle);
            ConsiderFloor(MediaPipe33LandmarkIndex.RightAnkle);
            ConsiderFloor(MediaPipe33LandmarkIndex.LeftHeel);
            ConsiderFloor(MediaPipe33LandmarkIndex.RightHeel);
            ConsiderFloor(MediaPipe33LandmarkIndex.LeftFootIndex);
            ConsiderFloor(MediaPipe33LandmarkIndex.RightFootIndex);
            if (!float.IsFinite(floorY))
            {
                gaitArms = Vector3.Zero;
                return false;
            }

            BodyTrackerPose waist = trackerFrame.GetTracker(1);
            Vector3 bodyForward = waist.RotationValid
                ? Vector3.Transform(Vector3.UnitZ, waist.Orientation)
                : Vector3.UnitZ;
            if (!WorldLandmark.IsFinite(bodyForward)
                || bodyForward.LengthSquared() <= 1e-8f)
                bodyForward = Vector3.UnitZ;
            else
                bodyForward = Vector3.Normalize(bodyForward);

            float ArmFeature(Vector3 shoulder, Vector3 elbow, Vector3 wrist) =>
                0.70f * Vector3.Dot(wrist - shoulder, bodyForward)
                + 0.30f * Vector3.Dot(elbow - shoulder, bodyForward);

            float leftFeature = ArmFeature(leftShoulder, leftElbow, leftWrist);
            float rightFeature = ArmFeature(rightShoulder, rightElbow, rightWrist);
            gaitArms = new Vector3(
                leftFeature - rightFeature,
                leftWrist.Y - floorY,
                rightWrist.Y - floorY);
            return WorldLandmark.IsFinite(gaitArms);
        }

        private void SendContinuousBodyTrackerFrameOsc(
            BodyTrackerFrame frame,
            uint outputSourceEpoch,
            Vector3? gaitArms,
            CameraLowerBodyClassification legClassification)
        {
            if (_bodyOsc == null) return;

            var messages = new List<(string Address, float X, float Y, float Z)>(22);

            if (gaitArms.HasValue)
            {
                Vector3 arms = gaitArms.Value;
                messages.Add(("/tracking/gait/arms", arms.X, arms.Y, arms.Z));
            }

            // Keep gait/action semantics in the same OSC bundle and source
            // frame as the World3D tracker poses. Invalid is sent explicitly;
            // otherwise a stale kick/WalkStep from an earlier frame could
            // survive a confidence dropout in the native receiver.
            messages.Add(("/tracking/gait/leg/left",
                (float)legClassification.Left.TransportState,
                legClassification.Left.KneeAngleDegrees,
                legClassification.Left.Confidence));
            messages.Add(("/tracking/gait/leg/right",
                (float)legClassification.Right.TransportState,
                legClassification.Right.KneeAngleDegrees,
                legClassification.Right.Confidence));

            if (frame.Head.PositionValid)
            {
                Vector3 p = frame.Head.Position;
                messages.Add(("/tracking/trackers/head/position", p.X, p.Y, p.Z));
            }
            if (frame.Head.RotationValid)
            {
                Vector3 e = QuaternionToUnityEulerDegrees(frame.Head.Orientation);
                messages.Add(("/tracking/trackers/head/rotation", e.X, e.Y, e.Z));
            }

            for (int slot = 1; slot <= BodyTrackerFrame.TrackerSlotCount; slot++)
            {
                BodyTrackerPose tracker = frame.GetTracker(slot);
                if (tracker.PositionValid)
                {
                    Vector3 p = tracker.Position;
                    messages.Add(($"/tracking/trackers/{slot}/position", p.X, p.Y, p.Z));
                }
                if (tracker.RotationValid)
                {
                    Vector3 e = QuaternionToUnityEulerDegrees(tracker.Orientation);
                    messages.Add(($"/tracking/trackers/{slot}/rotation", e.X, e.Y, e.Z));
                }
            }

            uint poseMask = frame.OscPoseMask;
            OscTrackerFrameFlags flags = OscTrackerFrameContract.BuildPolicyFlags(
                frame,
                // Continuously solve HMD yaw minus measured body yaw. If the
                // camera sees the turn they cancel; if it misses the torso
                // turn, the lower body still follows the headset.
                followHmdYaw: false,
                // Camera-emulated feet must yield to VRIK's planted walk cycle
                // during stick/WIP locomotion. Physical trackers bypass this
                // policy in the native runtime, and confirmed kick feet remain
                // live through semantic arbitration.
                allowGaitFootRelease: true);
            Vector3 header = OscTrackerFrameContract.PackHeader(
                outputSourceEpoch, poseMask, flags);
            messages.Add((OscTrackerFrameContract.Address,
                header.X, header.Y, header.Z));
            SendOscBundle(messages);
        }

        // Direct OCU camera-body transport. The Configurator sends MediaPipe's
        // untouched hip-centred world landmarks in one atomic UDP datagram.
        // Axis mapping, floor placement, body basis and virtual tracker poses
        // are owned exactly once by the native runtime. This deliberately keeps
        // UI/preview code in C# without putting C# coordinate math in the game
        // tracking path.
        private void SendRawMediaPipeFramePacket(
            WorldLandmarkFrame frame,
            uint outputSourceEpoch,
            Vector3? gaitArms,
            CameraLowerBodyClassification legClassification)
        {
            const int headerBytes = 72;
            const int landmarkBytes = 16;
            const int packetBytes = headerBytes
                + WorldLandmarkFrame.MediaPipeLandmarkCount * landmarkBytes;
            var packet = new byte[packetBytes];
            packet[0] = (byte)'O';
            packet[1] = (byte)'C';
            packet[2] = (byte)'U';
            packet[3] = (byte)'3';

            void U32(int offset, uint value) =>
                BinaryPrimitives.WriteUInt32LittleEndian(
                    packet.AsSpan(offset, 4), value);
            void I32(int offset, int value) =>
                BinaryPrimitives.WriteInt32LittleEndian(
                    packet.AsSpan(offset, 4), value);
            void I64(int offset, long value) =>
                BinaryPrimitives.WriteInt64LittleEndian(
                    packet.AsSpan(offset, 8), value);
            void F32(int offset, float value) =>
                BinaryPrimitives.WriteInt32LittleEndian(
                    packet.AsSpan(offset, 4), BitConverter.SingleToInt32Bits(value));

            U32(4, 1); // protocol version
            U32(8, outputSourceEpoch);
            uint flags = frame.InputMirrored ? 1u : 0u;
            if (gaitArms.HasValue) flags |= 1u << 1;
            U32(12, flags);
            I64(16, frame.Sequence);
            I64(24, frame.TimestampMs);
            I32(32, (int)legClassification.Left.TransportState);
            I32(36, (int)legClassification.Right.TransportState);
            F32(40, legClassification.Left.KneeAngleDegrees);
            F32(44, legClassification.Left.Confidence);
            F32(48, legClassification.Right.KneeAngleDegrees);
            F32(52, legClassification.Right.Confidence);
            Vector3 arms = gaitArms ?? Vector3.Zero;
            F32(56, arms.X);
            F32(60, arms.Y);
            F32(64, arms.Z);
            U32(68, 0);

            for (int i = 0; i < WorldLandmarkFrame.MediaPipeLandmarkCount; i++)
            {
                WorldLandmark landmark = frame[i];
                int offset = headerBytes + i * landmarkBytes;
                Vector3 world = landmark.WorldPosition;
                F32(offset, world.X);
                F32(offset + 4, world.Y);
                F32(offset + 8, world.Z);
                F32(offset + 12, landmark.IsValid ? landmark.Confidence : -1f);
            }

            lock (_bodyOscGate)
            {
                try
                {
                    _bodyOsc?.Send(packet, packet.Length);
                }
                catch { }
            }
        }

        private void SendEmptyBodyTrackerFrame(uint outputSourceEpoch)
        {
            Vector3 header = OscTrackerFrameContract.PackHeader(
                outputSourceEpoch,
                poseMask: 0,
                OscTrackerFrameFlags.None);
            SendOscVec(OscTrackerFrameContract.Address,
                header.X, header.Y, header.Z);
        }

        private void SendContinuousGaitArms(Vector3 gaitArms)
        {
            SendOscVec("/tracking/gait/arms", gaitArms.X, gaitArms.Y, gaitArms.Z);
        }

        // Inverse of the DLL's Unity ordering:
        // Quaternion.Euler(x,y,z) = Qy(y) * Qx(x) * Qz(z).
        // Keeping this exact matters for foot orientation; a generic XYZ
        // extractor silently swaps the order near a raised horizontal leg.
        private static Vector3 QuaternionToUnityEulerDegrees(Quaternion value)
        {
            if (!BodyTrackerPose.IsFinite(value)
                || value.LengthSquared() <= 1e-12f)
                return Vector3.Zero;

            value = Quaternion.Normalize(value);
            float sinX = Math.Clamp(
                2f * (value.W * value.X - value.Y * value.Z), -1f, 1f);
            float x = MathF.Asin(sinX);
            float cosX = MathF.Cos(x);
            float y;
            float z;
            if (MathF.Abs(cosX) > 1e-5f)
            {
                y = MathF.Atan2(
                    2f * (value.X * value.Z + value.W * value.Y),
                    1f - 2f * (value.X * value.X + value.Y * value.Y));
                z = MathF.Atan2(
                    2f * (value.X * value.Y + value.W * value.Z),
                    1f - 2f * (value.X * value.X + value.Z * value.Z));
            }
            else
            {
                // At +/-90 degrees X, Y and Z are not independently unique.
                // Choose Z=0 and retain the equivalent combined Y rotation.
                float r01 = 2f * (value.X * value.Y - value.W * value.Z);
                float r00 = 1f - 2f * (value.Y * value.Y + value.Z * value.Z);
                y = MathF.Atan2(sinX >= 0f ? r01 : -r01, r00);
                z = 0f;
            }

            const float toDegrees = 180f / MathF.PI;
            return new Vector3(x * toDegrees, y * toDegrees, z * toDegrees);
        }

        private void SendOscVec(string address, float x, float y, float z)
        {
            lock (_bodyOscGate)
            {
                if (_bodyOsc == null) return;
                SendOscVecLocked(_bodyOsc, address, x, y, z);
            }
        }

        private void SendOscVecLocked(
            UdpClient osc,
            string address,
            float x,
            float y,
            float z)
        {
            try
            {
                byte[] packet = BuildOscVecPacket(address, x, y, z);
                osc.Send(packet, packet.Length);
            }
            catch { }
        }

        // expectedSession == null is StopBodyCapture claiming whichever session
        // is still current after its bounded join. A non-null value is the
        // capture thread proving it still owns the published socket.
        private UdpClient? ClearBodyOscSession(UdpClient? expectedSession)
        {
            lock (_bodyOscGate)
            {
                UdpClient? current = _bodyOsc;
                if (current == null
                    || (expectedSession != null
                        && !ReferenceEquals(current, expectedSession)))
                    return null;

                uint stopEpoch = (uint)(Environment.TickCount64
                    % OscTrackerFrameContract.MaximumExactFloatInteger);
                if (stopEpoch == 0) stopEpoch = 1;
                Vector3 header = OscTrackerFrameContract.PackHeader(
                    stopEpoch,
                    poseMask: 0,
                    OscTrackerFrameFlags.None);
                SendOscVecLocked(current, OscTrackerFrameContract.Address,
                    header.X, header.Y, header.Z);
                _bodyOsc = null;
                return current;
            }
        }

        private byte[] BuildOscVecPacket(string address, float x, float y, float z)
        {
            if (!_bodyOscPackets.TryGetValue(address, out byte[]? packet))
            {
                int addressBytes = System.Text.Encoding.ASCII.GetByteCount(address);
                int addressField = (addressBytes + 1 + 3) & ~3;
                const int typeField = 8; // ",fff\0" rounded to four-byte alignment
                packet = new byte[addressField + typeField + 12];
                System.Text.Encoding.ASCII.GetBytes(address, 0, address.Length, packet, 0);
                packet[addressField] = (byte)',';
                packet[addressField + 1] = (byte)'f';
                packet[addressField + 2] = (byte)'f';
                packet[addressField + 3] = (byte)'f';
                _bodyOscPackets[address] = packet;
            }

            int valuesOffset = packet.Length - 12;
            BinaryPrimitives.WriteInt32BigEndian(packet.AsSpan(valuesOffset, 4), BitConverter.SingleToInt32Bits(x));
            BinaryPrimitives.WriteInt32BigEndian(packet.AsSpan(valuesOffset + 4, 4), BitConverter.SingleToInt32Bits(y));
            BinaryPrimitives.WriteInt32BigEndian(packet.AsSpan(valuesOffset + 8, 4), BitConverter.SingleToInt32Bits(z));
            return packet;
        }

        private void SendOscBundle(
            IReadOnlyList<(string Address, float X, float Y, float Z)> messages)
        {
            lock (_bodyOscGate)
            {
                try
                {
                    if (_bodyOsc == null || messages.Count == 0) return;
                    var packets = new byte[messages.Count][];
                    int bundleLength = 16; // "#bundle\0" + immediate timetag
                    for (int i = 0; i < messages.Count; i++)
                    {
                        var message = messages[i];
                        byte[] source = BuildOscVecPacket(
                            message.Address, message.X, message.Y, message.Z);
                        packets[i] = (byte[])source.Clone();
                        bundleLength = checked(bundleLength + 4 + source.Length);
                    }

                    byte[] bundle = new byte[bundleLength];
                    System.Text.Encoding.ASCII.GetBytes("#bundle", 0, 7, bundle, 0);
                    bundle[15] = 1; // OSC immediate timetag
                    int offset = 16;
                    foreach (byte[] packet in packets)
                    {
                        BinaryPrimitives.WriteInt32BigEndian(
                            bundle.AsSpan(offset, 4), packet.Length);
                        offset += 4;
                        packet.CopyTo(bundle, offset);
                        offset += packet.Length;
                    }
                    _bodyOsc.Send(bundle, bundle.Length);
                }
                catch { }
            }
        }
    }

    // ── Silhouette panel ─────────────────────────────────────────────────
    // Simple standing human outline; tracker dots light green when the
    // matching keypoint tracks. KAT treadmill = puck under the feet.
    internal class BodySilhouettePanel : Panel
    {
        [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
        internal bool ReferenceOnly { get; set; }
        [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
        internal bool RuntimeMonitor { get; set; }
        private uint _runtimeValid, _runtimeTracked;
        private float[,] _kp = new float[133, 3];
        private bool _valid;
        private bool _kat;

        public BodySilhouettePanel() { DoubleBuffered = true; }

        internal void SetRuntimeTrackerState(uint valid, uint tracked)
        {
            if (_runtimeValid == valid && _runtimeTracked == tracked) return;
            _runtimeValid = valid;
            _runtimeTracked = tracked & valid;
            Invalidate();
        }

        internal void SetTreadmillConnected(bool connected)
        {
            if (_kat == connected) return;
            _kat = connected;
            AccessibleDescription = connected ? "Treadmill connected with fresh reader data" : "No live treadmill connection data";
            Invalidate();
        }

        public void UpdateState(float[,] kp, bool valid, bool kat)
        {
            Array.Clear(_kp);
            Array.Copy(kp, _kp, Math.Min(kp.Length, _kp.Length));
            _valid = valid;
            _kat = kat;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            float w = ClientSize.Width, h = ClientSize.Height;
            float cx = w / 2f;
            // Body proportions inside the panel
            float top = h * (ReferenceOnly ? 0.12f : 0.06f), bottom = h * 0.86f;
            float bodyH = bottom - top;
            float headR = bodyH * 0.07f;

            using var outlinePen = new Pen(ModernUiTheme.TextMuted, 2.5f);

            float headCy = top + headR;
            float neckY = headCy + headR;
            float shoY = neckY + bodyH * 0.04f;
            float hipY = top + bodyH * 0.47f;
            float kneeY = top + bodyH * 0.72f;
            float ankY = bottom;
            float shoHalf = bodyH * 0.13f;
            float hipHalf = bodyH * 0.075f;
            float elbY = shoY + bodyH * 0.17f;
            float wriY = shoY + bodyH * 0.33f;
            float armX = shoHalf + bodyH * 0.05f;

            // Outline
            g.DrawEllipse(outlinePen, cx - headR, headCy - headR, headR * 2, headR * 2);
            g.DrawLine(outlinePen, cx, neckY, cx, hipY);                      // spine
            g.DrawLine(outlinePen, cx - shoHalf, shoY, cx + shoHalf, shoY);   // shoulders
            g.DrawLine(outlinePen, cx - shoHalf, shoY, cx - armX, elbY);      // upper arms
            g.DrawLine(outlinePen, cx + shoHalf, shoY, cx + armX, elbY);
            g.DrawLine(outlinePen, cx - armX, elbY, cx - armX, wriY);         // forearms
            g.DrawLine(outlinePen, cx + armX, elbY, cx + armX, wriY);
            g.DrawLine(outlinePen, cx - hipHalf, hipY, cx + hipHalf, hipY);   // hips
            g.DrawLine(outlinePen, cx - hipHalf, hipY, cx - hipHalf, kneeY);  // thighs
            g.DrawLine(outlinePen, cx + hipHalf, hipY, cx + hipHalf, kneeY);
            g.DrawLine(outlinePen, cx - hipHalf, kneeY, cx - hipHalf, ankY);  // shins
            g.DrawLine(outlinePen, cx + hipHalf, kneeY, cx + hipHalf, ankY);

            // KAT treadmill puck under the feet
            if (_kat || ReferenceOnly)
            {
                using var puckBrush = new SolidBrush(_kat ? ModernUiTheme.AccentSoft : ModernUiTheme.Surface);
                using var puckPen = new Pen(_kat ? ModernUiTheme.KeyGlow : ModernUiTheme.TextMuted, 2f);
                g.FillEllipse(puckBrush, cx - w * 0.30f, ankY + 4, w * 0.60f, h * 0.055f);
                g.DrawEllipse(puckPen, cx - w * 0.30f, ankY + 4, w * 0.60f, h * 0.055f);
                DrawCenteredText(g, _kat ? "KAT connected" : "KAT — no live data", cx, ankY + h * 0.055f + 8,
                    _kat ? ModernUiTheme.AccentText : ModernUiTheme.TextMuted);
            }
            else
            {
                DrawCenteredText(g, "no treadmill", cx, ankY + 10, Color.FromArgb(70, 70, 78));
            }

            // Tracker dots: (label position, keypoint index or -1 for derived)
            const float minConf = 0.3f;
            bool KpOk(int i) => !ReferenceOnly && _valid && _kp[i, 2] > minConf;
            void Dot(float x, float y, bool on, int role = -1, int alternativeRole = -1)
            {
                uint mask = role >= 0 ? 1u << role : 0;
                if (alternativeRole >= 0) mask |= 1u << alternativeRole;
                bool inferred = false;
                if (RuntimeMonitor)
                {
                    on = (_runtimeTracked & mask) != 0;
                    inferred = !on && (_runtimeValid & mask) != 0;
                }
                using var b = new SolidBrush(on ? ModernUiTheme.KeyGlowBright
                    : inferred ? Color.FromArgb(230, 178, 70) : ModernUiTheme.TextMuted);
                g.FillEllipse(b, x - 5, y - 5, 10, 10);
                if (on)
                {
                    using var glow = new Pen(Color.FromArgb(90, 90, 230, 90), 4f);
                    g.DrawEllipse(glow, x - 7, y - 7, 14, 14);
                }
            }

            Dot(cx, headCy, KpOk(0));                                          // head
            Dot(cx, (shoY + hipY) / 2f, KpOk(5) && KpOk(6), 3);                // chest
            Dot(cx, hipY, KpOk(11) && KpOk(12), 0);                            // waist
            Dot(cx - armX, elbY, KpOk(7), 6);                                 // elbows
            Dot(cx + armX, elbY, KpOk(8), 7);
            Dot(cx - armX, wriY, KpOk(9), 10);                                // wrists
            Dot(cx + armX, wriY, KpOk(10), 11);
            Dot(cx - hipHalf, kneeY, KpOk(13), 4);                            // knees
            Dot(cx + hipHalf, kneeY, KpOk(14), 5);
            bool leftFoot = KpOk(15) && (KpOk(19) || KpOk(17) || KpOk(18));
            bool rightFoot = KpOk(16) && (KpOk(22) || KpOk(20) || KpOk(21));
            Dot(cx - hipHalf, ankY, leftFoot, 1, 12);                         // feet / ankles
            Dot(cx + hipHalf, ankY, rightFoot, 2, 13);

            DrawCenteredText(g, RuntimeMonitor ? "BODY POSES" : ReferenceOnly ? "BODY LAYOUT" : (_valid ? "TRACKING" : "no pose"),
                cx, ReferenceOnly ? 12 : top - 2, !ReferenceOnly && _valid ? ModernUiTheme.KeyGlowBright : ModernUiTheme.TextMuted);
        }

        private static void DrawCenteredText(Graphics g, string text, float cx, float y, Color color)
        {
            using var f = new Font("Segoe UI", 8f, FontStyle.Bold);
            var sz = g.MeasureString(text, f);
            using var b = new SolidBrush(color);
            g.DrawString(text, f, b, cx - sz.Width / 2f, y);
        }
    }
}
