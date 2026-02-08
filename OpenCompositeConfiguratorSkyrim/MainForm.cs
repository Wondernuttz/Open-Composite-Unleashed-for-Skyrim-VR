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
        private string _mo2ModDir = ""; // Auto-detected if running from mod folder
        private readonly string _gameType; // Set via constructor: "skyrim" or "fallout4"
        private readonly string _gameName; // Display name

        // Top bar
        private TextBox _txtPath = null!;
        private Button _btnBrowse = null!;

        // Tab system (borderless panels with toggle buttons)
        private Button _btnTabSettings = null!;
        private Button _btnTabKeyboard = null!;
        private Panel _tabSettings = null!;
        private Panel _tabKeyboard = null!;

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

        // Keyboard display settings
        private NumericUpDown _nudDisplayTilt = null!;
        private NumericUpDown _nudDisplayOpacity = null!;
        private NumericUpDown _nudDisplayScale = null!;

        // Keyboard sound settings
        private CheckBox _chkSoundsEnabled = null!;
        private NumericUpDown _nudSoundVolume = null!;
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

        // Button positions on the controller image
        private static readonly Dictionary<string, PointF[]> ButtonPositions = new()
        {
            { "left_stick",    new[] { new PointF(0.269f, 0.150f) } },
            { "x",             new[] { new PointF(0.306f, 0.250f) } },
            { "y",             new[] { new PointF(0.353f, 0.194f) } },
            { "right_stick",   new[] { new PointF(0.709f, 0.144f) } },
            { "a",             new[] { new PointF(0.666f, 0.250f) } },
            { "b",             new[] { new PointF(0.625f, 0.189f) } },
        };

        // ═══════════════════════════════════════════════════════════════════════
        // KEYBOARD BINDINGS TAB - Data structures
        // ═══════════════════════════════════════════════════════════════════════

        private Panel _keyboardPanel = null!;
        private ComboBox _cmbAction = null!;
        private Label _lblCurrentBinding = null!;
        private Label _lblKbStatus = null!;
        private Button _btnSaveBindings = null!;
        private Button _btnVRDefaults = null!;
        private Button _btnResetDefaults = null!;

        private string? _selectedKeyId = null;
        private Button? _selectedKeyButton = null;

        private readonly Dictionary<string, int> _keyBindings = new();
        private readonly Dictionary<string, Button> _keyButtons = new();

        // Per-context bindings: context name → (action name → full field array)
        // Each field array has 20 elements matching controlmapvr.txt columns
        private readonly Dictionary<string, List<string[]>> _contextBindings = new();
        private readonly List<string> _contextNames = new();
        // Tracks controller binding changes: context → action → fieldIndex → newHexValue
        // fieldIndex: 6=OculusRight, 7=OculusLeft
        private readonly Dictionary<string, Dictionary<string, Dictionary<int, string>>> _controllerChanges = new();
        private ComboBox _cmbContext = null!;
        private PictureBox _picBindingsController = null!;

        // Oculus button display names
        private static readonly Dictionary<string, string> OculusButtonNames = new()
        {
            { "0xff", "(none)" },
            { "0x21", "A / Trigger" },
            { "0x02", "B / Menu" },
            { "0x07", "X" },
            { "0x01", "Y" },
            { "0x20", "Grip" },
            { "0x0c", "Right Stick" },
            { "0x000b", "Left Stick" },
            { "0x0001", "Stick Up" },
            { "0x0002", "Stick Down" },
            { "0x0004", "Stick Left" },
            { "0x0008", "Stick Right" },
            { "0x1000", "Trigger Click" },
            { "0x2000", "Menu Button" },
            { "0x4000", "X Button" },
            { "0x8000", "Y Button" },
        };

        // DirectInput scancodes
        private static readonly Dictionary<string, int> KeyScancodes = new()
        {
            // Number row
            { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 },
            { "5", 0x06 }, { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 },
            { "9", 0x0A }, { "0", 0x0B }, { "-", 0x0C }, { "=", 0x0D },

            // Top row
            { "Q", 0x10 }, { "W", 0x11 }, { "E", 0x12 }, { "R", 0x13 },
            { "T", 0x14 }, { "Y", 0x15 }, { "U", 0x16 }, { "I", 0x17 },
            { "O", 0x18 }, { "P", 0x19 }, { "[", 0x1A }, { "]", 0x1B },

            // Home row
            { "A", 0x1E }, { "S", 0x1F }, { "D", 0x20 }, { "F", 0x21 },
            { "G", 0x22 }, { "H", 0x23 }, { "J", 0x24 }, { "K", 0x25 },
            { "L", 0x26 }, { ";", 0x27 }, { "'", 0x28 }, { "\\", 0x2B },

            // Bottom row
            { "Z", 0x2C }, { "X", 0x2D }, { "C", 0x2E }, { "V", 0x2F },
            { "B", 0x30 }, { "N", 0x31 }, { "M", 0x32 }, { ",", 0x33 },
            { ".", 0x34 }, { "/", 0x35 },

            // Special keys
            { "Esc", 0x01 }, { "Tab", 0x0F }, { "Caps", 0x3A }, { "LShift", 0x2A },
            { "RShift", 0x36 }, { "LCtrl", 0x1D }, { "RCtrl", 0x9D }, { "LAlt", 0x38 },
            { "RAlt", 0xB8 }, { "Space", 0x39 }, { "Enter", 0x1C }, { "Backspace", 0x0E },
            { "`", 0x29 },

            // Function keys
            { "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E },
            { "F5", 0x3F }, { "F6", 0x40 }, { "F7", 0x41 }, { "F8", 0x42 },
            { "F9", 0x43 }, { "F10", 0x44 }, { "F11", 0x57 }, { "F12", 0x58 },

            // Arrow keys
            { "Up", 0xC8 }, { "Down", 0xD0 }, { "Left", 0xCB }, { "Right", 0xCD },

            // Navigation cluster
            { "Ins", 0xD2 }, { "Del", 0xD3 }, { "Home", 0xC7 }, { "End", 0xCF },
            { "PgUp", 0xC9 }, { "PgDn", 0xD1 },

            // Numpad
            { "Num0", 0x52 }, { "Num1", 0x4F }, { "Num2", 0x50 }, { "Num3", 0x51 },
            { "Num4", 0x4B }, { "Num5", 0x4C }, { "Num6", 0x4D }, { "Num7", 0x47 },
            { "Num8", 0x48 }, { "Num9", 0x49 }, { "Num.", 0x53 }, { "Num+", 0x4E },
            { "Num-", 0x4A }, { "Num*", 0x37 }, { "Num/", 0xB5 }, { "NumEnter", 0x9C },
        };

        // Game actions that can be remapped
        private static readonly (string id, string display, int defaultScancode)[] GameActions = new[]
        {
            ("Forward", "Forward", 0x11),           // W
            ("Back", "Back", 0x1F),                 // S
            ("Strafe Left", "Strafe Left", 0x1E),   // A
            ("Strafe Right", "Strafe Right", 0x20), // D
            ("Jump", "Jump", 0x39),                 // Space
            ("Sprint", "Sprint", 0x38),             // Left Alt
            ("Sneak", "Sneak", 0x1D),               // Left Ctrl
            ("Run", "Run", 0x2A),                   // Left Shift
            ("Toggle Always Run", "Toggle Always Run", 0x3A),
            ("Auto-Move", "Auto-Move", 0x2E),       // C
            ("Activate", "Activate", 0x12),         // E
            ("Ready Weapon", "Ready Weapon", 0x13), // R
            ("Shout", "Shout/Power", 0x2C),         // Z
            ("Tween Menu", "Game Menu (Tab)", 0x0F),
            ("Journal", "Journal", 0x24),           // J
            ("Wait", "Wait", 0x14),                 // T
            ("Favorites", "Favorites", 0x10),       // Q
            ("Quick Inventory", "Quick Inventory", 0x17), // I
            ("Quick Magic", "Quick Magic", 0x19),   // P
            ("Quick Stats", "Quick Stats", 0x35),   // /
            ("Quick Map", "Quick Map", 0x32),       // M
            ("Hotkey1", "Hotkey 1", 0x02),
            ("Hotkey2", "Hotkey 2", 0x03),
            ("Hotkey3", "Hotkey 3", 0x04),
            ("Hotkey4", "Hotkey 4", 0x05),
            ("Hotkey5", "Hotkey 5", 0x06),
            ("Hotkey6", "Hotkey 6", 0x07),
            ("Hotkey7", "Hotkey 7", 0x08),
            ("Hotkey8", "Hotkey 8", 0x09),
            ("Toggle POV", "Toggle POV", 0x21),     // F
            ("Quicksave", "Quicksave", 0x3F),       // F5
            ("Quickload", "Quickload", 0x43),       // F9
            ("Pause", "Pause", 0x01),               // Esc
            ("Console", "Console", 0x29),           // `
        };

        // Controller Combos
        private readonly List<ComboEntry> _combos = new();
        private Panel _comboListPanel = null!;
        private Label _lblComboStatus = null!;

        // Color scheme for bound keys
        private static readonly Color BoundKeyColor = Color.FromArgb(80, 140, 200);
        private static readonly Color SelectedKeyColor = Color.FromArgb(200, 160, 60);
        private static readonly Color UnboundKeyColor = Color.FromArgb(50, 50, 58);

        // ═══════════════════════════════════════════════════════════════════════

        public MainForm(string gameType = "skyrim")
        {
            _gameType = gameType;
            _gameName = gameType == "skyrim" ? "Skyrim VR" : "Fallout 4 VR";

            LoadControllerImage();
            LoadKofiImage();
            LoadWindowIcon();
            LoadConfiguratorSettings();
            InitializeUI();
            SetupButtonMap();
            SetDefaults();
            LoadDefaultKeyBindings();
            ApplyCurrentGamePaths();
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
                string iconResource = _gameType == "skyrim"
                    ? "OpenCompositeConfigurator.Resources.Bluefox.png"
                    : "OpenCompositeConfigurator.Resources.FO4Fox.png";

                var assembly = Assembly.GetExecutingAssembly();
                using var stream = assembly.GetManifestResourceStream(iconResource);
                if (stream != null)
                {
                    using var bitmap = new Bitmap(stream);
                    Icon = Icon.FromHandle(bitmap.GetHicon());
                }
            }
            catch
            {
                try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            }
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
            Text = "OpenComposite Configurator";
            Size = new Size(1280, 1040);
            MinimumSize = new Size(1260, 980);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(30, 30, 35);
            ForeColor = Color.FromArgb(220, 220, 220);
            Font = new Font("Segoe UI", 9.5f);
            AutoScroll = false;

            int y = 12;
            int leftMargin = 16;
            int rightEdge = 1240;

            // ══════════════════════════════════════════════════════════════════
            // TOP BAR (outside tabs)
            // ══════════════════════════════════════════════════════════════════

            var lblGame = MakeLabel($"Game: {_gameName}", leftMargin, y + 3, 200);
            lblGame.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
            lblGame.ForeColor = Color.FromArgb(100, 200, 250);
            Controls.Add(lblGame);

            _txtPath = new TextBox
            {
                Location = new Point(leftMargin + 225, y),
                Width = 610,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.FromArgb(180, 180, 180),
                BorderStyle = BorderStyle.FixedSingle,
                ReadOnly = true,
                Text = "(No folder selected \u2014 click Browse)"
            };
            Controls.Add(_txtPath);

            _btnBrowse = MakeButton("Browse...", leftMargin + 845, y - 1, 110, 27);
            _btnBrowse.Click += BtnBrowse_Click;
            Controls.Add(_btnBrowse);

            y += 40;

            // ══════════════════════════════════════════════════════════════════
            // TAB BUTTONS (borderless — just two toggle buttons, no TabControl)
            // ══════════════════════════════════════════════════════════════════

            _btnTabSettings = new Button
            {
                Text = "Settings",
                Location = new Point(leftMargin, y),
                Size = new Size(120, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                ForeColor = Color.White,
                BackColor = Color.FromArgb(50, 50, 60),
                Cursor = Cursors.Hand,
            };
            _btnTabSettings.FlatAppearance.BorderSize = 0;
            _btnTabSettings.FlatAppearance.MouseOverBackColor = Color.FromArgb(60, 60, 70);
            _btnTabSettings.Click += (s, e) => SwitchTab(0);
            Controls.Add(_btnTabSettings);

            _btnTabKeyboard = new Button
            {
                Text = "Bindings",
                Location = new Point(leftMargin + 125, y),
                Size = new Size(100, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabKeyboard.FlatAppearance.BorderSize = 0;
            _btnTabKeyboard.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabKeyboard.Click += (s, e) => SwitchTab(1);
            Controls.Add(_btnTabKeyboard);

            y += 32;

            // Panel 1: Settings — no scrollbar
            _tabSettings = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 900),
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = true,
            };
            Controls.Add(_tabSettings);

            // Panel 2: Keyboard Bindings — no scrollbar, window is big enough
            _tabKeyboard = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 900),
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabKeyboard);

            // Build content for each tab
            BuildSettingsTab();
            BuildKeyboardTab();

            // Form size
            ClientSize = new Size(ClientSize.Width, Math.Max(_tabSettings.Bottom + 20, 1000));
        }

        // ═══════════════════════════════════════════════════════════════════════
        // SETTINGS TAB
        // ═══════════════════════════════════════════════════════════════════════

        private void BuildSettingsTab()
        {
            var container = _tabSettings;
            int y = 10;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 20;
            int col1 = leftMargin;
            int col2 = leftMargin + 220;

            // MO2 Setup Instructions
            var lblMO2Note = new Label
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 36),
                Text = "MO2 Users: Place this EXE in your mod folder (e.g., \\mods\\OC Unleashed\\) " +
                       "and create a desktop shortcut. Settings will auto-save to both locations.",
                ForeColor = Color.FromArgb(150, 200, 250),
                Font = new Font("Segoe UI", 9f, FontStyle.Italic),
                BackColor = Color.FromArgb(40, 45, 60),
                BorderStyle = BorderStyle.FixedSingle,
                Padding = new Padding(8, 8, 8, 8),
                AutoSize = false
            };
            container.Controls.Add(lblMO2Note);

            y += 48;
            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── SAVE/RELOAD BUTTONS (top right corner) ──
            _btnSave = MakeButton("Save opencomposite.ini", rightEdge - 310, y, 200, 30);
            _btnSave.BackColor = Color.FromArgb(40, 120, 40);
            _btnSave.ForeColor = Color.White;
            _btnSave.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            _btnSave.Click += BtnSave_Click;
            container.Controls.Add(_btnSave);

            _btnReload = MakeButton("Reload", rightEdge - 100, y, 90, 30);
            _btnReload.Click += BtnReload_Click;
            container.Controls.Add(_btnReload);

            // ── KEYBOARD SHORTCUT SECTION ──
            var lblSection = MakeSectionLabel("VR Keyboard Shortcut", leftMargin, y);
            container.Controls.Add(lblSection);
            y += 28;

            _chkShortcutEnabled = MakeCheckBox("Enable controller shortcut to open keyboard anywhere", leftMargin, y);
            _chkShortcutEnabled.Checked = true;
            container.Controls.Add(_chkShortcutEnabled);

            var lblComboHint = MakeLabel("Select one or more buttons below \u2014 all must be pressed together to activate.", leftMargin + 28, y + 20, 500);
            lblComboHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblComboHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            container.Controls.Add(lblComboHint);
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
            container.Controls.Add(_picControllers);

            // Button checkboxes to the right of the image
            int cx = leftMargin + 340;
            int chkY = y;

            var lblLeft = MakeLabel("Left Hand", cx, chkY, 100);
            lblLeft.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblLeft.ForeColor = Color.FromArgb(180, 200, 255);
            container.Controls.Add(lblLeft);

            var lblRight = MakeLabel("Right Hand", cx + 200, chkY, 100);
            lblRight.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblRight.ForeColor = Color.FromArgb(180, 200, 255);
            container.Controls.Add(lblRight);

            chkY += 22;

            _chkLeftStick = MakeCheckBox("Stick Click", cx, chkY);
            _chkRightStick = MakeCheckBox("Stick Click", cx + 200, chkY);
            container.Controls.Add(_chkLeftStick);
            container.Controls.Add(_chkRightStick);
            chkY += 22;

            _chkLeftY = MakeCheckBox("Y Button", cx, chkY);
            _chkRightB = MakeCheckBox("B Button", cx + 200, chkY);
            container.Controls.Add(_chkLeftY);
            container.Controls.Add(_chkRightB);
            chkY += 22;

            _chkLeftX = MakeCheckBox("X Button", cx, chkY);
            _chkRightA = MakeCheckBox("A Button", cx + 200, chkY);
            container.Controls.Add(_chkLeftX);
            container.Controls.Add(_chkRightA);
            chkY += 22;

            var lblLeftGrip = MakeLabel("Grip = Exit Keyboard", cx, chkY + 2, 190);
            lblLeftGrip.ForeColor = Color.FromArgb(140, 140, 140);
            lblLeftGrip.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            var lblRightGrip = MakeLabel("Grip = Exit Keyboard", cx + 200, chkY + 2, 190);
            lblRightGrip.ForeColor = Color.FromArgb(140, 140, 140);
            lblRightGrip.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            container.Controls.Add(lblLeftGrip);
            container.Controls.Add(lblRightGrip);
            chkY += 22;

            var lblLeftTrigger = MakeLabel("Trigger = Laser Click", cx, chkY + 2, 190);
            lblLeftTrigger.ForeColor = Color.FromArgb(140, 140, 140);
            lblLeftTrigger.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            var lblRightTrigger = MakeLabel("Trigger = Laser Click", cx + 200, chkY + 2, 190);
            lblRightTrigger.ForeColor = Color.FromArgb(140, 140, 140);
            lblRightTrigger.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            container.Controls.Add(lblLeftTrigger);
            container.Controls.Add(lblRightTrigger);

            foreach (var chk in new[] { _chkLeftStick, _chkLeftX, _chkLeftY,
                                        _chkRightStick, _chkRightA, _chkRightB })
            {
                chk.CheckedChanged += (s, e) => _picControllers.Invalidate();
            }

            int imageBottom = _picControllers.Bottom + 10;
            chkY += 28;
            y = Math.Max(imageBottom, chkY);

            // Tap count
            var lblTaps = MakeLabel("Tap Count:", leftMargin, y + 3, 100);
            container.Controls.Add(lblTaps);

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
            container.Controls.Add(tapPanel);

            var lblTiming = MakeLabel("Timing (ms):", leftMargin + 430, y + 3, 100);
            container.Controls.Add(lblTiming);

            _nudTiming = new NumericUpDown
            {
                Location = new Point(leftMargin + 535, y),
                Width = 80,
                Minimum = 100, Maximum = 3000, Increment = 50, Value = 500,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White
            };
            container.Controls.Add(_nudTiming);

            _lblTimingDesc = MakeLabel("Max time between taps", leftMargin + 625, y + 3, 220);
            _lblTimingDesc.ForeColor = Color.FromArgb(140, 140, 140);
            container.Controls.Add(_lblTimingDesc);

            _rdoX1.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX2.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX3.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX4.CheckedChanged += (s, e) => UpdateTimingLabel();

            y += 38;

            // ── KEYBOARD DISPLAY SETTINGS ──
            var lblDisplay = MakeLabel("Keyboard Display:", leftMargin, y + 3, 140);
            lblDisplay.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            lblDisplay.ForeColor = Color.FromArgb(180, 200, 255);
            container.Controls.Add(lblDisplay);

            var lblTilt = MakeLabel("Tilt:", leftMargin + 150, y + 3, 40);
            container.Controls.Add(lblTilt);
            _nudDisplayTilt = new NumericUpDown
            {
                Location = new Point(leftMargin + 190, y),
                Width = 70,
                DecimalPlaces = 1, Increment = 1m, Minimum = -30m, Maximum = 80m, Value = 22.5m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayTilt);
            container.Controls.Add(MakeLabel("\u00B0", leftMargin + 265, y + 3, 20));

            container.Controls.Add(MakeLabel("Transparency:", leftMargin + 320, y + 3, 110));
            _nudDisplayOpacity = new NumericUpDown
            {
                Location = new Point(leftMargin + 430, y),
                Width = 70,
                Minimum = 1, Maximum = 100, Increment = 5, Value = 30,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayOpacity);
            container.Controls.Add(MakeLabel("%", leftMargin + 505, y + 3, 25));

            container.Controls.Add(MakeLabel("Size:", leftMargin + 545, y + 3, 40));
            _nudDisplayScale = new NumericUpDown
            {
                Location = new Point(leftMargin + 585, y),
                Width = 70,
                Minimum = 50, Maximum = 150, Increment = 5, Value = 100,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayScale);
            container.Controls.Add(MakeLabel("%", leftMargin + 660, y + 3, 25));

            var lblDisplayHint = MakeLabel("(adjustable in VR)", leftMargin + 700, y + 3, 200);
            lblDisplayHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblDisplayHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            container.Controls.Add(lblDisplayHint);

            y += 34;

            // ── KEYBOARD FEEDBACK SETTINGS ──
            _chkSoundsEnabled = MakeCheckBox("Enable keyboard feedback", leftMargin, y);
            _chkSoundsEnabled.Checked = true;
            container.Controls.Add(_chkSoundsEnabled);
            y += 26;

            container.Controls.Add(MakeLabel("Master Volume:", leftMargin + 28, y + 3, 120));
            _nudSoundVolume = new NumericUpDown
            {
                Location = new Point(leftMargin + 155, y),
                Width = 70,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudSoundVolume);
            container.Controls.Add(MakeLabel("%", leftMargin + 230, y + 3, 25));

            container.Controls.Add(MakeLabel("Key Press Volume:", leftMargin + 280, y + 3, 130));
            _nudPressVolume = new NumericUpDown
            {
                Location = new Point(leftMargin + 415, y),
                Width = 70,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudPressVolume);
            container.Controls.Add(MakeLabel("%", leftMargin + 490, y + 3, 25));

            container.Controls.Add(MakeLabel("Haptic Feedback:", leftMargin + 540, y + 3, 120));
            _nudKbHapticStrength = new NumericUpDown
            {
                Location = new Point(leftMargin + 665, y),
                Width = 70,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudKbHapticStrength);
            container.Controls.Add(MakeLabel("%", leftMargin + 740, y + 3, 25));

            y += 38;
            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── RESTART NOTICE ──
            var lblRestartNote = MakeLabel("Settings below require game restart to take effect", leftMargin, y, rightEdge - leftMargin);
            lblRestartNote.ForeColor = Color.FromArgb(255, 180, 100);
            lblRestartNote.Font = new Font("Segoe UI", 9f, FontStyle.Italic);
            container.Controls.Add(lblRestartNote);
            y += 22;

            // ── GENERAL + SKYRIM SETTINGS SIDE BY SIDE ──
            int settingsY = y;

            var lblGeneral = MakeSectionLabel("General Settings", leftMargin, y);
            container.Controls.Add(lblGeneral);
            y += 26;

            int gc1 = leftMargin;
            int gc2 = leftMargin + 220;

            container.Controls.Add(MakeLabel("Supersampling:", gc1, y + 3, 120));
            _nudSuperSample = new NumericUpDown
            {
                Location = new Point(gc1 + 120, y), Width = 75,
                DecimalPlaces = 1, Increment = 0.1m, Minimum = 0.5m, Maximum = 3.0m, Value = 1.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudSuperSample);

            container.Controls.Add(MakeLabel("Haptic Strength:", gc2, y + 3, 120));
            _nudHapticStrength = new NumericUpDown
            {
                Location = new Point(gc2 + 120, y), Width = 75,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.1m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudHapticStrength);
            y += 28;

            _chkRenderHands = MakeCheckBox("Render custom hands", gc1, y);
            _chkRenderHands.Checked = true;
            container.Controls.Add(_chkRenderHands);
            _chkHaptics = MakeCheckBox("Enable haptics", gc2, y);
            _chkHaptics.Checked = true;
            container.Controls.Add(_chkHaptics);
            y += 24;

            _chkHiddenMesh = MakeCheckBox("Enable hidden mesh fix", gc1, y);
            _chkHiddenMesh.Checked = true;
            container.Controls.Add(_chkHiddenMesh);
            _chkInvertShaders = MakeCheckBox("Invert using shaders", gc2, y);
            container.Controls.Add(_chkInvertShaders);
            y += 24;

            _chkDx10 = MakeCheckBox("DX10 mode (compatibility)", gc1, y);
            container.Controls.Add(_chkDx10);
            y += 28;

            int generalBottom = y;

            // ── SKYRIM-ONLY SETTINGS (right side) ──
            _pnlSkyrimOnly = new Panel
            {
                Location = new Point(leftMargin + 440, settingsY),
                Size = new Size(rightEdge - leftMargin - 440, 200),
                BackColor = Color.Transparent
            };
            container.Controls.Add(_pnlSkyrimOnly);

            int sy = 0;
            int sc1 = 0;
            int sc2 = 210;

            var lblSkyrim = MakeSectionLabel("Skyrim VR Settings", sc1, sy);
            _pnlSkyrimOnly.Controls.Add(lblSkyrim);
            sy += 26;

            _chkInputSmoothing = MakeCheckBox("Input smoothing", sc1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkInputSmoothing);
            _pnlSkyrimOnly.Controls.Add(MakeLabel("Window:", sc2, sy + 3, 60));
            _nudInputWindow = new NumericUpDown
            {
                Location = new Point(sc2 + 60, sy), Width = 55,
                Minimum = 1, Maximum = 20, Value = 5,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudInputWindow);
            sy += 26;

            _chkControllerSmoothing = MakeCheckBox("Controller smoothing", sc1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkControllerSmoothing);
            _chkDisableTriggerTouch = MakeCheckBox("Disable trigger touch", sc2, sy);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTriggerTouch);
            sy += 26;

            _chkDisableTrackpad = MakeCheckBox("Disable trackpad", sc1, sy);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTrackpad);
            _chkVRIKKnuckles = MakeCheckBox("VRIK Knuckles support", sc2, sy);
            _pnlSkyrimOnly.Controls.Add(_chkVRIKKnuckles);
            sy += 26;

            _pnlSkyrimOnly.Controls.Add(MakeLabel("Left dead zone:", sc1, sy + 3, 110));
            _nudLeftDeadZone = new NumericUpDown
            {
                Location = new Point(sc1 + 110, sy), Width = 70,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudLeftDeadZone);
            _pnlSkyrimOnly.Controls.Add(MakeLabel("Right:", sc2, sy + 3, 50));
            _nudRightDeadZone = new NumericUpDown
            {
                Location = new Point(sc2 + 50, sy), Width = 70,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudRightDeadZone);
            sy += 28;

            _pnlSkyrimOnly.Size = new Size(rightEdge - leftMargin - 440, sy);

            int skyrimBottom = _gameType == "skyrim" ? settingsY + _pnlSkyrimOnly.Height : 0;
            y = Math.Max(generalBottom, skyrimBottom) + 8;
            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── AUDIO SWITCH ──
            _chkAudioSwitch = MakeCheckBox("Auto-switch audio to headset (not for Virtual Desktop)", leftMargin, y);
            container.Controls.Add(_chkAudioSwitch);

            container.Controls.Add(MakeLabel("Device name:", leftMargin + 400, y + 3, 100));
            _txtAudioDevice = new TextBox
            {
                Location = new Point(leftMargin + 505, y), Width = 120,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                BorderStyle = BorderStyle.FixedSingle, Text = "quest"
            };
            container.Controls.Add(_txtAudioDevice);
            var lblAudioHint = MakeLabel("(partial match)", leftMargin + 635, y + 3, 100);
            lblAudioHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblAudioHint.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            container.Controls.Add(lblAudioHint);
            y += 32;

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── KO-FI SECTION ──
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
                container.Controls.Add(_picKofi);

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
                container.Controls.Add(lblKofiLink);

                var lblKofiQuote = MakeLabel(
                    "\u201CI do this all for free and for the love of the VR gaming community, and I always will. "
                    + "I\u2019ll never demand anything in return, but if you want to show support, I won\u2019t complain. "
                    + "I love bringing VR to life with you all regardless.\u201D",
                    leftMargin + kofiIconSize + 8, y + 24, rightEdge - leftMargin - kofiIconSize - 12);
                lblKofiQuote.ForeColor = Color.FromArgb(160, 160, 160);
                lblKofiQuote.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
                lblKofiQuote.AutoSize = false;
                lblKofiQuote.Size = new Size(rightEdge - leftMargin - kofiIconSize - 12, 48);
                container.Controls.Add(lblKofiQuote);

                y += 80;
            }

            // Status label at the bottom
            _lblStatus = MakeLabel("", leftMargin, y + 6, 600);
            _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
            container.Controls.Add(_lblStatus);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // BINDINGS TAB
        // ═══════════════════════════════════════════════════════════════════════

        private void ParseControlmapTemplate()
        {
            var asm = Assembly.GetExecutingAssembly();
            using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt");
            if (stream == null) return;
            using var reader = new StreamReader(stream);
            string[] lines = reader.ReadToEnd().Split('\n');

            string currentContext = "";
            foreach (string rawLine in lines)
            {
                string line = rawLine.TrimEnd('\r');
                if (string.IsNullOrWhiteSpace(line)) continue;

                if (line.TrimStart().StartsWith("//"))
                {
                    string comment = line.TrimStart().TrimStart('/').Trim();
                    // Strip column headers after context name (e.g. "(Vive---) (Oculus---)")
                    int tabIdx = comment.IndexOf('\t');
                    if (tabIdx >= 0) comment = comment.Substring(0, tabIdx).Trim();
                    // Detect context headers (skip ordinal/column-description comments)
                    if (comment.Length > 0 && !comment.StartsWith("1st") && !comment.StartsWith("2nd") &&
                        !comment.StartsWith("3rd") && !comment.StartsWith("4th") && !comment.StartsWith("5th") &&
                        !comment.StartsWith("6th") && !comment.StartsWith("7th") && !comment.StartsWith("8th") &&
                        !comment.StartsWith("9th") && !comment.StartsWith("10th") && !comment.StartsWith("11th") &&
                        !comment.StartsWith("12th") && !comment.StartsWith("13th") && !comment.StartsWith("14th") &&
                        !comment.StartsWith("15th") && !comment.StartsWith("16th") && !comment.StartsWith("17th") &&
                        !comment.StartsWith("18th") && !comment.StartsWith("19th") && !comment.StartsWith("20th") &&
                        !comment.StartsWith("Blank") && !comment.StartsWith("See") &&
                        !comment.StartsWith("(Vive") && !comment.StartsWith("(Oculus") && !comment.StartsWith("(Windows") &&
                        !comment.StartsWith("\"") && !comment.StartsWith("If "))
                    {
                        currentContext = comment;
                        if (!_contextBindings.ContainsKey(currentContext))
                        {
                            _contextBindings[currentContext] = new List<string[]>();
                            _contextNames.Add(currentContext);
                        }
                    }
                    continue;
                }

                if (string.IsNullOrEmpty(currentContext)) continue;

                // Parse tab-separated fields (up to 20), remove empty entries from consecutive tabs
                var fields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                // Trim whitespace from each field
                for (int i = 0; i < fields.Length; i++) fields[i] = fields[i].Trim();
                if (fields.Length >= 2)
                {
                    _contextBindings[currentContext].Add(fields);
                }
            }
        }

        // Simplified display names for controlmap contexts
        private static readonly Dictionary<string, string> ContextDisplayNames = new()
        {
            { "Main Gameplay", "Gameplay" },
            { "Menu Mode", "Menus" },
            { "Console", "Console" },
            { "Item Menus", "Item Menus" },
            { "Inventory", "Inventory" },
            { "Debug Text", "Debug Text" },
            { "Favorites menu", "Favorites" },
            { "Map Menu", "Map" },
            { "Stats", "Stats" },
            { "Cursor", "Cursor" },
            { "Book", "Book" },
            { "Debug overlay", "Debug Overlay" },
            { "Journal", "Journal" },
            { "TFC mode", "Free Camera" },
            { "Debug Map Menu-like mode", "Debug Map" },
            { "Lockpicking", "Lockpicking" },
            { "Favor", "Favor" },
            { "Crafting Menus", "Crafting" },
            { "Barter Menus", "Barter" },
            { "Race Sex Menu", "Character Creator" },
            { "Dialogue Menu", "Dialogue" },
        };

        // Controller button positions for bindings tab (bigger image, more buttons)
        private static readonly Dictionary<string, (string display, PointF pos, bool isStickDir)> ControllerButtons = new()
        {
            // Left controller
            { "left_stick",  ("L Stick Click", new PointF(0.272f, 0.153f), false) },
            { "x_button",   ("X Button",      new PointF(0.308f, 0.254f), false) },
            { "y_button",   ("Y Button",      new PointF(0.353f, 0.189f), false) },
            { "l_trigger",  ("L Trigger",     new PointF(0.455f, 0.106f), false) },
            { "l_grip",     ("L Grip",        new PointF(0.383f, 0.512f), false) },
            // Right controller
            { "right_stick", ("R Stick Click", new PointF(0.713f, 0.147f), false) },
            { "a_button",   ("A Button",      new PointF(0.670f, 0.254f), false) },
            { "b_button",   ("B Button",      new PointF(0.627f, 0.189f), false) },
            { "r_trigger",  ("R Trigger",     new PointF(0.537f, 0.106f), false) },
            { "r_grip",     ("R Grip",        new PointF(0.605f, 0.515f), false) },
            // Left stick directions (offset 0.060 vertical, 0.045 horizontal)
            { "left_stick_up",    ("L Stick Up",    new PointF(0.272f, 0.153f - 0.060f), true) },
            { "left_stick_down",  ("L Stick Down",  new PointF(0.272f, 0.153f + 0.060f), true) },
            { "left_stick_left",  ("L Stick Left",  new PointF(0.272f - 0.045f, 0.153f), true) },
            { "left_stick_right", ("L Stick Right", new PointF(0.272f + 0.045f, 0.153f), true) },
            // Right stick directions
            { "right_stick_up",    ("R Stick Up",    new PointF(0.713f, 0.147f - 0.060f), true) },
            { "right_stick_down",  ("R Stick Down",  new PointF(0.713f, 0.147f + 0.060f), true) },
            { "right_stick_left",  ("R Stick Left",  new PointF(0.713f - 0.045f, 0.147f), true) },
            { "right_stick_right", ("R Stick Right", new PointF(0.713f + 0.045f, 0.147f), true) },
        };

        // Map controller button IDs to their hex codes in controlmapvr (Oculus Right = field 6, Left = field 7)
        // OpenVR button IDs: 0x01=B/Y(AppMenu), 0x02=Grip, 0x07=A/X, 0x20=StickPress, 0x21=Trigger
        // NOTE: 0x0b/0x0c = stick AXIS (movement/look), 0x20 = stick PRESS (click in)
        private static readonly Dictionary<string, (string hexRight, string hexLeft)> ControllerButtonHex = new()
        {
            { "left_stick",  ("",     "0x20") },   // Left stick press (Sprint in Gameplay)
            { "x_button",    ("",     "0x07") },   // X = A/X button on left hand
            { "y_button",    ("",     "0x01") },   // Y = AppMenu button on left hand
            { "l_trigger",   ("",     "0x21") },   // Left trigger
            { "l_grip",      ("",     "0x02") },   // Left grip (Cancel, Ready Weapon)
            { "right_stick", ("0x20", "") },        // Right stick press
            { "a_button",    ("0x07", "") },        // A = A/X button on right hand
            { "b_button",    ("0x01", "") },        // B = AppMenu button on right hand
            { "r_trigger",   ("0x21", "") },        // Right trigger
            { "r_grip",      ("0x02", "") },        // Right grip (Shout, Cancel)
        };

        private string? _hoveredCtrlButton = null;
        private string? _selectedCtrlButton = null;
        private Label _lblCtrlButton = null!;
        private ComboBox _cmbCtrlAction = null!;
        private ComboBox _cmbCtrlType = null!;
        private CheckBox _chkDisableMouse = null!;

        private void BuildKeyboardTab()
        {
            // Parse template for all contexts
            ParseControlmapTemplate();

            var container = _tabKeyboard;
            int y = 4;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 20;

            // ══════════════════════════════════════════════════════════
            // KEYBOARD SECTION
            // ══════════════════════════════════════════════════════════

            var lblKbSection = MakeSectionLabel("Keyboard Bindings", leftMargin, y);
            container.Controls.Add(lblKbSection);

            // Keyboard mode dropdown
            container.Controls.Add(MakeLabel("Type:", leftMargin + 180, y + 2, 40));
            _cmbContext = new ComboBox
            {
                Location = new Point(leftMargin + 225, y),
                Width = 150,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f)
            };
            foreach (var ctx in _contextNames)
            {
                string display = ContextDisplayNames.GetValueOrDefault(ctx, ctx);
                _cmbContext.Items.Add(display);
            }
            if (_cmbContext.Items.Count > 0) _cmbContext.SelectedIndex = 0;
            container.Controls.Add(_cmbContext);

            // Disable mouse checkbox
            _chkDisableMouse = new CheckBox
            {
                Text = "Disable Mouse Bindings (VR)",
                Location = new Point(leftMargin + 400, y + 2),
                Size = new Size(210, 20),
                ForeColor = Color.FromArgb(220, 180, 100),
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.Transparent,
                Checked = false
            };
            container.Controls.Add(_chkDisableMouse);

            // Save button top right
            _btnSaveBindings = MakeButton("Save controlmapvr.txt", rightEdge - 170, y, 180, 26);
            _btnSaveBindings.BackColor = Color.FromArgb(40, 120, 40);
            _btnSaveBindings.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            _btnSaveBindings.Click += BtnSaveBindings_Click;
            container.Controls.Add(_btnSaveBindings);

            y += 26;

            // Key selection + action binding row
            container.Controls.Add(MakeLabel("Key:", leftMargin, y + 4, 35));

            _lblCurrentBinding = new Label
            {
                Text = "(click a key)",
                Location = new Point(leftMargin + 35, y + 4),
                Size = new Size(80, 20),
                ForeColor = Color.FromArgb(255, 200, 40),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold)
            };
            container.Controls.Add(_lblCurrentBinding);

            container.Controls.Add(MakeLabel("Action:", leftMargin + 120, y + 4, 50));

            _cmbAction = new ComboBox
            {
                Location = new Point(leftMargin + 172, y),
                Width = 180,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f)
            };
            _cmbAction.Items.Add("(none - unbind)");
            foreach (var action in GameActions)
                _cmbAction.Items.Add(action.display);
            _cmbAction.SelectedIndex = 0;
            _cmbAction.SelectedIndexChanged += CmbAction_SelectedIndexChanged;
            _cmbAction.Enabled = false;
            container.Controls.Add(_cmbAction);

            _btnVRDefaults = MakeButton("VR Safe Defaults", leftMargin + 370, y, 120, 24);
            _btnVRDefaults.BackColor = Color.FromArgb(40, 100, 160);
            _btnVRDefaults.Font = new Font("Segoe UI", 8f);
            _btnVRDefaults.Click += BtnVRDefaults_Click;
            container.Controls.Add(_btnVRDefaults);

            _btnResetDefaults = MakeButton("Reset to Game Defaults", leftMargin + 500, y, 140, 24);
            _btnResetDefaults.Font = new Font("Segoe UI", 8f);
            _btnResetDefaults.Click += (s, e) => ResetControlmapToDefaults();
            container.Controls.Add(_btnResetDefaults);

            y += 28;

            // Keyboard visual
            _keyboardPanel = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(1120, 300),
                BackColor = Color.FromArgb(25, 25, 30)
            };
            container.Controls.Add(_keyboardPanel);
            CreateKeyboardLayout();

            y += 305;

            // ══════════════════════════════════════════════════════════
            // CONTROLLER BINDINGS + COMBOS (side by side)
            // Left: Combos list   |   Right: Controller image + bindings
            // ══════════════════════════════════════════════════════════

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 8;

            // ── Top row: Controller Bindings header + controls on the right ──
            int splitX = 440; // divider between combos (left) and controller (right)

            var lblCtrlSection = MakeSectionLabel("Controller Bindings", splitX + 10, y);
            container.Controls.Add(lblCtrlSection);

            // Type dropdown
            container.Controls.Add(MakeLabel("Type:", splitX + 200, y + 2, 40));
            _cmbCtrlType = new ComboBox
            {
                Location = new Point(splitX + 240, y),
                Width = 150,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f)
            };
            foreach (var ctx in _contextNames)
            {
                string display = ContextDisplayNames.GetValueOrDefault(ctx, ctx);
                _cmbCtrlType.Items.Add(display);
            }
            if (_cmbCtrlType.Items.Count > 0) _cmbCtrlType.SelectedIndex = 0;
            _cmbCtrlType.SelectedIndexChanged += CmbCtrlType_SelectedIndexChanged;
            container.Controls.Add(_cmbCtrlType);

            // ── Left column: Controller Combos ──
            var lblComboSection = MakeSectionLabel("Controller Combos", leftMargin, y);
            container.Controls.Add(lblComboSection);

            y += 26;

            // Button / Action row for controller bindings (right side)
            container.Controls.Add(MakeLabel("Button:", splitX + 10, y + 2, 50));
            _lblCtrlButton = new Label
            {
                Text = "(click a button)",
                Location = new Point(splitX + 65, y + 2),
                Size = new Size(120, 20),
                ForeColor = Color.FromArgb(255, 200, 40),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold)
            };
            container.Controls.Add(_lblCtrlButton);

            container.Controls.Add(MakeLabel("Action:", splitX + 200, y + 2, 50));
            _cmbCtrlAction = new ComboBox
            {
                Location = new Point(splitX + 250, y),
                Width = 200,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f),
                Enabled = false
            };
            _cmbCtrlAction.Items.Add("(none)");
            PopulateActionComboFromTemplate(_cmbCtrlAction);
            _cmbCtrlAction.SelectedIndex = 0;
            _cmbCtrlAction.SelectedIndexChanged += CmbCtrlAction_SelectedIndexChanged;
            container.Controls.Add(_cmbCtrlAction);

            // Left: combo description
            var lblComboDesc = new Label
            {
                Text = "Map button combos to keyboard keys",
                Location = new Point(leftMargin, y + 2),
                Size = new Size(splitX - leftMargin - 10, 18),
                ForeColor = Color.FromArgb(130, 130, 130),
                Font = new Font("Segoe UI", 8f, FontStyle.Italic)
            };
            container.Controls.Add(lblComboDesc);

            y += 26;

            int sideY = y; // both columns start here

            // ── Right column: Controller image ──
            int imgWidth = rightEdge - splitX - 10;
            int imgHeight = (int)(imgWidth / 1.78f); // 16:9 aspect
            _picBindingsController = new PictureBox
            {
                Location = new Point(splitX + 10, sideY),
                Size = new Size(imgWidth, imgHeight),
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(30, 30, 35),
                Image = _controllerImage,
                Cursor = Cursors.Hand
            };
            _picBindingsController.Paint += PicBindingsController_Paint;
            _picBindingsController.MouseMove += PicBindingsController_MouseMove;
            _picBindingsController.MouseClick += PicBindingsController_MouseClick;
            container.Controls.Add(_picBindingsController);

            // Vertical separator between combos and controller
            var sepPanel = new Panel
            {
                Location = new Point(splitX, sideY),
                Size = new Size(1, imgHeight),
                BackColor = Color.FromArgb(60, 60, 65)
            };
            container.Controls.Add(sepPanel);

            // ── Left column: Combo list + Add button ──
            int comboListWidth = splitX - leftMargin - 10;

            // Add Combo button at top of list
            var btnAddCombo = MakeButton("+ Add Combo", leftMargin, sideY, 110, 26);
            btnAddCombo.BackColor = Color.FromArgb(40, 100, 160);
            btnAddCombo.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
            btnAddCombo.Click += BtnAddCombo_Click;
            container.Controls.Add(btnAddCombo);

            _lblComboStatus = MakeLabel("", leftMargin + 120, sideY + 4, comboListWidth - 120);
            _lblComboStatus.ForeColor = Color.FromArgb(130, 130, 130);
            container.Controls.Add(_lblComboStatus);

            // Combo list panel fills remaining height
            int comboListTop = sideY + 30;
            int comboListHeight = imgHeight - 30;
            _comboListPanel = new Panel
            {
                Location = new Point(leftMargin, comboListTop),
                Size = new Size(comboListWidth, Math.Max(comboListHeight, 100)),
                BackColor = Color.FromArgb(25, 25, 30),
                AutoScroll = false,
                BorderStyle = BorderStyle.None
            };
            container.Controls.Add(_comboListPanel);

            // Status label below both columns
            y = sideY + imgHeight + 6;

            _lblKbStatus = MakeLabel("", leftMargin, y, 800);
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
            container.Controls.Add(_lblKbStatus);
            y += 24;
        }

        // ── Binding lookup helper ──

        /// <summary>
        /// Gets the context name (original key) for the currently selected display name in a Type combobox.
        /// </summary>
        private string? GetSelectedContextName(ComboBox cmb)
        {
            if (cmb.SelectedIndex < 0 || cmb.SelectedIndex >= _contextNames.Count) return null;
            return _contextNames[cmb.SelectedIndex];
        }

        /// <summary>
        /// Looks up which action has a given hex value in a given field index for the specified context.
        /// fieldIndex: 1=keyboard, 2=mouse, 6=OculusRight, 7=OculusLeft
        /// </summary>
        private string? FindActionForHexInContext(string contextName, int fieldIndex, string hexValue)
        {
            if (!_contextBindings.TryGetValue(contextName, out var actions)) return null;
            foreach (var fields in actions)
            {
                if (fields.Length <= fieldIndex) continue;
                string val = fields[fieldIndex].Trim().ToLowerInvariant();
                // Handle comma-separated multi-bindings like "0x0001,0x0002"
                var parts = val.Split(',');
                foreach (var part in parts)
                {
                    if (part.Trim() == hexValue.ToLowerInvariant())
                        return fields[0]; // action name
                }
            }
            return null;
        }

        /// <summary>
        /// Sets a combo box to the action matching actionName, or index 0 if not found.
        /// Searches combo items by text (action name).
        /// </summary>
        private void SelectActionInCombo(ComboBox cmb, string? actionName)
        {
            _suppressCtrlActionChange = true;
            try
            {
                if (actionName == null) { cmb.SelectedIndex = 0; return; }
                for (int i = 0; i < cmb.Items.Count; i++)
                {
                    if (cmb.Items[i]?.ToString() == actionName)
                    {
                        cmb.SelectedIndex = i;
                        return;
                    }
                }
                cmb.SelectedIndex = 0;
            }
            finally { _suppressCtrlActionChange = false; }
        }

        /// <summary>
        /// Populates a combo box with all unique action names from all template contexts.
        /// </summary>
        private void PopulateActionComboFromTemplate(ComboBox cmb)
        {
            var seen = new HashSet<string>();
            foreach (var ctx in _contextNames)
            {
                if (!_contextBindings.TryGetValue(ctx, out var actions)) continue;
                foreach (var fields in actions)
                {
                    string actionName = fields[0];
                    if (seen.Add(actionName))
                        cmb.Items.Add(actionName);
                }
            }
        }

        // ── Controller image interaction ──

        private (float drawW, float drawH, float offX, float offY) GetBindingsImageBounds()
        {
            float imgW = _picBindingsController.Width;
            float imgH = _picBindingsController.Height;
            float imgAspect = _controllerImage != null ? (float)_controllerImage.Width / _controllerImage.Height : 1.6f;
            float boxAspect = imgW / imgH;
            if (boxAspect > imgAspect)
                return (imgH * imgAspect, imgH, (imgW - imgH * imgAspect) / 2, 0);
            else
                return (imgW, imgW / imgAspect, 0, (imgH - imgW / imgAspect) / 2);
        }

        private string? HitTestControllerButton(float fx, float fy)
        {
            string? closest = null;
            float closestDist = float.MaxValue;
            foreach (var kvp in ControllerButtons)
            {
                float hitRadius = kvp.Value.isStickDir ? 0.025f : 0.04f;
                float dx = fx - kvp.Value.pos.X;
                float dy = fy - kvp.Value.pos.Y;
                float dist = (float)Math.Sqrt(dx * dx + dy * dy);
                if (dist < hitRadius && dist < closestDist)
                {
                    closestDist = dist;
                    closest = kvp.Key;
                }
            }
            return closest;
        }

        private void PicBindingsController_MouseMove(object? sender, MouseEventArgs e)
        {
            var (drawW, drawH, offX, offY) = GetBindingsImageBounds();
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            string? hit = HitTestControllerButton(fx, fy);
            if (hit != _hoveredCtrlButton)
            {
                _hoveredCtrlButton = hit;
                _picBindingsController.Invalidate();
            }
        }

        private void PicBindingsController_MouseClick(object? sender, MouseEventArgs e)
        {
            var (drawW, drawH, offX, offY) = GetBindingsImageBounds();
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            string? hit = HitTestControllerButton(fx, fy);
            if (hit != null && ControllerButtons.TryGetValue(hit, out var info))
            {
                _selectedCtrlButton = hit;
                _lblCtrlButton.Text = info.display;
                _picBindingsController.Invalidate();

                if (info.isStickDir)
                {
                    // Stick directions are axis-based (engine-mapped), show known defaults
                    _cmbCtrlAction.Enabled = false;
                    SelectActionInCombo(_cmbCtrlAction, null);
                    string dirAction = hit switch
                    {
                        "left_stick_up" => "Forward",
                        "left_stick_down" => "Back",
                        "left_stick_left" => "Strafe Left",
                        "left_stick_right" => "Strafe Right",
                        "right_stick_up" => "Jump",
                        "right_stick_down" => "Toggle Sneak",
                        "right_stick_left" => "Look Left",
                        "right_stick_right" => "Look Right",
                        _ => "Unknown"
                    };
                    _lblKbStatus.Text = $"{info.display}: {dirAction} (VR axis — not remappable here)";
                    _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
                }
                else
                {
                    _cmbCtrlAction.Enabled = true;

                    // Look up current binding
                    string? ctx = GetSelectedContextName(_cmbCtrlType);
                    string? action = null;
                    if (ctx != null && ControllerButtonHex.TryGetValue(hit, out var hex))
                    {
                        if (!string.IsNullOrEmpty(hex.hexRight))
                            action = FindActionForHexInContext(ctx, 6, hex.hexRight);
                        if (action == null && !string.IsNullOrEmpty(hex.hexLeft))
                            action = FindActionForHexInContext(ctx, 7, hex.hexLeft);
                    }
                    SelectActionInCombo(_cmbCtrlAction, action);

                    _lblKbStatus.Text = action != null
                        ? $"{info.display}: {action}"
                        : $"{info.display}: Not Used";
                    _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
                }
            }
        }

        private void CmbCtrlType_SelectedIndexChanged(object? sender, EventArgs e)
        {
            // Re-lookup the binding when context type changes with a button selected
            if (_selectedCtrlButton == null) return;
            if (!ControllerButtonHex.TryGetValue(_selectedCtrlButton, out var hex)) return;

            string? ctx = GetSelectedContextName(_cmbCtrlType);
            string? action = null;
            if (ctx != null)
            {
                if (!string.IsNullOrEmpty(hex.hexRight))
                    action = FindActionForHexInContext(ctx, 6, hex.hexRight);
                if (action == null && !string.IsNullOrEmpty(hex.hexLeft))
                    action = FindActionForHexInContext(ctx, 7, hex.hexLeft);
            }
            SelectActionInCombo(_cmbCtrlAction, action);

            var info = ControllerButtons[_selectedCtrlButton];
            _lblKbStatus.Text = action != null
                ? $"{info.display}: {action}"
                : $"{info.display}: Not Used";
            _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
        }

        private bool _suppressCtrlActionChange = false;

        private void CmbCtrlAction_SelectedIndexChanged(object? sender, EventArgs e)
        {
            if (_suppressCtrlActionChange) return;
            if (_selectedCtrlButton == null) return;
            if (!ControllerButtonHex.TryGetValue(_selectedCtrlButton, out var hex)) return;

            string? ctx = GetSelectedContextName(_cmbCtrlType);
            if (ctx == null || !_contextBindings.TryGetValue(ctx, out var actions)) return;

            string newAction = _cmbCtrlAction.SelectedItem?.ToString() ?? "(none)";
            bool isNone = newAction == "(none)";

            // Determine which fields this button affects
            bool hasRight = !string.IsNullOrEmpty(hex.hexRight);
            bool hasLeft = !string.IsNullOrEmpty(hex.hexLeft);

            // Clear old action that had this button's hex code
            foreach (var fields in actions)
            {
                if (fields.Length <= 7) continue;
                string actionName = fields[0];
                if (hasRight && fields[6].Trim().ToLowerInvariant() == hex.hexRight.ToLowerInvariant())
                {
                    fields[6] = "0xff";
                    RecordControllerChange(ctx, actionName, 6, "0xff");
                }
                if (hasLeft && fields[7].Trim().ToLowerInvariant() == hex.hexLeft.ToLowerInvariant())
                {
                    fields[7] = "0xff";
                    RecordControllerChange(ctx, actionName, 7, "0xff");
                }
            }

            // Assign hex code to the new action
            if (!isNone)
            {
                foreach (var fields in actions)
                {
                    if (fields.Length <= 7) continue;
                    if (fields[0] != newAction) continue;
                    if (hasRight)
                    {
                        fields[6] = hex.hexRight;
                        RecordControllerChange(ctx, newAction, 6, hex.hexRight);
                    }
                    if (hasLeft)
                    {
                        fields[7] = hex.hexLeft;
                        RecordControllerChange(ctx, newAction, 7, hex.hexLeft);
                    }
                    break;
                }
            }

            var btnInfo = ControllerButtons[_selectedCtrlButton];
            _lblKbStatus.Text = isNone
                ? $"{btnInfo.display}: Unbound (unsaved)"
                : $"{btnInfo.display} → {newAction} (unsaved)";
            _lblKbStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private void RecordControllerChange(string context, string action, int fieldIndex, string hexValue)
        {
            if (!_controllerChanges.ContainsKey(context))
                _controllerChanges[context] = new Dictionary<string, Dictionary<int, string>>();
            if (!_controllerChanges[context].ContainsKey(action))
                _controllerChanges[context][action] = new Dictionary<int, string>();
            _controllerChanges[context][action][fieldIndex] = hexValue;
        }

        private static PointF[] GetDirectionTrianglePoints(string dirKey, float cx, float cy)
        {
            float h = 5f, w = 3.5f;
            if (dirKey.EndsWith("_up"))
                return new[] { new PointF(cx, cy - h), new PointF(cx - w, cy + h), new PointF(cx + w, cy + h) };
            if (dirKey.EndsWith("_down"))
                return new[] { new PointF(cx, cy + h), new PointF(cx - w, cy - h), new PointF(cx + w, cy - h) };
            if (dirKey.EndsWith("_left"))
                return new[] { new PointF(cx - h, cy), new PointF(cx + h, cy - w), new PointF(cx + h, cy + w) };
            return new[] { new PointF(cx + h, cy), new PointF(cx - h, cy - w), new PointF(cx - h, cy + w) };
        }

        private void PicBindingsController_Paint(object? sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var (drawW, drawH, offX, offY) = GetBindingsImageBounds();

            foreach (var kvp in ControllerButtons)
            {
                float cx = offX + kvp.Value.pos.X * drawW;
                float cy = offY + kvp.Value.pos.Y * drawH;

                bool isHovered = kvp.Key == _hoveredCtrlButton;
                bool isSelected = kvp.Key == _selectedCtrlButton;
                bool isDir = kvp.Value.isStickDir;

                if (isDir)
                {
                    // Draw directional triangles
                    var triPts = GetDirectionTrianglePoints(kvp.Key, cx, cy);

                    // Ghost triangle (always visible)
                    using (var ghostPen = new Pen(Color.FromArgb(80, 255, 255, 255), 1.2f))
                    using (var ghostBrush = new SolidBrush(Color.FromArgb(40, 255, 255, 255)))
                    {
                        g.FillPolygon(ghostBrush, triPts);
                        g.DrawPolygon(ghostPen, triPts);
                    }

                    if (isSelected)
                    {
                        using var pen = new Pen(Color.FromArgb(230, 255, 180, 40), 2f);
                        using var brush = new SolidBrush(Color.FromArgb(140, 255, 180, 40));
                        g.FillPolygon(brush, triPts);
                        g.DrawPolygon(pen, triPts);
                    }
                    else if (isHovered)
                    {
                        using var pen = new Pen(Color.FromArgb(200, 100, 200, 255), 1.8f);
                        using var brush = new SolidBrush(Color.FromArgb(80, 100, 200, 255));
                        g.FillPolygon(brush, triPts);
                        g.DrawPolygon(pen, triPts);
                    }
                }
                else
                {
                    // Regular buttons: circles
                    float r = 14f;

                    using (var ghostPen = new Pen(Color.FromArgb(50, 255, 255, 255), 1.5f))
                    using (var ghostBrush = new SolidBrush(Color.FromArgb(25, 255, 255, 255)))
                    {
                        g.FillEllipse(ghostBrush, cx - r, cy - r, r * 2, r * 2);
                        g.DrawEllipse(ghostPen, cx - r, cy - r, r * 2, r * 2);
                    }

                    if (isSelected)
                    {
                        using var pen = new Pen(Color.FromArgb(220, 255, 180, 40), 3f);
                        using var brush = new SolidBrush(Color.FromArgb(100, 255, 180, 40));
                        g.FillEllipse(brush, cx - r, cy - r, r * 2, r * 2);
                        g.DrawEllipse(pen, cx - r, cy - r, r * 2, r * 2);
                    }
                    else if (isHovered)
                    {
                        using var pen = new Pen(Color.FromArgb(180, 100, 200, 255), 2.5f);
                        using var brush = new SolidBrush(Color.FromArgb(60, 100, 200, 255));
                        g.FillEllipse(brush, cx - r, cy - r, r * 2, r * 2);
                        g.DrawEllipse(pen, cx - r, cy - r, r * 2, r * 2);
                    }

                    // Draw button label on hover/select (circles only)
                    if (isHovered || isSelected)
                    {
                        using var font = new Font("Segoe UI", 7f, FontStyle.Bold);
                        using var textBrush = new SolidBrush(Color.White);
                        var sf = new StringFormat { Alignment = StringAlignment.Center };
                        g.DrawString(kvp.Value.display, font, textBrush, cx, cy + 14f + 2, sf);
                    }
                }
            }
        }

        private string ResolveScancodeDisplay(string hexStr)
        {
            if (hexStr == "0xff") return "-";
            if (int.TryParse(hexStr.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out int sc))
            {
                foreach (var kvp in KeyScancodes)
                {
                    if (kvp.Value == sc) return kvp.Key;
                }
                return hexStr;
            }
            return hexStr;
        }

        private void CreateKeyboardLayout()
        {
            int startX = 10;
            int startY = 10;
            int keyW = 46;
            int keyH = 40;
            int gap = 3;

            // Row 1: Esc, F1-F12
            int x = startX;
            int y = startY;
            AddKey("Esc", "Esc", x, y, keyW, keyH); x += keyW + gap + 25;
            AddKey("F1", "F1", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F2", "F2", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F3", "F3", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F4", "F4", x, y, keyW, keyH); x += keyW + gap + 12;
            AddKey("F5", "F5", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F6", "F6", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F7", "F7", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F8", "F8", x, y, keyW, keyH); x += keyW + gap + 12;
            AddKey("F9", "F9", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F10", "F10", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F11", "F11", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F12", "F12", x, y, keyW, keyH);

            // Navigation cluster - positioned relative to main keyboard width, not F-row
            // Main keyboard ends around x=742 (Backspace), so start nav at ~790 for clear gap
            int navX = startX + 15 * (keyW + gap) + 55; // About 790px - clear 50px gap from main keyboard
            AddKey("Ins", "Ins", navX, y, keyW, keyH);
            AddKey("Home", "Hm", navX + keyW + gap, y, keyW, keyH);
            AddKey("PgUp", "PU", navX + 2 * (keyW + gap), y, keyW, keyH);

            // Numpad starts after nav cluster with gap
            int numX = navX + 3 * (keyW + gap) + 15;

            y += keyH + gap + 8;

            // Row 2: ` 1-0 - = Backspace
            x = startX;
            AddKey("`", "`", x, y, keyW, keyH); x += keyW + gap;
            AddKey("1", "1", x, y, keyW, keyH); x += keyW + gap;
            AddKey("2", "2", x, y, keyW, keyH); x += keyW + gap;
            AddKey("3", "3", x, y, keyW, keyH); x += keyW + gap;
            AddKey("4", "4", x, y, keyW, keyH); x += keyW + gap;
            AddKey("5", "5", x, y, keyW, keyH); x += keyW + gap;
            AddKey("6", "6", x, y, keyW, keyH); x += keyW + gap;
            AddKey("7", "7", x, y, keyW, keyH); x += keyW + gap;
            AddKey("8", "8", x, y, keyW, keyH); x += keyW + gap;
            AddKey("9", "9", x, y, keyW, keyH); x += keyW + gap;
            AddKey("0", "0", x, y, keyW, keyH); x += keyW + gap;
            AddKey("-", "-", x, y, keyW, keyH); x += keyW + gap;
            AddKey("=", "=", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Backspace", "Back", x, y, keyW * 2 + gap, keyH);

            // Nav row 2
            AddKey("Del", "Del", navX, y, keyW, keyH);
            AddKey("End", "End", navX + keyW + gap, y, keyW, keyH);
            AddKey("PgDn", "PD", navX + 2 * (keyW + gap), y, keyW, keyH);

            y += keyH + gap;

            // Row 3: Tab Q-] \
            x = startX;
            AddKey("Tab", "Tab", x, y, (int)(keyW * 1.4), keyH); x += (int)(keyW * 1.4) + gap;
            AddKey("Q", "Q", x, y, keyW, keyH); x += keyW + gap;
            AddKey("W", "W", x, y, keyW, keyH); x += keyW + gap;
            AddKey("E", "E", x, y, keyW, keyH); x += keyW + gap;
            AddKey("R", "R", x, y, keyW, keyH); x += keyW + gap;
            AddKey("T", "T", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Y", "Y", x, y, keyW, keyH); x += keyW + gap;
            AddKey("U", "U", x, y, keyW, keyH); x += keyW + gap;
            AddKey("I", "I", x, y, keyW, keyH); x += keyW + gap;
            AddKey("O", "O", x, y, keyW, keyH); x += keyW + gap;
            AddKey("P", "P", x, y, keyW, keyH); x += keyW + gap;
            AddKey("[", "[", x, y, keyW, keyH); x += keyW + gap;
            AddKey("]", "]", x, y, keyW, keyH); x += keyW + gap;
            AddKey("\\", "\\", x, y, (int)(keyW * 1.5), keyH);

            // Numpad row 1
            AddKey("Num7", "7", numX, y, keyW, keyH);
            AddKey("Num8", "8", numX + keyW + gap, y, keyW, keyH);
            AddKey("Num9", "9", numX + 2 * (keyW + gap), y, keyW, keyH);

            y += keyH + gap;

            // Row 4: Caps A-' Enter
            x = startX;
            AddKey("Caps", "Caps", x, y, (int)(keyW * 1.7), keyH); x += (int)(keyW * 1.7) + gap;
            AddKey("A", "A", x, y, keyW, keyH); x += keyW + gap;
            AddKey("S", "S", x, y, keyW, keyH); x += keyW + gap;
            AddKey("D", "D", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F", "F", x, y, keyW, keyH); x += keyW + gap;
            AddKey("G", "G", x, y, keyW, keyH); x += keyW + gap;
            AddKey("H", "H", x, y, keyW, keyH); x += keyW + gap;
            AddKey("J", "J", x, y, keyW, keyH); x += keyW + gap;
            AddKey("K", "K", x, y, keyW, keyH); x += keyW + gap;
            AddKey("L", "L", x, y, keyW, keyH); x += keyW + gap;
            AddKey(";", ";", x, y, keyW, keyH); x += keyW + gap;
            AddKey("'", "'", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Enter", "Enter", x, y, (int)(keyW * 2.2), keyH);

            // Numpad row 2
            AddKey("Num4", "4", numX, y, keyW, keyH);
            AddKey("Num5", "5", numX + keyW + gap, y, keyW, keyH);
            AddKey("Num6", "6", numX + 2 * (keyW + gap), y, keyW, keyH);

            y += keyH + gap;

            // Row 5: Shift Z-/ Shift
            x = startX;
            AddKey("LShift", "Shift", x, y, (int)(keyW * 2.1), keyH); x += (int)(keyW * 2.1) + gap;
            AddKey("Z", "Z", x, y, keyW, keyH); x += keyW + gap;
            AddKey("X", "X", x, y, keyW, keyH); x += keyW + gap;
            AddKey("C", "C", x, y, keyW, keyH); x += keyW + gap;
            AddKey("V", "V", x, y, keyW, keyH); x += keyW + gap;
            AddKey("B", "B", x, y, keyW, keyH); x += keyW + gap;
            AddKey("N", "N", x, y, keyW, keyH); x += keyW + gap;
            AddKey("M", "M", x, y, keyW, keyH); x += keyW + gap;
            AddKey(",", ",", x, y, keyW, keyH); x += keyW + gap;
            AddKey(".", ".", x, y, keyW, keyH); x += keyW + gap;
            AddKey("/", "/", x, y, keyW, keyH); x += keyW + gap;
            AddKey("RShift", "Shift", x, y, (int)(keyW * 2.7), keyH);

            // Arrow Up (centered above Down)
            AddKey("Up", "\u25B2", navX + keyW + gap, y, keyW, keyH);

            // Numpad row 3
            AddKey("Num1", "1", numX, y, keyW, keyH);
            AddKey("Num2", "2", numX + keyW + gap, y, keyW, keyH);
            AddKey("Num3", "3", numX + 2 * (keyW + gap), y, keyW, keyH);

            y += keyH + gap;

            // Row 6: Ctrl Alt Space Alt Ctrl
            x = startX;
            AddKey("LCtrl", "Ctrl", x, y, (int)(keyW * 1.4), keyH); x += (int)(keyW * 1.4) + gap;
            x += keyW + gap; // Skip Win
            AddKey("LAlt", "Alt", x, y, (int)(keyW * 1.4), keyH); x += (int)(keyW * 1.4) + gap;
            AddKey("Space", "Space", x, y, keyW * 6 + gap * 5, keyH); x += keyW * 6 + gap * 5 + gap;
            AddKey("RAlt", "Alt", x, y, (int)(keyW * 1.4), keyH); x += (int)(keyW * 1.4) + gap;
            x += keyW + gap; // Skip Win
            AddKey("RCtrl", "Ctrl", x, y, (int)(keyW * 1.4), keyH);

            // Arrows (Left, Down, Right)
            AddKey("Left", "\u25C0", navX, y, keyW, keyH);
            AddKey("Down", "\u25BC", navX + keyW + gap, y, keyW, keyH);
            AddKey("Right", "\u25B6", navX + 2 * (keyW + gap), y, keyW, keyH);

            // Numpad row 4 (wide 0, decimal)
            AddKey("Num0", "0", numX, y, keyW * 2 + gap, keyH);
            AddKey("Num.", ".", numX + keyW * 2 + gap + gap, y, keyW, keyH);
        }

        private void AddKey(string id, string label, int x, int y, int w, int h)
        {
            var btn = new Button
            {
                Text = label,
                Location = new Point(x, y),
                Size = new Size(w, h),
                FlatStyle = FlatStyle.Flat,
                BackColor = UnboundKeyColor,
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 8.5f, FontStyle.Bold),
                Cursor = Cursors.Hand,
                Tag = id
            };
            btn.FlatAppearance.BorderColor = Color.FromArgb(70, 70, 80);
            btn.FlatAppearance.BorderSize = 1;
            btn.Click += Key_Click;

            _keyboardPanel.Controls.Add(btn);
            _keyButtons[id] = btn;
        }

        private void Key_Click(object? sender, EventArgs e)
        {
            if (sender is not Button btn) return;
            string keyId = (string)btn.Tag;

            // Deselect previous
            if (_selectedKeyButton != null)
            {
                UpdateKeyColor(_selectedKeyButton);
            }

            // Select new
            _selectedKeyId = keyId;
            _selectedKeyButton = btn;
            btn.BackColor = SelectedKeyColor;
            btn.FlatAppearance.BorderColor = Color.FromArgb(255, 200, 40);

            _lblCurrentBinding.Text = keyId;
            _cmbAction.Enabled = true;

            // Find current action for this key
            int scancode = KeyScancodes.GetValueOrDefault(keyId, 0xFF);
            var currentAction = _keyBindings.FirstOrDefault(kvp => kvp.Value == scancode);
            if (currentAction.Key != null)
            {
                int idx = Array.FindIndex(GameActions, a => a.id == currentAction.Key);
                _cmbAction.SelectedIndex = idx >= 0 ? idx + 1 : 0;
            }
            else
            {
                _cmbAction.SelectedIndex = 0;
            }
        }

        private void UpdateKeyColor(Button btn)
        {
            string keyId = (string)btn.Tag;
            int scancode = KeyScancodes.GetValueOrDefault(keyId, 0xFF);
            bool hasBind = _keyBindings.Any(kvp => kvp.Value == scancode);

            btn.BackColor = hasBind ? BoundKeyColor : UnboundKeyColor;
            btn.FlatAppearance.BorderColor = hasBind ? Color.FromArgb(100, 160, 220) : Color.FromArgb(70, 70, 80);
        }

        private void UpdateAllKeyColors()
        {
            foreach (var btn in _keyButtons.Values)
            {
                if (btn != _selectedKeyButton)
                    UpdateKeyColor(btn);
            }
        }

        private void CmbAction_SelectedIndexChanged(object? sender, EventArgs e)
        {
            if (_selectedKeyId == null) return;

            int scancode = KeyScancodes.GetValueOrDefault(_selectedKeyId, 0xFF);
            if (scancode == 0xFF) return;

            // Unbind any action currently using this scancode
            var existingKey = _keyBindings.FirstOrDefault(kvp => kvp.Value == scancode).Key;
            if (existingKey != null)
                _keyBindings[existingKey] = 0xFF;

            // Add new binding
            if (_cmbAction.SelectedIndex > 0)
            {
                var action = GameActions[_cmbAction.SelectedIndex - 1];

                // Clear old binding for this action (will be replaced below)
                if (_keyBindings.ContainsKey(action.id))
                    _keyBindings[action.id] = 0xFF;

                _keyBindings[action.id] = scancode;
            }

            UpdateAllKeyColors();

            _lblKbStatus.Text = "Binding updated (unsaved)";
            _lblKbStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private void LoadDefaultKeyBindings()
        {
            _keyBindings.Clear();
            foreach (var action in GameActions)
            {
                _keyBindings[action.id] = action.defaultScancode;
            }
            UpdateAllKeyColors();
        }

        private void TryLoadControlmapVR()
        {
            if (string.IsNullOrEmpty(_gameDir)) return;

            // Check MO2 mod folder first (top level, not root\), then game Data folder
            string filePath = "";
            if (!string.IsNullOrEmpty(_mo2ModDir))
            {
                string modRoot = Directory.GetParent(_mo2ModDir)!.FullName;
                string mo2Path = Path.Combine(modRoot, "interface", "controls", "pc", "controlmapvr.txt");
                if (File.Exists(mo2Path)) filePath = mo2Path;
            }
            if (string.IsNullOrEmpty(filePath))
            {
                string gamePath = Path.Combine(_gameDir, "Data", "Interface", "Controls", "PC", "controlmapvr.txt");
                if (File.Exists(gamePath)) filePath = gamePath;
            }
            if (string.IsNullOrEmpty(filePath)) return;

            try
            {
                // Build reverse lookup: scancode -> key name
                var scancodeToKey = KeyScancodes.ToDictionary(kvp => kvp.Value, kvp => kvp.Key);

                string currentContext = "";
                foreach (string rawLine in File.ReadAllLines(filePath))
                {
                    string line = rawLine.TrimEnd('\r');
                    if (string.IsNullOrWhiteSpace(line)) { currentContext = ""; continue; }

                    if (line.TrimStart().StartsWith("//"))
                    {
                        string comment = line.TrimStart().TrimStart('/').Trim();
                        int tabIdx = comment.IndexOf('\t');
                        if (tabIdx >= 0) comment = comment[..tabIdx].Trim();
                        if (comment.Length > 0 && !comment.StartsWith("1st") && !comment.StartsWith("2nd") &&
                            !comment.StartsWith("3rd") && !comment.StartsWith("4th") && !comment.StartsWith("5th") &&
                            !comment.StartsWith("6th") && !comment.StartsWith("7th") && !comment.StartsWith("8th") &&
                            !comment.StartsWith("9th") && !comment.StartsWith("10th") && !comment.StartsWith("11th") &&
                            !comment.StartsWith("12th") && !comment.StartsWith("13th") && !comment.StartsWith("14th") &&
                            !comment.StartsWith("15th") && !comment.StartsWith("16th") && !comment.StartsWith("17th") &&
                            !comment.StartsWith("18th") && !comment.StartsWith("19th") && !comment.StartsWith("20th") &&
                            !comment.StartsWith("Blank") && !comment.StartsWith("See") &&
                            !comment.StartsWith("(Vive") && !comment.StartsWith("(Oculus") && !comment.StartsWith("(Windows") &&
                            !comment.StartsWith("\"") && !comment.StartsWith("If "))
                            currentContext = comment;
                        continue;
                    }

                    // Parse tab-separated: ActionName  scancode  mouse  gamepad  ...
                    var parts = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length < 2) continue;
                    for (int p = 0; p < parts.Length; p++) parts[p] = parts[p].Trim();

                    string actionName = parts[0];
                    string scStr = parts[1].ToLowerInvariant();

                    // Handle comma-separated multi-key bindings (take first scancode)
                    if (scStr.Contains(','))
                        scStr = scStr.Split(',')[0].Trim();

                    // Parse keyboard scancode (hex) — Main Gameplay context only.
                    // Actions like "Console" appear in multiple contexts with different scancodes;
                    // loading from all contexts would let Menu Mode (0x35) overwrite Main Gameplay (0x29).
                    if (currentContext == "Main Gameplay" &&
                        scStr.StartsWith("0x") && int.TryParse(scStr[2..], System.Globalization.NumberStyles.HexNumber, null, out int scancode))
                    {
                        var action = GameActions.FirstOrDefault(a => a.id == actionName);
                        if (action.id != null)
                        {
                            _keyBindings[actionName] = scancode;
                        }
                    }

                    // Update controller bindings in _contextBindings from saved file
                    if (!string.IsNullOrEmpty(currentContext) && _contextBindings.TryGetValue(currentContext, out var ctxActions))
                    {
                        foreach (var fields in ctxActions)
                        {
                            if (fields[0] != actionName || fields.Length < 8) continue;
                            // Update Oculus Right (field 6) and Oculus Left (field 7) from saved file
                            if (parts.Length > 6) fields[6] = parts[6];
                            if (parts.Length > 7) fields[7] = parts[7];
                            break;
                        }
                    }
                }

                UpdateAllKeyColors();
                _lblKbStatus.Text = "Loaded existing controlmapvr.txt bindings";
                _lblKbStatus.ForeColor = Color.FromArgb(100, 180, 255);
            }
            catch
            {
                // Silently ignore load errors - just use defaults
            }
        }

        private void BtnResetKeyDefaults_Click(object? sender, EventArgs e)
        {
            LoadDefaultKeyBindings();
            _lblKbStatus.Text = "Reset to game defaults (unsaved)";
            _lblKbStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private void BtnVRDefaults_Click(object? sender, EventArgs e)
        {
            var result = MessageBox.Show(
                "This will restore all bindings to the VR Safe Defaults (keyboard conflicts removed, mouse unbound).\n\nAre you sure?",
                "Restore VR Safe Defaults",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question);

            if (result != DialogResult.Yes) return;

            string filePath = GetControlmapSavePath();

            // Write from embedded VR safe template
            var asm = Assembly.GetExecutingAssembly();
            using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_vrsafe.txt");
            if (stream == null)
            {
                MessageBox.Show("Embedded VR safe defaults not found!", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            using var reader = new StreamReader(stream);
            File.WriteAllText(filePath, reader.ReadToEnd());

            // Reload bindings from the fresh file
            _keyBindings.Clear();
            LoadDefaultKeyBindings();
            TryLoadControlmapVR();
            _chkDisableMouse.Checked = true;

            _lblKbStatus.Text = "VR Safe Defaults restored! Restart the game to apply.";
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        private void BtnSaveBindings_Click(object? sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                MessageBox.Show("Please select a game folder first (on Settings tab).", "No Folder Selected",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try
            {
                SaveControlmapVR();
                _lblKbStatus.Text = "Saved controlmapvr.txt! Restart the game to apply changes.";
                _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private string GetControlmapSavePath()
        {
            string controlsPath;
            if (!string.IsNullOrEmpty(_mo2ModDir))
            {
                string modRoot = Directory.GetParent(_mo2ModDir)!.FullName;
                controlsPath = Path.Combine(modRoot, "interface", "controls", "pc");
            }
            else
                controlsPath = Path.Combine(_gameDir, "Data", "Interface", "Controls", "PC");
            Directory.CreateDirectory(controlsPath);
            return Path.Combine(controlsPath, "controlmapvr.txt");
        }

        /// <summary>
        /// Finds the start and end character positions of field N (0-indexed) in a tab-separated line.
        /// Fields are separated by one or more tabs.
        /// </summary>
        private static (int start, int end) FindFieldBounds(string line, int fieldIndex)
        {
            int pos = 0;
            int currentField = 0;

            while (currentField < fieldIndex && pos < line.Length)
            {
                // Skip current field content
                while (pos < line.Length && line[pos] != '\t') pos++;
                // Skip tabs between fields
                while (pos < line.Length && line[pos] == '\t') pos++;
                currentField++;
            }

            int start = pos;
            int end = pos;
            while (end < line.Length && line[end] != '\t') end++;
            return (start, end);
        }

        private void SaveControlmapVR()
        {
            string filePath = GetControlmapSavePath();

            // Read existing file if it exists, otherwise write fresh from template
            string[] sourceLines;
            if (File.Exists(filePath))
            {
                sourceLines = File.ReadAllLines(filePath);
            }
            else
            {
                // First time — no file exists yet, use template as base
                var asm = Assembly.GetExecutingAssembly();
                using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt");
                if (stream == null)
                    throw new Exception("Embedded controlmapvr_template.txt not found in assembly!");
                using var reader = new StreamReader(stream);
                sourceLines = reader.ReadToEnd().Split('\n').Select(l => l.TrimEnd('\r')).ToArray();
            }

            // Surgical patch: only modify specific fields on lines that need changes
            string currentContext = "";

            for (int i = 0; i < sourceLines.Length; i++)
            {
                string line = sourceLines[i];

                // Track context
                if (line.TrimStart().StartsWith("//"))
                {
                    string comment = line.TrimStart().TrimStart('/').Trim();
                    int tabIdx = comment.IndexOf('\t');
                    if (tabIdx >= 0) comment = comment[..tabIdx].Trim();
                    if (comment.Length > 0 && !comment.StartsWith("1st") && !comment.StartsWith("2nd") &&
                             !comment.StartsWith("3rd") && !comment.StartsWith("4th") && !comment.StartsWith("5th") &&
                             !comment.StartsWith("6th") && !comment.StartsWith("7th") && !comment.StartsWith("8th") &&
                             !comment.StartsWith("9th") && !comment.StartsWith("10th") && !comment.StartsWith("11th") &&
                             !comment.StartsWith("12th") && !comment.StartsWith("13th") && !comment.StartsWith("14th") &&
                             !comment.StartsWith("15th") && !comment.StartsWith("16th") && !comment.StartsWith("17th") &&
                             !comment.StartsWith("18th") && !comment.StartsWith("19th") && !comment.StartsWith("20th") &&
                             !comment.StartsWith("Blank") && !comment.StartsWith("See") &&
                             !comment.StartsWith("(Vive") && !comment.StartsWith("(Oculus") && !comment.StartsWith("(Windows") &&
                             !comment.StartsWith("\"") && !comment.StartsWith("If "))
                        currentContext = comment;
                    continue;
                }

                if (string.IsNullOrWhiteSpace(line))
                {
                    currentContext = "";
                    continue;
                }

                // Only patch action lines that actually need changes
                int firstTab = line.IndexOf('\t');
                if (firstTab <= 0) continue;

                string actionName = line[..firstTab].Trim();
                bool isMainGameplay = currentContext == "Main Gameplay";
                bool needsKbChange = isMainGameplay && _keyBindings.ContainsKey(actionName);
                bool needsMouseChange = _chkDisableMouse.Checked;
                bool needsCtrlChange = _controllerChanges.TryGetValue(currentContext, out var ctxChanges)
                                       && ctxChanges.ContainsKey(actionName);

                if (!needsKbChange && !needsMouseChange && !needsCtrlChange) continue;

                // Find keyboard field bounds (field 1)
                int kbStart = firstTab;
                while (kbStart < line.Length && line[kbStart] == '\t') kbStart++;
                int kbEnd = line.IndexOf('\t', kbStart);
                if (kbEnd < 0) kbEnd = line.Length;

                // Find mouse field bounds (field 2)
                int mouseStart = kbEnd;
                while (mouseStart < line.Length && line[mouseStart] == '\t') mouseStart++;
                int mouseEnd = line.IndexOf('\t', mouseStart);
                if (mouseEnd < 0) mouseEnd = line.Length;

                // Patch keyboard scancode (Main Gameplay only, and only if actually different)
                if (needsKbChange && _keyBindings.TryGetValue(actionName, out int scancode))
                {
                    string existingKb = line[kbStart..kbEnd].Trim().ToLowerInvariant();
                    if (existingKb.Contains(','))
                        existingKb = existingKb.Split(',')[0].Trim();
                    int existingScancode = 0xFF;
                    if (existingKb.StartsWith("0x") && int.TryParse(existingKb[2..], System.Globalization.NumberStyles.HexNumber, null, out int parsed))
                        existingScancode = parsed;

                    if (scancode != existingScancode)
                    {
                        string newScancode = scancode != 0xFF ? $"0x{scancode:X2}" : "0xff";
                        int lenDiff = newScancode.Length - (kbEnd - kbStart);
                        line = line[..kbStart] + newScancode + line[kbEnd..];
                        mouseStart += lenDiff;
                        mouseEnd += lenDiff;
                    }
                }

                // Patch mouse field (all contexts)
                if (needsMouseChange && mouseStart < line.Length)
                {
                    line = line[..mouseStart] + "0xff" + line[mouseEnd..];
                }

                // Patch controller fields (Oculus Right = field 6, Oculus Left = field 7)
                if (needsCtrlChange && ctxChanges!.TryGetValue(actionName, out var fieldChanges))
                {
                    // Patch from right to left so earlier patches don't shift later positions
                    var sortedFields = fieldChanges.Keys.OrderByDescending(k => k).ToList();
                    foreach (int fieldIdx in sortedFields)
                    {
                        var (fStart, fEnd) = FindFieldBounds(line, fieldIdx);
                        if (fStart < line.Length)
                        {
                            line = line[..fStart] + fieldChanges[fieldIdx] + line[fEnd..];
                        }
                    }
                }

                sourceLines[i] = line;
            }

            File.WriteAllLines(filePath, sourceLines);
            _controllerChanges.Clear();
        }

        private void ResetControlmapToDefaults()
        {
            var result = MessageBox.Show(
                "This will restore ALL bindings (keyboard, mouse, and VR controllers) to original game defaults.\n\nAre you sure?",
                "Reset to Game Defaults",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);

            if (result != DialogResult.Yes) return;

            string filePath = GetControlmapSavePath();

            // Write fresh from embedded template
            var asm = Assembly.GetExecutingAssembly();
            using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt");
            if (stream == null) return;
            using var reader = new StreamReader(stream);
            File.WriteAllText(filePath, reader.ReadToEnd());

            // Reload bindings from the fresh file
            _keyBindings.Clear();
            LoadDefaultKeyBindings();
            TryLoadControlmapVR();

            _lblKbStatus.Text = "All bindings reset to original game defaults";
            _lblKbStatus.ForeColor = Color.FromArgb(255, 100, 100);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // CONTROLLER IMAGE HANDLING
        // ═══════════════════════════════════════════════════════════════════════

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
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            float hitRadius = 0.06f;
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

            if ((ModifierKeys & Keys.Shift) != 0)
            {
                if (_calibrationStep < CalibrationOrder.Length)
                {
                    string name = CalibrationOrder[_calibrationStep];
                    _calibrationLog.Add($"{name}: ({fx:F3}, {fy:F3})");
                    _calibrationStep++;

                    if (_calibrationStep < CalibrationOrder.Length)
                        _lblStatus.Text = $"Logged {name}. Now Shift+Click: {CalibrationOrder[_calibrationStep]}";
                    else
                    {
                        _lblStatus.Text = "All done! Coordinates copied to clipboard.";
                        Clipboard.SetText(string.Join("\n", _calibrationLog));
                    }
                }
                else
                {
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
                chk.Checked = !chk.Checked;
            }
        }

        private void PicControllers_Paint(object? sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;

            var (drawW, drawH, offX, offY) = GetImageBounds();

            var highlightColor = Color.FromArgb(200, 255, 200, 40);
            using var pen = new Pen(highlightColor, 2.5f);
            using var brush = new SolidBrush(Color.FromArgb(70, 255, 200, 40));

            var selected = GetSelectedButtons();
            float r = 10;

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

        // ═══════════════════════════════════════════════════════════════════════
        // CONTROLLER COMBOS
        // ═══════════════════════════════════════════════════════════════════════

        private void BtnAddCombo_Click(object? sender, EventArgs e)
        {
            if (_combos.Count >= 16)
            {
                MessageBox.Show("Maximum of 16 combos reached.", "Limit Reached",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            using var dlg = new ComboEditForm(KeyScancodes);
            if (dlg.ShowDialog(this) == DialogResult.OK && dlg.Result != null)
            {
                _combos.Add(dlg.Result);
                RebuildComboList();
                _lblComboStatus.Text = "Combo added \u2014 click Save to apply";
                _lblComboStatus.ForeColor = Color.FromArgb(255, 200, 40);
            }
        }

        private void EditCombo(int index)
        {
            if (index < 0 || index >= _combos.Count) return;

            using var dlg = new ComboEditForm(KeyScancodes, _combos[index]);
            if (dlg.ShowDialog(this) == DialogResult.OK && dlg.Result != null)
            {
                _combos[index] = dlg.Result;
                RebuildComboList();
                _lblComboStatus.Text = "Combo updated \u2014 click Save to apply";
                _lblComboStatus.ForeColor = Color.FromArgb(255, 200, 40);
            }
        }

        private void DeleteCombo(int index)
        {
            if (index < 0 || index >= _combos.Count) return;

            _combos.RemoveAt(index);
            RebuildComboList();
            _lblComboStatus.Text = "Combo removed \u2014 click Save to apply";
            _lblComboStatus.ForeColor = Color.FromArgb(255, 200, 40);
        }

        private void RebuildComboList()
        {
            _comboListPanel.Controls.Clear();
            int y = 4;

            if (_combos.Count == 0)
            {
                var lblEmpty = new Label
                {
                    Text = "No combos configured. Click \"+ Add Combo\" to create one.",
                    Location = new Point(10, y),
                    AutoSize = true,
                    ForeColor = Color.FromArgb(100, 100, 100),
                    Font = new Font("Segoe UI", 9f, FontStyle.Italic)
                };
                _comboListPanel.Controls.Add(lblEmpty);
                return;
            }

            for (int i = 0; i < _combos.Count; i++)
            {
                int idx = i; // capture for lambdas
                var combo = _combos[i];

                int pw = _comboListPanel.ClientSize.Width;

                var lblCombo = new Label
                {
                    Text = $"{i + 1}. {combo.GetDisplaySummary(KeyScancodes)}",
                    Location = new Point(6, y + 2),
                    Size = new Size(pw - 130, 20),
                    ForeColor = Color.White,
                    Font = new Font("Segoe UI", 8.5f)
                };
                _comboListPanel.Controls.Add(lblCombo);

                var btnEdit = new Button
                {
                    Text = "Edit",
                    Location = new Point(pw - 120, y),
                    Size = new Size(52, 22),
                    FlatStyle = FlatStyle.Flat,
                    BackColor = Color.FromArgb(50, 80, 120),
                    ForeColor = Color.White,
                    Font = new Font("Segoe UI", 7.5f)
                };
                btnEdit.FlatAppearance.BorderSize = 0;
                btnEdit.Click += (s, e) => EditCombo(idx);
                _comboListPanel.Controls.Add(btnEdit);

                var btnDelete = new Button
                {
                    Text = "\u2715",
                    Location = new Point(pw - 62, y),
                    Size = new Size(40, 22),
                    FlatStyle = FlatStyle.Flat,
                    BackColor = Color.FromArgb(100, 45, 45),
                    ForeColor = Color.White,
                    Font = new Font("Segoe UI", 8f)
                };
                btnDelete.FlatAppearance.BorderSize = 0;
                btnDelete.Click += (s, e) => DeleteCombo(idx);
                _comboListPanel.Controls.Add(btnDelete);

                y += 28;
            }
        }

        private void ReadCombosFromIni()
        {
            _combos.Clear();
            var entries = _ini.GetAllInSection("combos");
            foreach (var (key, value) in entries)
            {
                var combo = ComboEntry.FromIniValue(value);
                if (combo != null && _combos.Count < 16)
                    _combos.Add(combo);
            }
            RebuildComboList();
        }

        private void WriteCombosToIni()
        {
            _ini.ClearSection("combos");
            for (int i = 0; i < _combos.Count; i++)
            {
                _ini.Set("combos", $"combo{i + 1}", _combos[i].ToIniValue());
            }
        }

        private void SwitchTab(int index)
        {
            _tabSettings.Visible = (index == 0);
            _tabKeyboard.Visible = (index == 1);

            // Update button styles
            _btnTabSettings.Font = new Font("Segoe UI", 10f, index == 0 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabSettings.ForeColor = index == 0 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabSettings.BackColor = index == 0 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabKeyboard.Font = new Font("Segoe UI", 10f, index == 1 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabKeyboard.ForeColor = index == 1 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabKeyboard.BackColor = index == 1 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // FILE OPERATIONS
        // ═══════════════════════════════════════════════════════════════════════

        private void BtnBrowse_Click(object? sender, EventArgs e)
        {
            using var dlg = new FolderBrowserDialog
            {
                Description = "Select your game installation folder (e.g., C:\\Games\\SteamLibrary\\steamapps\\common\\SkyrimVR)",
                ShowNewFolderButton = false
            };

            if (dlg.ShowDialog() == DialogResult.OK)
            {
                _gameDir = dlg.SelectedPath;
                _txtPath.Text = _gameDir;
                SaveConfiguratorSettings();
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

            bool savedToMO2 = false;
            if (!string.IsNullOrEmpty(_mo2ModDir))
            {
                try
                {
                    string mo2Path = Path.Combine(_mo2ModDir, "opencomposite.ini");
                    _ini.Save(mo2Path);
                    savedToMO2 = true;
                }
                catch { }
            }

            string locationMsg = savedToMO2
                ? "Saved to game folder and MO2 mod folder"
                : "Saved to game folder";
            _lblStatus.Text = $"{locationMsg} \u2014 changes apply in-game within 1 second";
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

            // Also load key bindings from controlmapvr.txt if it exists
            TryLoadControlmapVR();

        }

        private void ReadFromIni()
        {
            _chkShortcutEnabled.Checked = ParseBool(_ini.Get("keyboard", "shortcutEnabled", "true"));

            string btnRaw = _ini.Get("keyboard", "shortcutButton", "left_stick").ToLowerInvariant().Trim();
            if (btnRaw == "both_grips") btnRaw = "left_grip+right_grip";

            foreach (var kvp in _btnCheckboxMap) kvp.Value().Checked = false;

            foreach (string part in btnRaw.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
            {
                if (_btnCheckboxMap.TryGetValue(part, out var getChk))
                    getChk().Checked = true;
            }

            string mode = _ini.Get("keyboard", "shortcutMode", "double_tap").ToLowerInvariant();
            int tapCount = mode switch
            {
                "long_press" => 1, "double_tap" => 2, "triple_tap" => 3, "quadruple_tap" => 4, _ => 2
            };
            _rdoX1.Checked = tapCount == 1;
            _rdoX2.Checked = tapCount == 2;
            _rdoX3.Checked = tapCount == 3;
            _rdoX4.Checked = tapCount == 4;

            if (int.TryParse(_ini.Get("keyboard", "shortcutTiming", "500"), out int timing))
                _nudTiming.Value = Math.Clamp(timing, 100, 3000);

            if (float.TryParse(_ini.Get("keyboard", "displayTilt", "22.5"), out float dt))
                _nudDisplayTilt.Value = (decimal)Math.Clamp(dt, -30f, 80f);
            if (int.TryParse(_ini.Get("keyboard", "displayOpacity", "30"), out int dop))
                _nudDisplayOpacity.Value = Math.Clamp(dop, 1, 100);
            if (int.TryParse(_ini.Get("keyboard", "displayScale", "100"), out int dsc))
                _nudDisplayScale.Value = Math.Clamp(dsc, 50, 150);

            _chkSoundsEnabled.Checked = ParseBool(_ini.Get("keyboard", "soundsEnabled", "true"));
            if (int.TryParse(_ini.Get("keyboard", "soundVolume", "50"), out int svol))
                _nudSoundVolume.Value = Math.Clamp(svol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "pressVolume", "50"), out int pvol))
                _nudPressVolume.Value = Math.Clamp(pvol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "hapticStrength", "50"), out int kbhap))
                _nudKbHapticStrength.Value = Math.Clamp(kbhap, 0, 100);

            if (float.TryParse(_ini.Get("", "supersampleRatio", "1.0"), out float ss))
                _nudSuperSample.Value = (decimal)Math.Clamp(ss, 0.5f, 3.0f);
            _chkRenderHands.Checked = ParseBool(_ini.Get("", "renderCustomHands", "true"));
            _chkHaptics.Checked = ParseBool(_ini.Get("", "haptics", "true"));
            if (float.TryParse(_ini.Get("", "hapticStrength", "0.1"), out float hs))
                _nudHapticStrength.Value = (decimal)Math.Clamp(hs, 0f, 1f);
            _chkHiddenMesh.Checked = ParseBool(_ini.Get("", "enableHiddenMeshFix", "true"));
            _chkInvertShaders.Checked = ParseBool(_ini.Get("", "invertUsingShaders", "false"));
            _chkDx10.Checked = ParseBool(_ini.Get("", "dx10Mode", "false"));
            _chkAudioSwitch.Checked = ParseBool(_ini.Get("", "enableAudioSwitch", "false"));
            _txtAudioDevice.Text = _ini.Get("", "audioDeviceName", "quest");
            if (string.IsNullOrEmpty(_txtAudioDevice.Text)) _txtAudioDevice.Text = "quest";

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

            UpdateTimingLabel();
            _picControllers.Invalidate();

            ReadCombosFromIni();
        }

        private void WriteToIni()
        {
            _ini.Set("keyboard", "shortcutEnabled", _chkShortcutEnabled.Checked ? "true" : "false");

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
            _ini.Set("keyboard", "soundVolume", ((int)_nudSoundVolume.Value).ToString());
            _ini.Set("keyboard", "pressVolume", ((int)_nudPressVolume.Value).ToString());
            _ini.Set("keyboard", "hapticStrength", ((int)_nudKbHapticStrength.Value).ToString());

            _ini.Set("", "supersampleRatio", _nudSuperSample.Value.ToString("0.0"));
            _ini.Set("", "renderCustomHands", _chkRenderHands.Checked ? "true" : "false");
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
            }

            WriteCombosToIni();
        }

        private void LoadConfiguratorSettings()
        {
            string exePath = Application.ExecutablePath;
            string exeDir = Path.GetDirectoryName(exePath) ?? "";

            string rootSubDir = Path.Combine(exeDir, "root");
            if (exeDir.Contains("\\mods\\", StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(rootSubDir))
            {
                _mo2ModDir = rootSubDir;
            }

            string settingsPath = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "OpenCompositeConfigurator", "settings.ini");

            if (!File.Exists(settingsPath)) return;

            try
            {
                var settings = new IniFile();
                settings.Load(settingsPath);
                string key = _gameType == "skyrim" ? "skyrimGameDir" : "fallout4GameDir";
                _gameDir = settings.Get("paths", key, "");
            }
            catch { }
        }

        private void SaveConfiguratorSettings()
        {
            string settingsDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "OpenCompositeConfigurator");

            Directory.CreateDirectory(settingsDir);
            string settingsPath = Path.Combine(settingsDir, "settings.ini");

            try
            {
                var settings = new IniFile();
                string key = _gameType == "skyrim" ? "skyrimGameDir" : "fallout4GameDir";
                settings.Set("paths", key, _gameDir);
                settings.Save(settingsPath);
            }
            catch { }
        }

        private void ApplyCurrentGamePaths()
        {
            _txtPath.Text = string.IsNullOrEmpty(_gameDir)
                ? "(No folder selected \u2014 click Browse)"
                : _gameDir;

            if (!string.IsNullOrEmpty(_gameDir))
                LoadFromDir();
        }

        private void SetDefaults()
        {
            _chkLeftStick.Checked = true;
            _pnlSkyrimOnly.Visible = _gameType == "skyrim";
            UpdateTimingLabel();
            UpdateFormTitle();
        }

        private void UpdateFormTitle()
        {
            Text = $"OC Unleashed {_gameName} Configurator";
        }

        // ═══════════════════════════════════════════════════════════════════════
        // UI HELPERS
        // ═══════════════════════════════════════════════════════════════════════

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
