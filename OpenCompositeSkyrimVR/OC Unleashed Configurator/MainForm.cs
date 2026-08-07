using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    public partial class MainForm : Form
    {
        // INI data
        private readonly IniFile _ini = new();
        private bool _isLoading = false; // suppresses audio during ini load
        private string _gameDir = "";
        private string _mo2ModDir = ""; // Auto-detected if running from mod folder
        private bool _installWarningShown = false;
        private readonly string _gameType; // Set via constructor: "skyrim" or "fallout4"
        private readonly string _gameName; // Display name

        // Top bar
        private Label _lblInstallNotice = null!;

        // Tab system (borderless panels with toggle buttons)
        private Button _btnTabSettings = null!;
        private Button _btnTabKeyboard = null!;
        private Button _btnTabVideo = null!;
        private Button _btnTabHaptics = null!;
        private Panel _tabSettings = null!;
        private Panel _tabKeyboard = null!;
        private Panel _tabVideo = null!;
        private Panel _tabHaptics = null!;

        // Video tab controls
        private CheckBox _chkFsrEnabled = null!;
        private CheckBox _chkFsrNativeAA = null!;
        private CheckBox _chkMotionVectorsEnabled = null!;
        private CheckBox _chkActorMV = null!;
        private CheckBox _chkAswEnabled = null!;
        private CheckBox _chkAswExperimentalMode = null!;
        private CheckBox _chkAswBufferEnabled = null!;
        private CheckBox _chkAswUpscalerReset = null!;
        private CheckBox _chkAswUpscalerReactiveMask = null!;
        private NumericUpDown _nudAswWarpStrength = null!;
        private NumericUpDown _nudAswRotationScale = null!;
        private NumericUpDown _nudAswTranslationScale = null!;
        private NumericUpDown _nudAswLocoScale = null!;
        private NumericUpDown _nudAswDepthScale = null!;
        private CheckBox _chkAswAutoNative = null!;
        private NumericUpDown _nudAswAutoEngageFps = null!;
        private CheckBox _chkAswDebugMode = null!;
        // Advanced ASW + trigger settings (no UI controls — preserved through saves)
        private float _aswNearFadeDepth = 0.0f;
        private float _aswFPControllerScale = 0.45f;
        private float _aswEdgeFadeWidth = 3.0f;
        private float _aswMVConfidence = 0.0f;
        private float _aswMVPixelScale = 1.0f;
        private float _triggerDeadzone = 0.0f;
        private float _triggerMax = 1.0f;
        private NumericUpDown _nudFsr3Sharpness = null!;
        private NumericUpDown _nudMotionVectorScale = null!;
        private NumericUpDown _nudFsr3JitterScale = null!;
        private CheckBox _chkFsr3JitterCancellation = null!;
        private CheckBox _chkFsr3CameraMV = null!;
        private NumericUpDown _nudFsr3ReactivenessScale = null!;
        private NumericUpDown _nudFsr3ShadingChangeScale = null!;
        private NumericUpDown _nudFsr3AccumulationPerFrame = null!;
        private ComboBox _cmbFsr3DebugMode = null!;
        private NumericUpDown _nudFsr3ViewToMeters = null!;
        private CheckBox _chkCasEnabled = null!;
        private CheckBox _chkBlueSkyDefenderEnabled = null!;
        private NumericUpDown _nudBlueSkyLambda = null!;
        private NumericUpDown _nudBlueSkyEpsilon = null!;
        private NumericUpDown _nudFsrRenderScale = null!;
        private NumericUpDown _nudCasSharpness = null!;
        private Label _lblFsrRenderScale = null!;
        private Label _lblCasSharpness = null!;
        private CheckBox _chkDlssEnabled = null!;
        private ComboBox _cmbDlssPreset = null!;
        private ComboBox _cmbDlssModel = null!;
        private NumericUpDown _nudDlssRenderScaleOverride = null!;
        private NumericUpDown _nudDlssMvScale = null!;
        private NumericUpDown _nudDlssJitterScale = null!;
        private CheckBox _chkDlssNgxVerboseLogging = null!;

        // MIP bias controls
        private CheckBox _chkMipBiasEnabled = null!;
        private ComboBox _cmbMipBiasMode = null!;
        private NumericUpDown _nudMipBiasFixed = null!;
        private NumericUpDown _nudMipBiasOffset = null!;
        private NumericUpDown _nudDlssMipBiasOffset = null!;
        private NumericUpDown _nudFsr3MipBiasOffset = null!;

        // VRS controls
        private CheckBox _chkVrsEnabled = null!;
        private NumericUpDown _nudVrsInnerRadius = null!;
        private NumericUpDown _nudVrsMidRadius = null!;
        private NumericUpDown _nudVrsOuterRadius = null!;
        private CheckBox _chkVrsFavorHorizontal = null!;
        private Label _lblVrsInnerRadius = null!;
        private Label _lblVrsMidRadius = null!;
        private Label _lblVrsOuterRadius = null!;
        private ComboBox _cboVrsPreset = null!;
        private Label _lblFsrStatus = null!;
        private Label _lblVideoStatus = null!;

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

        // Developer/tuning surfaces (haptics page, FBT capture recorder, live
        // triple-A foot trim) are hidden from users but kept in the build.
        // Create an empty file named "devtools.on" next to the exe to restore
        // them.
        internal static readonly bool ShowDevTools =
            System.IO.File.Exists(System.IO.Path.Combine(AppContext.BaseDirectory, "devtools.on"));
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
        private NumericUpDown _nudPosSmoothMinCutoff = null!;
        private NumericUpDown _nudPosSmoothBeta = null!;
        private NumericUpDown _nudRotSmoothMinCutoff = null!;
        private NumericUpDown _nudRotSmoothBeta = null!;
        private Label _lblPosCutoff = null!;
        private Label _lblPosBeta = null!;
        private Label _lblRotCutoff = null!;
        private Label _lblRotBeta = null!;
        private CheckBox _chkDisableTriggerTouch = null!;
        private CheckBox _chkDisableThumbrestTouch = null!;
        private CheckBox _chkDisableTrackpad = null!;
        private CheckBox _chkVRIKKnuckles = null!;
        private CheckBox _chkGpuTiming = null!;
        private CheckBox _chkCombatHapticShield = null!;
        private CheckBox _chkCombatHapticWeapon = null!;
        private CheckBox _chkCombatHapticBow = null!;
        private CheckBox _chkCombatHapticMagic = null!;
        private NumericUpDown _nudCombatHapticStrength = null!;

        // Keyboard display settings
        private NumericUpDown _nudDisplayTilt = null!;
        private NumericUpDown _nudDisplayOpacity = null!;
        private NumericUpDown _nudDisplayScale = null!;
        private ComboBox _cmbKbTheme = null!;

        // Keyboard sound settings
        private CheckBox _chkSoundsEnabled = null!;
        private NumericUpDown _nudSoundVolume = null!;
        private NumericUpDown _nudPressVolume = null!;
        private NumericUpDown _nudKbHapticStrength = null!;

        // Dead zones (Skyrim)
        private NumericUpDown _nudLeftDeadZone = null!;
        private NumericUpDown _nudRightDeadZone = null!;

        // Controller axis adjustments (Skyrim)
        private Panel _pnlAxisAdjust = null!;
        private CheckBox _chkAdjustTilt = null!;
        private NumericUpDown _nudTiltDeg = null!;
        private CheckBox _chkLeftRotation = null!;
        private NumericUpDown _nudLeftRotX = null!;
        private NumericUpDown _nudLeftRotY = null!;
        private NumericUpDown _nudLeftRotZ = null!;
        private CheckBox _chkRightRotation = null!;
        private NumericUpDown _nudRightRotX = null!;
        private NumericUpDown _nudRightRotY = null!;
        private NumericUpDown _nudRightRotZ = null!;
        private CheckBox _chkLeftPosition = null!;
        private NumericUpDown _nudLeftPosX = null!;
        private NumericUpDown _nudLeftPosY = null!;
        private NumericUpDown _nudLeftPosZ = null!;
        private CheckBox _chkRightPosition = null!;
        private NumericUpDown _nudRightPosX = null!;
        private NumericUpDown _nudRightPosY = null!;
        private NumericUpDown _nudRightPosZ = null!;
        private CheckBox _chkLeftLaserRotation = null!;
        private NumericUpDown _nudLeftLaserRotX = null!;
        private NumericUpDown _nudLeftLaserRotY = null!;
        private NumericUpDown _nudLeftLaserRotZ = null!;
        private CheckBox _chkRightLaserRotation = null!;
        // Menu laser quad grid toggle: self-saving (writes menu_quad_settings.ini
        // directly, hot-reloaded by the DLL ~1s) — excluded from dirty-tracking.
        private CheckBox _chkShowQuadGrid = null!;
        private CheckBox _chkMenuLaserEnabled = null!;
        private bool _suppressQuadGridWrite;
        private NumericUpDown _nudRightLaserRotX = null!;
        private NumericUpDown _nudRightLaserRotY = null!;
        private NumericUpDown _nudRightLaserRotZ = null!;

        // Support footer
        private PictureBox _picKofi = null!;
        private Image? _kofiImage;
        private readonly List<Control> _footerControls = new();

        // Bottom buttons
        private Button _btnSave = null!;
        private Button _btnReload = null!;

        // Unsaved-changes indicator: breathing overlay banner + on-close save prompt
        private Label _lblUnsavedBanner = null!;
        private System.Windows.Forms.Timer _breatheTimer = null!;
        private bool _flashOn = false;
        private bool _dirty = false;

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
        private Button _btnVRIKDefaults = null!;  // legacy, retained for binary compat — UI replaced by _cmbBindingPreset
        private Button _btnResetDefaults = null!;
        private Button _btnValidateBindings = null!;
        private ComboBox _cmbBindingPreset = null!;
        private Button _btnApplyBindingPreset = null!;
        private Button _btnSaveAsBindingPreset = null!;
        private Button _btnDeleteBindingPreset = null!;

        // User-saved presets persist between launches in app-data. Live entries get loaded into
        // _cmbBindingPreset on startup and into _userBindingPresets so Apply
        // knows where the file lives.
        private readonly Dictionary<string, string> _userBindingPresets = new(StringComparer.OrdinalIgnoreCase);

        private static string GetUserPresetsDir()
        {
            string baseDir = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            string dir = Path.Combine(baseDir, "OpenCompositeConfigurator", "Presets");
            Directory.CreateDirectory(dir);
            return dir;
        }

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
            WireDirtyTracking(this);   // subscribe AFTER initial population so loading never marks dirty
        }

        private void LoadControllerImage()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.controllers.png");
            if (stream != null)
                _controllerImage = Image.FromStream(stream);
            LoadKnucklesImage();
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
            Size = new Size(1280, 1060);
            MinimumSize = new Size(1260, 800);
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

            var lblGame = MakeLabel($"OCU: {_gameName}", leftMargin, y + 3, 200);
            lblGame.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
            lblGame.ForeColor = Color.FromArgb(100, 200, 250);
            Controls.Add(lblGame);

            _lblInstallNotice = new Label
            {
                Location = new Point(leftMargin + 225, y),
                Width = 860,
                Height = 24,
                BackColor = Color.Transparent,
                ForeColor = Color.FromArgb(180, 180, 180),
                Font = new Font("Segoe UI", 9f, FontStyle.Italic),
                TextAlign = ContentAlignment.MiddleCenter,
                Text = "Keep this EXE in the OCU mod folder. Create a desktop shortcut; do not move the EXE."
            };
            Controls.Add(_lblInstallNotice);

            // Floating overlay on the top notice line (Visible=false until dirty) — big bold letters breathe, no box, no reflow
            _lblUnsavedBanner = new Label
            {
                Location = new Point(leftMargin + 225, y - 5),
                Width = 860,
                Height = 34,
                Visible = false,
                BackColor = Color.FromArgb(30, 30, 35),
                ForeColor = Color.FromArgb(120, 75, 20),
                Font = new Font("Segoe UI", 20f, FontStyle.Bold),
                TextAlign = ContentAlignment.MiddleCenter,
                Text = "YOU HAVE UNSAVED CHANGES, REMEMBER TO SAVE."
            };
            Controls.Add(_lblUnsavedBanner);
            _lblUnsavedBanner.BringToFront();

            _breatheTimer = new System.Windows.Forms.Timer { Interval = 450 };
            _breatheTimer.Tick += BreatheTimer_Tick;

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

            _btnTabGestures = new Button
            {
                Text = "Gestures",
                Location = new Point(leftMargin + 230, y),
                Size = new Size(95, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabGestures.FlatAppearance.BorderSize = 0;
            _btnTabGestures.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabGestures.Click += (s, e) => SwitchTab(2);
            Controls.Add(_btnTabGestures);

            _btnTabVideo = new Button
            {
                Text = "Video",
                Location = new Point(leftMargin + 330, y),
                Size = new Size(90, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabVideo.FlatAppearance.BorderSize = 0;
            _btnTabVideo.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabVideo.Click += (s, e) => SwitchTab(3);
            Controls.Add(_btnTabVideo);

            _btnTabHaptics = new Button
            {
                Text = "Haptics",
                Location = new Point(leftMargin + 425, y),
                Size = new Size(90, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabHaptics.FlatAppearance.BorderSize = 0;
            _btnTabHaptics.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabHaptics.Click += (s, e) => SwitchTab(4);
            _btnTabHaptics.Visible = ShowDevTools;
            Controls.Add(_btnTabHaptics);

            _btnTabBody = new Button
            {
                Text = "Body Tracking",
                Location = new Point(leftMargin + (ShowDevTools ? 520 : 425), y),
                Size = new Size(115, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabBody.FlatAppearance.BorderSize = 0;
            _btnTabBody.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabBody.Click += (s, e) => SwitchTab(5);
            // Camera FBT is shelved from the public build until it passes
            // in-headset testing; devtools.on restores the tab.
            _btnTabBody.Visible = ShowDevTools;
            Controls.Add(_btnTabBody);

            _btnTabTextures = new Button
            {
                Text = "Controller Textures",
                // Haptics and Body Tracking are devtools-only, so this sits
                // where they would be when they're hidden.
                Location = new Point(leftMargin + (ShowDevTools ? 640 : 425), y),
                Size = new Size(150, 30),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 10f),
                ForeColor = Color.FromArgb(160, 160, 160),
                BackColor = Color.FromArgb(35, 35, 40),
                Cursor = Cursors.Hand,
            };
            _btnTabTextures.FlatAppearance.BorderSize = 0;
            _btnTabTextures.FlatAppearance.MouseOverBackColor = Color.FromArgb(50, 50, 55);
            _btnTabTextures.Click += (s, e) => SwitchTab(6);
            Controls.Add(_btnTabTextures);

            y += 32;

            // Panel 1: Settings
            _tabSettings = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = true,
            };
            Controls.Add(_tabSettings);

            // Panel 2: Keyboard Bindings
            _tabKeyboard = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabKeyboard);

            // Panel 3: Gestures
            _tabGestures = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabGestures);

            // Panel 4: Video Settings
            _tabVideo = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabVideo);

            // Panel 5: Haptics
            _tabHaptics = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabHaptics);

            // Panel 6: Body Tracking
            _tabBody = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabBody);

            // Panel 7: Controller Textures
            _tabTextures = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 800), // resized after content built
                BackColor = Color.FromArgb(30, 30, 35),
                AutoScroll = false,
                Visible = false,
            };
            Controls.Add(_tabTextures);

            // Build content for each tab (each auto-sizes its panel)
            BuildSettingsTab();
            BuildKeyboardTab();
            BuildGesturesTab();
            BuildVideoTab();
            BuildHapticsTab();
            BuildBodyTrackingTab();
            BuildTexturesTab();

            // Sync all tabs to the same height (tallest content)
            int tallestTab = Math.Max(Math.Max(Math.Max(Math.Max(Math.Max(Math.Max(_tabSettings.Height, _tabKeyboard.Height), _tabGestures.Height), _tabVideo.Height), _tabHaptics.Height), _tabBody.Height), _tabTextures.Height);
            _tabSettings.Size = new Size(_tabSettings.Width, tallestTab);
            _tabKeyboard.Size = new Size(_tabKeyboard.Width, tallestTab);
            _tabGestures.Size = new Size(_tabGestures.Width, tallestTab);
            _tabVideo.Size = new Size(_tabVideo.Width, tallestTab);
            _tabHaptics.Size = new Size(_tabHaptics.Width, tallestTab);
            _tabBody.Size = new Size(_tabBody.Width, tallestTab);
            _tabTextures.Size = new Size(_tabTextures.Width, tallestTab);

            // Support footer right after the tabs
            int kofiY = _tabSettings.Location.Y + tallestTab + 4;
            const string supportUrl = "https://buymeacoffee.com/coldbomb1f";

            // Support footer
            var kofiSep = new Label
            {
                Location = new Point(leftMargin, kofiY),
                Size = new Size(rightEdge - leftMargin, 1),
                BackColor = Color.FromArgb(60, 60, 65)
            };
            Controls.Add(kofiSep);
            _footerControls.Add(kofiSep);
            kofiY += 6;

            // One line: italic text + bold link + icon
            var lblKofiMsg = new Label
            {
                Text = "OCU is maintained for the VR community. If you want to support ColdBomb,",
                Location = new Point(leftMargin, kofiY),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f, FontStyle.Italic),
                ForeColor = Color.FromArgb(160, 160, 160)
            };
            Controls.Add(lblKofiMsg);
            _footerControls.Add(lblKofiMsg);

            int linkX = leftMargin + lblKofiMsg.PreferredWidth + 4;
            var lblKofiLink = new LinkLabel
            {
                Text = "buy him a coffee",
                Location = new Point(linkX, kofiY),
                AutoSize = true,
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                LinkColor = Color.FromArgb(41, 171, 226),
                ActiveLinkColor = Color.FromArgb(80, 200, 255),
                VisitedLinkColor = Color.FromArgb(41, 171, 226)
            };
            lblKofiLink.LinkClicked += (s, e) =>
            {
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(supportUrl) { UseShellExecute = true }); }
                catch { }
            };
            Controls.Add(lblKofiLink);
            _footerControls.Add(lblKofiLink);

            int iconX = linkX + lblKofiLink.PreferredWidth + 4;
            _picKofi = new PictureBox
            {
                Location = new Point(iconX, kofiY - 2),
                Size = new Size(22, 22),
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.Transparent,
                Image = _kofiImage,
                Cursor = Cursors.Hand
            };
            _picKofi.Click += (s, e) =>
            {
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(supportUrl) { UseShellExecute = true }); }
                catch { }
            };
            Controls.Add(_picKofi);
            _footerControls.Add(_picKofi);

            // Size form to fit tabs + support footer
            ClientSize = new Size(ClientSize.Width, kofiY + 26);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            FitWindowToScreen();

            // Vortex / manual installs: sync the game-root runtime files from
            // the mod's root payload (install on first run, update on change).
            // Under MO2 this is strictly read-only - Root Builder owns root
            // deployment and the mod folder is never touched.
            string runtimeStatus = RuntimeInstaller.RunStartupCheck(this, GetConfiguratorDir());
            if (!string.IsNullOrEmpty(runtimeStatus))
            {
                _lblStatus.Text = runtimeStatus;
                _lblVideoStatus.Text = runtimeStatus;
            }
        }

        // Keep the entire window, including the support footer at the very bottom,
        // inside the screen's usable area. Runs after the form is shown and placed,
        // so it reads the real position and the actual monitor. If the content is
        // taller than the working area, shrink the TAB PANELS (they scroll
        // internally) and pull the footer up so it is always visible — scrolling
        // the whole form hid the footer below the fold.
        private void FitWindowToScreen()
        {
            Rectangle wa = Screen.FromControl(this).WorkingArea;
            int overflow = Height - wa.Height;
            if (overflow > 0)
            {
                if (MinimumSize.Height > wa.Height)
                    MinimumSize = new Size(MinimumSize.Width, wa.Height);

                int newTabH = Math.Max(300, _tabSettings.Height - overflow);
                int delta = _tabSettings.Height - newTabH;
                foreach (var tab in new[] { _tabSettings, _tabKeyboard, _tabGestures, _tabVideo, _tabHaptics, _tabBody })
                {
                    tab.AutoScroll = true;
                    tab.Height = newTabH;
                }
                foreach (Control c in _footerControls)
                    c.Top -= delta;
                Height -= delta;
            }
            if (Bottom > wa.Bottom)
                Top = wa.Bottom - Height;
            if (Top < wa.Top)
                Top = wa.Top;
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
                Size = new Size(rightEdge - leftMargin, 24),
                Text = "This configurator edits the OCU files in this mod folder. Keep the EXE here and launch it from a shortcut.",
                ForeColor = Color.FromArgb(150, 200, 250),
                Font = new Font("Segoe UI", 8.5f, FontStyle.Italic),
                BackColor = Color.FromArgb(40, 45, 60),
                BorderStyle = BorderStyle.FixedSingle,
                Padding = new Padding(6, 4, 6, 4),
                AutoSize = false
            };
            container.Controls.Add(lblMO2Note);

            y += 30;
            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 8;

            // ── SAVE/RELOAD BUTTONS (top right corner) ──
            var btnSettingsMasterReset = MakeButton("Master Reset", rightEdge - 450, y, 130, 30);
            btnSettingsMasterReset.BackColor = Color.FromArgb(120, 60, 40);
            btnSettingsMasterReset.ForeColor = Color.White;
            btnSettingsMasterReset.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            btnSettingsMasterReset.Click += BtnSettingsMasterReset_Click;
            container.Controls.Add(btnSettingsMasterReset);

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
            y += 46;

            // Controller image on the left
            _picControllers = new PictureBox
            {
                Location = new Point(leftMargin, y),
                Size = new Size(300, 155),
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

            // Index trackpad swipe row: only visible when the knuckles model is
            // selected on the Bindings tab (ApplyControllerModel toggles it)
            chkY += 22;
            _lblTrackpadSwipe = MakeLabel("Trackpad:", cx, chkY + 3, 70);
            _lblTrackpadSwipe.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            _lblTrackpadSwipe.ForeColor = Color.FromArgb(180, 200, 255);
            _lblTrackpadSwipe.Visible = false;
            container.Controls.Add(_lblTrackpadSwipe);

            _cmbTrackpadSwipe = new ComboBox
            {
                Location = new Point(cx + 74, chkY),
                Width = 130,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f),
                Visible = false,
            };
            _cmbTrackpadSwipe.Items.AddRange(new object[] { "None", "Swipe Up", "Swipe Down" });
            _cmbTrackpadSwipe.SelectedIndex = 0;
            container.Controls.Add(_cmbTrackpadSwipe);

            _lblTrackpadSwipeHint = MakeLabel("Swipe the Index trackpad to open the keyboard", cx + 210, chkY + 3, 300);
            _lblTrackpadSwipeHint.ForeColor = Color.FromArgb(140, 140, 140);
            _lblTrackpadSwipeHint.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Italic);
            _lblTrackpadSwipeHint.Visible = false;
            container.Controls.Add(_lblTrackpadSwipeHint);

            foreach (var chk in new[] { _chkLeftStick, _chkLeftX, _chkLeftY,
                                        _chkRightStick, _chkRightA, _chkRightB })
            {
                chk.CheckedChanged += (s, e) => _picControllers.Invalidate();
            }

            int imageBottom = _picControllers.Bottom + 10;
            chkY += 22;

            // ── Tap count (right under Trigger = Laser Click) ──
            container.Controls.Add(MakeLabel("Tap:", cx, chkY + 3, 30));
            var tapPanel = new Panel
            {
                Location = new Point(cx + 32, chkY),
                Size = new Size(260, 26),
                BackColor = Color.Transparent
            };
            _rdoX1 = MakeRadioButton("x1 Hold", 0, 1, 80);
            _rdoX2 = MakeRadioButton("x2", 82, 1, 45);
            _rdoX3 = MakeRadioButton("x3", 130, 1, 45);
            _rdoX4 = MakeRadioButton("x4", 178, 1, 45);
            _rdoX2.Checked = true;
            tapPanel.Controls.AddRange(new Control[] { _rdoX1, _rdoX2, _rdoX3, _rdoX4 });
            container.Controls.Add(tapPanel);
            chkY += 26;

            // ── Timing (under tap, against the picture) ──
            container.Controls.Add(MakeLabel("Timing:", cx, chkY + 3, 55));
            _nudTiming = new NumericUpDown
            {
                Location = new Point(cx + 58, chkY),
                Width = 70,
                Minimum = 100, Maximum = 3000, Increment = 50, Value = 500,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudTiming);
            _lblTimingDesc = MakeLabel("ms", cx + 132, chkY + 3, 30);
            _lblTimingDesc.ForeColor = Color.FromArgb(140, 140, 140);
            container.Controls.Add(_lblTimingDesc);

            _rdoX1.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX2.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX3.CheckedChanged += (s, e) => UpdateTimingLabel();
            _rdoX4.CheckedChanged += (s, e) => UpdateTimingLabel();
            chkY += 28;

            // ── Right column: Display, feedback, sounds (next to controller image) ──
            int rx = 680;
            int ry = _picControllers.Top;

            // Row 1: Display settings
            container.Controls.Add(MakeLabel("Tilt:", rx, ry + 3, 35));
            _nudDisplayTilt = new NumericUpDown
            {
                Location = new Point(rx + 35, ry), Width = 65,
                DecimalPlaces = 1, Increment = 1m, Minimum = -30m, Maximum = 80m, Value = 22.5m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayTilt);
            container.Controls.Add(MakeLabel("\u00B0", rx + 102, ry + 3, 16));

            container.Controls.Add(MakeLabel("Trans:", rx + 130, ry + 3, 45));
            _nudDisplayOpacity = new NumericUpDown
            {
                Location = new Point(rx + 178, ry), Width = 55,
                Minimum = 1, Maximum = 100, Increment = 5, Value = 30,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayOpacity);
            container.Controls.Add(MakeLabel("%", rx + 236, ry + 3, 20));

            container.Controls.Add(MakeLabel("Size:", rx + 268, ry + 3, 35));
            _nudDisplayScale = new NumericUpDown
            {
                Location = new Point(rx + 305, ry), Width = 55,
                Minimum = 50, Maximum = 150, Increment = 5, Value = 100,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDisplayScale);
            container.Controls.Add(MakeLabel("%", rx + 363, ry + 3, 20));
            ry += 30;

            // Row 2: Keyboard theme (applies live — the DLL hot-swaps on save)
            container.Controls.Add(MakeLabel("Theme:", rx, ry + 3, 50));
            _cmbKbTheme = new ComboBox
            {
                Location = new Point(rx + 55, ry),
                Width = 160,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f),
            };
            // Only Parchment has passed in-headset review; the other skins
            // return with devtools.on until they are release-ready.
            _cmbKbTheme.Items.AddRange(ShowDevTools
                ? new object[] { "Parchment", "SkyUI Dark", "Dwemer", "Sovngarde" }
                : new object[] { "Parchment" });
            _cmbKbTheme.SelectedIndex = 0;
            container.Controls.Add(_cmbKbTheme);
            ry += 28;

            // Row 3: Feedback checkbox
            _chkSoundsEnabled = MakeCheckBox("Keyboard feedback", rx, ry);
            _chkSoundsEnabled.Checked = true;
            container.Controls.Add(_chkSoundsEnabled);
            ry += 26;

            // Row 4: Volume controls. AutoSize so DPI scaling can never clip
            // the label to nothing (reported cut off on a scaled display).
            var lblKbMasterVol = MakeLabel("Master:", rx, ry + 3, 55);
            lblKbMasterVol.AutoSize = true;
            container.Controls.Add(lblKbMasterVol);
            _nudSoundVolume = new NumericUpDown
            {
                Location = new Point(rx + 55, ry), Width = 55,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudSoundVolume);
            container.Controls.Add(MakeLabel("%", rx + 113, ry + 3, 20));

            container.Controls.Add(MakeLabel("Press:", rx + 148, ry + 3, 45));
            _nudPressVolume = new NumericUpDown
            {
                Location = new Point(rx + 195, ry), Width = 55,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudPressVolume);
            container.Controls.Add(MakeLabel("%", rx + 253, ry + 3, 20));

            // Keyboard haptic strength moved to the dedicated Haptics tab
            ry += 36;

            // No reset-position control: the keyboard spawns head-relative and clamped to
            // arm's reach, so it can never be lost — moving it is the reset. The DLL still
            // persists the parked offset (positionForward/Down/Right) for sticky placement.

            y = Math.Max(Math.Max(imageBottom, chkY), ry);

            y += 12;
            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 8;

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
                DecimalPlaces = 1, Increment = 0.1m, Minimum = 0.5m, Maximum = 2.0m, Value = 1.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudSuperSample);

            // Haptic master switch + strength moved to the dedicated Haptics tab
            y += 28;

            _chkRenderHands = MakeCheckBox("Render custom hands", gc1, y);
            _chkRenderHands.Checked = true;
            container.Controls.Add(_chkRenderHands);
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
            sy += 26;

            // Controller smoothing fine-tuning (1€ filter parameters)
            int smIndent = 16; // indent under the checkbox
            _lblPosCutoff = MakeLabel("Pos cutoff:", sc1 + smIndent, sy + 3, 80);
            _pnlSkyrimOnly.Controls.Add(_lblPosCutoff);
            _nudPosSmoothMinCutoff = new NumericUpDown
            {
                Location = new Point(sc1 + smIndent + 80, sy), Width = 65,
                DecimalPlaces = 2, Increment = 0.25m, Minimum = 0.01m, Maximum = 20.0m, Value = 1.25m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudPosSmoothMinCutoff);
            _lblPosBeta = MakeLabel("Pos beta:", sc2, sy + 3, 70);
            _pnlSkyrimOnly.Controls.Add(_lblPosBeta);
            _nudPosSmoothBeta = new NumericUpDown
            {
                Location = new Point(sc2 + 70, sy), Width = 65,
                DecimalPlaces = 1, Increment = 1.0m, Minimum = 0.0m, Maximum = 100.0m, Value = 20.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudPosSmoothBeta);
            sy += 26;

            _lblRotCutoff = MakeLabel("Rot cutoff:", sc1 + smIndent, sy + 3, 80);
            _pnlSkyrimOnly.Controls.Add(_lblRotCutoff);
            _nudRotSmoothMinCutoff = new NumericUpDown
            {
                Location = new Point(sc1 + smIndent + 80, sy), Width = 65,
                DecimalPlaces = 2, Increment = 0.25m, Minimum = 0.01m, Maximum = 20.0m, Value = 1.50m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudRotSmoothMinCutoff);
            _lblRotBeta = MakeLabel("Rot beta:", sc2, sy + 3, 70);
            _pnlSkyrimOnly.Controls.Add(_lblRotBeta);
            _nudRotSmoothBeta = new NumericUpDown
            {
                Location = new Point(sc2 + 70, sy), Width = 65,
                DecimalPlaces = 1, Increment = 0.1m, Minimum = 0.0m, Maximum = 10.0m, Value = 0.2m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudRotSmoothBeta);
            sy += 26;

            // Enable/disable smoothing controls based on checkbox
            _chkControllerSmoothing.CheckedChanged += (s, e) =>
            {
                bool en = _chkControllerSmoothing.Checked;
                _nudPosSmoothMinCutoff.Enabled = en;
                _nudPosSmoothBeta.Enabled = en;
                _nudRotSmoothMinCutoff.Enabled = en;
                _nudRotSmoothBeta.Enabled = en;
                _lblPosCutoff.Enabled = en;
                _lblPosBeta.Enabled = en;
                _lblRotCutoff.Enabled = en;
                _lblRotBeta.Enabled = en;
            };

            // ── Right column within Skyrim panel ──
            int sc3 = 380;
            int sc4 = 570;
            int sy2 = 26; // aligned with first content row

            _chkDisableTriggerTouch = MakeCheckBox("Disable trigger touch", sc3, sy2);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTriggerTouch);
            _chkDisableThumbrestTouch = MakeCheckBox("Disable thumbrest touch", sc4, sy2);
            _chkDisableThumbrestTouch.CheckedChanged += (s, e) =>
            {
                if (!ThumbrestBindingEnabled() && IsThumbrestButton(_selectedCtrlButton))
                {
                    _selectedCtrlButton = null;
                    _lblCtrlButton.Text = "None";
                    SelectActionInCombo(_cmbCtrlAction, null);
                    _cmbCtrlAction.Enabled = false;
                }
                _picBindingsController?.Invalidate();
            };
            _pnlSkyrimOnly.Controls.Add(_chkDisableThumbrestTouch);
            sy2 += 26;

            _chkDisableTrackpad = MakeCheckBox("Disable trackpad", sc3, sy2);
            _pnlSkyrimOnly.Controls.Add(_chkDisableTrackpad);
            _chkVRIKKnuckles = MakeCheckBox("VRIK Knuckles support", sc4, sy2);
            _pnlSkyrimOnly.Controls.Add(_chkVRIKKnuckles);
            sy2 += 26;

            _chkGpuTiming = MakeCheckBox("GPU frame timing", sc3, sy2);
            _chkGpuTiming.Checked = true;
            _pnlSkyrimOnly.Controls.Add(_chkGpuTiming);
            sy2 += 26;

            _pnlSkyrimOnly.Controls.Add(MakeLabel("L dead zone:", sc3, sy2 + 3, 90));
            _nudLeftDeadZone = new NumericUpDown
            {
                Location = new Point(sc3 + 90, sy2), Width = 65,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudLeftDeadZone);
            _pnlSkyrimOnly.Controls.Add(MakeLabel("R:", sc3 + 165, sy2 + 3, 20));
            _nudRightDeadZone = new NumericUpDown
            {
                Location = new Point(sc3 + 185, sy2), Width = 65,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.0m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _pnlSkyrimOnly.Controls.Add(_nudRightDeadZone);
            sy2 += 28;

            // Combat haptics moved to the dedicated Haptics tab (BuildHapticsTab)

            _pnlSkyrimOnly.Size = new Size(rightEdge - leftMargin - 440, Math.Max(sy, sy2));

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

            // ── CONTROLLER AXIS ADJUSTMENTS (Skyrim only) ──
            {
                int axisTop = y;
                _pnlAxisAdjust = new Panel
                {
                    Location = new Point(leftMargin, y),
                    BackColor = Color.Transparent,
                    Visible = _gameType == "skyrim"
                };
                container.Controls.Add(_pnlAxisAdjust);

                int ay = 0;
                int ax1 = 0;        // left group start
                int ax2 = 620;      // right group start

                var lblAxisSection = MakeSectionLabel("Controller Axis Adjustments", ax1, ay);
                _pnlAxisAdjust.Controls.Add(lblAxisSection);

                var btnResetAxis = MakeButton("Reset All to Defaults", 280, ay, 150, 24);
                btnResetAxis.BackColor = Color.FromArgb(120, 60, 40);
                btnResetAxis.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
                btnResetAxis.Click += BtnResetAxis_Click;
                _pnlAxisAdjust.Controls.Add(btnResetAxis);

                var lblAxisWarn = MakeLabel("(defaults are all 0 \u2014 only change if alignment feels off)", 450, ay + 3, 500);
                lblAxisWarn.ForeColor = Color.FromArgb(160, 160, 160);
                lblAxisWarn.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
                _pnlAxisAdjust.Controls.Add(lblAxisWarn);
                ay += 24;

                // Row 1: Tilt
                _chkAdjustTilt = MakeCheckBox("Adjust tilt", ax1, ay);
                _pnlAxisAdjust.Controls.Add(_chkAdjustTilt);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Tilt:", ax1 + 140, ay + 3, 35));
                _nudTiltDeg = MakeAxisNud(ax1 + 175, ay, -90m, 90m, 0.5m, 1);
                _pnlAxisAdjust.Controls.Add(_nudTiltDeg);
                _pnlAxisAdjust.Controls.Add(MakeLabel("\u00B0 (degrees)", ax1 + 245, ay + 3, 80));
                _chkAdjustTilt.CheckedChanged += (s, e) => _nudTiltDeg.Enabled = _chkAdjustTilt.Checked;
                ay += 24;

                // Row 2: Left rotation + Left position
                _chkLeftRotation = MakeCheckBox("Left rotation", ax1, ay);
                _pnlAxisAdjust.Controls.Add(_chkLeftRotation);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax1 + 140, ay + 3, 16));
                _nudLeftRotX = MakeAxisNud(ax1 + 158, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftRotX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax1 + 230, ay + 3, 16));
                _nudLeftRotY = MakeAxisNud(ax1 + 248, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftRotY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax1 + 320, ay + 3, 16));
                _nudLeftRotZ = MakeAxisNud(ax1 + 338, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftRotZ);
                _chkLeftRotation.CheckedChanged += (s, e) =>
                {
                    bool en = _chkLeftRotation.Checked;
                    _nudLeftRotX.Enabled = en; _nudLeftRotY.Enabled = en; _nudLeftRotZ.Enabled = en;
                };

                _chkLeftPosition = MakeCheckBox("Left position", ax2, ay);
                _pnlAxisAdjust.Controls.Add(_chkLeftPosition);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax2 + 140, ay + 3, 16));
                _nudLeftPosX = MakeAxisNud(ax2 + 158, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudLeftPosX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax2 + 230, ay + 3, 16));
                _nudLeftPosY = MakeAxisNud(ax2 + 248, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudLeftPosY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax2 + 320, ay + 3, 16));
                _nudLeftPosZ = MakeAxisNud(ax2 + 338, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudLeftPosZ);
                _chkLeftPosition.CheckedChanged += (s, e) =>
                {
                    bool en = _chkLeftPosition.Checked;
                    _nudLeftPosX.Enabled = en; _nudLeftPosY.Enabled = en; _nudLeftPosZ.Enabled = en;
                };
                ay += 24;

                // Row 3: Right rotation + Right position
                _chkRightRotation = MakeCheckBox("Right rotation", ax1, ay);
                _pnlAxisAdjust.Controls.Add(_chkRightRotation);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax1 + 140, ay + 3, 16));
                _nudRightRotX = MakeAxisNud(ax1 + 158, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightRotX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax1 + 230, ay + 3, 16));
                _nudRightRotY = MakeAxisNud(ax1 + 248, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightRotY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax1 + 320, ay + 3, 16));
                _nudRightRotZ = MakeAxisNud(ax1 + 338, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightRotZ);
                _chkRightRotation.CheckedChanged += (s, e) =>
                {
                    bool en = _chkRightRotation.Checked;
                    _nudRightRotX.Enabled = en; _nudRightRotY.Enabled = en; _nudRightRotZ.Enabled = en;
                };

                _chkRightPosition = MakeCheckBox("Right position", ax2, ay);
                _pnlAxisAdjust.Controls.Add(_chkRightPosition);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax2 + 140, ay + 3, 16));
                _nudRightPosX = MakeAxisNud(ax2 + 158, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudRightPosX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax2 + 230, ay + 3, 16));
                _nudRightPosY = MakeAxisNud(ax2 + 248, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudRightPosY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax2 + 320, ay + 3, 16));
                _nudRightPosZ = MakeAxisNud(ax2 + 338, ay, -0.50m, 0.50m, 0.005m, 3);
                _pnlAxisAdjust.Controls.Add(_nudRightPosZ);
                _chkRightPosition.CheckedChanged += (s, e) =>
                {
                    bool en = _chkRightPosition.Checked;
                    _nudRightPosX.Enabled = en; _nudRightPosY.Enabled = en; _nudRightPosZ.Enabled = en;
                };
                ay += 24;

                var lblLaserSection = MakeSectionLabel("Laser Aim Calibration", ax1, ay);
                _pnlAxisAdjust.Controls.Add(lblLaserSection);

                var lblLaserWarn = MakeLabel("Use at your own risk. Report useful headset/runtime/controller values to OCU devs.", ax1 + 185, ay + 3, 760);
                lblLaserWarn.ForeColor = Color.FromArgb(255, 185, 70);
                lblLaserWarn.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
                _pnlAxisAdjust.Controls.Add(lblLaserWarn);
                ay += 24;

                _chkMenuLaserEnabled = MakeCheckBox(
                    "Enable menu lasers (uncheck for classic controller-only menus; Save + game restart required)", ax1, ay);
                _chkMenuLaserEnabled.Checked = true;
                _pnlAxisAdjust.Controls.Add(_chkMenuLaserEnabled);
                ay += 24;

                _chkLeftLaserRotation = MakeCheckBox("Left laser", ax1, ay);
                _pnlAxisAdjust.Controls.Add(_chkLeftLaserRotation);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax1 + 140, ay + 3, 16));
                _nudLeftLaserRotX = MakeAxisNud(ax1 + 158, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftLaserRotX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax1 + 230, ay + 3, 16));
                _nudLeftLaserRotY = MakeAxisNud(ax1 + 248, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftLaserRotY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax1 + 320, ay + 3, 16));
                _nudLeftLaserRotZ = MakeAxisNud(ax1 + 338, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudLeftLaserRotZ);
                _chkLeftLaserRotation.CheckedChanged += (s, e) =>
                {
                    bool en = _chkLeftLaserRotation.Checked;
                    _nudLeftLaserRotX.Enabled = en; _nudLeftLaserRotY.Enabled = en; _nudLeftLaserRotZ.Enabled = en;
                };

                _chkRightLaserRotation = MakeCheckBox("Right laser", ax2, ay);
                _pnlAxisAdjust.Controls.Add(_chkRightLaserRotation);
                _pnlAxisAdjust.Controls.Add(MakeLabel("X:", ax2 + 140, ay + 3, 16));
                _nudRightLaserRotX = MakeAxisNud(ax2 + 158, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightLaserRotX);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Y:", ax2 + 230, ay + 3, 16));
                _nudRightLaserRotY = MakeAxisNud(ax2 + 248, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightLaserRotY);
                _pnlAxisAdjust.Controls.Add(MakeLabel("Z:", ax2 + 320, ay + 3, 16));
                _nudRightLaserRotZ = MakeAxisNud(ax2 + 338, ay, -90m, 90m, 1m, 1);
                _pnlAxisAdjust.Controls.Add(_nudRightLaserRotZ);
                _chkRightLaserRotation.CheckedChanged += (s, e) =>
                {
                    bool en = _chkRightLaserRotation.Checked;
                    _nudRightLaserRotX.Enabled = en; _nudRightLaserRotY.Enabled = en; _nudRightLaserRotZ.Enabled = en;
                };
                ay += 28;

                _chkShowQuadGrid = MakeCheckBox("Show menu laser calibration grid (applies live in-game, about 1 second)", ax1, ay);
                _chkShowQuadGrid.CheckedChanged += (s, e) =>
                {
                    if (!_suppressQuadGridWrite)
                        SetMenuQuadFlag("show_calibration_quad", _chkShowQuadGrid.Checked);
                };
                _pnlAxisAdjust.Controls.Add(_chkShowQuadGrid);
                ay += 28;

                _pnlAxisAdjust.Controls.Add(MakeSeparator(ax1, ay, rightEdge - leftMargin));
                ay += 12;

                var lblSteamVrSection = MakeSectionLabel("SteamVR OCU Profile", ax1, ay);
                _pnlAxisAdjust.Controls.Add(lblSteamVrSection);
                ay += 28;

                var btnApplySteamVrProfile = MakeButton("Apply SteamVR OCU Profile", ax1, ay, 220, 28);
                btnApplySteamVrProfile.BackColor = Color.FromArgb(40, 120, 40);
                btnApplySteamVrProfile.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
                btnApplySteamVrProfile.Click += BtnApplySteamVrProfile_Click;
                _pnlAxisAdjust.Controls.Add(btnApplySteamVrProfile);

                var btnRestoreSteamVrDefaults = MakeButton("Restore SteamVR Defaults", ax1 + 235, ay, 210, 28);
                btnRestoreSteamVrDefaults.BackColor = Color.FromArgb(120, 60, 40);
                btnRestoreSteamVrDefaults.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
                btnRestoreSteamVrDefaults.Click += BtnRestoreSteamVrDefaults_Click;
                _pnlAxisAdjust.Controls.Add(btnRestoreSteamVrDefaults);

                var btnOpenSteamVrSettings = MakeButton("Open SteamVR Settings Folder", ax1 + 460, ay, 225, 28);
                btnOpenSteamVrSettings.Font = new Font("Segoe UI", 8.5f);
                btnOpenSteamVrSettings.Click += BtnOpenSteamVrSettingsFolder_Click;
                _pnlAxisAdjust.Controls.Add(btnOpenSteamVrSettings);

                var lblSteamVrDesc = MakeLabel("Important for SteamVR/OpenXR headsets: set SteamVR as the active OpenXR runtime, then apply this profile. Patches %LOCALAPPDATA%\\openvr\\steamvr.vrsettings.", ax1 + 700, ay + 3, rightEdge - leftMargin - 710);
                lblSteamVrDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblSteamVrDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                lblSteamVrDesc.Height = 40;
                _pnlAxisAdjust.Controls.Add(lblSteamVrDesc);
                ay += 48;

                var btnOpenSetupReadme = MakeButton("Open Setup Readme", ax1, ay, 180, 28);
                btnOpenSetupReadme.Font = new Font("Segoe UI", 8.5f, FontStyle.Bold);
                btnOpenSetupReadme.Click += BtnOpenSetupReadme_Click;
                _pnlAxisAdjust.Controls.Add(btnOpenSetupReadme);

                var lblSetupReadmeDesc = MakeLabel("Opens the OCU setup manual in your external browser.", ax1 + 195, ay + 5, 520);
                lblSetupReadmeDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblSetupReadmeDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                _pnlAxisAdjust.Controls.Add(lblSetupReadmeDesc);
                ay += 40;

                _pnlAxisAdjust.Size = new Size(rightEdge - leftMargin, ay);
                if (_pnlAxisAdjust.Visible) y += ay;
            }

            // Status label at the bottom
            _lblStatus = MakeLabel("", leftMargin, y + 6, 600);
            _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
            container.Controls.Add(_lblStatus);

            // Auto-size panel to fit content
            container.Size = new Size(container.Width, y + 30);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // BINDINGS TAB
        // ═══════════════════════════════════════════════════════════════════════

        private void ParseControlmapTemplate()
        {
            LoadControlmapTemplateModel(resetContextNames: true);
        }

        private void LoadControlmapTemplateModel(bool resetContextNames)
        {
            var asm = Assembly.GetExecutingAssembly();
            using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt");
            if (stream == null) return;
            using var reader = new StreamReader(stream);
            LoadControlmapModelFromText(reader.ReadToEnd(), resetContextNames);
        }

        private void LoadControlmapModelFromText(string controlmapText, bool resetContextNames)
        {
            string[] lines = controlmapText.Split('\n');
            _contextBindings.Clear();
            if (resetContextNames)
                _contextNames.Clear();

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
                            if (resetContextNames || !_contextNames.Contains(currentContext))
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

        private void RefreshKeyboardBindingsFromControlmapModel()
        {
            if (!_contextBindings.TryGetValue("Main Gameplay", out var actions))
                return;

            foreach (var fields in actions)
            {
                if (fields.Length < 2) continue;

                string actionName = fields[0];
                string scStr = fields[1].ToLowerInvariant();
                if (scStr.Contains(','))
                    scStr = scStr.Split(',')[0].Trim();

                if (scStr.StartsWith("0x") &&
                    int.TryParse(scStr[2..], System.Globalization.NumberStyles.HexNumber, null, out int scancode))
                {
                    var action = GameActions.FirstOrDefault(a => a.id == actionName);
                    if (action.id != null)
                        _keyBindings[actionName] = scancode;
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
            { "l_thumbrest", ("L Thumbrest",   new PointF(0.398f, 0.244f), false) },
            // Right controller
            { "right_stick", ("R Stick Click", new PointF(0.713f, 0.147f), false) },
            { "a_button",   ("A Button",      new PointF(0.670f, 0.254f), false) },
            { "b_button",   ("B Button",      new PointF(0.627f, 0.189f), false) },
            { "r_trigger",  ("R Trigger",     new PointF(0.537f, 0.106f), false) },
            { "r_grip",     ("R Grip",        new PointF(0.605f, 0.515f), false) },
            { "r_thumbrest", ("R Thumbrest",   new PointF(0.585f, 0.244f), false) },
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
        // OpenVR button IDs: 0x01=B/Y(AppMenu), 0x02=Grip, 0x04=DPad Up, 0x07=A/X, 0x20=StickPress, 0x21=Trigger
        // NOTE: 0x0b/0x0c = stick AXIS (movement/look), 0x20 = stick PRESS (click in)
        private static readonly Dictionary<string, (string hexRight, string hexLeft)> ControllerButtonHex = new()
        {
            { "left_stick",  ("",     "0x20") },   // Left stick press (Sprint in Gameplay)
            { "x_button",    ("",     "0x07") },   // X = A/X button on left hand
            { "y_button",    ("",     "0x01") },   // Y = AppMenu button on left hand
            { "l_trigger",   ("",     "0x21") },   // Left trigger
            { "l_grip",      ("",     "0x02") },   // Left grip (Cancel, Ready Weapon)
            { "l_thumbrest", ("",     "0x04") },   // Left thumbrest touch
            { "right_stick", ("0x20", "") },        // Right stick press
            { "a_button",    ("0x07", "") },        // A = A/X button on right hand
            { "b_button",    ("0x01", "") },        // B = AppMenu button on right hand
            { "r_trigger",   ("0x21", "") },        // Right trigger
            { "r_grip",      ("0x02", "") },        // Right grip (Shout, Cancel)
            { "r_thumbrest", ("0x04", "") },        // Right thumbrest touch
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

            _btnSaveBindings = MakeButton("Save All Bindings", rightEdge - 210, y, 210, 26);
            _btnSaveBindings.BackColor = Color.FromArgb(40, 120, 40);
            _btnSaveBindings.Font = new Font("Segoe UI", 9f, FontStyle.Bold);
            _btnSaveBindings.Click += BtnSaveBindings_Click;
            var tipSave = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400 };
            tipSave.SetToolTip(_btnSaveBindings,
                "Writes the entire controlmapvr.txt\n" +
                "(keyboard, mouse, gamepad, AND all\n" +
                "controller bindings) plus combos.\n\n" +
                "Restart the game to apply.");
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

            _btnVRDefaults = MakeButton("Master Reset", leftMargin + 370, y, 120, 24);
            _btnVRDefaults.BackColor = Color.FromArgb(40, 100, 160);
            _btnVRDefaults.Font = new Font("Segoe UI", 8f, FontStyle.Bold);
            _btnVRDefaults.Click += BtnVRDefaults_Click;
            container.Controls.Add(_btnVRDefaults);

            _btnResetDefaults = MakeButton("Reset to Game Defaults", leftMargin + 500, y, 140, 24);
            _btnResetDefaults.Font = new Font("Segoe UI", 8f);
            _btnResetDefaults.Click += (s, e) => ResetControlmapToDefaults();
            container.Controls.Add(_btnResetDefaults);

            _btnValidateBindings = MakeButton("Validate Bindings", leftMargin + 650, y, 135, 24);
            _btnValidateBindings.BackColor = Color.FromArgb(120, 80, 40);
            _btnValidateBindings.Font = new Font("Segoe UI", 8f);
            _btnValidateBindings.Click += BtnValidateBindings_Click;
            var tipValidate = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400 };
            tipValidate.SetToolTip(_btnValidateBindings,
                "Scans the live controlmapvr.txt for known\n" +
                "startup-crash binding patterns.\n\n" +
                "Known deterministic issues are repaired.\n" +
                "Ambiguous issues are reported for review.");
            container.Controls.Add(_btnValidateBindings);

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

            // Type dropdown — kept on the section-header row alongside the section label.
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

            var tipCtrlType = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400 };
            tipCtrlType.SetToolTip(_cmbCtrlType,
                "Picks which Skyrim input context\n" +
                "the binding UI is editing.\n\n" +
                "Each context (Main Gameplay, Menu Mode,\n" +
                "Inventory, etc.) has its own set of action\n" +
                "mappings in controlmapvr.txt.\n\n" +
                "Click a button on the controller image to\n" +
                "see what action is bound to it in the\n" +
                "selected context.");

            // Move Preset row down one line so it doesn't collide with the Type dropdown.
            // This is its own visually-distinct row of preset-management controls.
            y += 28;

            // Binding preset dropdown — applies controller bindings while preserving keyboard.
            // Replaces the old single "VRIK V2.1.0" button. Active preset persists in
            // opencomposite.ini under [Configurator] activeBindingPreset.
            container.Controls.Add(MakeLabel("Preset:", rightEdge - 565, y + 4, 50));
            _cmbBindingPreset = new ComboBox
            {
                Location = new Point(rightEdge - 515, y),
                Width = 145,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8f)
            };
            _cmbBindingPreset.Items.AddRange(new object[] {
                "Vanilla",
                "VR Safe",
                "VRIK V2.1.0",
                "Snippy",
                "Kvite",
                "Cangar",
                "Cangar Spellsiphon",
                "Vanilla + Oculus Touch Hotkeys",
                "Peak Combat",
                "Wondernutts",
            });
            _cmbBindingPreset.SelectedIndex = 2;
            _cmbBindingPreset.SelectedIndexChanged += CmbBindingPreset_SelectedIndexChanged;
            container.Controls.Add(_cmbBindingPreset);

            _btnApplyBindingPreset = MakeButton("Apply Preset", rightEdge - 365, y, 105, 24);
            _btnApplyBindingPreset.BackColor = Color.FromArgb(120, 80, 40);
            _btnApplyBindingPreset.Font = new Font("Segoe UI", 8f);
            _btnApplyBindingPreset.Click += BtnApplyBindingPreset_Click;
            container.Controls.Add(_btnApplyBindingPreset);

            // Save As — captures the currently-applied controlmapvr.txt as a named
            // user preset under %AppData%\OpenCompositeConfigurator\Presets\ so the
            // user's edits survive across sessions and reappear in the dropdown.
            _btnSaveAsBindingPreset = MakeButton("Save As…", rightEdge - 255, y, 80, 24);
            _btnSaveAsBindingPreset.BackColor = Color.FromArgb(40, 110, 60);
            _btnSaveAsBindingPreset.Font = new Font("Segoe UI", 8f);
            _btnSaveAsBindingPreset.Click += BtnSaveAsBindingPreset_Click;
            container.Controls.Add(_btnSaveAsBindingPreset);

            // Delete removes a Save-As preset from disk and the dropdown.
            // Disabled when a built-in preset is selected (those are embedded in the
            // EXE and can't be deleted from outside).
            _btnDeleteBindingPreset = MakeButton("Remove", rightEdge - 170, y, 80, 24);
            _btnDeleteBindingPreset.BackColor = Color.FromArgb(140, 50, 50);
            _btnDeleteBindingPreset.ForeColor = Color.White;
            _btnDeleteBindingPreset.Font = new Font("Segoe UI", 8f);
            // Stay enabled at all times so the white text doesn't fade out under
            // the disabled-button render path. The click handler already shows
            // a friendly "built-in can't be deleted" message when needed.
            _btnDeleteBindingPreset.Click += BtnDeleteBindingPreset_Click;
            container.Controls.Add(_btnDeleteBindingPreset);

            // Tooltips for every button on this row so users understand what each
            // does without cluttering the labels. AutoPopDelay set high so users
            // can actually finish reading.
            var tipPreset = new ToolTip { AutoPopDelay = 12000, InitialDelay = 400 };
            tipPreset.SetToolTip(_cmbBindingPreset,
                "Built-in presets ship with the EXE.\n" +
                "User presets created with Save As live under\n" +
                "%AppData%\\OpenCompositeConfigurator\\Presets\\\n" +
                "and can be deleted.");
            tipPreset.SetToolTip(_btnApplyBindingPreset,
                "Writes the selected preset's controller bindings\n" +
                "to your live controlmapvr.txt.\n\n" +
                "Your keyboard / mouse / gamepad bindings\n" +
                "are preserved (merge mode).\n\n" +
                "Restart the game to apply.");
            tipPreset.SetToolTip(_btnSaveAsBindingPreset,
                "Captures your current controlmapvr.txt\n" +
                "as a named user preset.\n\n" +
                "Saved to:\n" +
                "  %AppData%\\OpenCompositeConfigurator\\Presets\\\n\n" +
                "Captures EVERYTHING in the file —\n" +
                "keyboard, mouse, gamepad, AND controllers.\n\n" +
                "Click 'Save All Bindings' first if you\n" +
                "have unsaved edits in the UI.");
            tipPreset.SetToolTip(_btnDeleteBindingPreset,
                "Removes the selected user preset\n" +
                "from disk and the dropdown.\n\n" +
                "Only works on user-saved presets.\n" +
                "Built-ins are embedded in the EXE\n" +
                "and can't be deleted.");

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
            int imgHeight = (int)(imgWidth / 2.0f); // slightly wider ratio to save height
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
            _picBindingsController.MouseDown += PicBindingsController_MouseDown;
            _picBindingsController.MouseUp += PicBindingsController_MouseUp;
            container.Controls.Add(_picBindingsController);

            // Active dot layout starts as the Touch defaults (plus any saved calibration)
            ApplyControllerModel("touch");

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

            // Controller model switcher + dot calibration, under the photo
            BuildControllerSwitcherRow(container, splitX + 10, sideY + imgHeight + 4, imgWidth);

            // Status label below both columns
            y = sideY + imgHeight + 34;

            _lblKbStatus = MakeLabel("", leftMargin, y, 800);
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
            container.Controls.Add(_lblKbStatus);
            y += 24;

            // Auto-size panel to fit content
            container.Size = new Size(container.Width, y + 10);
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
            return FindActionsForHexInContext(contextName, fieldIndex, hexValue).FirstOrDefault();
        }

        private List<string> FindActionsForHexInContext(string contextName, int fieldIndex, string hexValue)
        {
            var matches = new List<string>();
            if (!_contextBindings.TryGetValue(contextName, out var actions)) return matches;
            string wanted = NormalizeControllerHex(hexValue);
            foreach (var fields in actions)
            {
                if (fields.Length <= fieldIndex) continue;
                string val = fields[fieldIndex].Trim().ToLowerInvariant();
                // Handle comma-separated multi-bindings like "0x0001,0x0002"
                var parts = val.Split(',');
                foreach (var part in parts)
                {
                    if (NormalizeControllerHex(part) == wanted)
                    {
                        matches.Add(fields[0]);
                        break;
                    }
                }
            }
            return matches;
        }

        private static string NormalizeControllerHex(string value)
        {
            string v = value.Trim().ToLowerInvariant();
            if (v.StartsWith("0x") && int.TryParse(v[2..], System.Globalization.NumberStyles.HexNumber, null, out int parsed))
                return $"0x{parsed:x}";
            return v;
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
                RemoveSyntheticControllerActionItems(cmb);
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

        private void SelectActionsInCombo(ComboBox cmb, IReadOnlyList<string> actionNames)
        {
            _suppressCtrlActionChange = true;
            try
            {
                RemoveSyntheticControllerActionItems(cmb);
                if (actionNames.Count == 0)
                {
                    cmb.SelectedIndex = 0;
                    return;
                }
                if (actionNames.Count == 1)
                {
                    string actionName = actionNames[0];
                    for (int i = 0; i < cmb.Items.Count; i++)
                    {
                        if (cmb.Items[i]?.ToString() == actionName)
                        {
                            cmb.SelectedIndex = i;
                            return;
                        }
                    }
                    cmb.SelectedIndex = 0;
                    return;
                }

                string synthetic = "Multiple: " + string.Join(" + ", actionNames);
                cmb.Items.Insert(1, synthetic);
                cmb.SelectedIndex = 1;
            }
            finally { _suppressCtrlActionChange = false; }
        }

        private static void RemoveSyntheticControllerActionItems(ComboBox cmb)
        {
            for (int i = cmb.Items.Count - 1; i >= 0; i--)
            {
                if (cmb.Items[i]?.ToString()?.StartsWith("Multiple: ", StringComparison.Ordinal) == true)
                    cmb.Items.RemoveAt(i);
            }
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
            var img = ActiveControllerImage;
            float imgAspect = img != null ? (float)img.Width / img.Height : 1.6f;
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
            foreach (var kvp in _activeControllerButtons)
            {
                if (!IsControllerButtonVisible(kvp.Key))
                    continue;

                float hitRadius = HitRadiusFor(kvp.Key, kvp.Value.isStickDir);
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

        private bool ThumbrestBindingEnabled()
        {
            return _chkDisableThumbrestTouch != null && !_chkDisableThumbrestTouch.Checked;
        }

        private static bool IsThumbrestButton(string? buttonId)
        {
            return buttonId == "l_thumbrest" || buttonId == "r_thumbrest";
        }

        private bool IsControllerButtonVisible(string buttonId)
        {
            return !IsThumbrestButton(buttonId) || ThumbrestBindingEnabled();
        }

        private void PicBindingsController_MouseMove(object? sender, MouseEventArgs e)
        {
            if (HandleDotDragMove(e)) return;
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

            if (_chkMoveDots != null && _chkMoveDots.Checked)
                return; // calibration mode: clicks are for dragging dots, not binding

            string? hit = HitTestControllerButton(fx, fy);
            if (hit != null && _activeControllerButtons.TryGetValue(hit, out var info))
            {
                _selectedCtrlButton = hit;
                RefreshSelectedControllerBinding(updateStatus: true);
                if (_selectedCtrlButton != null)
                    return;
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
            RefreshSelectedControllerBinding(updateStatus: true);
        }

        private string? FindSelectedControllerAction()
        {
            return FindSelectedControllerActions().FirstOrDefault();
        }

        private List<string> FindSelectedControllerActions()
        {
            var actions = new List<string>();
            if (_selectedCtrlButton == null) return actions;
            if (!ControllerButtonHex.TryGetValue(_selectedCtrlButton, out var hex)) return actions;
            string? ctx = GetSelectedContextName(_cmbCtrlType);
            if (ctx == null) return actions;

            if (!string.IsNullOrEmpty(hex.hexRight))
                actions.AddRange(FindActionsForHexInContext(ctx, 6, hex.hexRight));
            if (!string.IsNullOrEmpty(hex.hexLeft))
                actions.AddRange(FindActionsForHexInContext(ctx, 7, hex.hexLeft));
            return actions.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        }

        private void RefreshSelectedControllerBinding(bool updateStatus)
        {
            if (_selectedCtrlButton == null) return;
            if (!_activeControllerButtons.TryGetValue(_selectedCtrlButton, out var info)) return;

            _lblCtrlButton.Text = info.display;

            if (IsTrackpadButton(_selectedCtrlButton))
            {
                // Index trackpad: behavior is fixed by OCU's input translation,
                // not by controlmap hex, so show what it does instead of a combo.
                _cmbCtrlAction.Enabled = false;
                SelectActionInCombo(_cmbCtrlAction, null);
                if (updateStatus)
                {
                    _lblKbStatus.Text = $"{info.display}: click = A Button (lower half) or B/Menu (upper half). With VRIK Knuckles support enabled it feeds VRIK gestures instead. Not separately remappable.";
                    _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
                }
                _picBindingsController.Invalidate();
                return;
            }

            if (info.isStickDir)
            {
                _cmbCtrlAction.Enabled = false;
                SelectActionInCombo(_cmbCtrlAction, null);

                if (updateStatus)
                {
                    string dirAction = _selectedCtrlButton switch
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
                    _lblKbStatus.Text = $"{info.display}: {dirAction} (VR axis - not remappable here)";
                    _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
                }

                _picBindingsController.Invalidate();
                return;
            }

            _cmbCtrlAction.Enabled = true;
            var boundActions = FindSelectedControllerActions();
            SelectActionsInCombo(_cmbCtrlAction, boundActions);

            if (updateStatus)
            {
                _lblKbStatus.Text = boundActions.Count > 0
                    ? $"{info.display}: {string.Join(" + ", boundActions)}"
                    : $"{info.display}: Not Used";
                _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
            }

            _picBindingsController.Invalidate();
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
            if (newAction.StartsWith("Multiple: ", StringComparison.Ordinal))
                return;
            bool isNone = newAction == "(none)";

            // Determine which fields this button affects
            bool hasRight = !string.IsNullOrEmpty(hex.hexRight);
            bool hasLeft = !string.IsNullOrEmpty(hex.hexLeft);

            // Write every VR device column pair so bindings apply no matter what
            // controller type the runtime reports. Skyrim reads: Vive 4/5,
            // Oculus 6/7, WMR 8/9 (0-indexed). Index knuckles report "knuckles"
            // and fall into the Vive columns, which is why Oculus-only writes
            // silently did nothing for Index and Vive users.
            int[] rightVrFields = { 4, 6, 8 };
            int[] leftVrFields = { 5, 7, 9 };

            // Clear old action that had this button's hex code
            foreach (var fields in actions)
            {
                if (fields.Length <= 9) continue;
                string actionName = fields[0];
                if (hasRight)
                {
                    foreach (int f in rightVrFields)
                    {
                        if (TryRemoveControllerHex(fields[f], hex.hexRight, out var rightValue))
                        {
                            fields[f] = rightValue;
                            RecordControllerChange(ctx, actionName, f, rightValue);
                        }
                    }
                }
                if (hasLeft)
                {
                    foreach (int f in leftVrFields)
                    {
                        if (TryRemoveControllerHex(fields[f], hex.hexLeft, out var leftValue))
                        {
                            fields[f] = leftValue;
                            RecordControllerChange(ctx, actionName, f, leftValue);
                        }
                    }
                }
            }

            // Assign hex code to the new action
            if (!isNone)
            {
                foreach (var fields in actions)
                {
                    if (fields.Length <= 9) continue;
                    if (fields[0] != newAction) continue;
                    if (hasRight)
                    {
                        foreach (int f in rightVrFields)
                        {
                            fields[f] = hex.hexRight;
                            RecordControllerChange(ctx, newAction, f, hex.hexRight);
                        }
                    }
                    if (hasLeft)
                    {
                        foreach (int f in leftVrFields)
                        {
                            fields[f] = hex.hexLeft;
                            RecordControllerChange(ctx, newAction, f, hex.hexLeft);
                        }
                    }
                    break;
                }
            }

            var btnInfo = _activeControllerButtons[_selectedCtrlButton];
            _lblKbStatus.Text = isNone
                ? $"{btnInfo.display}: Unbound (unsaved)"
                : $"{btnInfo.display} → {newAction} (unsaved)";
            _lblKbStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private static bool TryRemoveControllerHex(string currentValue, string hexValue, out string newValue)
        {
            var remaining = currentValue
                .Split(',')
                .Select(part => part.Trim())
                .Where(part => part.Length > 0 && NormalizeControllerHex(part) != NormalizeControllerHex(hexValue))
                .ToList();

            newValue = remaining.Count == 0 ? "0xff" : string.Join(",", remaining);
            return newValue != currentValue.Trim();
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

            foreach (var kvp in _activeControllerButtons)
            {
                if (!IsControllerButtonVisible(kvp.Key))
                    continue;

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
                    // Regular buttons: circles (knuckles face dots are half
                    // size; grips and triggers stay full size on all models)
                    float r = DotRadiusFor(kvp.Key);

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
                        g.DrawString(kvp.Value.display, font, textBrush, cx, cy + r + 2, sf);
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
            LoadControlmapTemplateModel(resetContextNames: false);
            _controllerChanges.Clear();

            string filePath = GetExistingControlmapPath();
            if (string.IsNullOrEmpty(filePath))
            {
                UpdateAllKeyColors();
                RefreshSelectedControllerBinding(updateStatus: false);
                return;
            }

            try
            {
                LoadControlmapModelFromText(File.ReadAllText(filePath), resetContextNames: false);
                RefreshKeyboardBindingsFromControlmapModel();

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
                RefreshSelectedControllerBinding(updateStatus: false);
                _lblKbStatus.Text = "Loaded existing controlmapvr.txt bindings";
                _lblKbStatus.ForeColor = Color.FromArgb(100, 180, 255);
            }
            catch
            {
                // Silently ignore load errors - just use defaults
                RefreshSelectedControllerBinding(updateStatus: false);
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
            if (!EnsureValidInstallForSave())
                return;

            var result = MessageBox.Show(
                "This will reset the Bindings page to the VR Safe Defaults.\n\nKeyboard conflicts are removed and mouse bindings are unbound.\n\nProceed?",
                "Bindings Master Reset",
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

            _lblKbStatus.Text = "Bindings reset to VR Safe Defaults. Restart the game to apply.";
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        private void BtnVRIKDefaults_Click(object? sender, EventArgs e)
        {
            var result = MessageBox.Show(
                "This will restore all bindings to the VRIK Rift-Index-WMR Controller Bindings V2.1.0 preset.\n\nThis is the recommended binding scheme for VRIK users.\n\nAre you sure?",
                "Restore VRIK V2.1.0 Bindings",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question);

            if (result != DialogResult.Yes) return;

            string filePath = GetControlmapSavePath();

            var asm = Assembly.GetExecutingAssembly();
            using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_vrik.txt");
            if (stream == null)
            {
                MessageBox.Show("Embedded VRIK defaults not found!", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            using var reader = new StreamReader(stream);
            File.WriteAllText(filePath, reader.ReadToEnd());

            _keyBindings.Clear();
            LoadDefaultKeyBindings();
            TryLoadControlmapVR();

            _lblKbStatus.Text = "VRIK V2.1.0 bindings restored! Restart the game to apply.";
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        // Maps the dropdown's display name to the embedded preset resource.
        // Adding a new entry here + bundling a new controlmapvr_<name>.txt as an
        // embedded resource is all it takes to ship a new preset.
        private static readonly Dictionary<string, string> BindingPresetResources = new()
        {
            { "Vanilla", "OpenCompositeConfigurator.controlmapvr_template.txt" },
            { "VR Safe", "OpenCompositeConfigurator.controlmapvr_vrsafe.txt" },
            { "VRIK V2.1.0", "OpenCompositeConfigurator.controlmapvr_vrik.txt" },
            { "Snippy", "OpenCompositeConfigurator.controlmapvr_snippy.txt" },
            { "Kvite", "OpenCompositeConfigurator.controlmapvr_kvite.txt" },
            { "Cangar", "OpenCompositeConfigurator.controlmapvr_cangar.txt" },
            { "Cangar Spellsiphon", "OpenCompositeConfigurator.controlmapvr_cangar_spellsiphon.txt" },
            { "Vanilla + Oculus Touch Hotkeys", "OpenCompositeConfigurator.controlmapvr_oculus_optimized.txt" },
            { "Peak Combat", "OpenCompositeConfigurator.controlmapvr_peakcombat.txt" },
            { "Wondernutts", "OpenCompositeConfigurator.controlmapvr_wondernutts.txt" },
        };

        private void CmbBindingPreset_SelectedIndexChanged(object? sender, EventArgs e)
        {
            UpdateDeletePresetEnabled();
            PreviewSelectedBindingPreset();
        }

        private void PreviewSelectedBindingPreset()
        {
            string presetName = _cmbBindingPreset.SelectedItem?.ToString() ?? "";
            if (string.IsNullOrEmpty(presetName)) return;
            if (!TryResolveBindingPresetSource(presetName, out var resourceName, out var externalPath, out _)) return;
            if (!TryBuildControllerPresetMergedText(resourceName, externalPath, out var presetText, out _)) return;

            LoadControlmapModelFromText(presetText, resetContextNames: false);
            RefreshSelectedControllerBinding(updateStatus: true);
            _picBindingsController?.Invalidate();

            if (_selectedCtrlButton != null)
            {
                _lblKbStatus.Text += $" ({presetName} preview; click Apply Preset to write)";
                _lblKbStatus.ForeColor = Color.FromArgb(255, 200, 100);
            }
        }

        private bool TryReadBindingPresetText(string presetName, out string presetText, out string error)
        {
            presetText = "";
            error = "";

            if (BindingPresetResources.TryGetValue(presetName, out var resourceName))
            {
                var asm = Assembly.GetExecutingAssembly();
                using var stream = asm.GetManifestResourceStream(resourceName);
                if (stream == null)
                {
                    error = $"Embedded preset '{resourceName}' not found";
                    return false;
                }
                presetText = new StreamReader(stream).ReadToEnd();
                return true;
            }

            if (_userBindingPresets.TryGetValue(presetName, out var path))
            {
                if (!File.Exists(path))
                {
                    error = $"User preset file not found: {path}";
                    return false;
                }
                presetText = File.ReadAllText(path);
                return true;
            }

            error = $"Unknown preset '{presetName}'";
            return false;
        }

        private bool TryResolveBindingPresetSource(string presetName, out string? resourceName, out string? externalPath, out string error)
        {
            resourceName = null;
            externalPath = null;
            error = "";

            if (BindingPresetResources.TryGetValue(presetName, out resourceName))
                return true;

            if (_userBindingPresets.TryGetValue(presetName, out externalPath))
                return true;

            error = $"Unknown preset '{presetName}'";
            return false;
        }

        private bool TryBuildControllerPresetMergedText(string? presetResourceName, string? externalPresetPath, out string outputText, out string error)
        {
            outputText = "";
            error = "";

            static string BindingKey(string context, string eventName) => $"{context}\u001f{eventName}";

            string savePath = GetControlmapSavePath();
            var userBindings = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
            if (File.Exists(savePath))
            {
                string currentContext = "";
                foreach (string rawLine in File.ReadAllLines(savePath))
                {
                    string line = rawLine.TrimEnd('\r').TrimEnd();
                    if (string.IsNullOrWhiteSpace(line)) { currentContext = ""; continue; }
                    if (TryReadControlmapContextHeader(line, out var parsedContext))
                    {
                        currentContext = parsedContext;
                        continue;
                    }
                    if (line.TrimStart().StartsWith("//")) continue;

                    var fields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                    for (int i = 0; i < fields.Length; i++) fields[i] = fields[i].Trim();
                    if (fields.Length < 4) continue;

                    userBindings[BindingKey(currentContext, fields[0])] = fields;
                }
            }

            string presetText;
            if (!string.IsNullOrEmpty(externalPresetPath))
            {
                if (!File.Exists(externalPresetPath))
                {
                    error = $"User preset file not found: {externalPresetPath}";
                    return false;
                }
                presetText = File.ReadAllText(externalPresetPath);
            }
            else
            {
                if (string.IsNullOrEmpty(presetResourceName))
                {
                    error = "No preset source provided";
                    return false;
                }
                var asm = Assembly.GetExecutingAssembly();
                using var presetStream = asm.GetManifestResourceStream(presetResourceName);
                if (presetStream == null)
                {
                    error = $"Embedded preset '{presetResourceName}' not found";
                    return false;
                }
                presetText = new StreamReader(presetStream).ReadToEnd();
            }

            var output = new StringBuilder();
            string presetContext = "";
            foreach (string rawLine in presetText.Split('\n'))
            {
                string line = rawLine.TrimEnd('\r');
                if (string.IsNullOrWhiteSpace(line))
                {
                    presetContext = "";
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                if (TryReadControlmapContextHeader(line, out var parsedContext))
                {
                    presetContext = parsedContext;
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                if (line.TrimStart().StartsWith("//"))
                {
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                var presetFields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                for (int i = 0; i < presetFields.Length; i++) presetFields[i] = presetFields[i].Trim();
                if (presetFields.Length < 4)
                {
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                string eventName = presetFields[0];
                if (presetContext == "Favor" && eventName == "Activate")
                    continue;

                if (userBindings.TryGetValue(BindingKey(presetContext, eventName), out var userFields))
                {
                    if (userFields.Length > 1) presetFields[1] = userFields[1];
                    if (userFields.Length > 2) presetFields[2] = userFields[2];
                    if (userFields.Length > 3) presetFields[3] = userFields[3];
                    if (presetFields.Length > 10 && userFields.Length > 10) presetFields[10] = userFields[10];
                    if (presetFields.Length > 11 && userFields.Length > 11) presetFields[11] = userFields[11];
                    if (presetFields.Length > 12 && userFields.Length > 12) presetFields[12] = userFields[12];
                }

                if (presetFields.Length > 2 && presetFields[2].Contains('!'))
                    presetFields[2] = "0xff";

                output.Append(string.Join('\t', presetFields));
                output.Append('\n');
            }

            outputText = output.ToString()
                .Replace("\r\n", "\n")
                .Replace('\r', '\n')
                .TrimEnd('\n')
                .Replace("\n", "\r\n") + "\r\n";
            return true;
        }

        private void BtnApplyBindingPreset_Click(object? sender, EventArgs e)
        {
            string presetName = _cmbBindingPreset.SelectedItem?.ToString() ?? "VRIK V2.1.0";

            // Resolve preset source: built-in embedded resource, or a user-saved
            // file under %AppData%\OpenCompositeConfigurator\Presets\.
            string? resourceName = null;
            string? externalPath = null;
            if (BindingPresetResources.TryGetValue(presetName, out resourceName))
            {
                // built-in
            }
            else if (_userBindingPresets.TryGetValue(presetName, out var path))
            {
                externalPath = path;
            }
            else
            {
                MessageBox.Show($"Unknown preset '{presetName}'", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            var result = MessageBox.Show(
                $"Apply '{presetName}' controller bindings?\n\nYour KEYBOARD bindings will be preserved — only controller (VR) fields are replaced from the preset.\n\nUse the \"Reset to Game Defaults\" button if you want a full wholesale reset including keyboard.\n\nProceed?",
                $"Apply {presetName} Preset",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question);

            if (result != DialogResult.Yes) return;

            if (!ApplyControllerPresetMergingKeyboard(resourceName, externalPath, out string error))
            {
                MessageBox.Show($"Failed to apply preset: {error}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Persist the active preset choice in opencomposite.ini so the dropdown
            // restores to the right value next launch.
            _ini.Set("Configurator", "activeBindingPreset", presetName);
            _ini.Save();

            var (bindingRepairs, _, _) = ValidateAndRepairControlmap();

            // Refresh in-memory state from the just-written controlmapvr.txt
            _keyBindings.Clear();
            LoadDefaultKeyBindings();
            TryLoadControlmapVR();

            string repairMsg = bindingRepairs > 0 ? $" + {bindingRepairs} validation repair(s)" : "";
            _lblKbStatus.Text = $"{presetName} controller bindings applied{repairMsg} (keyboard preserved). Restart the game to apply.";
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
            if (_selectedCtrlButton != null)
            {
                RefreshSelectedControllerBinding(updateStatus: true);
                _lblKbStatus.Text += $" ({presetName} applied)";
            }
        }

        // Merge the chosen preset's controller fields with the user's existing
        // keyboard / mouse / gamepad fields, line-by-line keyed on context + event.
        //
        // Field layout (1-indexed per controlmap.txt header / 0-indexed in array):
        //   [0]  event name
        //   [1]  keyboard          ← preserved from user
        //   [2]  mouse             ← preserved from user
        //   [3]  gamepad           ← preserved from user
        //   [4]  Vive primary      ← from preset
        //   [5]  Vive secondary    ← from preset
        //   [6]  Oculus right      ← from preset
        //   [7]  Oculus left       ← from preset
        //   [8]  WMR primary       ← from preset
        //   [9]  WMR secondary     ← from preset
        //   [10] remap-keyboard    ← preserved from user
        //   [11] remap-mouse       ← preserved from user
        //   [12] remap-gamepad     ← preserved from user
        //   [13..18] remap-VR      ← from preset
        //   [19] optional binary flag
        //
        // Comments and blank lines pass through verbatim so context headers
        // stay intact (Skyrim's parser uses blank lines to delimit input contexts).
        private bool ApplyControllerPresetMergingKeyboard(string? presetResourceName, string? externalPresetPath, out string error)
        {
            error = "";

            static string BindingKey(string context, string eventName) => $"{context}\u001f{eventName}";

            static bool TryReadContextHeader(string line, out string context)
            {
                context = "";
                if (!line.TrimStart().StartsWith("//"))
                    return false;

                string comment = line.TrimStart().TrimStart('/').Trim();
                int tabIdx = comment.IndexOf('\t');
                if (tabIdx >= 0) comment = comment[..tabIdx].Trim();

                bool isContext = comment.Length > 0 && !comment.StartsWith("1st") && !comment.StartsWith("2nd") &&
                                 !comment.StartsWith("3rd") && !comment.StartsWith("4th") && !comment.StartsWith("5th") &&
                                 !comment.StartsWith("6th") && !comment.StartsWith("7th") && !comment.StartsWith("8th") &&
                                 !comment.StartsWith("9th") && !comment.StartsWith("10th") && !comment.StartsWith("11th") &&
                                 !comment.StartsWith("12th") && !comment.StartsWith("13th") && !comment.StartsWith("14th") &&
                                 !comment.StartsWith("15th") && !comment.StartsWith("16th") && !comment.StartsWith("17th") &&
                                 !comment.StartsWith("18th") && !comment.StartsWith("19th") && !comment.StartsWith("20th") &&
                                 !comment.StartsWith("Blank") && !comment.StartsWith("See") &&
                                 !comment.StartsWith("(Vive") && !comment.StartsWith("(Oculus") && !comment.StartsWith("(Windows") &&
                                 !comment.StartsWith("\"") && !comment.StartsWith("If ");
                if (!isContext)
                    return false;

                context = comment;
                return true;
            }

            // Step 1: harvest user's current keyboard / mouse / gamepad / their remap flags,
            // keyed by context + event from the live controlmapvr.txt. If the live file doesn't
            // exist we fall through with an empty dictionary, and the preset's own
            // keyboard fields end up applied (no merge target).
            string savePath = GetControlmapSavePath();
            var userBindings = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
            if (File.Exists(savePath))
            {
                string currentContext = "";
                foreach (string rawLine in File.ReadAllLines(savePath))
                {
                    string line = rawLine.TrimEnd('\r').TrimEnd();
                    if (string.IsNullOrWhiteSpace(line)) { currentContext = ""; continue; }
                    if (TryReadContextHeader(line, out var parsedContext))
                    {
                        currentContext = parsedContext;
                        continue;
                    }
                    if (line.TrimStart().StartsWith("//")) continue;

                    var fields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                    for (int i = 0; i < fields.Length; i++) fields[i] = fields[i].Trim();
                    if (fields.Length < 4) continue;

                    userBindings[BindingKey(currentContext, fields[0])] = fields;
                }
            }

            // Step 2: read the preset — embedded resource for built-ins, or external file
            // for user-saved presets stored under %AppData%\OpenCompositeConfigurator\Presets\.
            string presetText;
            if (!string.IsNullOrEmpty(externalPresetPath))
            {
                if (!File.Exists(externalPresetPath))
                {
                    error = $"User preset file not found: {externalPresetPath}";
                    return false;
                }
                presetText = File.ReadAllText(externalPresetPath);
            }
            else
            {
                if (string.IsNullOrEmpty(presetResourceName))
                {
                    error = "No preset source provided";
                    return false;
                }
                var asm = Assembly.GetExecutingAssembly();
                using var presetStream = asm.GetManifestResourceStream(presetResourceName);
                if (presetStream == null)
                {
                    error = $"Embedded preset '{presetResourceName}' not found";
                    return false;
                }
                presetText = new StreamReader(presetStream).ReadToEnd();
            }

            // Step 3: walk preset, merge keyboard fields where context + event match,
            // emit. Keep blank lines and comments verbatim.
            var output = new StringBuilder();
            string presetContext = "";
            foreach (string rawLine in presetText.Split('\n'))
            {
                string line = rawLine.TrimEnd('\r');
                if (string.IsNullOrWhiteSpace(line))
                {
                    presetContext = "";
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                if (TryReadContextHeader(line, out var parsedContext))
                {
                    presetContext = parsedContext;
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                if (line.TrimStart().StartsWith("//"))
                {
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                var presetFields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                for (int i = 0; i < presetFields.Length; i++) presetFields[i] = presetFields[i].Trim();
                if (presetFields.Length < 4)
                {
                    output.Append(line);
                    output.Append('\n');
                    continue;
                }

                string eventName = presetFields[0];

                // The Favor context in Skyrim VR does not accept an Activate row.
                // Some experimental presets included one, and that can make the
                // engine reject the controlmap before SKSE/SkyUI ask for mappings.
                if (presetContext == "Favor" && eventName == "Activate")
                    continue;

                if (userBindings.TryGetValue(BindingKey(presetContext, eventName), out var userFields))
                {
                    // Keyboard / mouse / gamepad
                    if (userFields.Length > 1) presetFields[1] = userFields[1];
                    if (userFields.Length > 2) presetFields[2] = userFields[2];
                    if (userFields.Length > 3) presetFields[3] = userFields[3];

                    // Their respective remap flags (11th-13th, 0-indexed [10..12])
                    if (presetFields.Length > 10 && userFields.Length > 10) presetFields[10] = userFields[10];
                    if (presetFields.Length > 11 && userFields.Length > 11) presetFields[11] = userFields[11];
                    if (presetFields.Length > 12 && userFields.Length > 12) presetFields[12] = userFields[12];
                }

                // Skyrim VR's mouse column expects direct mouse IDs. Symbolic aliases
                // such as !0,Tween Menu can break menu-context initialization and
                // crash SKSE's GetMappedKey when SkyUI resolves controls.
                if (presetFields.Length > 2 && presetFields[2].Contains('!'))
                    presetFields[2] = "0xff";

                output.Append(string.Join('\t', presetFields));
                output.Append('\n');
            }

            try
            {
                string outputText = output.ToString()
                    .Replace("\r\n", "\n")
                    .Replace('\r', '\n')
                    .TrimEnd('\n')
                    .Replace("\n", "\r\n") + "\r\n";
                File.WriteAllText(savePath, outputText);
            }
            catch (Exception ex)
            {
                error = $"Could not write {savePath}: {ex.Message}";
                return false;
            }

            return true;
        }

        // Walks %AppData%\OpenCompositeConfigurator\Presets\ for user-saved
        // controlmapvr.txt files and adds them to the dropdown. File stem is the
        // preset name (e.g. "MyCustom.txt" appears as "MyCustom"). Built-ins
        // always sort first; user presets follow.
        private void LoadUserBindingPresets()
        {
            _userBindingPresets.Clear();
            string dir = GetUserPresetsDir();
            foreach (string path in Directory.GetFiles(dir, "*.txt"))
            {
                string name = Path.GetFileNameWithoutExtension(path);
                if (string.IsNullOrEmpty(name)) continue;
                if (BindingPresetResources.ContainsKey(name)) continue; // don't shadow built-ins
                _userBindingPresets[name] = path;
                if (!_cmbBindingPreset.Items.Contains(name))
                    _cmbBindingPreset.Items.Add(name);
            }
        }

        private void BtnSaveAsBindingPreset_Click(object? sender, EventArgs e)
        {
            // Source: the live controlmapvr.txt the game would load right now. If the
            // user has unsaved edits in the UI, they should click "Save Bindings" first
            // to commit them to disk; Save As is for capturing the disk state as a
            // reusable preset.
            string livePath = GetControlmapSavePath();
            if (!File.Exists(livePath))
            {
                MessageBox.Show(
                    "No live controlmapvr.txt to save yet. Apply a preset (or click Save Bindings) first, then come back to capture it as a named preset.",
                    "Nothing to save",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            // Default name: "<active preset> Custom" if a preset is selected, else "Custom Preset".
            string activePreset = _cmbBindingPreset.SelectedItem?.ToString() ?? "Custom";
            string defaultName = $"{activePreset} Custom";

            string presetName = PromptForString(
                "Name this preset (will appear in the dropdown):",
                "Save As Preset",
                defaultName);
            if (string.IsNullOrWhiteSpace(presetName)) return;

            foreach (char c in Path.GetInvalidFileNameChars())
                presetName = presetName.Replace(c, '_');

            if (BindingPresetResources.ContainsKey(presetName))
            {
                MessageBox.Show($"'{presetName}' is a built-in preset name. Pick a different one.",
                    "Name conflict", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            string destPath = Path.Combine(GetUserPresetsDir(), presetName + ".txt");
            if (File.Exists(destPath))
            {
                var ow = MessageBox.Show($"A user preset named '{presetName}' already exists. Overwrite?",
                    "Confirm overwrite", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                if (ow != DialogResult.Yes) return;
            }

            try
            {
                File.Copy(livePath, destPath, overwrite: true);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Could not save preset: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            _userBindingPresets[presetName] = destPath;
            if (!_cmbBindingPreset.Items.Contains(presetName))
                _cmbBindingPreset.Items.Add(presetName);
            _cmbBindingPreset.SelectedItem = presetName;

            _lblKbStatus.Text = $"Saved preset '{presetName}'. It will appear in the dropdown next session too.";
            _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        // Delete stays enabled regardless of selection so its text remains white.
        // The click handler shows a friendly "built-in can't be deleted" message
        // when the user clicks while a built-in is selected.
        private void UpdateDeletePresetEnabled()
        {
            // Intentional no-op — kept as a hook in case we want to revisit
            // disabled-state rendering later. The button is always enabled.
        }

        private void BtnDeleteBindingPreset_Click(object? sender, EventArgs e)
        {
            string presetName = _cmbBindingPreset.SelectedItem?.ToString() ?? "";
            if (string.IsNullOrEmpty(presetName)) return;

            if (BindingPresetResources.ContainsKey(presetName))
            {
                MessageBox.Show($"'{presetName}' is a built-in preset and can't be deleted.",
                    "Built-in preset", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (!_userBindingPresets.TryGetValue(presetName, out var path))
            {
                MessageBox.Show($"Preset '{presetName}' isn't tracked in user presets — nothing to delete.",
                    "Not found", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            var confirm = MessageBox.Show(
                $"Delete user preset '{presetName}'?\n\nThis removes the file from\n{path}\n\nYour live controlmapvr.txt is not affected.",
                "Confirm delete", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (confirm != DialogResult.Yes) return;

            try
            {
                if (File.Exists(path)) File.Delete(path);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Could not delete the preset file: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            _userBindingPresets.Remove(presetName);
            _cmbBindingPreset.Items.Remove(presetName);
            // After removal, fall back to a sensible selection — VRIK V2.1.0 default.
            int fallback = _cmbBindingPreset.Items.IndexOf("VRIK V2.1.0");
            _cmbBindingPreset.SelectedIndex = fallback >= 0 ? fallback : 0;

            // If the deleted preset was the persisted active one, clear that too so
            // next launch doesn't try to restore a deleted preset.
            string activePersisted = _ini.Get("Configurator", "activeBindingPreset", "");
            if (activePersisted.Equals(presetName, StringComparison.OrdinalIgnoreCase))
            {
                _ini.Set("Configurator", "activeBindingPreset", _cmbBindingPreset.SelectedItem?.ToString() ?? "");
                _ini.Save();
            }

            _lblKbStatus.Text = $"Deleted user preset '{presetName}'.";
            _lblKbStatus.ForeColor = Color.FromArgb(220, 180, 100);
        }

        // Tiny modal text-prompt since WinForms doesn't ship one.
        private static string PromptForString(string prompt, string title, string defaultValue)
        {
            using var f = new Form
            {
                Width = 460,
                Height = 160,
                Text = title,
                StartPosition = FormStartPosition.CenterParent,
                FormBorderStyle = FormBorderStyle.FixedDialog,
                MinimizeBox = false,
                MaximizeBox = false,
                BackColor = Color.FromArgb(30, 30, 35),
                ForeColor = Color.White
            };
            var lbl = new Label { Left = 12, Top = 14, Width = 420, Text = prompt, ForeColor = Color.White };
            var tb = new TextBox { Left = 12, Top = 40, Width = 420, Text = defaultValue, BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
            var ok = new Button { Text = "OK", Left = 268, Top = 78, Width = 75, DialogResult = DialogResult.OK, BackColor = Color.FromArgb(120, 80, 40), ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
            var cancel = new Button { Text = "Cancel", Left = 357, Top = 78, Width = 75, DialogResult = DialogResult.Cancel, BackColor = Color.FromArgb(60, 60, 65), ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
            f.Controls.Add(lbl); f.Controls.Add(tb); f.Controls.Add(ok); f.Controls.Add(cancel);
            f.AcceptButton = ok; f.CancelButton = cancel;
            return f.ShowDialog() == DialogResult.OK ? tb.Text.Trim() : "";
        }

        private void BtnSaveBindings_Click(object? sender, EventArgs e)
        {
            if (!EnsureValidInstallForSave())
                return;

            try
            {
                SaveControlmapVR();
                var (bindingRepairs, _, _) = ValidateAndRepairControlmap();

                // Also save combos to opencomposite.ini (combos live there, not in controlmapvr.txt)
                if (_combos.Count > 0 || _ini.GetAllInSection("combos").Count > 0)
                {
                    SaveCombosToIniFiles();
                }

                string comboMsg = _combos.Count > 0 ? $" + {_combos.Count} combo(s)" : "";
                string repairMsg = bindingRepairs > 0 ? $" + {bindingRepairs} validation repair(s)" : "";
                _lblKbStatus.Text = $"Saved controlmapvr.txt{comboMsg}{repairMsg}! Restart the game to apply binding changes.";
                _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
                _lblComboStatus.Text = _combos.Count > 0 ? "Combos saved" : "";
                _lblComboStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void SaveCombosToIniFiles()
        {
            WriteCombosToIni();

            foreach (string path in GetOpenCompositeIniSavePaths(createDirectories: true))
                _ini.Save(path);
        }

        private void AutoSaveCombos(string statusText)
        {
            try
            {
                SaveCombosToIniFiles();
                _lblComboStatus.Text = statusText;
                _lblComboStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            catch (Exception ex)
            {
                _lblComboStatus.Text = "Combo save failed";
                _lblComboStatus.ForeColor = Color.FromArgb(255, 120, 120);
                MessageBox.Show($"Failed to auto-save combos: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private string GetConfiguratorDir()
        {
            return Path.GetDirectoryName(Application.ExecutablePath) ?? "";
        }

        private bool IsInstalledModFolderValid()
        {
            string exeDir = GetConfiguratorDir();
            return Directory.Exists(Path.Combine(exeDir, "root")) &&
                   Directory.Exists(Path.Combine(exeDir, "interface"));
        }

        private string GetInvalidInstallMessage()
        {
            return "OCU Configurator is not running from the OCU mod folder.\n\n" +
                   "Move it back beside the 'root' and 'interface' folders, then create a desktop shortcut to the EXE. " +
                   "Do not copy the EXE to your desktop or another folder.";
        }

        private bool EnsureValidInstallForSave()
        {
            if (IsInstalledModFolderValid())
                return true;

            string message = GetInvalidInstallMessage();
            _lblStatus.Text = "Save blocked - Configurator is outside the OCU mod folder.";
            _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
            _lblVideoStatus.Text = _lblStatus.Text;
            _lblVideoStatus.ForeColor = _lblStatus.ForeColor;
            MessageBox.Show(message, "Invalid Configurator Location",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }

        private string GetInstalledModContentDir()
        {
            return GetConfiguratorDir();
        }

        private IEnumerable<string> GetControlmapCandidatePaths(bool createPrimary)
        {
            if (!IsInstalledModFolderValid())
                yield break;

            string modControlsPath = Path.Combine(GetInstalledModContentDir(), "interface", "controls", "pc");
            if (createPrimary)
                Directory.CreateDirectory(modControlsPath);
            yield return Path.Combine(modControlsPath, "controlmapvr.txt");
        }

        private string GetExistingControlmapPath()
        {
            foreach (string path in GetControlmapCandidatePaths(createPrimary: false))
            {
                if (File.Exists(path))
                    return path;
            }

            return "";
        }

        private string GetControlmapSavePath()
        {
            if (!IsInstalledModFolderValid())
                throw new InvalidOperationException(GetInvalidInstallMessage());

            string controlsPath = Path.Combine(GetInstalledModContentDir(), "interface", "controls", "pc");
            Directory.CreateDirectory(controlsPath);
            return Path.Combine(controlsPath, "controlmapvr.txt");
        }

        private string GetInstalledRootDir()
        {
            string localRoot = Path.Combine(GetConfiguratorDir(), "root");
            if (IsInstalledModFolderValid())
                return localRoot;

            return "";
        }

        private IEnumerable<string> GetOpenCompositeIniSavePaths(bool createDirectories)
        {
            if (!IsInstalledModFolderValid())
                throw new InvalidOperationException(GetInvalidInstallMessage());

            string installedRoot = GetInstalledRootDir();
            if (createDirectories)
                Directory.CreateDirectory(installedRoot);

            return new[] { Path.Combine(installedRoot, "opencomposite.ini") };
        }

        private string GetOpenCompositeIniLoadPath()
        {
            string installedRoot = GetInstalledRootDir();
            if (!string.IsNullOrEmpty(installedRoot))
            {
                string installedPath = Path.Combine(installedRoot, "opencomposite.ini");
                if (File.Exists(installedPath) || string.IsNullOrEmpty(_gameDir))
                    return installedPath;
            }

            return "";
        }

        // menu_quad_settings.ini lives next to opencomposite.ini in the mod root
        // (persistent copy) AND in the game dir (the copy the DLL hot-reloads ~1s
        // while the game runs; Root Builder sweeps it back on exit). Write both so
        // a toggle is live mid-game and survives the exit sweep. Targeted line
        // edits only — the file carries the user's calibration comments.
        private IEnumerable<string> GetMenuQuadIniPaths()
        {
            string root = GetInstalledRootDir();
            if (!string.IsNullOrEmpty(root))
            {
                string p = Path.Combine(root, "menu_quad_settings.ini");
                if (File.Exists(p)) yield return p;
            }
            if (!string.IsNullOrEmpty(_gameDir))
            {
                string p = Path.Combine(_gameDir, "menu_quad_settings.ini");
                if (File.Exists(p)) yield return p;
            }
        }

        private bool GetMenuQuadFlag(string key, bool fallback)
        {
            foreach (string path in GetMenuQuadIniPaths())
            {
                try
                {
                    foreach (string line in File.ReadLines(path))
                    {
                        string t = line.TrimStart();
                        if (t.StartsWith(key + "=", StringComparison.OrdinalIgnoreCase))
                            return t.Substring(key.Length + 1).Trim() != "0";
                    }
                }
                catch { }
            }
            return fallback;
        }

        private void SetMenuQuadFlag(string key, bool on)
        {
            string value = key + "=" + (on ? "1" : "0");
            foreach (string path in GetMenuQuadIniPaths())
            {
                try
                {
                    var lines = File.ReadAllLines(path).ToList();
                    int idx = lines.FindIndex(l => l.TrimStart().StartsWith(key + "=", StringComparison.OrdinalIgnoreCase));
                    if (idx >= 0) lines[idx] = value;
                    else lines.Add(value);
                    File.WriteAllLines(path, lines);
                }
                catch { }
            }
        }

        private string DescribeIniSaveLocations(IReadOnlyCollection<string> savePaths)
        {
            return "installed OCU mod folder";
        }

        private void BtnValidateBindings_Click(object? sender, EventArgs e)
        {
            try
            {
                var (repairs, warnings, message) = ValidateAndRepairControlmap();
                if (repairs > 0)
                {
                    _keyBindings.Clear();
                    LoadDefaultKeyBindings();
                    TryLoadControlmapVR();
                }

                _lblKbStatus.Text = repairs > 0
                    ? $"Validated bindings: {repairs} repair(s). Restart the game to apply."
                    : warnings > 0
                        ? $"Validated bindings: {warnings} warning(s)."
                        : "Validated bindings: no known crash patterns found.";
                _lblKbStatus.ForeColor = repairs > 0
                    ? Color.FromArgb(100, 200, 100)
                    : warnings > 0
                        ? Color.FromArgb(255, 200, 100)
                        : Color.FromArgb(100, 180, 255);

                MessageBox.Show(message, "Binding Validation",
                    MessageBoxButtons.OK,
                    warnings > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Binding validation failed: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private (int repairs, int warnings, string message) ValidateAndRepairControlmap()
        {
            string filePath = GetControlmapSavePath();
            if (!File.Exists(filePath))
                return (0, 1, $"No live controlmapvr.txt was found at:\n{filePath}");

            string[] sourceLines = File.ReadAllLines(filePath);
            var outputLines = new List<string>(sourceLines.Length);
            var repairNotes = new List<string>();
            var warningNotes = new List<string>();
            var seenRows = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            int repairs = 0;
            int mouseAliasRepairs = 0;
            int favorActivateRepairs = 0;
            string currentContext = "";

            foreach (string rawLine in sourceLines)
            {
                string line = rawLine.TrimEnd('\r');
                if (string.IsNullOrWhiteSpace(line))
                {
                    currentContext = "";
                    outputLines.Add(line);
                    continue;
                }

                if (TryReadControlmapContextHeader(line, out var parsedContext))
                {
                    currentContext = parsedContext;
                    outputLines.Add(line);
                    continue;
                }

                if (line.TrimStart().StartsWith("//"))
                {
                    outputLines.Add(line);
                    continue;
                }

                var fields = line.Split('\t', StringSplitOptions.RemoveEmptyEntries);
                for (int i = 0; i < fields.Length; i++) fields[i] = fields[i].Trim();

                if (fields.Length < 4)
                {
                    warningNotes.Add($"Malformed row in '{currentContext}': {TrimForReport(line)}");
                    outputLines.Add(line);
                    continue;
                }

                string actionName = fields[0];
                seenRows.Add($"{currentContext}\u001f{actionName}");

                if (currentContext == "Favor" && actionName == "Activate")
                {
                    favorActivateRepairs++;
                    repairs++;
                    continue;
                }

                if (fields.Length < 8)
                    warningNotes.Add($"Short row in '{currentContext}' for '{actionName}' has only {fields.Length} fields.");

                if (fields.Length > 2 && fields[2].Contains('!'))
                {
                    var (mouseStart, mouseEnd) = FindFieldBounds(line, 2);
                    if (mouseStart < line.Length)
                    {
                        line = line[..mouseStart] + "0xff" + line[mouseEnd..];
                        mouseAliasRepairs++;
                        repairs++;
                    }
                }

                outputLines.Add(line);
            }

            foreach (var (context, action) in CriticalControlmapRows)
            {
                if (!seenRows.Contains($"{context}\u001f{action}"))
                    warningNotes.Add($"Missing critical row: {context} / {action}");
            }

            if (repairs > 0)
            {
                string backupPath = filePath + ".validate-" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + ".bak";
                File.Copy(filePath, backupPath, overwrite: false);
                File.WriteAllLines(filePath, outputLines);

                if (favorActivateRepairs > 0)
                    repairNotes.Add($"Removed {favorActivateRepairs} invalid Favor/Activate row(s).");
                if (mouseAliasRepairs > 0)
                    repairNotes.Add($"Replaced {mouseAliasRepairs} symbolic mouse alias field(s) with 0xff.");
                repairNotes.Add($"Backup written to: {backupPath}");
            }

            var report = new StringBuilder();
            report.AppendLine($"Validated: {filePath}");
            report.AppendLine();

            if (repairNotes.Count > 0)
            {
                report.AppendLine("Repairs:");
                foreach (string note in repairNotes.Take(12))
                    report.AppendLine(" - " + note);
                if (repairNotes.Count > 12)
                    report.AppendLine($" - ...and {repairNotes.Count - 12} more.");
                report.AppendLine();
            }

            if (warningNotes.Count > 0)
            {
                report.AppendLine("Warnings:");
                foreach (string note in warningNotes.Take(12))
                    report.AppendLine(" - " + note);
                if (warningNotes.Count > 12)
                    report.AppendLine($" - ...and {warningNotes.Count - 12} more.");
                report.AppendLine();
            }

            if (repairNotes.Count == 0 && warningNotes.Count == 0)
                report.AppendLine("No known startup-crash binding patterns found.");

            return (repairs, warningNotes.Count, report.ToString());
        }

        private static readonly (string context, string action)[] CriticalControlmapRows =
        {
            ("Main Gameplay", "Activate"),
            ("Main Gameplay", "Ready Weapon"),
            ("Main Gameplay", "Tween Menu"),
            ("Main Gameplay", "Shout"),
            ("Main Gameplay", "Favorites"),
            ("Menu Mode", "Cancel"),
        };

        private static bool TryReadControlmapContextHeader(string line, out string context)
        {
            context = "";
            if (!line.TrimStart().StartsWith("//"))
                return false;

            string comment = line.TrimStart().TrimStart('/').Trim();
            int tabIdx = comment.IndexOf('\t');
            if (tabIdx >= 0) comment = comment[..tabIdx].Trim();

            bool isContext = comment.Length > 0 && !comment.StartsWith("1st") && !comment.StartsWith("2nd") &&
                             !comment.StartsWith("3rd") && !comment.StartsWith("4th") && !comment.StartsWith("5th") &&
                             !comment.StartsWith("6th") && !comment.StartsWith("7th") && !comment.StartsWith("8th") &&
                             !comment.StartsWith("9th") && !comment.StartsWith("10th") && !comment.StartsWith("11th") &&
                             !comment.StartsWith("12th") && !comment.StartsWith("13th") && !comment.StartsWith("14th") &&
                             !comment.StartsWith("15th") && !comment.StartsWith("16th") && !comment.StartsWith("17th") &&
                             !comment.StartsWith("18th") && !comment.StartsWith("19th") && !comment.StartsWith("20th") &&
                             !comment.StartsWith("Blank") && !comment.StartsWith("See") &&
                             !comment.StartsWith("(Vive") && !comment.StartsWith("(Oculus") && !comment.StartsWith("(Windows") &&
                             !comment.StartsWith("\"") && !comment.StartsWith("If ");
            if (!isContext)
                return false;

            context = comment;
            return true;
        }

        private static string TrimForReport(string value)
        {
            value = value.Trim();
            return value.Length <= 96 ? value : value[..96] + "...";
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
            const string deleteControlmapLine = "\u001F_DELETE_CONTROL_MAP_LINE";

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
                if (currentContext == "Favor" && actionName == "Activate")
                {
                    sourceLines[i] = deleteControlmapLine;
                    continue;
                }

                bool isMainGameplay = currentContext == "Main Gameplay";
                bool needsKbChange = isMainGameplay && _keyBindings.ContainsKey(actionName);
                bool needsMouseChange = _chkDisableMouse.Checked;
                bool needsCtrlChange = _controllerChanges.TryGetValue(currentContext, out var ctxChanges)
                                       && ctxChanges.ContainsKey(actionName);

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
                bool needsMouseAliasFix = mouseStart < line.Length && line[mouseStart..mouseEnd].Contains('!');

                if (!needsKbChange && !needsMouseChange && !needsCtrlChange && !needsMouseAliasFix) continue;

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

                // Patch mouse field (all contexts). Symbolic aliases are unsafe in
                // the mouse column even when mouse input itself remains enabled.
                if ((needsMouseChange || needsMouseAliasFix) && mouseStart < line.Length)
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

            var outputLines = sourceLines.Where(line => line != deleteControlmapLine).ToList();
            while (outputLines.Count > 0 && string.IsNullOrWhiteSpace(outputLines[^1]))
                outputLines.RemoveAt(outputLines.Count - 1);

            File.WriteAllLines(filePath, outputLines);
            _controllerChanges.Clear();
        }

        private void ResetControlmapToDefaults()
        {
            if (!EnsureValidInstallForSave())
                return;

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
            var img = ActiveControllerImage;
            float imgAspect = img != null ? (float)img.Width / img.Height : 1.6f;
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

            float hitRadius = ShortcutHitRadius;
            string? closest = null;
            float closestDist = float.MaxValue;

            foreach (var kvp in ShortcutButtonPositions())
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
            float r = ShortcutDotRadius;
            var positions = ShortcutButtonPositions();

            foreach (string btnId in selected)
            {
                if (!positions.TryGetValue(btnId, out var points))
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

            SyncComboEditorModel();
            using var dlg = new ComboEditForm(KeyScancodes);
            if (dlg.ShowDialog(this) == DialogResult.OK && dlg.Result != null)
            {
                _combos.Add(dlg.Result);
                RebuildComboList();
                AutoSaveCombos("Combo added");
            }
        }

        // Harvest the per-context action → keyboard scancode map from the parsed
        // controlmapvr.txt. Skip actions whose keyboard field is 0xff (unbound) since
        // a combo firing an unbound key triggers nothing useful. Multi-keybinds
        // ("0x02,0x4F") use the FIRST scancode.
        private Dictionary<string, List<(string action, int scancode)>> BuildActionsByContext()
        {
            var result = new Dictionary<string, List<(string action, int scancode)>>();
            foreach (var ctx in _contextNames)
            {
                if (!_contextBindings.TryGetValue(ctx, out var actions)) continue;
                var list = new List<(string action, int scancode)>();
                foreach (var fields in actions)
                {
                    if (fields.Length < 2) continue;
                    string actionName = fields[0];
                    string keyboardField = fields[1];
                    string firstHex = keyboardField.Split(',')[0].Trim();
                    if (string.Equals(firstHex, "0xff", StringComparison.OrdinalIgnoreCase)) continue;
                    if (!firstHex.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) continue;
                    if (!int.TryParse(firstHex[2..], System.Globalization.NumberStyles.HexNumber, null, out int sc)) continue;
                    list.Add((actionName, sc));
                }
                if (list.Count > 0) result[ctx] = list;
            }
            return result;
        }

        private void EditCombo(int index)
        {
            if (index < 0 || index >= _combos.Count) return;

            SyncComboEditorModel();
            using var dlg = new ComboEditForm(KeyScancodes, _combos[index]);
            if (dlg.ShowDialog(this) == DialogResult.OK && dlg.Result != null)
            {
                _combos[index] = dlg.Result;
                RebuildComboList();
                AutoSaveCombos("Combo updated");
            }
        }

        private void DeleteCombo(int index)
        {
            if (index < 0 || index >= _combos.Count) return;

            _combos.RemoveAt(index);
            RebuildComboList();
            AutoSaveCombos("Combo removed");
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

        // ═══════════════════════════════════════════════════════════════════════
        // VIDEO TAB
        // ═══════════════════════════════════════════════════════════════════════

        private void BuildVideoTab()
        {
            var container = _tabVideo;
            int y = 10;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 20;
            int halfWidth = (rightEdge - leftMargin) / 2;
            int col2 = leftMargin + halfWidth + 10;

            // ── DLSS 4 SUPER RESOLUTION ──
            Panel dlssAdv = null!;
            var lblDlssSection = MakeSectionLabel("NVIDIA DLSS / DLAA", leftMargin, y);
            container.Controls.Add(lblDlssSection);
            var btnDlssAdv = MakeButton("\u25bc Advanced", rightEdge - 100, y + 2, 94, 22);
            btnDlssAdv.Font = new Font("Segoe UI", 7.5f);
            btnDlssAdv.BackColor = Color.FromArgb(45, 48, 62);
            btnDlssAdv.Click += (s, e) =>
            {
                bool show = !dlssAdv.Visible; dlssAdv.Visible = show;
                if (show) dlssAdv.BringToFront();
                btnDlssAdv.Text = show ? "\u25b2 Advanced" : "\u25bc Advanced";
            };
            container.Controls.Add(btnDlssAdv);
            y += 26;

            _chkDlssEnabled = MakeCheckBox("Enable DLSS Super Resolution", leftMargin, y);
            _chkDlssEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkDlssEnabled.Checked;
                _cmbDlssPreset.Enabled = en;
                _cmbDlssModel.Enabled = en;
                _nudDlssRenderScaleOverride.Enabled = en;
                _nudDlssMvScale.Enabled = en;
                _nudDlssJitterScale.Enabled = en;
                if (en) {
                    _chkFsrEnabled.Checked = false; // mutually exclusive
                    _chkMotionVectorsEnabled.Checked = true;
                }
                CheckPotatoMode();
            };
            container.Controls.Add(_chkDlssEnabled);

            var lblDlssDesc = MakeLabel("NVIDIA NGX upscaling / DLAA. Uses the configured model hint; default is K.",
                leftMargin + 230, y + 3, rightEdge - leftMargin - 250);
            lblDlssDesc.ForeColor = Color.FromArgb(130, 130, 130);
            lblDlssDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblDlssDesc);
            y += 30;

            var lblDlssWarn = MakeLabel("\u26a0  When OCU DLSS or FSR 3 is enabled, Community Shaders upscaling is blocked to prevent upscaler conflicts.",
                leftMargin, y, rightEdge - leftMargin - 10);
            lblDlssWarn.ForeColor = Color.FromArgb(255, 185, 35);
            lblDlssWarn.Font = new Font("Segoe UI", 8f, FontStyle.Bold);
            container.Controls.Add(lblDlssWarn);
            y += 20;

            // Preset ComboBox
            var lblDlssPresetLabel = MakeLabel("DLSS Preset:", leftMargin, y + 3, 85);
            container.Controls.Add(lblDlssPresetLabel);
            _cmbDlssPreset = new ComboBox
            {
                Location = new Point(leftMargin + 90, y), Size = new Size(168, 24),
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(35, 37, 50), ForeColor = Color.White,
                Enabled = false,
            };
            _cmbDlssPreset.Items.AddRange(new object[] {
                "Quality (67%)", "Balanced (58%)", "Performance (50%)", "Ultra Perf (33%)", "DLAA / Native AA", "Ultra Quality (77%)" });
            _cmbDlssPreset.SelectedIndex = 1;
            _cmbDlssPreset.SelectedIndexChanged += (s, e) =>
            {
                // DLSS resolves its render scale from dlssPreset/dlssRenderScaleOverride in OCU.
                // Do not mirror it into the FSR render-scale control; that setting belongs to FSR.
            };
            container.Controls.Add(_cmbDlssPreset);
            y += 30;

            // Quick preset buttons
            var lblDlssPresets = MakeLabel("Quick Presets:", leftMargin, y + 5, 90);
            container.Controls.Add(lblDlssPresets);
            int dlssPx = leftMargin + 94;
            var btnDlssQuality = MakeButton("Quality", dlssPx, y, 75, 26);
            btnDlssQuality.Click += (s, e) => { _chkDlssEnabled.Checked = true; _cmbDlssPreset.SelectedIndex = 0; };
            container.Controls.Add(btnDlssQuality); dlssPx += 79;
            var btnDlssBalanced = MakeButton("Balanced", dlssPx, y, 80, 26);
            btnDlssBalanced.Click += (s, e) => { _chkDlssEnabled.Checked = true; _cmbDlssPreset.SelectedIndex = 1; };
            container.Controls.Add(btnDlssBalanced); dlssPx += 84;
            var btnDlssPerf = MakeButton("Performance", dlssPx, y, 95, 26);
            btnDlssPerf.Click += (s, e) => { _chkDlssEnabled.Checked = true; _cmbDlssPreset.SelectedIndex = 2; };
            container.Controls.Add(btnDlssPerf); dlssPx += 99;
            var btnDlssDlaa = MakeButton("DLAA", dlssPx, y, 60, 26);
            btnDlssDlaa.Click += (s, e) => { _chkDlssEnabled.Checked = true; _cmbDlssPreset.SelectedIndex = 4; };
            container.Controls.Add(btnDlssDlaa); dlssPx += 64;
            var btnDlssOff = MakeButton("Off", dlssPx, y, 55, 26);
            btnDlssOff.BackColor = Color.FromArgb(120, 60, 40);
            btnDlssOff.Click += (s, e) => { _chkDlssEnabled.Checked = false; };
            container.Controls.Add(btnDlssOff);
            y += 34;

            // ── DLSS ADVANCED OVERLAY PANEL ──
            {
                int advW = rightEdge - leftMargin;
                dlssAdv = new Panel
                {
                    Location = new Point(leftMargin, y), Size = new Size(advW, 112),
                    BackColor = Color.FromArgb(26, 28, 40), BorderStyle = BorderStyle.FixedSingle, Visible = false,
                };
                int ap = 4;
                var lblDlssAdvHdr = new Label
                {
                    Text = "\u26a0  Expert settings \u2014 normally not needed",
                    ForeColor = Color.FromArgb(255, 185, 35), Font = new Font("Segoe UI", 8f, FontStyle.Bold),
                    Location = new Point(ap, ap), AutoSize = true,
                };
                dlssAdv.Controls.Add(lblDlssAdvHdr);
                var btnDlssClose = MakeButton("\u00d7", advW - 50, ap - 2, 42, 22);
                btnDlssClose.BackColor = Color.FromArgb(90, 40, 40);
                btnDlssClose.Click += (s, e) => { dlssAdv.Visible = false; btnDlssAdv.Text = "\u25bc Advanced"; };
                dlssAdv.Controls.Add(btnDlssClose);
                ap += 26;

                dlssAdv.Controls.Add(MakeLabel("Model:", 20, ap + 3, 52));
                _cmbDlssModel = new ComboBox
                {
                    Location = new Point(72, ap), Width = 92,
                    DropDownStyle = ComboBoxStyle.DropDownList,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                    Enabled = false
                };
                _cmbDlssModel.Items.AddRange(new object[] { "Default", "J", "K", "L", "M" });
                _cmbDlssModel.SelectedIndex = 2;
                dlssAdv.Controls.Add(_cmbDlssModel);

                dlssAdv.Controls.Add(MakeLabel("Scale override:", 185, ap + 3, 105));
                _nudDlssRenderScaleOverride = new NumericUpDown
                {
                    Location = new Point(292, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.01m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                    Enabled = false
                };
                dlssAdv.Controls.Add(_nudDlssRenderScaleOverride);
                var lblScaleOverrideHint = MakeLabel("0 = preset scale; use only for custom testing.", 370, ap + 3, advW - 386);
                lblScaleOverrideHint.ForeColor = Color.FromArgb(130, 130, 130);
                lblScaleOverrideHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                dlssAdv.Controls.Add(lblScaleOverrideHint);
                ap += 26;

                dlssAdv.Controls.Add(MakeLabel("MV scale:", 20, ap + 3, 65));
                _nudDlssMvScale = new NumericUpDown
                {
                    Location = new Point(88, ap), Width = 65,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 2.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                    Enabled = false
                };
                dlssAdv.Controls.Add(_nudDlssMvScale);

                dlssAdv.Controls.Add(MakeLabel("Jitter:", 170, ap + 3, 48));
                _nudDlssJitterScale = new NumericUpDown
                {
                    Location = new Point(220, ap), Width = 65,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.40m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                    Enabled = false
                };
                dlssAdv.Controls.Add(_nudDlssJitterScale);
                _chkDlssNgxVerboseLogging = MakeCheckBox("NGX verbose logging", 310, ap);
                dlssAdv.Controls.Add(_chkDlssNgxVerboseLogging);
                container.Controls.Add(dlssAdv);
            }

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            // ── FSR SUPER RESOLUTION ──
            Panel fsrAdv = null!;
            var lblFsrSection = MakeSectionLabel("AMD FSR 3 / Native AA", leftMargin, y);
            container.Controls.Add(lblFsrSection);
            var btnFsrAdv = MakeButton("\u25bc Advanced", rightEdge - 100, y + 2, 94, 22);
            btnFsrAdv.Font = new Font("Segoe UI", 7.5f);
            btnFsrAdv.BackColor = Color.FromArgb(45, 48, 62);
            btnFsrAdv.Click += (s, e) =>
            {
                bool show = !fsrAdv.Visible;
                fsrAdv.Visible = show;
                if (show) fsrAdv.BringToFront();
                btnFsrAdv.Text = show ? "\u25b2 Advanced" : "\u25bc Advanced";
            };
            container.Controls.Add(btnFsrAdv);
            y += 26;

            _chkFsrEnabled = MakeCheckBox("Enable FSR 3", leftMargin, y);
            _chkFsrEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkFsrEnabled.Checked;
                _chkFsrNativeAA.Enabled = en;
                _nudFsrRenderScale.Enabled = en && !_chkFsrNativeAA.Checked;
                if (en) { _chkMotionVectorsEnabled.Checked = true; _chkFsr3JitterCancellation.Checked = true;
                    if (_chkDlssEnabled != null) _chkDlssEnabled.Checked = false; } // mutually exclusive
                else { _chkMotionVectorsEnabled.Checked = false; _chkFsr3JitterCancellation.Checked = false; }
                UpdateFsrStatus();
                CheckPotatoMode();
            };
            container.Controls.Add(_chkFsrEnabled);

            _chkFsrNativeAA = MakeCheckBox("Native AA", leftMargin + 128, y);
            _chkFsrNativeAA.Enabled = false;
            _chkFsrNativeAA.CheckedChanged += (s, e) =>
            {
                if (_chkFsrNativeAA.Checked)
                {
                    _chkFsrEnabled.Checked = true;
                    _nudFsrRenderScale.Value = 1.00m;
                }
                _nudFsrRenderScale.Enabled = _chkFsrEnabled.Checked && !_chkFsrNativeAA.Checked;
                UpdateFsrStatus();
            };
            container.Controls.Add(_chkFsrNativeAA);

            // Render Scale — same row as checkbox, to the right
            _lblFsrRenderScale = MakeLabel("Render Scale:", leftMargin + 230, y + 3, 95);
            container.Controls.Add(_lblFsrRenderScale);

            _nudFsrRenderScale = new NumericUpDown
            {
                Location = new Point(leftMargin + 325, y), Width = 70,
                DecimalPlaces = 2, Increment = 0.01m, Minimum = 0.33m, Maximum = 1.00m, Value = 0.67m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
            };
            _nudFsrRenderScale.ValueChanged += (s, e) => UpdateFsrStatus();
            container.Controls.Add(_nudFsrRenderScale);

            var lblScaleHint = MakeLabel("(0.33-1.00, lower = more GPU savings)", leftMargin + 400, y + 3, 220);
            lblScaleHint.ForeColor = Color.FromArgb(110, 110, 110);
            lblScaleHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblScaleHint);
            y += 26;

            // ── FSR ADVANCED OVERLAY PANEL (does not advance y — floats over content below) ──
            {
                int advW = rightEdge - leftMargin;
                fsrAdv = new Panel
                {
                    Location = new Point(leftMargin, y),
                    Size = new Size(advW, 278),
                    BackColor = Color.FromArgb(26, 28, 40),
                    BorderStyle = BorderStyle.FixedSingle,
                    Visible = false
                };
                int ap = 4;

                var lblFsrAdvHdr = new Label
                {
                    Location = new Point(6, ap), AutoSize = false,
                    Size = new Size(advW - 58, 20),
                    Text = "\u26a0  Expert settings \u2014 do not adjust unless you know what you're doing",
                    ForeColor = Color.FromArgb(255, 185, 35),
                    Font = new Font("Segoe UI", 8f, FontStyle.Bold)
                };
                fsrAdv.Controls.Add(lblFsrAdvHdr);

                var btnFsrClose = MakeButton("\u00d7", advW - 50, ap - 2, 42, 22);
                btnFsrClose.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
                btnFsrClose.BackColor = Color.FromArgb(90, 40, 40);
                btnFsrClose.Click += (s, e) => { fsrAdv.Visible = false; btnFsrAdv.Text = "\u25bc Advanced"; };
                fsrAdv.Controls.Add(btnFsrClose);
                ap += 26;

                // Motion Vectors checkbox
                _chkMotionVectorsEnabled = MakeCheckBox("Enable Motion Vectors", 20, ap);
                fsrAdv.Controls.Add(_chkMotionVectorsEnabled);
                _chkActorMV = MakeCheckBox("Actor MV", 205, ap);
                fsrAdv.Controls.Add(_chkActorMV);
                _chkFsr3CameraMV = MakeCheckBox("Camera MV", 300, ap);
                fsrAdv.Controls.Add(_chkFsr3CameraMV);
                var lblMvDesc = MakeLabel("SKSE/game, actor, and camera motion vectors feed temporal upscalers and ASW.", 395, ap + 3, advW - 411);
                lblMvDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblMvDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblMvDesc);
                ap += 26;

                // Motion Vector Scale
                var lblMvScale = MakeLabel("MV Scale:", 20, ap + 3, 80);
                fsrAdv.Controls.Add(lblMvScale);
                _nudMotionVectorScale = new NumericUpDown
                {
                    Location = new Point(100, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.10m, Maximum = 2.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudMotionVectorScale);
                var lblMvScaleDesc = MakeLabel("Motion vector magnitude multiplier. 1.0 = raw engine data. Do not adjust unless you know what you're doing.", 175, ap + 3, advW - 191);
                lblMvScaleDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblMvScaleDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblMvScaleDesc);
                ap += 26;

                // FSR3 built-in sharpness
                var lblFsr3Sharp = MakeLabel("FSR 3 Sharpness:", 20, ap + 3, 115);
                fsrAdv.Controls.Add(lblFsr3Sharp);
                _nudFsr3Sharpness = new NumericUpDown
                {
                    Location = new Point(135, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.30m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3Sharpness);
                var lblFsr3SharpDesc = MakeLabel("FSR 3 built-in sharpening (0 = off, 1 = max). Separate from CAS.", 210, ap + 3, advW - 226);
                lblFsr3SharpDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblFsr3SharpDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblFsr3SharpDesc);
                ap += 26;

                // Jitter Scale
                var lblJitterScale = MakeLabel("Jitter Scale:", 20, ap + 3, 100);
                fsrAdv.Controls.Add(lblJitterScale);
                _nudFsr3JitterScale = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.30m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3JitterScale);
                var lblJitterDesc = MakeLabel("Sub-pixel jitter amplitude (lower = more stable, less temporal detail).", 195, ap + 3, advW - 211);
                lblJitterDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblJitterDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblJitterDesc);
                ap += 26;

                // Jitter Cancellation checkbox
                _chkFsr3JitterCancellation = MakeCheckBox("MV Jitter Cancellation", 20, ap);
                fsrAdv.Controls.Add(_chkFsr3JitterCancellation);
                var lblJcDesc = MakeLabel("Game MVs include jitter \u2014 FSR3 compensates. Disable if you see double-jitter artifacts.", 224, ap + 3, advW - 240);
                lblJcDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblJcDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblJcDesc);
                ap += 26;

                // View-to-Meters
                var lblV2m = MakeLabel("View-to-Meters:", 20, ap + 3, 110);
                fsrAdv.Controls.Add(lblV2m);
                _nudFsr3ViewToMeters = new NumericUpDown
                {
                    Location = new Point(130, ap), Width = 80,
                    DecimalPlaces = 5, Increment = 0.001m, Minimum = 0.001m, Maximum = 0.100m, Value = 0.01428m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3ViewToMeters);
                var lblV2mDesc = MakeLabel("Skyrim units to meters (default 0.01428 = 1/70). Affects FSR3 motion estimation.", 215, ap + 3, advW - 231);
                lblV2mDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblV2mDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                fsrAdv.Controls.Add(lblV2mDesc);
                ap += 26;

                fsrAdv.Controls.Add(MakeLabel("Reactiveness:", 20, ap + 3, 100));
                _nudFsr3ReactivenessScale = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.10m, Minimum = 0.00m, Maximum = 8.00m, Value = 2.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3ReactivenessScale);

                fsrAdv.Controls.Add(MakeLabel("Shading:", 210, ap + 3, 60));
                _nudFsr3ShadingChangeScale = new NumericUpDown
                {
                    Location = new Point(272, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.10m, Minimum = 0.00m, Maximum = 8.00m, Value = 2.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3ShadingChangeScale);

                fsrAdv.Controls.Add(MakeLabel("Accum/frame:", 360, ap + 3, 88));
                _nudFsr3AccumulationPerFrame = new NumericUpDown
                {
                    Location = new Point(448, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.20m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                fsrAdv.Controls.Add(_nudFsr3AccumulationPerFrame);
                ap += 26;

                fsrAdv.Controls.Add(MakeLabel("Debug view:", 20, ap + 3, 85));
                _cmbFsr3DebugMode = new ComboBox
                {
                    Location = new Point(105, ap), Width = 195,
                    DropDownStyle = ComboBoxStyle.DropDownList,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                _cmbFsr3DebugMode.Items.AddRange(new object[] {
                    "Off", "FSR overlay", "Bypass", "Depth", "Final MV",
                    "Residual MV", "Raw bridge MV", "Bridge fallback", "Reactive mask" });
                _cmbFsr3DebugMode.SelectedIndex = 0;
                fsrAdv.Controls.Add(_cmbFsr3DebugMode);

                container.Controls.Add(fsrAdv);
            }

            // Status label
            _lblFsrStatus = new Label
            {
                Location = new Point(leftMargin + 20, y),
                Size = new Size(rightEdge - leftMargin - 20, 18),
                Text = "FSR is disabled \u2014 game renders at native resolution.",
                ForeColor = Color.FromArgb(160, 160, 160),
                Font = new Font("Segoe UI", 8.5f),
                AutoSize = false
            };
            container.Controls.Add(_lblFsrStatus);
            y += 22;

            // FSR Presets
            var lblPresets = MakeLabel("Presets:", leftMargin + 20, y + 4, 55);
            container.Controls.Add(lblPresets);

            int px = leftMargin + 78;
            var btnQuality = MakeButton("Quality", px, y, 80, 26);
            btnQuality.Click += (s, e) => { _chkFsrNativeAA.Checked = false; _chkFsrEnabled.Checked = true; _nudFsrRenderScale.Value = 0.67m; };
            container.Controls.Add(btnQuality);
            px += 84;

            var btnBalanced = MakeButton("Balanced", px, y, 85, 26);
            btnBalanced.Click += (s, e) => { _chkFsrNativeAA.Checked = false; _chkFsrEnabled.Checked = true; _nudFsrRenderScale.Value = 0.59m; };
            container.Controls.Add(btnBalanced);
            px += 89;

            var btnPerformance = MakeButton("Performance", px, y, 95, 26);
            btnPerformance.Click += (s, e) => { _chkFsrNativeAA.Checked = false; _chkFsrEnabled.Checked = true; _nudFsrRenderScale.Value = 0.50m; };
            container.Controls.Add(btnPerformance);
            px += 99;

            var btnUltra = MakeButton("Ultra Perf", px, y, 85, 26);
            btnUltra.Click += (s, e) => { _chkFsrNativeAA.Checked = false; _chkFsrEnabled.Checked = true; _nudFsrRenderScale.Value = 0.33m; };
            container.Controls.Add(btnUltra);
            px += 89;

            var btnNativeAA = MakeButton("Native AA", px, y, 85, 26);
            btnNativeAA.Click += (s, e) => { _chkFsrEnabled.Checked = true; _chkFsrNativeAA.Checked = true; _nudFsrRenderScale.Value = 1.00m; };
            container.Controls.Add(btnNativeAA);
            px += 89;

            var btnOff = MakeButton("Off", px, y, 55, 26);
            btnOff.BackColor = Color.FromArgb(120, 60, 40);
            btnOff.Click += (s, e) => { _chkFsrNativeAA.Checked = false; _chkFsrEnabled.Checked = false; _nudFsrRenderScale.Value = 0.67m; };
            container.Controls.Add(btnOff);
            y += 34;


            // ── OCU ASW ──
            Panel aswAdv = null!;
            var lblSwSection = MakeSectionLabel("OCU ASW", leftMargin, y);
            container.Controls.Add(lblSwSection);
            var btnAswAdv = MakeButton("\u25bc Advanced", rightEdge - 100, y + 2, 94, 22);
            btnAswAdv.Font = new Font("Segoe UI", 7.5f);
            btnAswAdv.BackColor = Color.FromArgb(45, 48, 62);
            btnAswAdv.Click += (s, e) =>
            {
                bool show = !aswAdv.Visible;
                aswAdv.Visible = show;
                if (show) aswAdv.BringToFront();
                btnAswAdv.Text = show ? "\u25b2 Advanced" : "\u25bc Advanced";
            };
            container.Controls.Add(btnAswAdv);
            y += 26;

            _chkAswEnabled = MakeCheckBox("Enable OCU ASW", leftMargin, y);
            _chkAswEnabled.CheckedChanged += (s, e) => { };
            container.Controls.Add(_chkAswEnabled);

            var lblSwDesc = MakeLabel("Experimental / future-use reprojection, mainly for Meta/Quest testing. Expect lateral parallax separation on walls, foliage, and overlays during strafing. Use VD/Meta SSW for normal play, and never combine both.", leftMargin + 230, y + 3, rightEdge - leftMargin - 250);
            lblSwDesc.ForeColor = Color.FromArgb(130, 130, 130);
            lblSwDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            lblSwDesc.Height = 60;
            container.Controls.Add(lblSwDesc);
            y += 68;

            // ── ASW ADVANCED OVERLAY PANEL (does not advance y — floats over content below) ──
            {
                int advW = rightEdge - leftMargin;
                aswAdv = new Panel
                {
                    Location = new Point(leftMargin, y),
                    Size = new Size(advW, 252),
                    BackColor = Color.FromArgb(26, 28, 40),
                    BorderStyle = BorderStyle.FixedSingle,
                    Visible = false
                };
                int ap = 4;

                var lblAswAdvHdr = new Label
                {
                    Location = new Point(6, ap), AutoSize = false,
                    Size = new Size(advW - 58, 20),
                    Text = "\u26a0  Expert settings \u2014 do not adjust unless you know what you're doing",
                    ForeColor = Color.FromArgb(255, 185, 35),
                    Font = new Font("Segoe UI", 8f, FontStyle.Bold)
                };
                aswAdv.Controls.Add(lblAswAdvHdr);

                var btnAswClose = MakeButton("\u00d7", advW - 50, ap - 2, 42, 22);
                btnAswClose.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
                btnAswClose.BackColor = Color.FromArgb(90, 40, 40);
                btnAswClose.Click += (s, e) => { aswAdv.Visible = false; btnAswAdv.Text = "\u25bc Advanced"; };
                aswAdv.Controls.Add(btnAswClose);
                ap += 26;

                _chkAswExperimentalMode = MakeCheckBox("Experimental mode", 20, ap);
                aswAdv.Controls.Add(_chkAswExperimentalMode);
                _chkAswUpscalerReactiveMask = MakeCheckBox("Upscaler reactive mask", 180, ap);
                aswAdv.Controls.Add(_chkAswUpscalerReactiveMask);
                _chkAswBufferEnabled = MakeCheckBox("Buffered scheduling", 380, ap);
                aswAdv.Controls.Add(_chkAswBufferEnabled);
                _chkAswUpscalerReset = MakeCheckBox("Reset upscaler history", 560, ap);
                aswAdv.Controls.Add(_chkAswUpscalerReset);
                ap += 26;

                _chkAswAutoNative = MakeCheckBox("Auto mode (native FPS when fast)", 20, ap);
                aswAdv.Controls.Add(_chkAswAutoNative);
                _chkAswDebugMode = MakeCheckBox("Debug: highlight synthetic frames", 280, ap);
                aswAdv.Controls.Add(_chkAswDebugMode);
                ap += 26;

                // Warp Strength
                var lblWarpStrength = MakeLabel("Warp Strength:", 20, ap + 3, 100);
                aswAdv.Controls.Add(lblWarpStrength);
                _nudAswWarpStrength = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 3.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswWarpStrength);
                var lblWarpStrengthDesc = MakeLabel("How much head-pose correction to apply. 1.0 = full, 0.5 = half, 0.0 = none.", 195, ap + 3, advW - 211);
                lblWarpStrengthDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblWarpStrengthDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblWarpStrengthDesc);
                ap += 26;

                // Rotation Scale
                var lblRotScale = MakeLabel("Rotation Scale:", 20, ap + 3, 100);
                aswAdv.Controls.Add(lblRotScale);
                _nudAswRotationScale = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 3.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswRotationScale);
                var lblRotDesc = MakeLabel("Head turn correction. Reduce if you see double-vision on head rotation.", 195, ap + 3, advW - 211);
                lblRotDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblRotDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblRotDesc);
                ap += 26;

                // Translation Scale
                var lblTransScale = MakeLabel("Translation Scale:", 20, ap + 3, 110);
                aswAdv.Controls.Add(lblTransScale);
                _nudAswTranslationScale = new NumericUpDown
                {
                    Location = new Point(130, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 3.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswTranslationScale);
                var lblTransDesc = MakeLabel("Positional correction (lean/strafe). Reduce if hands shoot forward.", 205, ap + 3, advW - 221);
                lblTransDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblTransDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblTransDesc);
                ap += 26;

                var lblLocoScale = MakeLabel("Loco Scale:", 20, ap + 3, 100);
                aswAdv.Controls.Add(lblLocoScale);
                _nudAswLocoScale = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 3.00m, Value = 0.70m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswLocoScale);
                var lblLocoDesc = MakeLabel("Game locomotion contribution to ASW warp. Default 0.50.", 195, ap + 3, advW - 211);
                lblLocoDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblLocoDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblLocoDesc);
                ap += 26;

                // Depth Scale
                var lblDepthScale = MakeLabel("Depth Scale:", 20, ap + 3, 100);
                aswAdv.Controls.Add(lblDepthScale);
                _nudAswDepthScale = new NumericUpDown
                {
                    Location = new Point(120, ap), Width = 70,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 2.00m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswDepthScale);
                var lblDepthDesc = MakeLabel("Parallax intensity. 0 = flat 2D shift, 1 = full depth, >1 = exaggerated.", 195, ap + 3, advW - 211);
                lblDepthDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblDepthDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblDepthDesc);
                ap += 26;

                // Auto-mode engage threshold
                var lblEngageFps = MakeLabel("Engage below FPS:", 20, ap + 3, 115);
                aswAdv.Controls.Add(lblEngageFps);
                _nudAswAutoEngageFps = new NumericUpDown
                {
                    Location = new Point(140, ap), Width = 60,
                    DecimalPlaces = 0, Increment = 1m, Minimum = 20m, Maximum = 90m, Value = 50m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
                };
                aswAdv.Controls.Add(_nudAswAutoEngageFps);
                var lblAutoDesc = MakeLabel("Auto mode: run uncapped when the game is faster than this; engage ASW below it.", 215, ap + 3, advW - 231);
                lblAutoDesc.ForeColor = Color.FromArgb(130, 130, 130);
                lblAutoDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aswAdv.Controls.Add(lblAutoDesc);

                container.Controls.Add(aswAdv);

            }

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            // ── DLAA (left column) + CAS (right column) — SIDE BY SIDE ──
            Panel aaAdv = null!;
            int rowStartY = y;

            // LEFT COLUMN: Anti-Aliasing / post processing
            var lblAASection = MakeSectionLabel("Post AA", leftMargin, y);
            container.Controls.Add(lblAASection);

            // RIGHT COLUMN: CAS Sharpening
            var lblCasSection = MakeSectionLabel("CAS Sharpening (RCAS)", col2, y);
            container.Controls.Add(lblCasSection);
            var btnAaAdv = MakeButton("\u25bc Advanced", rightEdge - 100, y + 2, 94, 22);
            btnAaAdv.Font = new Font("Segoe UI", 7.5f);
            btnAaAdv.BackColor = Color.FromArgb(45, 48, 62);
            btnAaAdv.Click += (s, e) =>
            {
                bool show = !aaAdv.Visible;
                aaAdv.Visible = show;
                if (show) aaAdv.BringToFront();
                btnAaAdv.Text = show ? "\u25b2 Advanced" : "\u25bc Advanced";
            };
            container.Controls.Add(btnAaAdv);
            y += 26;

            _chkBlueSkyDefenderEnabled = MakeCheckBox("BlueSkyDefender Post-AA", leftMargin, y);
            _chkBlueSkyDefenderEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkBlueSkyDefenderEnabled.Checked;
                _nudBlueSkyLambda.Enabled = en;
                _nudBlueSkyEpsilon.Enabled = en;
            };
            container.Controls.Add(_chkBlueSkyDefenderEnabled);

            // RIGHT: CAS checkbox
            _chkCasEnabled = MakeCheckBox("Enable CAS Sharpening", col2, y);
            _chkCasEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkCasEnabled.Checked;
                _nudCasSharpness.Enabled = en;
            };
            container.Controls.Add(_chkCasEnabled);
            y += 24;

            // ── AA/CAS ADVANCED OVERLAY PANEL (does not advance y — floats over content below) ──
            {
                int advW = rightEdge - leftMargin;
                aaAdv = new Panel
                {
                    Location = new Point(leftMargin, y),
                    Size = new Size(advW, 64),
                    BackColor = Color.FromArgb(26, 28, 40),
                    BorderStyle = BorderStyle.FixedSingle,
                    Visible = false
                };
                int ap = 4;

                var lblAaAdvHdr = new Label
                {
                    Location = new Point(6, ap), AutoSize = false,
                    Size = new Size(advW - 58, 20),
                    Text = "\u26a0  Expert settings \u2014 do not adjust unless you know what you're doing",
                    ForeColor = Color.FromArgb(255, 185, 35),
                    Font = new Font("Segoe UI", 8f, FontStyle.Bold)
                };
                aaAdv.Controls.Add(lblAaAdvHdr);

                var btnAaClose = MakeButton("\u00d7", advW - 50, ap - 2, 42, 22);
                btnAaClose.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
                btnAaClose.BackColor = Color.FromArgb(90, 40, 40);
                btnAaClose.Click += (s, e) => { aaAdv.Visible = false; btnAaAdv.Text = "\u25bc Advanced"; };
                aaAdv.Controls.Add(btnAaClose);
                ap += 26;

                // LEFT: DLAA tuning — Sensitivity + Threshold
                aaAdv.Controls.Add(MakeLabel("BlueSky sens:", 20, ap + 3, 90));
                _nudBlueSkyLambda = new NumericUpDown
                {
                    Location = new Point(112, ap), Width = 55,
                    DecimalPlaces = 1, Increment = 0.1m, Minimum = 1.0m, Maximum = 6.0m, Value = 3.0m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                aaAdv.Controls.Add(_nudBlueSkyLambda);

                aaAdv.Controls.Add(MakeLabel("Threshold:", 178, ap + 3, 65));
                _nudBlueSkyEpsilon = new NumericUpDown
                {
                    Location = new Point(246, ap), Width = 55,
                    DecimalPlaces = 2, Increment = 0.01m, Minimum = 0.01m, Maximum = 0.50m, Value = 0.10m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                aaAdv.Controls.Add(_nudBlueSkyEpsilon);

                // RIGHT: CAS sharpness (col2 - leftMargin = halfWidth + 10)
                _lblCasSharpness = MakeLabel("Sharpness:", halfWidth + 30, ap + 3, 75);
                aaAdv.Controls.Add(_lblCasSharpness);
                _nudCasSharpness = new NumericUpDown
                {
                    Location = new Point(halfWidth + 105, ap), Width = 60,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.00m, Maximum = 1.00m, Value = 0.20m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                aaAdv.Controls.Add(_nudCasSharpness);
                var lblSharpHint = MakeLabel("(0.0 = soft, 1.0 = max)", halfWidth + 170, ap + 3, 170);
                lblSharpHint.ForeColor = Color.FromArgb(110, 110, 110);
                lblSharpHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                aaAdv.Controls.Add(lblSharpHint);

                container.Controls.Add(aaAdv);
            }

            // LEFT: Post-AA description
            var lblDlaaHint = MakeLabel("Hardware-agnostic post-process AA. NVIDIA DLAA is selected in the DLSS preset above.", leftMargin + 20, y, halfWidth - 20);
            lblDlaaHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblDlaaHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblDlaaHint);

            // RIGHT: CAS description
            var lblCasDesc = MakeLabel("Extra sharpening at native res. Not needed with FSR 3 \u2014 FSR 3 has its own built-in sharpening.", col2 + 20, y, halfWidth - 20);
            lblCasDesc.ForeColor = Color.FromArgb(130, 130, 130);
            lblCasDesc.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblCasDesc);
            y += 22;

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            // ── NVIDIA FIXED FOVEATED RENDERING (VRS) ──
            var lblMipSection = MakeSectionLabel("Texture MIP Bias", leftMargin, y);
            container.Controls.Add(lblMipSection);
            var lblMipSectionHint = MakeLabel("(Recommended for increased sharpness while using upscaling)", leftMargin + 150, y + 3, 420);
            lblMipSectionHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblMipSectionHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblMipSectionHint);
            y += 26;

            _chkMipBiasEnabled = MakeCheckBox("Correct MIPs for upscaling", leftMargin, y);
            _chkMipBiasEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkMipBiasEnabled.Checked;
                _cmbMipBiasMode.Enabled = en;
                _nudMipBiasFixed.Enabled = en && _cmbMipBiasMode.SelectedIndex == 2;
                _nudMipBiasOffset.Enabled = en;
                _nudDlssMipBiasOffset.Enabled = en;
                _nudFsr3MipBiasOffset.Enabled = en;
            };
            container.Controls.Add(_chkMipBiasEnabled);

            container.Controls.Add(MakeLabel("Mode:", leftMargin + 230, y + 3, 45));
            _cmbMipBiasMode = new ComboBox
            {
                Location = new Point(leftMargin + 275, y), Width = 95,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            _cmbMipBiasMode.Items.AddRange(new object[] { "Auto", "Off", "Custom" });
            _cmbMipBiasMode.SelectedIndex = 0;
            _cmbMipBiasMode.SelectedIndexChanged += (s, e) =>
            {
                _nudMipBiasFixed.Enabled = _chkMipBiasEnabled.Checked && _cmbMipBiasMode.SelectedIndex == 2;
            };
            container.Controls.Add(_cmbMipBiasMode);

            container.Controls.Add(MakeLabel("Fixed:", leftMargin + 385, y + 3, 45));
            _nudMipBiasFixed = new NumericUpDown
            {
                Location = new Point(leftMargin + 430, y), Width = 70,
                DecimalPlaces = 3, Increment = 0.05m, Minimum = -4.00m, Maximum = 4.00m, Value = -0.766m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White,
                Enabled = false
            };
            container.Controls.Add(_nudMipBiasFixed);

            var lblMipHint = MakeLabel("Auto = log2(renderScale) plus offsets. DLSS default 0.0; FSR3 default +1.0.", leftMargin + 520, y + 3, rightEdge - leftMargin - 530);
            lblMipHint.ForeColor = Color.FromArgb(130, 130, 130);
            lblMipHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblMipHint);
            y += 28;

            container.Controls.Add(MakeLabel("Shared offset:", leftMargin + 20, y + 3, 95));
            _nudMipBiasOffset = new NumericUpDown
            {
                Location = new Point(leftMargin + 115, y), Width = 70,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = -3.00m, Maximum = 3.00m, Value = 0.00m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudMipBiasOffset);

            container.Controls.Add(MakeLabel("DLSS offset:", leftMargin + 205, y + 3, 85));
            _nudDlssMipBiasOffset = new NumericUpDown
            {
                Location = new Point(leftMargin + 290, y), Width = 70,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = -3.00m, Maximum = 3.00m, Value = 0.00m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudDlssMipBiasOffset);

            container.Controls.Add(MakeLabel("FSR3 offset:", leftMargin + 380, y + 3, 85));
            _nudFsr3MipBiasOffset = new NumericUpDown
            {
                Location = new Point(leftMargin + 465, y), Width = 70,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = -3.00m, Maximum = 3.00m, Value = 1.00m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudFsr3MipBiasOffset);
            y += 32;

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            Panel vrsAdv = null!;
            var lblVrsSection = MakeSectionLabel("NVIDIA Fixed Foveated Rendering (VRS)", leftMargin, y);
            container.Controls.Add(lblVrsSection);
            var btnVrsAdv = MakeButton("\u25bc Advanced", rightEdge - 100, y + 2, 94, 22);
            btnVrsAdv.Font = new Font("Segoe UI", 7.5f);
            btnVrsAdv.BackColor = Color.FromArgb(45, 48, 62);
            btnVrsAdv.Click += (s, e) =>
            {
                bool show = !vrsAdv.Visible;
                vrsAdv.Visible = show;
                if (show) vrsAdv.BringToFront();
                btnVrsAdv.Text = show ? "\u25b2 Advanced" : "\u25bc Advanced";
            };
            container.Controls.Add(btnVrsAdv);
            y += 26;

            var lblVrsInfo = MakeLabel("Requires NVIDIA RTX or GTX 16xx series GPU. Reduces shading rate in peripheral vision for better performance. May cause pixel flashes if used with FSR 3 or DLSS 4.", leftMargin + 20, y, rightEdge - leftMargin - 20);
            lblVrsInfo.ForeColor = Color.White;
            lblVrsInfo.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            container.Controls.Add(lblVrsInfo);
            y += 18;

            var lblVrsEyeTrackNote = MakeLabel("Note: If your headset has built-in eye-tracked foveated rendering, this fixed foveated rendering is redundant and should be left disabled.", leftMargin + 20, y, rightEdge - leftMargin - 20);
            lblVrsEyeTrackNote.ForeColor = Color.FromArgb(255, 200, 100);
            lblVrsEyeTrackNote.Font = new Font("Segoe UI", 8.5f, FontStyle.Italic);
            container.Controls.Add(lblVrsEyeTrackNote);
            y += 22;

            _chkVrsEnabled = MakeCheckBox("Enable VRS Foveated Rendering", leftMargin, y);
            _chkVrsEnabled.CheckedChanged += (s, e) =>
            {
                bool en = _chkVrsEnabled.Checked;
                _cboVrsPreset.Enabled = en;
                _nudVrsInnerRadius.Enabled = en;
                _nudVrsMidRadius.Enabled = en;
                _nudVrsOuterRadius.Enabled = en;
                // Don't use .Enabled on CheckBox — WinForms renders disabled text black on dark backgrounds
                _chkVrsFavorHorizontal.ForeColor = en ? Color.FromArgb(210, 210, 210) : Color.FromArgb(90, 90, 90);
                _chkVrsFavorHorizontal.AutoCheck = en;
                CheckPotatoMode();
            };
            container.Controls.Add(_chkVrsEnabled);

            // Preset dropdown — same row as checkbox
            var lblVrsPreset = MakeLabel("Preset:", leftMargin + 270, y + 3, 50);
            container.Controls.Add(lblVrsPreset);

            _cboVrsPreset = new ComboBox
            {
                Location = new Point(leftMargin + 322, y), Width = 120,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
            };
            _cboVrsPreset.Items.AddRange(new[] { "Conservative", "Balanced", "Aggressive", "Custom" });
            _cboVrsPreset.SelectedIndex = 0;
            _cboVrsPreset.SelectedIndexChanged += (s, e) =>
            {
                switch (_cboVrsPreset.SelectedIndex)
                {
                    case 0: // Conservative (pancake-lens friendly)
                        _nudVrsInnerRadius.Value = 0.70m; _nudVrsMidRadius.Value = 0.85m; _nudVrsOuterRadius.Value = 1.00m; break;
                    case 1: // Balanced
                        _nudVrsInnerRadius.Value = 0.60m; _nudVrsMidRadius.Value = 0.80m; _nudVrsOuterRadius.Value = 1.00m; break;
                    case 2: // Aggressive
                        _nudVrsInnerRadius.Value = 0.50m; _nudVrsMidRadius.Value = 0.70m; _nudVrsOuterRadius.Value = 0.90m; break;
                    case 3: // Custom — don't change values
                        break;
                }
            };
            container.Controls.Add(_cboVrsPreset);

            var lblPresetHint = MakeLabel("(Conservative recommended for pancake lenses)", leftMargin + 450, y + 3, 280);
            lblPresetHint.ForeColor = Color.FromArgb(110, 110, 110);
            lblPresetHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
            container.Controls.Add(lblPresetHint);
            y += 26;

            // ── VRS ADVANCED OVERLAY PANEL (does not advance y — floats over content below) ──
            {
                int advW = rightEdge - leftMargin;
                vrsAdv = new Panel
                {
                    Location = new Point(leftMargin, y),
                    Size = new Size(advW, 64),
                    BackColor = Color.FromArgb(26, 28, 40),
                    BorderStyle = BorderStyle.FixedSingle,
                    Visible = false
                };
                int ap = 4;

                var lblVrsAdvHdr = new Label
                {
                    Location = new Point(6, ap), AutoSize = false,
                    Size = new Size(advW - 58, 20),
                    Text = "\u26a0  Expert settings \u2014 do not adjust unless you know what you're doing",
                    ForeColor = Color.FromArgb(255, 185, 35),
                    Font = new Font("Segoe UI", 8f, FontStyle.Bold)
                };
                vrsAdv.Controls.Add(lblVrsAdvHdr);

                var btnVrsClose = MakeButton("\u00d7", advW - 50, ap - 2, 42, 22);
                btnVrsClose.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
                btnVrsClose.BackColor = Color.FromArgb(90, 40, 40);
                btnVrsClose.Click += (s, e) => { vrsAdv.Visible = false; btnVrsAdv.Text = "\u25bc Advanced"; };
                vrsAdv.Controls.Add(btnVrsClose);
                ap += 26;

                // VRS Radii — all three on one row + Favor Horizontal
                _lblVrsInnerRadius = MakeLabel("Inner:", 20, ap + 3, 42);
                vrsAdv.Controls.Add(_lblVrsInnerRadius);
                _nudVrsInnerRadius = new NumericUpDown
                {
                    Location = new Point(62, ap), Width = 60,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.10m, Maximum = 1.00m, Value = 0.60m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                _nudVrsInnerRadius.ValueChanged += (s, e) => { if (_cboVrsPreset.SelectedIndex != 3) _cboVrsPreset.SelectedIndex = 3; };
                vrsAdv.Controls.Add(_nudVrsInnerRadius);
                var lblInnerHint = MakeLabel("1x1", 125, ap + 3, 30);
                lblInnerHint.ForeColor = Color.FromArgb(110, 110, 110);
                lblInnerHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                vrsAdv.Controls.Add(lblInnerHint);

                _lblVrsMidRadius = MakeLabel("Mid:", 170, ap + 3, 32);
                vrsAdv.Controls.Add(_lblVrsMidRadius);
                _nudVrsMidRadius = new NumericUpDown
                {
                    Location = new Point(202, ap), Width = 60,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.10m, Maximum = 1.50m, Value = 0.80m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                _nudVrsMidRadius.ValueChanged += (s, e) => { if (_cboVrsPreset.SelectedIndex != 3) _cboVrsPreset.SelectedIndex = 3; };
                vrsAdv.Controls.Add(_nudVrsMidRadius);
                var lblMidHint = MakeLabel("2x1", 265, ap + 3, 30);
                lblMidHint.ForeColor = Color.FromArgb(110, 110, 110);
                lblMidHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                vrsAdv.Controls.Add(lblMidHint);

                _lblVrsOuterRadius = MakeLabel("Outer:", 310, ap + 3, 42);
                vrsAdv.Controls.Add(_lblVrsOuterRadius);
                _nudVrsOuterRadius = new NumericUpDown
                {
                    Location = new Point(352, ap), Width = 60,
                    DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.10m, Maximum = 1.50m, Value = 1.00m,
                    BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
                };
                _nudVrsOuterRadius.ValueChanged += (s, e) => { if (_cboVrsPreset.SelectedIndex != 3) _cboVrsPreset.SelectedIndex = 3; };
                vrsAdv.Controls.Add(_nudVrsOuterRadius);
                var lblOuterHint = MakeLabel("2x2", 415, ap + 3, 30);
                lblOuterHint.ForeColor = Color.FromArgb(110, 110, 110);
                lblOuterHint.Font = new Font("Segoe UI", 8f, FontStyle.Italic);
                vrsAdv.Controls.Add(lblOuterHint);

                // Favor Horizontal — far right on same row
                _chkVrsFavorHorizontal = MakeCheckBox("Favor Horizontal", 480, ap);
                _chkVrsFavorHorizontal.Checked = true;
                _chkVrsFavorHorizontal.ForeColor = Color.FromArgb(90, 90, 90);
                _chkVrsFavorHorizontal.AutoCheck = false;
                vrsAdv.Controls.Add(_chkVrsFavorHorizontal);

                container.Controls.Add(vrsAdv);
            }

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 12;

            // ── SAVE BUTTON ──
            var btnSaveVideo = MakeButton("Save opencomposite.ini", leftMargin, y, 200, 30);
            btnSaveVideo.BackColor = Color.FromArgb(40, 120, 40);
            btnSaveVideo.ForeColor = Color.White;
            btnSaveVideo.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            btnSaveVideo.Click += BtnSave_Click;
            container.Controls.Add(btnSaveVideo);

            _lblVideoStatus = new Label
            {
                Location = new Point(leftMargin + 210, y + 6),
                Size = new Size(rightEdge - leftMargin - 210, 20),
                Text = "",
                ForeColor = Color.FromArgb(160, 160, 160),
                Font = new Font("Segoe UI", 9f),
                AutoSize = false
            };
            container.Controls.Add(_lblVideoStatus);
            y += 40;

            // Resize panel to fit content
            container.Size = new Size(container.Width, y + 10);
        }

        private int DetectVrsPreset()
        {
            decimal ir = _nudVrsInnerRadius.Value, mr = _nudVrsMidRadius.Value, or2 = _nudVrsOuterRadius.Value;
            if (ir == 0.70m && mr == 0.85m && or2 == 1.00m) return 0; // Conservative
            if (ir == 0.60m && mr == 0.80m && or2 == 1.00m) return 1; // Balanced
            if (ir == 0.50m && mr == 0.70m && or2 == 0.90m) return 2; // Aggressive
            return 3; // Custom
        }

        private void UpdateFsrStatus()
        {
            if (!_chkFsrEnabled.Checked)
            {
                _lblFsrStatus.Text = "FSR is disabled \u2014 game renders at native resolution.";
                _lblFsrStatus.ForeColor = Color.FromArgb(160, 160, 160);
                return;
            }

            if (_chkFsrNativeAA.Checked)
            {
                _lblFsrStatus.Text = "FSR 3 Native AA active: native render scale with FSR temporal AA.";
                _lblFsrStatus.ForeColor = Color.FromArgb(100, 220, 100);
                return;
            }

            decimal scale = _nudFsrRenderScale.Value;
            int pctPixels = (int)(scale * scale * 100);
            string quality = scale >= 0.66m ? "Quality" : scale >= 0.58m ? "Balanced" : scale >= 0.49m ? "Performance" : "Ultra Performance";
            _lblFsrStatus.Text = $"FSR 3 active: rendering {scale:0.00}x ({pctPixels}% pixels) \u2014 {quality} mode";
            _lblFsrStatus.ForeColor = Color.FromArgb(100, 220, 100);
        }

        private void CheckPotatoMode()
        {
            if (_isLoading) return;
            if (_chkFsrEnabled.Checked && _chkVrsEnabled.Checked)
            {
                try
                {
                    var asm = Assembly.GetExecutingAssembly();
                    var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.PotatoMode.wav");
                    if (stream != null)
                    {
                        var player = new System.Media.SoundPlayer(stream);
                        player.Play();
                    }
                }
                catch { /* silently ignore if audio fails */ }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════
        // HAPTICS TAB — every rumble setting in one place. Master + strength
        // used to live in General settings, combat toggles in the Skyrim panel,
        // and keyboard click strength in the keyboard sound row; centralizing
        // them unclutters those tabs and gives space for explanations.
        // ═══════════════════════════════════════════════════════════════════════

        private void BuildHapticsTab()
        {
            var container = _tabHaptics;
            int y = 10;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 20;
            int c1 = leftMargin + 6;
            int c2 = leftMargin + 300;

            var btnHapticsSave = MakeButton("Save opencomposite.ini", rightEdge - 200, y - 4, 200, 30);
            btnHapticsSave.BackColor = Color.FromArgb(40, 120, 40);
            btnHapticsSave.ForeColor = Color.White;
            btnHapticsSave.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
            btnHapticsSave.Click += BtnSave_Click;
            container.Controls.Add(btnHapticsSave);

            var lblIntro = new Label
            {
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin - 210, 24),
                Text = "Controller rumble settings. The master switch gates everything, including rumble the game itself triggers.",
                ForeColor = Color.FromArgb(150, 200, 250),
                Font = new Font("Segoe UI", 8.5f, FontStyle.Italic),
                BackColor = Color.FromArgb(40, 45, 60),
                BorderStyle = BorderStyle.FixedSingle,
                Padding = new Padding(6, 4, 6, 4),
                AutoSize = false
            };
            container.Controls.Add(lblIntro);
            y += 34;

            // ── GENERAL ──
            container.Controls.Add(MakeSectionLabel("General", c1, y));
            y += 26;

            _chkHaptics = MakeCheckBox("Enable haptics (master switch)", c1, y);
            _chkHaptics.Checked = true;
            container.Controls.Add(_chkHaptics);

            container.Controls.Add(MakeLabel("Game rumble strength:", c2, y + 3, 150));
            _nudHapticStrength = new NumericUpDown
            {
                Location = new Point(c2 + 150, y), Width = 75,
                DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.0m, Maximum = 1.0m, Value = 0.1m,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudHapticStrength);
            y += 26;

            var lblGeneralHint = MakeLabel("Game rumble strength scales pulses the game engine fires (touch feedback, native effects), 0 to 1.", c1, y, rightEdge - c1);
            lblGeneralHint.ForeColor = Color.FromArgb(140, 140, 140);
            lblGeneralHint.Font = new Font("Segoe UI", 8.5f);
            container.Controls.Add(lblGeneralHint);
            y += 30;

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            // ── COMBAT (Skyrim VR, delivered by the OCU SKSE plugin) ──
            container.Controls.Add(MakeSectionLabel("Combat (Skyrim VR)", c1, y));
            y += 26;

            _chkCombatHapticShield = MakeCheckBox("Shield / weapon block", c1, y);
            _chkCombatHapticShield.Checked = true;
            container.Controls.Add(_chkCombatHapticShield);
            _chkCombatHapticWeapon = MakeCheckBox("Weapon && fist hits", c2, y);
            _chkCombatHapticWeapon.Checked = true;
            container.Controls.Add(_chkCombatHapticWeapon);
            y += 24;

            _chkCombatHapticBow = MakeCheckBox("Bow release", c1, y);
            _chkCombatHapticBow.Checked = true;
            container.Controls.Add(_chkCombatHapticBow);
            _chkCombatHapticMagic = MakeCheckBox("Magic casting", c2, y);
            _chkCombatHapticMagic.Checked = true;
            container.Controls.Add(_chkCombatHapticMagic);
            y += 26;

            container.Controls.Add(MakeLabel("Combat strength:", c1, y + 3, 110));
            _nudCombatHapticStrength = new NumericUpDown
            {
                Location = new Point(c1 + 110, y), Width = 55,
                Minimum = 0, Maximum = 100, Value = 80,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudCombatHapticStrength);
            container.Controls.Add(MakeLabel("(0-100)", c1 + 172, y + 3, 55));
            y += 26;

            var lblCombatHint = new Label
            {
                Location = new Point(c1, y),
                Size = new Size(rightEdge - c1, 60),
                Text = "Blocks thump the blocking hand; landed weapon and fist hits pulse the attacking hand (fists only while actually swinging). "
                     + "Bow release gives a light snap in both hands. Magic gives one pulse when a spell fires and a soft continuous rumble while "
                     + "streaming concentration spells like Flames. Blocks are strongest, then weapon hits, spells, fists, bow, stream hum. "
                     + "Weapon-clash mods (PLANCK etc.) do their own rumble and are unaffected.",
                ForeColor = Color.FromArgb(140, 140, 140),
                Font = new Font("Segoe UI", 8.5f),
                AutoSize = false
            };
            container.Controls.Add(lblCombatHint);
            y += 64;

            container.Controls.Add(MakeSeparator(leftMargin, y, rightEdge - leftMargin));
            y += 10;

            // ── VR KEYBOARD ──
            container.Controls.Add(MakeSectionLabel("VR Keyboard", c1, y));
            y += 26;

            container.Controls.Add(MakeLabel("Key click strength:", c1, y + 3, 120));
            _nudKbHapticStrength = new NumericUpDown
            {
                Location = new Point(c1 + 120, y), Width = 55,
                Minimum = 0, Maximum = 100, Increment = 5, Value = 50,
                BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White
            };
            container.Controls.Add(_nudKbHapticStrength);
            container.Controls.Add(MakeLabel("%", c1 + 178, y + 3, 20));
            y += 26;

            var lblKbHint = MakeLabel("Tick felt when the laser presses a key on the in-game VR keyboard.", c1, y, rightEdge - c1);
            lblKbHint.ForeColor = Color.FromArgb(140, 140, 140);
            lblKbHint.Font = new Font("Segoe UI", 8.5f);
            container.Controls.Add(lblKbHint);
            y += 34;

            container.Size = new Size(container.Width, y + 10);
        }

        private void SwitchTab(int index)
        {
            _tabSettings.Visible = (index == 0);
            _tabKeyboard.Visible = (index == 1);
            _tabGestures.Visible = (index == 2);
            _tabVideo.Visible = (index == 3);
            _tabHaptics.Visible = (index == 4);
            _tabBody.Visible = (index == 5);
            _tabTextures.Visible = (index == 6);

            // Update button styles
            _btnTabSettings.Font = new Font("Segoe UI", 10f, index == 0 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabSettings.ForeColor = index == 0 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabSettings.BackColor = index == 0 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabKeyboard.Font = new Font("Segoe UI", 10f, index == 1 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabKeyboard.ForeColor = index == 1 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabKeyboard.BackColor = index == 1 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabGestures.Font = new Font("Segoe UI", 10f, index == 2 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabGestures.ForeColor = index == 2 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabGestures.BackColor = index == 2 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabVideo.Font = new Font("Segoe UI", 10f, index == 3 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabVideo.ForeColor = index == 3 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabVideo.BackColor = index == 3 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabHaptics.Font = new Font("Segoe UI", 10f, index == 4 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabHaptics.ForeColor = index == 4 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabHaptics.BackColor = index == 4 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabBody.Font = new Font("Segoe UI", 10f, index == 5 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabBody.ForeColor = index == 5 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabBody.BackColor = index == 5 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);

            _btnTabTextures.Font = new Font("Segoe UI", 10f, index == 6 ? FontStyle.Bold : FontStyle.Regular);
            _btnTabTextures.ForeColor = index == 6 ? Color.White : Color.FromArgb(160, 160, 160);
            _btnTabTextures.BackColor = index == 6 ? Color.FromArgb(50, 50, 60) : Color.FromArgb(35, 35, 40);
        }

        // ═══════════════════════════════════════════════════════════════════════
        // FILE OPERATIONS
        // ═══════════════════════════════════════════════════════════════════════

        private void BtnApplySteamVrProfile_Click(object? sender, EventArgs e)
        {
            if (!TryGetSteamVrSettingsPath(out string path)) return;

            var confirm = MessageBox.Show(
                "This will back up and patch SteamVR's vrsettings for OCU.\n\nIt does not change your OpenXR runtime. Use XR Picker or SteamVR for that.\n\nContinue?",
                "Apply SteamVR OCU Profile",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Question);

            if (confirm != DialogResult.Yes) return;

            try
            {
                string backupPath = ApplySteamVrOcuProfile(path);
                _lblStatus.Text = $"SteamVR OCU profile applied. Backup: {Path.GetFileName(backupPath)}";
                _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
                MessageBox.Show($"SteamVR settings patched.\n\nBackup created:\n{backupPath}", "SteamVR OCU Profile",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                _lblStatus.Text = $"SteamVR profile failed: {ex.Message}";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
                MessageBox.Show(ex.Message, "SteamVR OCU Profile Failed",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void BtnRestoreSteamVrDefaults_Click(object? sender, EventArgs e)
        {
            if (!TryGetSteamVrSettingsPath(out string path)) return;

            var confirm = MessageBox.Show(
                "This will back up SteamVR's vrsettings and remove the OCU profile overrides so SteamVR defaults apply.\n\nContinue?",
                "Restore SteamVR Defaults",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);

            if (confirm != DialogResult.Yes) return;

            try
            {
                string backupPath = RestoreSteamVrDefaults(path);
                _lblStatus.Text = $"SteamVR defaults restored. Backup: {Path.GetFileName(backupPath)}";
                _lblStatus.ForeColor = Color.FromArgb(255, 200, 40);
                MessageBox.Show($"SteamVR OCU overrides removed.\n\nBackup created:\n{backupPath}", "SteamVR Defaults Restored",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                _lblStatus.Text = $"SteamVR restore failed: {ex.Message}";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
                MessageBox.Show(ex.Message, "SteamVR Restore Failed",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void BtnOpenSteamVrSettingsFolder_Click(object? sender, EventArgs e)
        {
            string folder = GetSteamVrSettingsFolder();
            Directory.CreateDirectory(folder);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(folder) { UseShellExecute = true });
        }

        private void BtnOpenSetupReadme_Click(object? sender, EventArgs e)
        {
            string readmePath = GetSetupReadmePath();
            if (!File.Exists(readmePath))
            {
                _lblStatus.Text = $"Setup readme not found: {readmePath}";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
                MessageBox.Show(readmePath, "Setup Readme Not Found", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(readmePath) { UseShellExecute = true });
        }

        private static string GetSetupReadmePath()
        {
            string baseDir = AppContext.BaseDirectory;
            string docsPath = Path.Combine(baseDir, "Docs", "OCU-Setup-Readme.html");
            if (File.Exists(docsPath)) return docsPath;

            return Path.Combine(baseDir, "OCU-Setup-Readme.html");
        }

        private static bool TryGetSteamVrSettingsPath(out string path)
        {
            string? resolved = ResolveSteamVrSettingsPath();
            if (resolved != null && File.Exists(resolved))
            {
                path = resolved;
                return true;
            }

            string initialDir = GetSteamVrSettingsFolder();
            using var dlg = new OpenFileDialog
            {
                Title = "Select steamvr.vrsettings",
                Filter = "SteamVR settings|steamvr.vrsettings|JSON files|*.json|All files|*.*",
                CheckFileExists = true,
                InitialDirectory = Directory.Exists(initialDir)
                    ? initialDir
                    : Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData)
            };

            if (dlg.ShowDialog() != DialogResult.OK) { path = ""; return false; }
            path = dlg.FileName;
            return true;
        }

        // SteamVR stores steamvr.vrsettings in <Steam>\config, NOT %LOCALAPPDATA%\openvr.
        // The authoritative pointer to that folder is openvrpaths.vrpath (which DOES live in
        // %LOCALAPPDATA%\openvr): its "config" array lists the SteamVR config dir(s). We read
        // that, then fall back to the Steam install path from the registry. Returns null if
        // neither resolves (caller then prompts with a file picker).
        private static string? ResolveSteamVrSettingsPath()
        {
            // 1) openvrpaths.vrpath -> config[] -> steamvr.vrsettings
            try
            {
                string vrpath = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "openvr", "openvrpaths.vrpath");
                if (File.Exists(vrpath) && JsonNode.Parse(File.ReadAllText(vrpath)) is JsonNode root
                    && root["config"] is JsonArray configs)
                {
                    // Prefer a config dir that already contains the file.
                    foreach (JsonNode? c in configs)
                    {
                        string? dir = c?.GetValue<string>();
                        if (string.IsNullOrWhiteSpace(dir)) continue;
                        string candidate = Path.Combine(dir, "steamvr.vrsettings");
                        if (File.Exists(candidate)) return candidate;
                    }
                    // Otherwise return the first known config dir's target path.
                    string? first = configs.Count > 0 ? configs[0]?.GetValue<string>() : null;
                    if (!string.IsNullOrWhiteSpace(first))
                        return Path.Combine(first, "steamvr.vrsettings");
                }
            }
            catch { /* fall through to registry lookup */ }

            // 2) Steam install path from registry -> <Steam>\config\steamvr.vrsettings
            try
            {
                string? steam =
                    Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string
                    ?? Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", null) as string
                    ?? Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam", "InstallPath", null) as string;
                if (!string.IsNullOrWhiteSpace(steam))
                    return Path.Combine(steam.Replace('/', '\\'), "config", "steamvr.vrsettings");
            }
            catch { /* fall through to null -> caller prompts */ }

            return null;
        }

        private static string GetSteamVrSettingsFolder()
        {
            string? dir = ResolveSteamVrSettingsPath() is string p ? Path.GetDirectoryName(p) : null;
            return !string.IsNullOrWhiteSpace(dir)
                ? dir
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr");
        }

        private static string GetSteamVrSettingsPath()
        {
            return ResolveSteamVrSettingsPath()
                ?? Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "openvr", "steamvr.vrsettings");
        }

        private static string ApplySteamVrOcuProfile(string path)
        {
            string rawJson = File.ReadAllText(path);
            JsonObject root = JsonNode.Parse(string.IsNullOrWhiteSpace(rawJson) ? "{}" : rawJson)?.AsObject()
                ?? throw new InvalidOperationException("SteamVR settings root is not a JSON object.");

            if (root["steamvr"] is not JsonObject steamVr)
            {
                steamVr = new JsonObject();
                root["steamvr"] = steamVr;
            }

            string dir = Path.GetDirectoryName(path) ?? throw new InvalidOperationException("Invalid SteamVR settings path.");
            string backupPath = Path.Combine(dir, $"steamvr.vrsettings.backup-{DateTime.Now:yyyyMMdd-HHmmss}");
            File.Copy(path, backupPath, overwrite: false);

            steamVr["allowSupersampleFiltering"] = false;
            steamVr["disableAsync"] = true;
            steamVr["enableHomeApp"] = false;
            steamVr["framesToThrottle"] = 0;
            steamVr["motionSmoothing"] = false;
            steamVr["showAdvancedSettings"] = true;
            steamVr["showMirrorView"] = false;
            steamVr["startCompositorFromAppLaunch"] = true;
            steamVr["startDashboardFromAppLaunch"] = false;
            steamVr["startMonitorFromAppLaunch"] = false;
            steamVr["supersampleManualOverride"] = true;
            steamVr["supersampleScale"] = 1;

            const string throttleShortcut = "frame_wait_throttle_toggle:187,0,0";
            string existingShortcuts = "";
            if (steamVr["debugCommandShortcuts"] is JsonValue shortcutValue &&
                shortcutValue.TryGetValue<string>(out string? shortcutText))
            {
                existingShortcuts = shortcutText;
            }

            if (!existingShortcuts.Contains("frame_wait_throttle_toggle", StringComparison.OrdinalIgnoreCase))
            {
                steamVr["debugCommandShortcuts"] = string.IsNullOrWhiteSpace(existingShortcuts)
                    ? throttleShortcut
                    : $"{existingShortcuts},{throttleShortcut}";
            }

            var options = new JsonSerializerOptions { WriteIndented = true };
            File.WriteAllText(path, root.ToJsonString(options));
            return backupPath;
        }

        private static string RestoreSteamVrDefaults(string path)
        {
            string rawJson = File.ReadAllText(path);
            JsonObject root = JsonNode.Parse(string.IsNullOrWhiteSpace(rawJson) ? "{}" : rawJson)?.AsObject()
                ?? throw new InvalidOperationException("SteamVR settings root is not a JSON object.");

            if (root["steamvr"] is not JsonObject steamVr)
            {
                throw new InvalidOperationException("No steamvr section found in steamvr.vrsettings.");
            }

            string dir = Path.GetDirectoryName(path) ?? throw new InvalidOperationException("Invalid SteamVR settings path.");
            string backupPath = Path.Combine(dir, $"steamvr.vrsettings.backup-{DateTime.Now:yyyyMMdd-HHmmss}");
            File.Copy(path, backupPath, overwrite: false);

            foreach (var key in new[]
            {
                "allowSupersampleFiltering",
                "disableAsync",
                "enableHomeApp",
                "framesToThrottle",
                "motionSmoothing",
                "showAdvancedSettings",
                "showMirrorView",
                "startCompositorFromAppLaunch",
                "startDashboardFromAppLaunch",
                "startMonitorFromAppLaunch",
                "supersampleManualOverride",
                "supersampleScale"
            })
            {
                steamVr.Remove(key);
            }

            const string throttleShortcutPrefix = "frame_wait_throttle_toggle:";
            if (steamVr["debugCommandShortcuts"] is JsonValue shortcutValue &&
                shortcutValue.TryGetValue<string>(out string? shortcutText))
            {
                var remainingShortcuts = shortcutText.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                    .Where(shortcut => !shortcut.StartsWith(throttleShortcutPrefix, StringComparison.OrdinalIgnoreCase))
                    .ToArray();

                if (remainingShortcuts.Length == 0)
                {
                    steamVr.Remove("debugCommandShortcuts");
                }
                else
                {
                    steamVr["debugCommandShortcuts"] = string.Join(",", remainingShortcuts);
                }
            }

            var options = new JsonSerializerOptions { WriteIndented = true };
            File.WriteAllText(path, root.ToJsonString(options));
            return backupPath;
        }

        private void BtnMasterReset_Click(object? sender, EventArgs e)
        {
            if (!EnsureValidInstallForSave())
                return;

            var result = MessageBox.Show(
                "This will reset all settings and key bindings back to defaults.\n\nAre you sure?",
                "Master Reset",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);

            if (result != DialogResult.Yes) return;

            // Reset ini: clear all lines so ReadFromIni() falls back to every hardcoded default
            _ini.Load("__reset__"); // file doesn't exist — Load() just clears _lines and returns
            _isLoading = true;
            ReadFromIni();
            _isLoading = false;

            // Write defaults to both ini locations. Master Reset is the one
            // save that intentionally produces a clean file, so skip the
            // merge-from-disk (which would resurrect the old contents).
            WriteToIni(mergeFromDisk: false);
            foreach (string path in GetOpenCompositeIniSavePaths(createDirectories: true))
                _ini.Save(path);

            // Reset controlmapvr.txt to embedded default template
            if (_gameType == "skyrim")
            {
                string filePath = GetControlmapSavePath();
                var asm = System.Reflection.Assembly.GetExecutingAssembly();
                using var stream = asm.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt");
                if (stream != null)
                {
                    using var reader = new StreamReader(stream);
                    File.WriteAllText(filePath, reader.ReadToEnd());
                    _keyBindings.Clear();
                    LoadDefaultKeyBindings();
                    TryLoadControlmapVR();
                }
            }

            string time = DateTime.Now.ToString("h:mm:ss tt");
            _lblStatus.Text = $"Reset to defaults at {time}";
            _lblStatus.ForeColor = Color.FromArgb(255, 185, 35);
            MessageBox.Show("All settings and bindings have been reset to defaults.", "Master Reset",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void BtnSettingsMasterReset_Click(object? sender, EventArgs e)
        {
            if (!EnsureValidInstallForSave())
                return;

            var result = MessageBox.Show(
                "This will reset only the Settings page and save opencomposite.ini.\n\nBindings are not touched.\n\nProceed?",
                "Settings Master Reset",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);

            if (result != DialogResult.Yes) return;

            try
            {
                ResetSettingsPageToDefaults();
                WriteToIni();
                var savePaths = GetOpenCompositeIniSavePaths(createDirectories: true).ToList();
                foreach (string path in savePaths)
                    _ini.Save(path);

                string time = DateTime.Now.ToString("h:mm:ss tt");
                string locationMsg = $"Settings reset and saved to {DescribeIniSaveLocations(savePaths)} at {time}";
                _lblStatus.Text = $"{locationMsg} - restart game for changes";
                _lblStatus.ForeColor = Color.FromArgb(255, 185, 35);
                _lblVideoStatus.Text = _lblStatus.Text;
                _lblVideoStatus.ForeColor = _lblStatus.ForeColor;
            }
            catch (Exception ex)
            {
                _lblStatus.Text = $"Settings reset failed: {ex.Message}";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
            }
        }

        private void ResetSettingsPageToDefaults()
        {
            _isLoading = true;
            try
            {
                _chkShortcutEnabled.Checked = true;
                foreach (var kvp in _btnCheckboxMap)
                    kvp.Value().Checked = false;
                _chkLeftStick.Checked = true;
                _rdoX1.Checked = false;
                _rdoX2.Checked = true;
                _rdoX3.Checked = false;
                _rdoX4.Checked = false;
                _nudTiming.Value = 500m;
                _nudDisplayTilt.Value = 22.5m;
                _nudDisplayOpacity.Value = 30m;
                _nudDisplayScale.Value = 100m;
                _cmbKbTheme.SelectedIndex = 0;
                _chkSoundsEnabled.Checked = true;
                _nudSoundVolume.Value = 50m;
                _nudPressVolume.Value = 50m;
                _nudKbHapticStrength.Value = 50m;

                _nudSuperSample.Value = 1.0m;
                _chkRenderHands.Checked = true;
                _chkHaptics.Checked = true;
                _nudHapticStrength.Value = 0.10m;
                _chkCombatHapticShield.Checked = true;
                _chkCombatHapticWeapon.Checked = true;
                _chkCombatHapticBow.Checked = true;
                _chkCombatHapticMagic.Checked = true;
                _nudCombatHapticStrength.Value = 80m;
                _chkHiddenMesh.Checked = true;
                _chkInvertShaders.Checked = false;
                _chkDx10.Checked = false;
                _chkAudioSwitch.Checked = false;
                _txtAudioDevice.Text = "quest";

                _chkInputSmoothing.Checked = false;
                _nudInputWindow.Value = 5m;
                _chkControllerSmoothing.Checked = true;
                _nudPosSmoothMinCutoff.Value = 1.25m;
                _nudPosSmoothBeta.Value = 20.0m;
                _nudRotSmoothMinCutoff.Value = 1.50m;
                _nudRotSmoothBeta.Value = 0.2m;
                SetControllerSmoothingControlsEnabled(true);

                _chkDisableTriggerTouch.Checked = true;
                _chkDisableThumbrestTouch.Checked = true;
                _chkDisableTrackpad.Checked = false;
                _chkVRIKKnuckles.Checked = false;
                _chkGpuTiming.Checked = true;
                _nudLeftDeadZone.Value = 0m;
                _nudRightDeadZone.Value = 0m;

                ResetAxisControls(updateStatus: false);
                UpdateTimingLabel();
                _picControllers?.Invalidate();
                _picBindingsController?.Invalidate();
            }
            finally
            {
                _isLoading = false;
            }
        }

        private void SetControllerSmoothingControlsEnabled(bool enabled)
        {
            _nudPosSmoothMinCutoff.Enabled = enabled;
            _nudPosSmoothBeta.Enabled = enabled;
            _nudRotSmoothMinCutoff.Enabled = enabled;
            _nudRotSmoothBeta.Enabled = enabled;
            _lblPosCutoff.Enabled = enabled;
            _lblPosBeta.Enabled = enabled;
            _lblRotCutoff.Enabled = enabled;
            _lblRotBeta.Enabled = enabled;
        }

        private void BtnSave_Click(object? sender, EventArgs e)
        {
            if (!EnsureValidInstallForSave())
                return;

            var selected = GetSelectedButtons();
            if (selected.Count == 0 && _chkShortcutEnabled.Checked)
            {
                _lblStatus.Text = "Cannot save \u2014 keyboard shortcut is enabled but no buttons are selected";
                _lblStatus.ForeColor = Color.FromArgb(255, 180, 80);
                _lblVideoStatus.Text = _lblStatus.Text;
                _lblVideoStatus.ForeColor = _lblStatus.ForeColor;
                return;
            }

            try
            {
                WriteToIni();
                var savePaths = GetOpenCompositeIniSavePaths(createDirectories: true).ToList();
                foreach (string path in savePaths)
                    _ini.Save(path);

                string time = DateTime.Now.ToString("h:mm:ss tt");
                string locationMsg = $"Saved to {DescribeIniSaveLocations(savePaths)} at {time}";
                _lblStatus.Text = $"{locationMsg} \u2014 restart game for changes";
                _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
                _lblVideoStatus.Text = $"{locationMsg} \u2014 restart game for changes";
                _lblVideoStatus.ForeColor = Color.FromArgb(100, 200, 100);
                ClearDirty();
            }
            catch (Exception ex)
            {
                _lblStatus.Text = $"Save failed: {ex.Message}";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
                _lblVideoStatus.Text = _lblStatus.Text;
                _lblVideoStatus.ForeColor = _lblStatus.ForeColor;
            }
        }

        private void BtnReload_Click(object? sender, EventArgs e)
        {
            LoadFromDir();
            ClearDirty();   // reloaded state matches disk — no unsaved changes
        }

        private void MarkDirty()
        {
            if (_isLoading || _dirty) return;
            _dirty = true;
            _lblUnsavedBanner.Visible = true;
            _lblUnsavedBanner.BringToFront();
            _flashOn = true;
            _lblUnsavedBanner.ForeColor = Color.FromArgb(255, 205, 60);
            _breatheTimer.Start();
        }

        private void ClearDirty()
        {
            _dirty = false;
            _breatheTimer.Stop();
            _lblUnsavedBanner.Visible = false;
        }

        // Subscribe dirty-tracking to every settings input; skip the Bindings tab (own save + nav combos)
        private void WireDirtyTracking(Control root)
        {
            foreach (Control c in root.Controls)
            {
                if (c == _tabKeyboard) continue;
                if (c == _tabGestures) continue; // own save flow, not part of the ini
                if (c == _chkShowQuadGrid) continue; // self-saving, applies instantly
                // Body Tracking self-savers (persist to ui.json on Start/close;
                // not part of the ini). Only "Send trackers to the game" and
                // "Enable Full-Body Walking" genuinely need Save.
                if (c == _cmbBodyCamera || c == _txtBodyCamUrl || c == _chkBodyMirror
                    || c == _chkBodyStream || c == _cmbBodyDevice || c == _cmbBodyPoseSource
                    || c == _nudBodyOffX || c == _nudBodyOffY) continue;
                switch (c)
                {
                    case CheckBox cb:     cb.CheckedChanged += (s, e) => MarkDirty(); break;
                    case RadioButton rb:  rb.CheckedChanged += (s, e) => MarkDirty(); break;
                    case NumericUpDown n: n.ValueChanged += (s, e) => MarkDirty(); break;
                    case TrackBar t:      t.ValueChanged += (s, e) => MarkDirty(); break;
                    case ComboBox cmb:    cmb.SelectedIndexChanged += (s, e) => MarkDirty(); break;
                    case TextBox tb:      tb.TextChanged += (s, e) => MarkDirty(); break;
                }
                if (c.HasChildren) WireDirtyTracking(c);
            }
        }

        // Color-flash: alternate the big bold letters between gold and orange-red (~1Hz, attention-grabbing, not a strobe)
        private void BreatheTimer_Tick(object? sender, EventArgs e)
        {
            _flashOn = !_flashOn;
            _lblUnsavedBanner.ForeColor = _flashOn ? Color.FromArgb(255, 205, 60) : Color.FromArgb(235, 75, 40);
        }

        private static Color BannerLerp(Color a, Color b, double t)
        {
            return Color.FromArgb(
                (int)(a.R + (b.R - a.R) * t),
                (int)(a.G + (b.G - a.G) * t),
                (int)(a.B + (b.B - a.B) * t));
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (_dirty)
            {
                var r = MessageBox.Show(
                    "You have unsaved changes.\n\nSave before exiting?",
                    "Unsaved Changes",
                    MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
                if (r == DialogResult.Cancel)
                    e.Cancel = true;
                else if (r == DialogResult.Yes)
                {
                    BtnSave_Click(this, EventArgs.Empty);
                    if (_dirty) e.Cancel = true;   // save didn't complete — keep window open, don't lose changes
                }
            }
            base.OnFormClosing(e);
        }

        private void LoadFromDir()
        {
            string loadPath = GetOpenCompositeIniLoadPath();
            if (string.IsNullOrEmpty(loadPath))
            {
                _ini.Load("__invalid_install__");
                _isLoading = true;
                _lblStatus.Text = "Invalid install - Configurator EXE must stay in the OCU mod folder.";
                _lblStatus.ForeColor = Color.FromArgb(255, 100, 100);
                _lblVideoStatus.Text = _lblStatus.Text;
                _lblVideoStatus.ForeColor = _lblStatus.ForeColor;
                ReadFromIni();
                TryLoadControlmapVR();
                _isLoading = false;
                return;
            }

            _ini.Load(loadPath);
            _isLoading = true;

            if (File.Exists(loadPath))
            {
                _lblStatus.Text = $"Loaded {loadPath}";
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
            _isLoading = false;

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

            string trackpadSwipe = _ini.Get("keyboard", "shortcutTrackpad", "none").ToLowerInvariant();
            _cmbTrackpadSwipe.SelectedIndex = trackpadSwipe switch
            {
                "swipe_up" => 1, "swipe_down" => 2, _ => 0
            };

            _chkGestureSounds.Checked = ParseBool(_ini.Get("keyboard", "gestureSounds", "true"));
            _cmbFinishSound.SelectedIndex =
                _ini.Get("keyboard", "gestureFinishSound", "impact").ToLowerInvariant() == "dark" ? 1 : 0;

            if (TryParseIniFloat(_ini.Get("keyboard", "displayTilt", "22.5"), out float dt))
                _nudDisplayTilt.Value = (decimal)Math.Clamp(dt, -30f, 80f);
            if (int.TryParse(_ini.Get("keyboard", "displayOpacity", "30"), out int dop))
                _nudDisplayOpacity.Value = Math.Clamp(dop, 1, 100);
            if (int.TryParse(_ini.Get("keyboard", "displayScale", "100"), out int dsc))
                _nudDisplayScale.Value = Math.Clamp(dsc, 50, 150);

            int kbThemeIndex = _ini.Get("keyboard", "theme", "parchment").ToLowerInvariant() switch
            {
                "skyui" => 1, "dwemer" => 2, "sovngarde" => 3, _ => 0
            };
            // The public build lists Parchment only; clamp so a saved dev
            // theme cannot index past the shortened list.
            _cmbKbTheme.SelectedIndex = kbThemeIndex < _cmbKbTheme.Items.Count ? kbThemeIndex : 0;

            _chkSoundsEnabled.Checked = ParseBool(_ini.Get("keyboard", "soundsEnabled", "true"));
            if (int.TryParse(_ini.Get("keyboard", "soundVolume", "50"), out int svol))
                _nudSoundVolume.Value = Math.Clamp(svol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "pressVolume", "50"), out int pvol))
                _nudPressVolume.Value = Math.Clamp(pvol, 0, 100);
            if (int.TryParse(_ini.Get("keyboard", "hapticStrength", "50"), out int kbhap))
                _nudKbHapticStrength.Value = Math.Clamp(kbhap, 0, 100);

            if (TryParseIniFloat(_ini.Get("", "supersampleRatio", "1.0"), out float ss))
                _nudSuperSample.Value = (decimal)Math.Clamp(ss, 0.5f, 2.0f);
            _chkRenderHands.Checked = ParseBool(_ini.Get("", "renderCustomHands", "true"));
            _chkHaptics.Checked = ParseBool(_ini.Get("", "haptics", "true"));
            if (!ShowDevTools)
                _chkHaptics.Checked = false;
            if (TryParseIniFloat(_ini.Get("", "hapticStrength", "0.1"), out float hs))
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
            _chkControllerSmoothing.Checked = ParseBool(_ini.Get("", "enableControllerSmoothing", "true"));
            if (float.TryParse(_ini.Get("", "posSmoothMinCutoff", "1.25"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float pmc))
                _nudPosSmoothMinCutoff.Value = (decimal)Math.Clamp(pmc, 0.01f, 20f);
            if (float.TryParse(_ini.Get("", "posSmoothBeta", "20"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float pb))
                _nudPosSmoothBeta.Value = (decimal)Math.Clamp(pb, 0f, 100f);
            if (float.TryParse(_ini.Get("", "rotSmoothMinCutoff", "1.5"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rmc))
                _nudRotSmoothMinCutoff.Value = (decimal)Math.Clamp(rmc, 0.01f, 20f);
            if (float.TryParse(_ini.Get("", "rotSmoothBeta", "0.2"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rb))
                _nudRotSmoothBeta.Value = (decimal)Math.Clamp(rb, 0f, 10f);
            // Sync enable state of smoothing controls
            {
                bool en = _chkControllerSmoothing.Checked;
                _nudPosSmoothMinCutoff.Enabled = en;
                _nudPosSmoothBeta.Enabled = en;
                _nudRotSmoothMinCutoff.Enabled = en;
                _nudRotSmoothBeta.Enabled = en;
                _lblPosCutoff.Enabled = en;
                _lblPosBeta.Enabled = en;
                _lblRotCutoff.Enabled = en;
                _lblRotBeta.Enabled = en;
            }
            _chkDisableTriggerTouch.Checked = ParseBool(_ini.Get("", "disableTriggerTouch", "true"));
            _chkDisableThumbrestTouch.Checked = ParseBool(_ini.Get("", "disableThumbrestTouch", "true"));
            _chkDisableTrackpad.Checked = ParseBool(_ini.Get("", "disableTrackPad", "false"));
            _chkVRIKKnuckles.Checked = ParseBool(_ini.Get("", "enableVRIKKnucklesTrackPadSupport", "false"));

            // Load any user-saved presets from %AppData% before we try to restore
            // the saved selection — otherwise a saved user-preset name wouldn't be
            // findable in the dropdown items.
            LoadUserBindingPresets();

            // Restore the binding-preset dropdown from the persisted choice. If absent
            // or unrecognized, keep the default (VRIK V2.1.0) chosen at construction.
            string savedPreset = _ini.Get("Configurator", "activeBindingPreset", "");
            if (!string.IsNullOrEmpty(savedPreset))
            {
                int idx = _cmbBindingPreset.Items.IndexOf(savedPreset);
                if (idx >= 0) _cmbBindingPreset.SelectedIndex = idx;
            }
            UpdateDeletePresetEnabled();
            _chkGpuTiming.Checked = ParseBool(_ini.Get("", "enableGpuTiming", "true"));
            _chkCombatHapticShield.Checked = ParseBool(_ini.Get("", "combatHapticShield", "true"));
            _chkCombatHapticWeapon.Checked = ParseBool(_ini.Get("", "combatHapticWeapon", "true"));
            _chkCombatHapticBow.Checked = ParseBool(_ini.Get("", "combatHapticBow", "true"));
            _chkCombatHapticMagic.Checked = ParseBool(_ini.Get("", "combatHapticMagic", "true"));
            _chkMenuLaserEnabled.Checked = ParseBool(_ini.Get("", "menuLaserEnabled", "true"));
            _chkNetTrackersEnabled.Checked = ParseBool(_ini.Get("", "networkTrackersEnabled", "false"));
            _chkCameraLegCalibration.Checked = ParseBool(_ini.Get("", "cameraLegCalibrationEnabled", "true"));
            _chkWalkInPlace.Checked = ParseBool(_ini.Get("", "walkInPlaceEnabled", "false"));
            if (!ShowDevTools)
            {
                // Camera FBT and walk-in-place are shelved from the public
                // build until they pass in-headset testing; a Save writes
                // them disabled. devtools.on restores the whole surface.
                _chkCameraLegCalibration.Checked = false;
                _chkNetTrackersEnabled.Checked = false;
                _chkWalkInPlace.Checked = false;
            }
            SelectWalkActivation(_ini.Get("", "walkInPlaceActivation", "none"));
            if (int.TryParse(_ini.Get("", "combatHapticStrength", "80"), out int chs))
                _nudCombatHapticStrength.Value = Math.Clamp(chs, 0, 100);
            if (TryParseIniFloat(_ini.Get("", "leftDeadZoneSize", "0.0"), out float ldz))
                _nudLeftDeadZone.Value = (decimal)Math.Clamp(ldz, 0f, 1f);
            if (TryParseIniFloat(_ini.Get("", "rightDeadZoneSize", "0.0"), out float rdz))
                _nudRightDeadZone.Value = (decimal)Math.Clamp(rdz, 0f, 1f);

            // Controller axis adjustments
            _chkAdjustTilt.Checked = ParseBool(_ini.Get("", "adjustTilt", "false"));
            if (float.TryParse(_ini.Get("", "tilt", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float tiltVal))
                _nudTiltDeg.Value = (decimal)Math.Clamp(tiltVal, -90f, 90f);
            _nudTiltDeg.Enabled = _chkAdjustTilt.Checked;

            _chkLeftRotation.Checked = ParseBool(_ini.Get("", "adjustLeftRotation", "false"));
            if (float.TryParse(_ini.Get("", "leftXRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lrx))
                _nudLeftRotX.Value = (decimal)Math.Clamp(lrx, -90f, 90f);
            if (float.TryParse(_ini.Get("", "leftYRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lry))
                _nudLeftRotY.Value = (decimal)Math.Clamp(lry, -90f, 90f);
            if (float.TryParse(_ini.Get("", "leftZRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lrz))
                _nudLeftRotZ.Value = (decimal)Math.Clamp(lrz, -90f, 90f);
            _nudLeftRotX.Enabled = _chkLeftRotation.Checked;
            _nudLeftRotY.Enabled = _chkLeftRotation.Checked;
            _nudLeftRotZ.Enabled = _chkLeftRotation.Checked;

            _chkRightRotation.Checked = ParseBool(_ini.Get("", "adjustRightRotation", "false"));
            if (float.TryParse(_ini.Get("", "rightXRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rrx))
                _nudRightRotX.Value = (decimal)Math.Clamp(rrx, -90f, 90f);
            if (float.TryParse(_ini.Get("", "rightYRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rry))
                _nudRightRotY.Value = (decimal)Math.Clamp(rry, -90f, 90f);
            if (float.TryParse(_ini.Get("", "rightZRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rrz))
                _nudRightRotZ.Value = (decimal)Math.Clamp(rrz, -90f, 90f);
            _nudRightRotX.Enabled = _chkRightRotation.Checked;
            _nudRightRotY.Enabled = _chkRightRotation.Checked;
            _nudRightRotZ.Enabled = _chkRightRotation.Checked;

            _chkLeftPosition.Checked = ParseBool(_ini.Get("", "adjustLeftPosition", "false"));
            if (float.TryParse(_ini.Get("", "leftXPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lpx))
                _nudLeftPosX.Value = (decimal)Math.Clamp(lpx, -0.5f, 0.5f);
            if (float.TryParse(_ini.Get("", "leftYPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lpy))
                _nudLeftPosY.Value = (decimal)Math.Clamp(lpy, -0.5f, 0.5f);
            if (float.TryParse(_ini.Get("", "leftZPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float lpz))
                _nudLeftPosZ.Value = (decimal)Math.Clamp(lpz, -0.5f, 0.5f);
            _nudLeftPosX.Enabled = _chkLeftPosition.Checked;
            _nudLeftPosY.Enabled = _chkLeftPosition.Checked;
            _nudLeftPosZ.Enabled = _chkLeftPosition.Checked;

            _chkRightPosition.Checked = ParseBool(_ini.Get("", "adjustRightPosition", "false"));
            if (float.TryParse(_ini.Get("", "rightXPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rpx))
                _nudRightPosX.Value = (decimal)Math.Clamp(rpx, -0.5f, 0.5f);
            if (float.TryParse(_ini.Get("", "rightYPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rpy))
                _nudRightPosY.Value = (decimal)Math.Clamp(rpy, -0.5f, 0.5f);
            if (float.TryParse(_ini.Get("", "rightZPosition", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rpz))
                _nudRightPosZ.Value = (decimal)Math.Clamp(rpz, -0.5f, 0.5f);
            _nudRightPosX.Enabled = _chkRightPosition.Checked;
            _nudRightPosY.Enabled = _chkRightPosition.Checked;
            _nudRightPosZ.Enabled = _chkRightPosition.Checked;

            _chkLeftLaserRotation.Checked = ParseBool(_ini.Get("", "adjustLeftLaserRotation", "false"));
            if (float.TryParse(_ini.Get("", "leftLaserXRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float llrx))
                _nudLeftLaserRotX.Value = (decimal)Math.Clamp(llrx, -90f, 90f);
            if (float.TryParse(_ini.Get("", "leftLaserYRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float llry))
                _nudLeftLaserRotY.Value = (decimal)Math.Clamp(llry, -90f, 90f);
            if (float.TryParse(_ini.Get("", "leftLaserZRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float llrz))
                _nudLeftLaserRotZ.Value = (decimal)Math.Clamp(llrz, -90f, 90f);
            _nudLeftLaserRotX.Enabled = _chkLeftLaserRotation.Checked;
            _nudLeftLaserRotY.Enabled = _chkLeftLaserRotation.Checked;
            _nudLeftLaserRotZ.Enabled = _chkLeftLaserRotation.Checked;

            _chkRightLaserRotation.Checked = ParseBool(_ini.Get("", "adjustRightLaserRotation", "false"));
            if (float.TryParse(_ini.Get("", "rightLaserXRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rlrx))
                _nudRightLaserRotX.Value = (decimal)Math.Clamp(rlrx, -90f, 90f);
            if (float.TryParse(_ini.Get("", "rightLaserYRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rlry))
                _nudRightLaserRotY.Value = (decimal)Math.Clamp(rlry, -90f, 90f);
            if (float.TryParse(_ini.Get("", "rightLaserZRotation", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float rlrz))
                _nudRightLaserRotZ.Value = (decimal)Math.Clamp(rlrz, -90f, 90f);
            _nudRightLaserRotX.Enabled = _chkRightLaserRotation.Checked;
            _nudRightLaserRotY.Enabled = _chkRightLaserRotation.Checked;
            _nudRightLaserRotZ.Enabled = _chkRightLaserRotation.Checked;

            _suppressQuadGridWrite = true;
            _chkShowQuadGrid.Checked = GetMenuQuadFlag("show_calibration_quad", true);
            _suppressQuadGridWrite = false;

            // FSR settings
            _chkFsrEnabled.Checked = ParseBool(_ini.Get("", "fsrEnabled", "false"));
            _chkFsrNativeAA.Checked = ParseBool(_ini.Get("", "fsrNativeAA", "false"));
            if (float.TryParse(_ini.Get("", "fsrRenderScale", "0.67"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float frs))
                _nudFsrRenderScale.Value = (decimal)Math.Clamp(frs, 0.33f, 1.0f);
            {
                bool en = _chkFsrEnabled.Checked;
                _chkFsrNativeAA.Enabled = en;
                _nudFsrRenderScale.Enabled = en && !_chkFsrNativeAA.Checked;
            }
            // DLSS settings
            _chkDlssEnabled.Checked = ParseBool(_ini.Get("", "dlssEnabled", "false"));
            if (int.TryParse(_ini.Get("", "dlssPreset", "1"), out int dlssPreset))
                _cmbDlssPreset.SelectedIndex = Math.Clamp(dlssPreset, 0, 5);
            string dlssModel = _ini.Get("", "dlssModel", "K").Trim();
            _cmbDlssModel.SelectedIndex = dlssModel.Equals("default", StringComparison.OrdinalIgnoreCase) || dlssModel.Equals("auto", StringComparison.OrdinalIgnoreCase) || dlssModel == "0"
                ? 0
                : Math.Max(0, _cmbDlssModel.Items.IndexOf(dlssModel.ToUpperInvariant()));
            if (float.TryParse(_ini.Get("", "dlssRenderScaleOverride", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float drso))
                _nudDlssRenderScaleOverride.Value = (decimal)Math.Clamp(drso, 0f, 1f);
            if (float.TryParse(_ini.Get("", "dlssMvScale", "1.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float dmv))
                _nudDlssMvScale.Value = (decimal)Math.Clamp(dmv, 0f, 2f);
            if (float.TryParse(_ini.Get("", "dlssJitterScale", "0.4"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float djs))
                _nudDlssJitterScale.Value = (decimal)Math.Clamp(djs, 0f, 1f);
            _chkDlssNgxVerboseLogging.Checked = ParseBool(_ini.Get("", "dlssNgxVerboseLogging", "false"));
            { bool en = _chkDlssEnabled.Checked; _cmbDlssPreset.Enabled = en; _cmbDlssModel.Enabled = en; _nudDlssRenderScaleOverride.Enabled = en; _nudDlssMvScale.Enabled = en; _nudDlssJitterScale.Enabled = en; }

            _chkMotionVectorsEnabled.Checked = ParseBool(_ini.Get("", "motionVectorsEnabled", "true"));
            _chkActorMV.Checked = ParseBool(_ini.Get("", "actorMV", "true"));
            if (float.TryParse(_ini.Get("", "motionVectorScale", "1.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float mvs))
                _nudMotionVectorScale.Value = (decimal)Math.Clamp(mvs, 0.1f, 2.0f);
            _chkAswEnabled.Checked = ParseBool(_ini.Get("", "aswEnabled", "false"));
            _chkAswExperimentalMode.Checked = ParseBool(_ini.Get("", "aswExperimentalMode", "false"));
            _chkAswBufferEnabled.Checked = ParseBool(_ini.Get("", "aswBufferEnabled", "false"));
            _chkAswUpscalerReset.Checked = ParseBool(_ini.Get("", "aswUpscalerReset", "false"));
            _chkAswUpscalerReactiveMask.Checked = ParseBool(_ini.Get("", "aswUpscalerReactiveMask", "true"));
            if (float.TryParse(_ini.Get("", "aswWarpStrength", "1.00"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float aws))
                _nudAswWarpStrength.Value = (decimal)Math.Clamp(aws, 0f, 3f);
            if (float.TryParse(_ini.Get("", "aswRotationScale", "1.00"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float ars))
                _nudAswRotationScale.Value = (decimal)Math.Clamp(ars, 0f, 2f);
            if (float.TryParse(_ini.Get("", "aswTranslationScale", "1.00"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float ats))
                _nudAswTranslationScale.Value = (decimal)Math.Clamp(ats, 0f, 3f);
            if (float.TryParse(_ini.Get("", "aswLocoScale", "0.50"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float alsNud))
                _nudAswLocoScale.Value = (decimal)Math.Clamp(alsNud, 0f, 3f);
            if (float.TryParse(_ini.Get("", "aswDepthScale", "1.00"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float ads))
                _nudAswDepthScale.Value = (decimal)Math.Clamp(ads, 0f, 2f);
            if (float.TryParse(_ini.Get("", "aswNearFadeDepth", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float anfd))
                _aswNearFadeDepth = Math.Clamp(anfd, 0f, 10f);
            if (float.TryParse(_ini.Get("", "aswFPControllerScale", "0.45"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float afp))
                _aswFPControllerScale = Math.Clamp(afp, 0f, 3f);
            if (float.TryParse(_ini.Get("", "aswEdgeFadeWidth", "3.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float aef))
                _aswEdgeFadeWidth = Math.Clamp(aef, 0f, 10f);
            if (float.TryParse(_ini.Get("", "aswMVConfidence", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float amc))
                _aswMVConfidence = Math.Clamp(amc, 0f, 5f);
            if (float.TryParse(_ini.Get("", "aswMVPixelScale", "1.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float amps))
                _aswMVPixelScale = Math.Clamp(amps, 0f, 3f);
            _chkAswAutoNative.Checked = ParseBool(_ini.Get("", "aswAutoNative", "false"));
            if (float.TryParse(_ini.Get("", "aswAutoEngageFps", "50"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float aengf))
                _nudAswAutoEngageFps.Value = (decimal)Math.Clamp(aengf, 20f, 90f);
            _chkAswDebugMode.Checked = _ini.Get("", "aswDebugMode", "0").Trim() == "10";
            if (float.TryParse(_ini.Get("", "triggerDeadzone", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float tdz))
                _triggerDeadzone = Math.Clamp(tdz, 0f, 0.5f);
            if (float.TryParse(_ini.Get("", "triggerMax", "1.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float tmax))
                _triggerMax = Math.Clamp(tmax, 0.5f, 1.0f);
            if (float.TryParse(_ini.Get("", "fsr3Sharpness", "0.3"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float f3s))
                _nudFsr3Sharpness.Value = (decimal)Math.Clamp(f3s, 0f, 1f);
            if (float.TryParse(_ini.Get("", "fsr3JitterScale", "0.3"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float fjs))
                _nudFsr3JitterScale.Value = (decimal)Math.Clamp(fjs, 0f, 1f);
            _chkFsr3JitterCancellation.Checked = ParseBool(_ini.Get("", "fsr3JitterCancellation", "false"));
            _chkFsr3CameraMV.Checked = ParseBool(_ini.Get("", "fsr3CameraMV", "true"));
            if (float.TryParse(_ini.Get("", "fsr3ViewToMeters", "0.01428"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float fv2m))
                _nudFsr3ViewToMeters.Value = (decimal)Math.Clamp(fv2m, 0.001f, 0.1f);
            if (float.TryParse(_ini.Get("", "fsr3ReactivenessScale", "2.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float frct))
                _nudFsr3ReactivenessScale.Value = (decimal)Math.Clamp(frct, 0f, 8f);
            if (float.TryParse(_ini.Get("", "fsr3ShadingChangeScale", "2.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float fshade))
                _nudFsr3ShadingChangeScale.Value = (decimal)Math.Clamp(fshade, 0f, 8f);
            if (float.TryParse(_ini.Get("", "fsr3AccumulationPerFrame", "0.20"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float facc))
                _nudFsr3AccumulationPerFrame.Value = (decimal)Math.Clamp(facc, 0f, 1f);
            if (int.TryParse(_ini.Get("", "fsr3DebugMode", "0"), out int fdbg))
                _cmbFsr3DebugMode.SelectedIndex = Math.Clamp(fdbg, 0, _cmbFsr3DebugMode.Items.Count - 1);
            UpdateFsrStatus();

            // CAS settings
            _chkCasEnabled.Checked = ParseBool(_ini.Get("", "casEnabled", "false"));
            if (float.TryParse(_ini.Get("", "casSharpness", _ini.Get("", "fsrSharpness", "0.5")), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float fsh))
                _nudCasSharpness.Value = (decimal)Math.Clamp(fsh, 0f, 1f);
            {
                bool en = _chkCasEnabled.Checked;
                _nudCasSharpness.Enabled = en;
            }

            _chkBlueSkyDefenderEnabled.Checked = ParseBool(_ini.Get("", "blueSkyDefenderEnabled", _ini.Get("", "fsr3PostAAEnabled", "false")));
            if (float.TryParse(_ini.Get("", "blueSkyDefenderLambda", _ini.Get("", "fsr3PostAALambda", "3.0")), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float bsl))
                _nudBlueSkyLambda.Value = (decimal)Math.Clamp(bsl, 1f, 6f);
            if (float.TryParse(_ini.Get("", "blueSkyDefenderEpsilon", _ini.Get("", "fsr3PostAAEpsilon", "0.10")), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float bse))
                _nudBlueSkyEpsilon.Value = (decimal)Math.Clamp(bse, 0.01f, 0.50f);
            {
                bool en = _chkBlueSkyDefenderEnabled.Checked;
                _nudBlueSkyLambda.Enabled = en;
                _nudBlueSkyEpsilon.Enabled = en;
            }

            // MIP bias settings
            _chkMipBiasEnabled.Checked = ParseBool(_ini.Get("", "mipBiasEnabled", "true"));
            string mipBias = _ini.Get("", "mipBias", "auto").Trim();
            if (mipBias.Equals("off", StringComparison.OrdinalIgnoreCase)
                || mipBias.Equals("false", StringComparison.OrdinalIgnoreCase)
                || mipBias.Equals("disabled", StringComparison.OrdinalIgnoreCase)
                || mipBias.Equals("none", StringComparison.OrdinalIgnoreCase))
            {
                _cmbMipBiasMode.SelectedIndex = 1;
            }
            else if (mipBias.Equals("auto", StringComparison.OrdinalIgnoreCase) || mipBias.Equals("default", StringComparison.OrdinalIgnoreCase) || mipBias.Length == 0)
            {
                _cmbMipBiasMode.SelectedIndex = 0;
            }
            else
            {
                _cmbMipBiasMode.SelectedIndex = 2;
                if (float.TryParse(mipBias, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float mbf))
                    _nudMipBiasFixed.Value = (decimal)Math.Clamp(mbf, -4f, 4f);
            }
            if (float.TryParse(_ini.Get("", "mipBiasOffset", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float mbo))
                _nudMipBiasOffset.Value = (decimal)Math.Clamp(mbo, -3f, 3f);
            if (float.TryParse(_ini.Get("", "dlssMipBiasOffset", "0.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float dmbo))
                _nudDlssMipBiasOffset.Value = (decimal)Math.Clamp(dmbo, -3f, 3f);
            if (float.TryParse(_ini.Get("", "fsr3MipBiasOffset", "1.0"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float fmbo))
                _nudFsr3MipBiasOffset.Value = (decimal)Math.Clamp(fmbo, -3f, 3f);
            {
                bool en = _chkMipBiasEnabled.Checked;
                _cmbMipBiasMode.Enabled = en;
                _nudMipBiasFixed.Enabled = en && _cmbMipBiasMode.SelectedIndex == 2;
                _nudMipBiasOffset.Enabled = en;
                _nudDlssMipBiasOffset.Enabled = en;
                _nudFsr3MipBiasOffset.Enabled = en;
            }

            // VRS settings
            _chkVrsEnabled.Checked = ParseBool(_ini.Get("", "vrsEnabled", "false"));
            if (float.TryParse(_ini.Get("", "vrsInnerRadius", "0.60"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float vrsIR))
                _nudVrsInnerRadius.Value = (decimal)Math.Clamp(vrsIR, 0.10f, 1.00f);
            if (float.TryParse(_ini.Get("", "vrsMidRadius", "0.80"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float vrsMR))
                _nudVrsMidRadius.Value = (decimal)Math.Clamp(vrsMR, 0.10f, 1.50f);
            if (float.TryParse(_ini.Get("", "vrsOuterRadius", "1.00"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float vrsOR))
                _nudVrsOuterRadius.Value = (decimal)Math.Clamp(vrsOR, 0.10f, 1.50f);
            _chkVrsFavorHorizontal.Checked = ParseBool(_ini.Get("", "vrsFavorHorizontal", "true"));
            {
                bool en = _chkVrsEnabled.Checked;
                _cboVrsPreset.Enabled = en;
                _nudVrsInnerRadius.Enabled = en;
                _nudVrsMidRadius.Enabled = en;
                _nudVrsOuterRadius.Enabled = en;
                _chkVrsFavorHorizontal.ForeColor = en ? Color.FromArgb(210, 210, 210) : Color.FromArgb(90, 90, 90);
                _chkVrsFavorHorizontal.AutoCheck = en;
            }
            // Detect which preset matches
            _cboVrsPreset.SelectedIndex = DetectVrsPreset();

            UpdateTimingLabel();
            _picControllers.Invalidate();

            ReadCombosFromIni();
            _isLoading = false;
        }

        private void WriteToIni(bool mergeFromDisk = true)
        {
            // Save = fresh disk content + UI values laid on top. Rebuilding
            // from UI state alone (the old Reset() here) destroyed every key
            // the Configurator doesn't model — hand-added DLL calibration
            // (renderModel* trim, laserOriginDown), [combos], comments — on
            // every Save ("config-eater"). Reloading first also picks up keys
            // added to the file while the Configurator was open.
            if (mergeFromDisk)
            {
                string diskPath = GetOpenCompositeIniLoadPath();
                if (!string.IsNullOrEmpty(diskPath) && File.Exists(diskPath))
                    _ini.Load(diskPath);
                else
                    _ini.Reset();
            }
            else
            {
                _ini.Reset();
            }

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
            _ini.Set("keyboard", "shortcutTrackpad", _cmbTrackpadSwipe.SelectedIndex switch
            {
                1 => "swipe_up", 2 => "swipe_down", _ => "none"
            });
            _ini.Set("keyboard", "gestureSounds", _chkGestureSounds.Checked ? "true" : "false");
            _ini.Set("keyboard", "gestureFinishSound", _cmbFinishSound.SelectedIndex == 1 ? "dark" : "impact");
            _ini.Set("keyboard", "displayTilt", _nudDisplayTilt.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("keyboard", "displayOpacity", ((int)_nudDisplayOpacity.Value).ToString());
            _ini.Set("keyboard", "displayScale", ((int)_nudDisplayScale.Value).ToString());
            _ini.Set("keyboard", "theme", _cmbKbTheme.SelectedIndex switch
            {
                1 => "skyui", 2 => "dwemer", 3 => "sovngarde", _ => "parchment"
            });
            _ini.Set("keyboard", "soundsEnabled", _chkSoundsEnabled.Checked ? "true" : "false");
            _ini.Set("keyboard", "soundVolume", ((int)_nudSoundVolume.Value).ToString());
            _ini.Set("keyboard", "pressVolume", ((int)_nudPressVolume.Value).ToString());
            _ini.Set("keyboard", "hapticStrength", ((int)_nudKbHapticStrength.Value).ToString());

            _ini.Set("", "supersampleRatio", _nudSuperSample.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "renderCustomHands", _chkRenderHands.Checked ? "true" : "false");
            _ini.Set("", "haptics", _chkHaptics.Checked ? "true" : "false");
            _ini.Set("", "menuLaserEnabled", _chkMenuLaserEnabled.Checked ? "true" : "false");
            // Always-visible control (both games) — must write unconditionally to match the unconditional read
            _ini.Set("", "hapticStrength", _nudHapticStrength.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "enableHiddenMeshFix", _chkHiddenMesh.Checked ? "true" : "false");
            _ini.Set("", "invertUsingShaders", _chkInvertShaders.Checked ? "true" : "false");
            _ini.Set("", "dx10Mode", _chkDx10.Checked ? "true" : "false");
            _ini.Set("", "enableAudioSwitch", _chkAudioSwitch.Checked ? "true" : "false");
            _ini.Set("", "audioDeviceName", _txtAudioDevice.Text);

            // Legacy Post-AA DLAA UI was removed. NVIDIA DLAA is now selected with dlssPreset=4.
            _ini.Set("", "dlaaEnabled", "false");
            _ini.Remove("", "dlaaLambda");
            _ini.Remove("", "dlaaEpsilon");
            _ini.Remove("", "dlssSharpness");

            if (_gameType == "skyrim")
            {
                _ini.Set("", "enableInputSmoothing", _chkInputSmoothing.Checked ? "true" : "false");
                _ini.Set("", "inputWindowSize", ((int)_nudInputWindow.Value).ToString());
                _ini.Set("", "enableControllerSmoothing", _chkControllerSmoothing.Checked ? "true" : "false");
                _ini.Set("", "posSmoothMinCutoff", _nudPosSmoothMinCutoff.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "posSmoothBeta", _nudPosSmoothBeta.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rotSmoothMinCutoff", _nudRotSmoothMinCutoff.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rotSmoothBeta", _nudRotSmoothBeta.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "disableTriggerTouch", _chkDisableTriggerTouch.Checked ? "true" : "false");
                _ini.Set("", "disableThumbrestTouch", _chkDisableThumbrestTouch.Checked ? "true" : "false");
                _ini.Set("", "disableTrackPad", _chkDisableTrackpad.Checked ? "true" : "false");
                _ini.Set("", "enableVRIKKnucklesTrackPadSupport", _chkVRIKKnuckles.Checked ? "true" : "false");
                _ini.Set("", "enableGpuTiming", _chkGpuTiming.Checked ? "true" : "false");
                _ini.Set("", "combatHapticShield", _chkCombatHapticShield.Checked ? "true" : "false");
                _ini.Set("", "combatHapticWeapon", _chkCombatHapticWeapon.Checked ? "true" : "false");
                _ini.Set("", "combatHapticBow", _chkCombatHapticBow.Checked ? "true" : "false");
                _ini.Set("", "combatHapticMagic", _chkCombatHapticMagic.Checked ? "true" : "false");
                _ini.Set("", "networkTrackersEnabled", _chkNetTrackersEnabled.Checked ? "true" : "false");
                _ini.Set("", "cameraLegCalibrationEnabled", _chkCameraLegCalibration.Checked ? "true" : "false");
                _ini.Set("", "walkInPlaceEnabled", _chkWalkInPlace.Checked ? "true" : "false");
                _ini.Set("", "walkInPlaceActivation", WalkActivationKey());
                _ini.Set("", "combatHapticStrength", ((int)_nudCombatHapticStrength.Value).ToString());
                _ini.Set("", "leftDeadZoneSize", _nudLeftDeadZone.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightDeadZoneSize", _nudRightDeadZone.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));

                // Controller axis adjustments
                _ini.Set("", "adjustTilt", _chkAdjustTilt.Checked ? "true" : "false");
                _ini.Set("", "tilt", _nudTiltDeg.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustLeftRotation", _chkLeftRotation.Checked ? "true" : "false");
                _ini.Set("", "leftXRotation", _nudLeftRotX.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftYRotation", _nudLeftRotY.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftZRotation", _nudLeftRotZ.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustRightRotation", _chkRightRotation.Checked ? "true" : "false");
                _ini.Set("", "rightXRotation", _nudRightRotX.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightYRotation", _nudRightRotY.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightZRotation", _nudRightRotZ.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustLeftPosition", _chkLeftPosition.Checked ? "true" : "false");
                _ini.Set("", "leftXPosition", _nudLeftPosX.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftYPosition", _nudLeftPosY.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftZPosition", _nudLeftPosZ.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustRightPosition", _chkRightPosition.Checked ? "true" : "false");
                _ini.Set("", "rightXPosition", _nudRightPosX.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightYPosition", _nudRightPosY.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightZPosition", _nudRightPosZ.Value.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustLeftLaserRotation", _chkLeftLaserRotation.Checked ? "true" : "false");
                _ini.Set("", "leftLaserXRotation", _nudLeftLaserRotX.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftLaserYRotation", _nudLeftLaserRotY.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "leftLaserZRotation", _nudLeftLaserRotZ.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "adjustRightLaserRotation", _chkRightLaserRotation.Checked ? "true" : "false");
                _ini.Set("", "rightLaserXRotation", _nudRightLaserRotX.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightLaserYRotation", _nudRightLaserRotY.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
                _ini.Set("", "rightLaserZRotation", _nudRightLaserRotZ.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
            }

            // FSR settings
            _ini.Set("", "fsrEnabled", _chkFsrEnabled.Checked ? "true" : "false");
            _ini.Set("", "fsrNativeAA", _chkFsrNativeAA.Checked ? "true" : "false");
            _ini.Set("", "fsrRenderScale", _nudFsrRenderScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "dlssEnabled", _chkDlssEnabled.Checked ? "true" : "false");
            _ini.Set("", "dlssPreset", _cmbDlssPreset.SelectedIndex.ToString());
            string dlssModel = _cmbDlssModel.SelectedIndex switch
            {
                1 => "J",
                2 => "K",
                3 => "L",
                4 => "M",
                _ => "default"
            };
            _ini.Set("", "dlssModel", dlssModel);
            _ini.Set("", "dlssRenderScaleOverride", _nudDlssRenderScaleOverride.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "dlssMvScale", _nudDlssMvScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "dlssJitterScale", _nudDlssJitterScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "dlssNgxVerboseLogging", _chkDlssNgxVerboseLogging.Checked ? "true" : "false");
            _ini.Set("", "motionVectorsEnabled", _chkMotionVectorsEnabled.Checked ? "true" : "false");
            _ini.Set("", "actorMV", _chkActorMV.Checked ? "true" : "false");
            _ini.Set("", "motionVectorScale", _nudMotionVectorScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswEnabled", _chkAswEnabled.Checked ? "true" : "false");
            _ini.Set("", "aswExperimentalMode", _chkAswExperimentalMode.Checked ? "true" : "false");
            _ini.Set("", "aswBufferEnabled", _chkAswBufferEnabled.Checked ? "true" : "false");
            _ini.Set("", "aswUpscalerReset", _chkAswUpscalerReset.Checked ? "true" : "false");
            _ini.Set("", "aswUpscalerReactiveMask", _chkAswUpscalerReactiveMask.Checked ? "true" : "false");
            _ini.Set("", "aswWarpStrength", _nudAswWarpStrength.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswRotationScale", _nudAswRotationScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswTranslationScale", _nudAswTranslationScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswDepthScale", _nudAswDepthScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswLocoScale", _nudAswLocoScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswNearFadeDepth", _aswNearFadeDepth.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswFPControllerScale", _aswFPControllerScale.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswEdgeFadeWidth", _aswEdgeFadeWidth.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswMVConfidence", _aswMVConfidence.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswMVPixelScale", _aswMVPixelScale.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswAutoNative", _chkAswAutoNative.Checked ? "true" : "false");
            _ini.Set("", "aswAutoEngageFps", _nudAswAutoEngageFps.Value.ToString("0", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "aswDebugMode", _chkAswDebugMode.Checked ? "10" : "0");
            _ini.Set("", "triggerDeadzone", _triggerDeadzone.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "triggerMax", _triggerMax.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3Sharpness", _nudFsr3Sharpness.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3JitterScale", _nudFsr3JitterScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3JitterCancellation", _chkFsr3JitterCancellation.Checked ? "true" : "false");
            _ini.Set("", "fsr3CameraMV", _chkFsr3CameraMV.Checked ? "true" : "false");
            _ini.Set("", "fsr3ViewToMeters", _nudFsr3ViewToMeters.Value.ToString("0.00000", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3ReactivenessScale", _nudFsr3ReactivenessScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3ShadingChangeScale", _nudFsr3ShadingChangeScale.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3AccumulationPerFrame", _nudFsr3AccumulationPerFrame.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3DebugMode", _cmbFsr3DebugMode.SelectedIndex.ToString());

            // CAS settings
            _ini.Set("", "casEnabled", _chkCasEnabled.Checked ? "true" : "false");
            _ini.Set("", "casSharpness", _nudCasSharpness.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsrSharpness", _nudCasSharpness.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "blueSkyDefenderEnabled", _chkBlueSkyDefenderEnabled.Checked ? "true" : "false");
            _ini.Set("", "blueSkyDefenderLambda", _nudBlueSkyLambda.Value.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "blueSkyDefenderEpsilon", _nudBlueSkyEpsilon.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3PostAAEnabled", "false");

            // MIP bias settings
            _ini.Set("", "mipBiasEnabled", _chkMipBiasEnabled.Checked ? "true" : "false");
            string mipBias = _cmbMipBiasMode.SelectedIndex switch
            {
                1 => "off",
                2 => _nudMipBiasFixed.Value.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture),
                _ => "auto"
            };
            _ini.Set("", "mipBias", mipBias);
            _ini.Set("", "mipBiasOffset", _nudMipBiasOffset.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "dlssMipBiasOffset", _nudDlssMipBiasOffset.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "fsr3MipBiasOffset", _nudFsr3MipBiasOffset.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));

            // VRS settings (NVIDIA only)
            _ini.Set("", "vrsEnabled", _chkVrsEnabled.Checked ? "true" : "false");
            _ini.Set("", "vrsInnerRadius", _nudVrsInnerRadius.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "vrsMidRadius", _nudVrsMidRadius.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "vrsOuterRadius", _nudVrsOuterRadius.Value.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture));
            _ini.Set("", "vrsFavorHorizontal", _chkVrsFavorHorizontal.Checked ? "true" : "false");

            WriteCombosToIni();
            _ini.Set("configurator", "activeBindingPreset", _cmbBindingPreset.SelectedItem?.ToString() ?? "");
        }

        private void LoadConfiguratorSettings()
        {
            string exePath = Application.ExecutablePath;
            string exeDir = Path.GetDirectoryName(exePath) ?? "";

            string rootSubDir = Path.Combine(exeDir, "root");
            string interfaceSubDir = Path.Combine(exeDir, "interface");
            if (Directory.Exists(rootSubDir) && Directory.Exists(interfaceSubDir))
            {
                _mo2ModDir = rootSubDir;
            }
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
            _lblInstallNotice.Text = "Keep this EXE in the OCU mod folder. Create a desktop shortcut; do not move the EXE.";
            _lblInstallNotice.ForeColor = Color.FromArgb(180, 180, 180);
            _lblInstallNotice.BackColor = Color.Transparent;

            if (!IsInstalledModFolderValid())
            {
                if (!_installWarningShown)
                {
                    _installWarningShown = true;
                    BeginInvoke(new Action(() =>
                    {
                        MessageBox.Show(GetInvalidInstallMessage(), "Invalid Configurator Location",
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    }));
                }
            }

            LoadFromDir();
        }

        private void SetDefaults()
        {
            _chkLeftStick.Checked = true;
            _pnlSkyrimOnly.Visible = _gameType == "skyrim";
            _pnlAxisAdjust.Visible = _gameType == "skyrim";
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

        private static bool TryParseIniFloat(string val, out float parsed)
        {
            return float.TryParse(
                val,
                System.Globalization.NumberStyles.Float,
                System.Globalization.CultureInfo.InvariantCulture,
                out parsed);
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

        private static NumericUpDown MakeAxisNud(int x, int y, decimal min, decimal max, decimal inc, int decimals) => new()
        {
            Location = new Point(x, y), Width = 65,
            DecimalPlaces = decimals, Increment = inc, Minimum = min, Maximum = max, Value = 0m,
            BackColor = Color.FromArgb(50, 50, 55), ForeColor = Color.White, Enabled = false
        };

        private void BtnResetAxis_Click(object? sender, EventArgs e)
        {
            ResetAxisControls(updateStatus: true);
        }

        private void ResetAxisControls(bool updateStatus)
        {
            _chkAdjustTilt.Checked = false;
            _nudTiltDeg.Value = 0m;
            _chkLeftRotation.Checked = false;
            _nudLeftRotX.Value = 0m; _nudLeftRotY.Value = 0m; _nudLeftRotZ.Value = 0m;
            _chkRightRotation.Checked = false;
            _nudRightRotX.Value = 0m; _nudRightRotY.Value = 0m; _nudRightRotZ.Value = 0m;
            _chkLeftPosition.Checked = false;
            _nudLeftPosX.Value = 0m; _nudLeftPosY.Value = 0m; _nudLeftPosZ.Value = 0m;
            _chkRightPosition.Checked = false;
            _nudRightPosX.Value = 0m; _nudRightPosY.Value = 0m; _nudRightPosZ.Value = 0m;
            _chkLeftLaserRotation.Checked = false;
            _nudLeftLaserRotX.Value = 0m; _nudLeftLaserRotY.Value = 0m; _nudLeftLaserRotZ.Value = 0m;
            _chkRightLaserRotation.Checked = false;
            _nudRightLaserRotX.Value = 0m; _nudRightLaserRotY.Value = 0m; _nudRightLaserRotZ.Value = 0m;
            if (updateStatus)
            {
                _lblStatus.Text = "Controller axis and laser settings reset to defaults";
                _lblStatus.ForeColor = Color.FromArgb(255, 200, 40);
            }
        }
    }
}
