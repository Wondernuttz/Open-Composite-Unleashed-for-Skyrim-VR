using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    public class MainForm : Form
    {
        // INI data
        private readonly IniFile _ini = new();
        private string _gameDir = "";
        private string _gameType = "skyrim"; // "skyrim" or "fallout4"

        // Top bar
        private ComboBox _cmbGame = null!;
        private TextBox _txtPath = null!;
        private Button _btnBrowse = null!;

        // Controller image panel
        private PictureBox _picControllers = null!;
        private Image? _controllerImage;

        // Keyboard shortcut section — button checkboxes
        private CheckBox _chkShortcutEnabled = null!;
        private CheckBox _chkLeftStick = null!;
        private CheckBox _chkLeftX = null!;
        private CheckBox _chkLeftY = null!;
        private CheckBox _chkRightStick = null!;
        private CheckBox _chkRightA = null!;
        private CheckBox _chkRightB = null!;

        private RadioButton _rdoX1 = null!;
        private RadioButton _rdoX2 = null!;
        private RadioButton _rdoX3 = null!;
        private RadioButton _rdoX4 = null!;
        private NumericUpDown _nudTiming = null!;
        private Label _lblTimingDesc = null!;

        // General settings
        private NumericUpDown _nudSuperSample = null!;
        // [EXPERIMENTAL] private ComboBox _cmbControllerModel = null!;
        private CheckBox _chkRenderHands = null!;
        private CheckBox _chkHaptics = null!;
        private NumericUpDown _nudHapticStrength = null!;
        private CheckBox _chkHiddenMesh = null!;
        private CheckBox _chkInvertShaders = null!;
        private CheckBox _chkDx10 = null!;
        private CheckBox _chkAudioSwitch = null!;
        private TextBox _txtAudioDevice = null!;

        // Skyrim-specific
        private Panel _pnlSkyrimOnly = null!;
        private CheckBox _chkInputSmoothing = null!;
        private NumericUpDown _nudInputWindow = null!;
        private CheckBox _chkControllerSmoothing = null!;
        private CheckBox _chkDisableTriggerTouch = null!;
        private CheckBox _chkDisableTrackpad = null!;
        private CheckBox _chkVRIKKnuckles = null!;
        private TextBox _txtKeyboardText = null!;

        // Keyboard display settings
        private NumericUpDown _nudDisplayTilt = null!;
        private NumericUpDown _nudDisplayOpacity = null!;
        private NumericUpDown _nudDisplayScale = null!;
        private CheckBox _chkSoundsEnabled = null!;
        private NumericUpDown _nudHoverVolume = null!;
        private NumericUpDown _nudPressVolume = null!;
        private NumericUpDown _nudKbHapticStrength = null!;

        // Dead zones (Skyrim)
        private NumericUpDown _nudLeftDeadZone = null!;
        private NumericUpDown _nudRightDeadZone = null!;

        // Ko-fi
        private PictureBox _picKofi = null!;
        private Image? _kofiImage;

        // Bottom buttons
        private Button _btnSave = null!;
        private Button _btnReload = null!;

        // Status
        private Label _lblStatus = null!;

        // Button ID → checkbox mapping
        private readonly Dictionary<string, Func<CheckBox>> _btnCheckboxMap = new();

        // Calibration log for Shift+Click
        private readonly List<string> _calibrationLog = new();
        private int _calibrationStep = 0;
        private static readonly string[] CalibrationOrder =
        {
            "Left Stick", "X Button", "Y Button",
            "Right Stick", "A Button", "B Button"
        };

        // Button positions on the controller image (fractions of drawn image area)
        // Tuned for the Quest 3 controller top-down photo
        // Button positions as fractions of the drawn image area.
        // Calibrate with Shift+Click on the controller image.
        private static readonly Dictionary<string, PointF[]> ButtonPositions = new()
        {
            { "left_stick",    new[] { new PointF(0.269f, 0.150f) } },
            { "x",             new[] { new PointF(0.306f, 0.250f) } },
            { "y",             new[] { new PointF(0.353f, 0.194f) } },
            { "right_stick",   new[] { new PointF(0.709f, 0.144f) } },
            { "a",             new[] { new PointF(0.666f, 0.250f) } },
            { "b",             new[] { new PointF(0.625f, 0.189f) } },
        };

        public MainForm()
        {
            LoadControllerImage();
            LoadKofiImage();
            LoadWindowIcon();
            InitializeUI();
            SetupButtonMap();
            SetDefaults();
        }

        private void LoadControllerImage()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.controllers.png");
            if (stream != null)
                _controllerImage = Image.FromStream(stream);
        }

        private void LoadKofiImage()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.kofi.png");
            if (stream != null)
                _kofiImage = Image.FromStream(stream);
        }

        private void LoadWindowIcon()
        {
            try
            {
                Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            }
            catch { }
        }

        private void SetupButtonMap()
        {
            _btnCheckboxMap["left_stick"] = () => _chkLeftStick;
            _btnCheckboxMap["x"] = () => _chkLeftX;
            _btnCheckboxMap["y"] = () => _chkLeftY;
            _btnCheckboxMap["right_stick"] = () => _chkRightStick;
            _btnCheckboxMap["a"] = () => _chkRightA;
            _btnCheckboxMap["b"] = () => _chkRightB;
        }

        private void InitializeUI()
        {
            Text = "OC Unleashed Configurator";
            Size = new Size(920, 920);
            MinimumSize = new Size(900, 850);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(30, 30, 35);
            ForeColor = Color.FromArgb(220, 220, 220);
            Font = new Font("Segoe UI", 9.5f);
            AutoScroll = true;

            int y = 12;
            int leftMargin = 16;
            int rightEdge = 870;

            // ── Game selector + path bar ──
            var lblGame = MakeLabel("Game:", leftMargin, y + 3, 50);
            Controls.Add(lblGame);

            _cmbGame = new ComboBox
            {
                Location = new Point(leftMargin + 55, y),
                Width = 160,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat
            };
            _cmbGame.Items.AddRange(new[] { "Skyrim VR", "Fallout 4 VR" });
            _cmbGame.SelectedIndex = 0;
            _cmbGame.SelectedIndexChanged += (s, e) =>
            {
                _gameType = _cmbGame.SelectedIndex == 0 ? "skyrim" : "fallout4";
                _pnlSkyrimOnly.Visible = _gameType == "skyrim";
                UpdateFormTitle();
            };
            Controls.Add(_cmbGame);

            _txtPath = new TextBox
            {
                Location = new Point(leftMargin + 225, y),
                Width = 510,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.FromArgb(180, 180, 180),
                BorderStyle = BorderStyle.FixedSingle,
                ReadOnly = true,
                Text = "(No folder selected \u2014 click Browse)"
            };
            Controls.Add(_txtPath);

            _btnBrowse = MakeButton("Browse...", leftMargin + 745, y - 1, 110, 27);
            _btnBrowse.Click += BtnBrowse_Click;
            Controls.Add(_btnBrowse);

            y += 40;
            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── KEYBOARD SHORTCUT SECTION ──
            var lblSection = MakeSectionLabel("VR Keyboard Shortcut", leftMargin, y);
            Controls.Add(lblSection);
            y += 28;

            _chkShortcutEnabled = MakeCheckBox("Enable controller shortcut to open keyboard anywhere", leftMargin, y);
            _chkShortcutEnabled.Checked = true;
            Controls.Add(_chkShortcutEnabled);

            var lblComboHint = MakeLabel("Select one or more buttons below \u2014 all must be pressed together to activate.", leftMargin + 28, y + 20, 500);
            lblComboHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblComboHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            Controls.Add(lblComboHint);
            y += 42;

            // Controller image on the left
            _picControllers = new PictureBox
            {
                Location = new Point(leftMargin, y),
                Size = new Size(320, 180),
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(40, 40, 48),
                Image = _controllerImage
            };
            _picControllers.Paint += PicControllers_Paint;
            _picControllers.MouseClick += PicControllers_MouseClick;
            _picControllers.Cursor = Cursors.Hand;
            Controls.Add(_picControllers);

            // Button checkboxes to the right of the image — two columns
            int cx = leftMargin + 340;
            int chkY = y;

            var lblLeft = MakeLabel("Left Hand", cx, chkY, 100);
            lblLeft.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblLeft.ForeColor = Color.FromArgb(180, 200, 255);
            Controls.Add(lblLeft);

            var lblRight = MakeLabel("Right Hand", cx + 200, chkY, 100);
            lblRight.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblRight.ForeColor = Color.FromArgb(180, 200, 255);
            Controls.Add(lblRight);

            chkY += 22;

            _chkLeftStick = MakeCheckBox("Stick Click", cx, chkY);
            _chkRightStick = MakeCheckBox("Stick Click", cx + 200, chkY);
            Controls.Add(_chkLeftStick);
            Controls.Add(_chkRightStick);
            chkY += 22;

            _chkLeftX = MakeCheckBox("X Button", cx, chkY);
            _chkRightA = MakeCheckBox("A Button", cx + 200, chkY);
            Controls.Add(_chkLeftX);
            Controls.Add(_chkRightA);
            chkY += 22;

            _chkLeftY = MakeCheckBox("Y Button", cx, chkY);
            _chkRightB = MakeCheckBox("B Button", cx + 200, chkY);
            Controls.Add(_chkLeftY);
            Controls.Add(_chkRightB);
            chkY += 22;

            var lblLeftGrip = MakeLabel("Grip = Exit Keyboard", cx, chkY + 2, 190);
            lblLeftGrip.ForeColor = Color.FromArgb(140, 140, 140);
            lblLeftGrip.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            var lblRightGrip = MakeLabel("Grip = Exit Keyboard", cx + 200, chkY + 2, 190);
            lblRightGrip.ForeColor = Color.FromArgb(140, 140, 140);
            lblRightGrip.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            Controls.Add(lblLeftGrip);
            Controls.Add(lblRightGrip);
            chkY += 22;

            var lblLeftTrigger = MakeLabel("Trigger = Laser Click", cx, chkY + 2, 190);
            lblLeftTrigger.ForeColor = Color.FromArgb(140, 140, 140);
            lblLeftTrigger.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            var lblRightTrigger = MakeLabel("Trigger = Laser Click", cx + 200, chkY + 2, 190);
            lblRightTrigger.ForeColor = Color.FromArgb(140, 140, 140);
            lblRightTrigger.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            Controls.Add(lblLeftTrigger);
            Controls.Add(lblRightTrigger);
            chkY += 6;

            // Wire up checkbox changes to repaint the image
            foreach (var chk in new[] { _chkLeftStick, _chkLeftX, _chkLeftY,
                                        _chkRightStick, _chkRightA, _chkRightB })
            {
                chk.CheckedChanged += (s, e) => _picControllers.Invalidate();
            }

            // (combo hint moved to above the image)

            // Align y below whichever is taller: image or checkboxes
            int imageBottom = _picControllers.Bottom + 10;
            chkY += 22;
            y = Math.Max(imageBottom, chkY);

            // Tap count
            var lblTaps = MakeLabel("Tap Count:", leftMargin, y + 3, 100);
            Controls.Add(lblTaps);

            int tapX = leftMargin + 105;
            var tapPanel = new Panel
            {
                Location = new Point(tapX, y),
                Size = new Size(310, 26),
                BackColor = Color.Transparent
            };
            _rdoX1 = MakeRadioButton("x1 Hold", 0, 1, 90);
            _rdoX2 = MakeRadioButton("x2", 95, 1, 55);
            _rdoX3 = MakeRadioButton("x3", 155, 1, 55);
            _rdoX4 = MakeRadioButton("x4", 215, 1, 55);
            _rdoX2.Checked = true;
            tapPanel.Controls.AddRange(new Control[] { _rdoX1, _rdoX2, _rdoX3, _rdoX4 });
            Controls.Add(tapPanel);

            // Timing
            var lblTiming = MakeLabel("Timing (ms):", leftMargin + 430, y + 3, 100);
            Controls.Add(lblTiming);

            _nudTiming = new NumericUpDown
            {
                Location = new Point(leftMargin + 535, y),
                Width = 80,
                Minimum = 100, Maximum = 3000, Increment = 50, Value = 500,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White
            };
            Controls.Add(_nudTiming);

            _lblTimingDesc = MakeLabel("Max time between taps", leftMargin + 625, y + 3, 220);
            _lblTimingDesc.ForeColor = Color.FromArgb(140, 140, 140);
            Controls.Add(_lblTimingDesc);

            _rdoX1.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX2.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX3.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX4.CheckedChanged += (s, e) => UpdateTimingLabel();

            y += 38;

            // ── KEYBOARD DISPLAY SETTINGS ──
            var lblDisplay = MakeLabel("Keyboard Display:", leftMargin, y + 3, 140);
            lblDisplay.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblDisplay.ForeColor = Color.FromArgb(180, 200, 255);
            Controls.Add(lblDisplay);

            // Row 1: Tilt, Transparency, Size
            var lblTilt = MakeLabel("Tilt:", leftMargin + 145, y + 3, 35);
            Controls.Add(lblTilt);
            _nudDisplayTilt = new NumericUpDown
            {
                Location = new Point(leftMargin + 180, y),
                Width = 60,
                DecimalPlaces = 1, Increment = 1m, Minimum = -30m, Maximum = 80m, Value = 22.5m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudDisplayTilt);
            Controls.Add(MakeLabel("\u00B0", leftMargin + 242, y + 3, 15));

            Controls.Add(MakeLabel("Transp:", leftMargin + 270, y + 3, 55));
            _nudDisplayOpacity = new NumericUpDown
            {
                Location = new Point(leftMargin + 325, y),
                Width = 55,
                Minimum = 1, Maximum = 100, Increment = 5, Value = 30,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudDisplayOpacity);
            Controls.Add(MakeLabel("%", leftMargin + 382, y + 3, 15));

            Controls.Add(MakeLabel("Size:", leftMargin + 410, y + 3, 40));
            _nudDisplayScale = new NumericUpDown
            {
                Location = new Point(leftMargin + 450, y),
                Width = 55,
                Minimum = 50, Maximum = 150, Increment = 5, Value = 100,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudDisplayScale);
            Controls.Add(MakeLabel("%", leftMargin + 507, y + 3, 15));

            var lblDisplayHint = MakeLabel("(adjustable in VR)", leftMargin + 540, y + 3, 140);
            lblDisplayHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblDisplayHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            Controls.Add(lblDisplayHint);

            y += 30;

            // Row 2: Sounds checkbox
            _chkSoundsEnabled = MakeCheckBox("Sounds", leftMargin + 145, y);
            Controls.Add(_chkSoundsEnabled);

            y += 28;

            // Row 3: Hover Volume, Press Volume, Haptic
            Controls.Add(MakeLabel("Hover:", leftMargin + 145, y + 3, 50));
            _nudHoverVolume = new NumericUpDown
            {
                Location = new Point(leftMargin + 195, y),
                Width = 50,
                Minimum = 0, Maximum = 100, Increment = 10, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudHoverVolume);
            Controls.Add(MakeLabel("%", leftMargin + 247, y + 3, 15));

            Controls.Add(MakeLabel("Press:", leftMargin + 275, y + 3, 45));
            _nudPressVolume = new NumericUpDown
            {
                Location = new Point(leftMargin + 320, y),
                Width = 50,
                Minimum = 0, Maximum = 100, Increment = 10, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudPressVolume);
            Controls.Add(MakeLabel("%", leftMargin + 372, y + 3, 15));

            Controls.Add(MakeLabel("Haptic:", leftMargin + 410, y + 3, 50));
            _nudKbHapticStrength = new NumericUpDown
            {
                Location = new Point(leftMargin + 460, y),
                Width = 55,
                Minimum = 0, Maximum = 100, Increment = 10, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudKbHapticStrength);
            Controls.Add(MakeLabel("%", leftMargin + 517, y + 3, 15));

            y += 34;
            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── GENERAL SETTINGS ──
            var lblGeneral = MakeSectionLabel("General Settings", leftMargin, y);
            Controls.Add(lblGeneral);
            y += 28;

            int col1 = leftMargin;
            int col2 = leftMargin + 430;

            var lblSS = MakeLabel("Supersampling:", col1, y + 3, 130);
            Controls.Add(lblSS);
            _nudSuperSample = new NumericUpDown
            {
                Location = new Point(col1 + 135, y), Width = 80,
                DecimalPlaces = 1, Increment = 0.1m, Minimum = 0.5m, Maximum = 3.0m, Value = 1.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudSuperSample);

            // [EXPERIMENTAL — future development] Quest 3 controller model dropdown
            // Disabled: Skyrim VR's IVRRenderModels shader doesn't apply UV-mapped
            // textures, rendering Quest 3 controllers as black silhouettes.
            // var lblCtrlModel = MakeLabel("Controller model:", col2, y + 3, 130);
            // Controls.Add(lblCtrlModel);
            // _cmbControllerModel = new ComboBox { ... };
            // _cmbControllerModel.Items.AddRange(new[] { "Hands", "Quest 3 Controllers" });
            // _cmbControllerModel.SelectedIndex = 0;
            // Controls.Add(_cmbControllerModel);
            y += 32;

            _chkRenderHands = MakeCheckBox("Render custom hands", col1, y);
            _chkRenderHands.Checked = true;
            Controls.Add(_chkRenderHands);
            y += 32;

            _chkHaptics = MakeCheckBox("Enable haptics (vibration)", col1, y);
            _chkHaptics.Checked = true;
            Controls.Add(_chkHaptics);

            var lblHapStr = MakeLabel("Haptic Strength:", col2, y + 3, 130);
            Controls.Add(lblHapStr);
            _nudHapticStrength = new NumericUpDown
            {
                Location = new Point(col2 + 135, y), Width = 80,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.1m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            Controls.Add(_nudHapticStrength);
            y += 32;

            _chkHiddenMesh = MakeCheckBox("Enable hidden mesh fix", col1, y);
            _chkHiddenMesh.Checked = true;
            Controls.Add(_chkHiddenMesh);

            _chkInvertShaders = MakeCheckBox("Invert using shaders", col2, y);
            Controls.Add(_chkInvertShaders);
            y += 32;

            _chkDx10 = MakeCheckBox("DX10 mode (compatibility)", col1, y);
            Controls.Add(_chkDx10);
            y += 32;

            _chkAudioSwitch = MakeCheckBox("Auto-switch audio to headset (not for Virtual Desktop)", col1, y);
            Controls.Add(_chkAudioSwitch);

            var lblAudioDev = MakeLabel("Device name:", col2, y + 3, 100);
            Controls.Add(lblAudioDev);
            _txtAudioDevice = new TextBox
            {
                Location = new Point(col2 + 105, y), Width = 140,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                BorderStyle = BorderStyle.FixedSingle, Text = "quest"
            };
            Controls.Add(_txtAudioDevice);
            var lblAudioHint = MakeLabel("(partial match)", col2 + 255, y + 3, 100);
            lblAudioHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblAudioHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            Controls.Add(lblAudioHint);
            y += 40;

            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── GAME CONTROLS SECTION ──
            var lblGameControls = MakeSectionLabel("Game Controls", leftMargin, y);
            Controls.Add(lblGameControls);
            y += 28;

            var btnKeyBindings = MakeButton("Keyboard Bindings (controlmapvr.txt)...", col1, y, 300, 32);
            btnKeyBindings.BackColor = Color.FromArgb(60, 80, 120);
            btnKeyBindings.Click += (s, e) => OpenKeyboardBindings();
            Controls.Add(btnKeyBindings);

            var lblKeyBindingsHint = MakeLabel("Remap WASD keys to avoid VR keyboard conflicts (requires game restart)", col1 + 310, y + 6, 450);
            lblKeyBindingsHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblKeyBindingsHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            Controls.Add(lblKeyBindingsHint);
            y += 44;

            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── SKYRIM-ONLY SETTINGS ──
            _pnlSkyrimOnly = new Panel
            {
                Location = new Point(0, y),
                Size = new Size(rightEdge + 20, 250),
                BackColor = Color.Transparent
            };
            Controls.Add(_pnlSkyrimOnly);

            int sy = 0;
            var lblSkyrim = MakeSectionLabel("Skyrim VR Settings", leftMargin, sy);
            _pnlSkyrimOnly.Controls.Add(lblSkyrim);
            sy += 28;

            _chkInputSmoothing = MakeCheckBox("Enable input smoothing (thumbstick)", col1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkInputSmoothing);
            var lblInputWin = MakeLabel("Smoothing window:", col2, sy + 3, 140);
            _pnlSkyrimOnly.Controls.Add(lblInputWin);
            _nudInputWindow = new NumericUpDown
            {
                Location = new Point(col2 + 145, sy), Width = 60,
                Minimum = 1, Maximum = 20, Value = 5,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudInputWindow);
            sy += 32;

            _chkControllerSmoothing = MakeCheckBox("Enable controller smoothing (hand tracking)", col1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkControllerSmoothing);
            _chkDisableTriggerTouch = MakeCheckBox("Disable trigger touch events", col2, sy);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTriggerTouch);
            sy += 32;

            _chkDisableTrackpad = MakeCheckBox("Disable trackpad emulation", col1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTrackpad);
            _chkVRIKKnuckles = MakeCheckBox("VRIK Knuckles trackpad support", col2, sy);
            _pnlSkyrimOnly.Controls.Add(_chkVRIKKnuckles);
            sy += 32;

            var lblDeadL = MakeLabel("Left dead zone:", col1, sy + 3, 130);
            _pnlSkyrimOnly.Controls.Add(lblDeadL);
            _nudLeftDeadZone = new NumericUpDown
            {
                Location = new Point(col1 + 135, sy), Width = 80,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudLeftDeadZone);
            var lblDeadR = MakeLabel("Right dead zone:", col2, sy + 3, 130);
            _pnlSkyrimOnly.Controls.Add(lblDeadR);
            _nudRightDeadZone = new NumericUpDown
            {
                Location = new Point(col2 + 135, sy), Width = 80,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudRightDeadZone);
            sy += 32;

            var lblKbText = MakeLabel("Keyboard default text:", col1, sy + 3, 160);
            _pnlSkyrimOnly.Controls.Add(lblKbText);
            _txtKeyboardText = new TextBox
            {
                Location = new Point(col1 + 165, sy), Width = 200,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                BorderStyle = BorderStyle.FixedSingle, Text = "Adventurer"
            };
            _pnlSkyrimOnly.Controls.Add(_txtKeyboardText);
            sy += 40;

            _pnlSkyrimOnly.Size = new Size(rightEdge + 20, sy);

            y += _pnlSkyrimOnly.Height + 8;
            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── SUPPORT / KO-FI SECTION ──
            {
                int kofiIconSize = 36;
                _picKofi = new PictureBox
                {
                    Location = new Point(leftMargin, y),
                    Size = new Size(kofiIconSize, kofiIconSize),
                    SizeMode = PictureBoxSizeMode.Zoom,
                    BackColor = Color.Transparent,
                    Image = _kofiImage,
                    Cursor = Cursors.Hand
                };
                _picKofi.Click += (s, e) =>
                {
                    try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("https://ko-fi.com/wondernutts") { UseShellExecute = true }); }
                    catch { }
                };
                Controls.Add(_picKofi);

                var lblKofiLink = new LinkLabel
                {
                    Text = "Support me on Ko-fi",
                    Location = new Point(leftMargin + kofiIconSize + 8, y + 2),
                    AutoSize = true,
                    Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                    LinkColor = Color.FromArgb(41, 171, 226),
                    ActiveLinkColor = Color.FromArgb(80, 200, 255),
                    VisitedLinkColor = Color.FromArgb(41, 171, 226)
                };
                lblKofiLink.Click += (s, e) =>
                {
                    try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("https://ko-fi.com/wondernutts") { UseShellExecute = true }); }
                    catch { }
                };
                Controls.Add(lblKofiLink);

                var lblKofiQuote = MakeLabel(
                    "\u201CI do this all for free and for the love of the VR gaming community, and I always will. "
                    + "I\u2019ll never demand anything in return, but if you want to show support, I won\u2019t complain. "
                    + "I love bringing VR to life with you all regardless.\u201D",
                    leftMargin + kofiIconSize + 8, y + 24, rightEdge - leftMargin - kofiIconSize - 12);
                lblKofiQuote.ForeColor = Color.FromArgb(160, 160, 160);
                lblKofiQuote.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
                lblKofiQuote.AutoSize = false;
                lblKofiQuote.Size = new Size(rightEdge - leftMargin - kofiIconSize - 12, 48);
                Controls.Add(lblKofiQuote);

                y += Math.Max(kofiIconSize, 70) + 8;
            }

            Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 16;

            // ── BOTTOM BUTTONS ──
            _btnSave = MakeButton("Save INI", leftMargin, y, 140, 36);
            _btnSave.BackColor = Color.FromArgb(40, 120, 40);
            _btnSave.ForeColor = Color.White;
            _btnSave.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
            _btnSave.Click += BtnSave_Click;
            Controls.Add(_btnSave);

            _btnReload = MakeButton("Reload", leftMargin + 155, y, 100, 36);
            _btnReload.Click += BtnReload_Click;
            Controls.Add(_btnReload);

            _lblStatus = MakeLabel("", leftMargin + 275, y + 9, 500);
            _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
            Controls.Add(_lblStatus);

            y += 50;
            ClientSize = new Size(ClientSize.Width, Math.Max(y + 10, 800));
        }

        private (float drawW, float drawH, float offX, float offY) GetImageBounds()
        {
            float imgW = _picControllers.Width;
            float imgH = _picControllers.Height;
            float imgAspect = _controllerImage != null ? (float)_controllerImage.Width / _controllerImage.Height : 1.6f;
            float boxAspect = imgW / imgH;
            if (boxAspect > imgAspect)
                return (imgH * imgAspect, imgH, (imgW - imgH * imgAspect) / 2, 0);
            else
                return (imgW, imgW / imgAspect, 0, (imgH - imgW / imgAspect) / 2);
        }

        private void PicControllers_MouseClick(object? sender, MouseEventArgs e)
        {
            var (drawW, drawH, offX, offY) = GetImageBounds();

            // Convert click to fraction of drawn image
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            // Find closest button within hit radius
            float hitRadius = 0.06f; // fraction of image dimension
            string? closest = null;
            float closestDist = float.MaxValue;

            foreach (var kvp in ButtonPositions)
            {
                foreach (var pt in kvp.Value)
                {
                    float dx = fx - pt.X;
                    float dy = fy - pt.Y;
                    float dist = (float)Math.Sqrt(dx * dx + dy * dy);
                    if (dist < hitRadius && dist < closestDist)
                    {
                        closestDist = dist;
                        closest = kvp.Key;
                    }
                }
            }

            // Shift+click: guided calibration — logs each button position in sequence
            if ((ModifierKeys & Keys.Shift) != 0)
            {
                if (_calibrationStep < CalibrationOrder.Length)
                {
                    string name = CalibrationOrder[_calibrationStep];
                    _calibrationLog.Add($"{name}: ({fx:F3}, {fy:F3})");
                    _calibrationStep++;

                    if (_calibrationStep < CalibrationOrder.Length)
                    {
                        _lblStatus.Text = $"Logged {name}. Now Shift+Click: {CalibrationOrder[_calibrationStep]}";
                    }
                    else
                    {
                        _lblStatus.Text = "All done! Coordinates copied to clipboard.";
                        string all = string.Join("\n", _calibrationLog);
                        Clipboard.SetText(all);
                    }
                }
                else
                {
                    // Reset for another round
                    _calibrationLog.Clear();
                    _calibrationStep = 0;
                    _lblStatus.Text = $"Reset. Shift+Click: {CalibrationOrder[0]}";
                }
                _lblStatus.ForeColor = Color.FromArgb(200, 180, 80);
                return;
            }

            if (closest != null && _btnCheckboxMap.TryGetValue(closest, out var getChk))
            {
                var chk = getChk();
                chk.Checked = !chk.Checked; // toggle — this fires CheckedChanged which repaints
            }
        }

        private void PicControllers_Paint(object? sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            float imgW = _picControllers.Width;
            float imgH = _picControllers.Height;
            var (drawW, drawH, offX, offY) = GetImageBounds();

            var highlightColor = Color.FromArgb(200, 255, 200, 40);
            using var pen = new Pen(highlightColor, 2.5f);
            using var brush = new SolidBrush(Color.FromArgb(70, 255, 200, 40));
            using var labelFont = new Font("Segoe UI", 8f, FontStyle.Bold);
            using var textBrush = new SolidBrush(highlightColor);

            // Get list of selected buttons
            var selected = GetSelectedButtons();
            float r = 10; // smaller radius

            foreach (string btnId in selected)
            {
                if (!ButtonPositions.TryGetValue(btnId, out var points))
                    continue;

                foreach (var pt in points)
                {
                    float cx = offX + pt.X * drawW;
                    float cy = offY + pt.Y * drawH;
                    g.FillEllipse(brush, cx - r, cy - r, r * 2, r * 2);
                    g.DrawEllipse(pen, cx - r, cy - r, r * 2, r * 2);
                }
            }

            // No overlay label — combo text shown in status bar on paint
            if (selected.Count > 0)
            {
                string label = string.Join(" + ", selected.Select(FormatButtonName));
                _lblStatus.Text = "Shortcut: " + label;
                _lblStatus.ForeColor = Color.FromArgb(255, 200, 40);
            }
            else
            {
                _lblStatus.Text = "";
            }
        }

        private static string FormatButtonName(string id) => id switch
        {
            "left_stick" => "L Stick",
            "right_stick" => "R Stick",
            "a" => "A",
            "b" => "B",
            "x" => "X",
            "y" => "Y",
            _ => id
        };

        private List<string> GetSelectedButtons()
        {
            var list = new List<string>();
            if (_chkLeftStick?.Checked == true) list.Add("left_stick");
            if (_chkLeftX?.Checked == true) list.Add("x");
            if (_chkLeftY?.Checked == true) list.Add("y");
            if (_chkRightStick?.Checked == true) list.Add("right_stick");
            if (_chkRightA?.Checked == true) list.Add("a");
            if (_chkRightB?.Checked == true) list.Add("b");
            return list;
        }

        private void UpdateTimingLabel()
        {
            if (_rdoX1.Checked)
                _lblTimingDesc.Text = "How long to hold the button(s)";
            else
                _lblTimingDesc.Text = "Max time between taps";
        }

        private void BtnBrowse_Click(object? sender, EventArgs e)
        {
            using var dlg = new FolderBrowserDialog
            {
                Description = "Select the folder where openvr_api.dll and opencomposite.ini live.\nFor MO2: the mod's root\\ folder. For manual installs: the game directory.",
                ShowNewFolderButton = false
            };

            if (dlg.ShowDialog() == DialogResult.OK)
            {
                _gameDir = dlg.SelectedPath;
                _txtPath.Text = _gameDir;
                LoadFromDir();
            }
        }

        private void BtnSave_Click(object? sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                MessageBox.Show("Please select a game folder first.", "No Folder Selected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            var selected = GetSelectedButtons();
            if (selected.Count == 0 && _chkShortcutEnabled.Checked)
            {
                MessageBox.Show("Please select at least one button for the keyboard shortcut.",
                    "No Button Selected", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            WriteToIni();
            string path = Path.Combine(_gameDir, "opencomposite.ini");
            _ini.Save(path);

            // The DLL watches opencomposite.ini and applies changes within ~1 second

            _lblStatus.Text = $"Saved \u2014 changes apply in-game within 1 second";
            _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        private void BtnReload_Click(object? sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                MessageBox.Show("Please select a game folder first.", "No Folder Selected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            LoadFromDir();
        }

        private void LoadFromDir()
        {
            string path = Path.Combine(_gameDir, "opencomposite.ini");
            _ini.Load(path);

            if (File.Exists(path))
            {
                _lblStatus.Text = $"Loaded {path}";
                _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            else
            {
                _lblStatus.Text = "No opencomposite.ini found \u2014 using defaults";
                _lblStatus.ForeColor = Color.FromArgb(200, 180, 80);
            }

            ReadFromIni();
        }

        private void ReadFromIni()
        {
            // Keyboard shortcut
            _chkShortcutEnabled.Checked = ParseBool(_ini.Get("keyboard", "shortcutEnabled", "true"));

            // Parse button combo: "left_stick" or "left_grip+right_grip" etc.
            string btnRaw = _ini.Get("keyboard", "shortcutButton", "left_stick").ToLowerInvariant().Trim();

            // Handle legacy "both_grips" value
            if (btnRaw == "both_grips")
                btnRaw = "left_grip+right_grip";

            // Clear all checkboxes first
            foreach (var kvp in _btnCheckboxMap)
                kvp.Value().Checked = false;

            // Check the ones specified
            foreach (string part in btnRaw.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                if (_btnCheckboxMap.TryGetValue(part, out var getChk))
                    getChk().Checked = true;
            }

            string mode = _ini.Get("keyboard", "shortcutMode", "double_tap").ToLowerInvariant();
            int tapCount = mode switch
            {
                "long_press" => 1,
                "double_tap" => 2,
                "triple_tap" => 3,
                "quadruple_tap" => 4,
                _ => 2
            };
            _rdoX1.Checked = tapCount == 1;
            _rdoX2.Checked = tapCount == 2;
            _rdoX3.Checked = tapCount == 3;
            _rdoX4.Checked = tapCount == 4;

            if (int.TryParse(_ini.Get("keyboard", "shortcutTiming", "500"), out int timing))
                _nudTiming.Value = Math.Clamp(timing, 100, 3000);

            // Keyboard display
            if (float.TryParse(_ini.Get("keyboard", "displayTilt", "22.5"), out float dt))
                _nudDisplayTilt.Value = (decimal)Math.Clamp(dt, -30f, 80f);
            if (int.TryParse(_ini.Get("keyboard", "displayOpacity", "30"), out int dop))
                _nudDisplayOpacity.Value = Math.Clamp(dop, 1, 100);
            if (int.TryParse(_ini.Get("keyboard", "displayScale", "100"), out int dsc))
                _nudDisplayScale.Value = Math.Clamp(dsc, 50, 150);
            _chkSoundsEnabled.Checked = ParseBool(_ini.Get("keyboard", "soundsEnabled", "true"));
            if (int.TryParse(_ini.Get("keyboard", "hoverVolume", "50"), out int hvol))
                _nudHoverVolume.Value = Math.Clamp(hvol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "pressVolume", "50"), out int pvol))
                _nudPressVolume.Value = Math.Clamp(pvol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "hapticStrength", "50"), out int khs))
                _nudKbHapticStrength.Value = Math.Clamp(khs, 0, 100);

            // General
            if (float.TryParse(_ini.Get("", "supersampleRatio", "1.0"), out float ss))
                _nudSuperSample.Value = (decimal)Math.Clamp(ss, 0.5f, 3.0f);
            _chkRenderHands.Checked = ParseBool(_ini.Get("", "renderCustomHands", "true"));
            // [EXPERIMENTAL] Quest 3 controller model — disabled
            // string ctrlModel = _ini.Get("", "controllerModel", "hands").Trim().ToLower();
            // _cmbControllerModel.SelectedIndex = ctrlModel == "quest3" ? 1 : 0;
            _chkHaptics.Checked = ParseBool(_ini.Get("", "haptics", "true"));
            if (float.TryParse(_ini.Get("", "hapticStrength", "0.1"), out float hs))
                _nudHapticStrength.Value = (decimal)Math.Clamp(hs, 0f, 1f);
            _chkHiddenMesh.Checked = ParseBool(_ini.Get("", "enableHiddenMeshFix", "true"));
            _chkInvertShaders.Checked = ParseBool(_ini.Get("", "invertUsingShaders", "false"));
            _chkDx10.Checked = ParseBool(_ini.Get("", "dx10Mode", "false"));
            _chkAudioSwitch.Checked = ParseBool(_ini.Get("", "enableAudioSwitch", "false"));
            _txtAudioDevice.Text = _ini.Get("", "audioDeviceName", "quest");
            if (string.IsNullOrEmpty(_txtAudioDevice.Text))
                _txtAudioDevice.Text = "quest";

            // Skyrim-specific
            _chkInputSmoothing.Checked = ParseBool(_ini.Get("", "enableInputSmoothing", "false"));
            if (int.TryParse(_ini.Get("", "inputWindowSize", "5"), out int iw))
                _nudInputWindow.Value = Math.Clamp(iw, 1, 20);
            _chkControllerSmoothing.Checked = ParseBool(_ini.Get("", "enableControllerSmoothing", "false"));
            _chkDisableTriggerTouch.Checked = ParseBool(_ini.Get("", "disableTriggerTouch", "false"));
            _chkDisableTrackpad.Checked = ParseBool(_ini.Get("", "disableTrackPad", "false"));
            _chkVRIKKnuckles.Checked = ParseBool(_ini.Get("", "enableVRIKKnucklesTrackPadSupport", "false"));
            if (float.TryParse(_ini.Get("", "leftDeadZoneSize", "0.0"), out float ldz))
                _nudLeftDeadZone.Value = (decimal)Math.Clamp(ldz, 0f, 1f);
            if (float.TryParse(_ini.Get("", "rightDeadZoneSize", "0.0"), out float rdz))
                _nudRightDeadZone.Value = (decimal)Math.Clamp(rdz, 0f, 1f);
            _txtKeyboardText.Text = _ini.Get("", "keyboardText", "Adventurer");
            if (string.IsNullOrEmpty(_txtKeyboardText.Text))
                _txtKeyboardText.Text = "Adventurer";

            UpdateTimingLabel();
            _picControllers.Invalidate();
        }

        private void WriteToIni()
        {
            _ini.Set("keyboard", "shortcutEnabled", _chkShortcutEnabled.Checked ? "true" : "false");

            // Build combo string from checked buttons
            var selected = GetSelectedButtons();
            string btnValue = selected.Count > 0 ? string.Join("+", selected) : "left_stick";
            _ini.Set("keyboard", "shortcutButton", btnValue);

            string mode;
            if (_rdoX1.Checked) mode = "long_press";
            else if (_rdoX3.Checked) mode = "triple_tap";
            else if (_rdoX4.Checked) mode = "quadruple_tap";
            else mode = "double_tap";
            _ini.Set("keyboard", "shortcutMode", mode);
            _ini.Set("keyboard", "shortcutTiming", ((int)_nudTiming.Value).ToString());
            _ini.Set("keyboard", "displayTilt", _nudDisplayTilt.Value.ToString("0.0"));
            _ini.Set("keyboard", "displayOpacity", ((int)_nudDisplayOpacity.Value).ToString());
            _ini.Set("keyboard", "displayScale", ((int)_nudDisplayScale.Value).ToString());
            _ini.Set("keyboard", "soundsEnabled", _chkSoundsEnabled.Checked ? "true" : "false");
            _ini.Set("keyboard", "hoverVolume", ((int)_nudHoverVolume.Value).ToString());
            _ini.Set("keyboard", "pressVolume", ((int)_nudPressVolume.Value).ToString());
            _ini.Set("keyboard", "hapticStrength", ((int)_nudKbHapticStrength.Value).ToString());

            // General
            _ini.Set("", "supersampleRatio", _nudSuperSample.Value.ToString("0.0"));
            _ini.Set("", "renderCustomHands", _chkRenderHands.Checked ? "true" : "false");
            // [EXPERIMENTAL] Quest 3 controller model — disabled
            // _ini.Set("", "controllerModel", _cmbControllerModel.SelectedIndex == 1 ? "quest3" : "hands");
            _ini.Set("", "haptics", _chkHaptics.Checked ? "true" : "false");
            _ini.Set("", "enableHiddenMeshFix", _chkHiddenMesh.Checked ? "true" : "false");
            _ini.Set("", "invertUsingShaders", _chkInvertShaders.Checked ? "true" : "false");
            _ini.Set("", "dx10Mode", _chkDx10.Checked ? "true" : "false");
            _ini.Set("", "enableAudioSwitch", _chkAudioSwitch.Checked ? "true" : "false");
            _ini.Set("", "audioDeviceName", _txtAudioDevice.Text);

            if (_gameType == "skyrim")
            {
                _ini.Set("", "hapticStrength", _nudHapticStrength.Value.ToString("0.00"));
                _ini.Set("", "enableInputSmoothing", _chkInputSmoothing.Checked ? "true" : "false");
                _ini.Set("", "inputWindowSize", ((int)_nudInputWindow.Value).ToString());
                _ini.Set("", "enableControllerSmoothing", _chkControllerSmoothing.Checked ? "true" : "false");
                _ini.Set("", "disableTriggerTouch", _chkDisableTriggerTouch.Checked ? "true" : "false");
                _ini.Set("", "disableTrackPad", _chkDisableTrackpad.Checked ? "true" : "false");
                _ini.Set("", "enableVRIKKnucklesTrackPadSupport", _chkVRIKKnuckles.Checked ? "true" : "false");
                _ini.Set("", "leftDeadZoneSize", _nudLeftDeadZone.Value.ToString("0.00"));
                _ini.Set("", "rightDeadZoneSize", _nudRightDeadZone.Value.ToString("0.00"));
                _ini.Set("", "keyboardText", _txtKeyboardText.Text);
            }
        }

        private void SetDefaults()
        {
            _chkLeftStick.Checked = true; // default: left stick
            _pnlSkyrimOnly.Visible = true;
            UpdateTimingLabel();
            UpdateFormTitle();
        }

        private void UpdateFormTitle()
        {
            string game = _gameType == "skyrim" ? "Skyrim VR" : "Fallout 4 VR";
            Text = $"OC Unleashed Configurator \u2014 {game}";
        }

        // ── Keyboard Bindings ──

        private void OpenKeyboardBindings()
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                MessageBox.Show("Please select a game folder first.", "No Folder Selected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            using var form = new KeyboardBindingsForm(_gameDir, _gameType);
            form.ShowDialog(this);
        }

        // ── UI Helpers ──

        private static bool ParseBool(string val)
        {
            val = val.Trim().ToLowerInvariant();
            return val == "true" || val == "on" || val == "enabled";
        }

        private static Label MakeLabel(string text, int x, int y, int width) => new()
        {
            Text = text, Location = new Point(x, y),
            Size = new Size(width, 20), ForeColor = Color.FromArgb(200, 200, 200), AutoSize = false
        };

        private static Label MakeSectionLabel(string text, int x, int y) => new()
        {
            Text = text, Location = new Point(x, y), AutoSize = true,
            Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Color.FromArgb(255, 200, 40)
        };

        private static CheckBox MakeCheckBox(string text, int x, int y) => new()
        {
            Text = text, Location = new Point(x, y), AutoSize = true,
            ForeColor = Color.FromArgb(210, 210, 210)
        };

        private static RadioButton MakeRadioButton(string text, int x, int y, int width) => new()
        {
            Text = text, Location = new Point(x, y), Size = new Size(width, 24),
            ForeColor = Color.FromArgb(210, 210, 210)
        };

        private static Button MakeButton(string text, int x, int y, int w, int h) => new()
        {
            Text = text, Location = new Point(x, y), Size = new Size(w, h),
            FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(55, 55, 65),
            ForeColor = Color.White, Cursor = Cursors.Hand
        };

        private static Panel MakeSeparator(int x, int y, int width) => new()
        {
            Location = new Point(x, y), Size = new Size(width, 1),
            BackColor = Color.FromArgb(60, 60, 70)
        };
    }
}
