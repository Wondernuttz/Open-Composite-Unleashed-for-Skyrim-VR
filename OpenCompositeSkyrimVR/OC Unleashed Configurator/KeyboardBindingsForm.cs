using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Form for configuring Skyrim/Fallout 4 keyboard bindings via controlmap.txt
    /// </summary>
    public class KeyboardBindingsForm : Form
    {
        private string _gameDir = "";
        private string _gameType = "skyrim"; // "skyrim" or "fallout4"

        private Panel _keyboardPanel = null!;
        private ComboBox _cmbAction = null!;
        private Label _lblCurrentBinding = null!;
        private Label _lblStatus = null!;
        private Button _btnSave = null!;
        private Button _btnVRDefaults = null!;
        private Button _btnResetDefaults = null!;

        // Currently selected key for remapping
        private string? _selectedKeyId = null;
        private Button? _selectedKeyButton = null;

        // Current bindings: action name -> scancode
        private readonly Dictionary<string, int> _bindings = new();

        // Key buttons by ID
        private readonly Dictionary<string, Button> _keyButtons = new();

        // DirectInput scancodes for keyboard keys
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

        // Reverse lookup: scancode -> key ID
        private static readonly Dictionary<int, string> ScancodeToKey;

        static KeyboardBindingsForm()
        {
            ScancodeToKey = KeyScancodes.ToDictionary(kvp => kvp.Value, kvp => kvp.Key);
        }

        // Game actions that can be remapped
        private static readonly (string id, string display, int defaultScancode)[] GameActions = new[]
        {
            // Movement
            ("Forward", "Forward", 0x11),           // W
            ("Back", "Back", 0x1F),                 // S
            ("Strafe Left", "Strafe Left", 0x1E),   // A
            ("Strafe Right", "Strafe Right", 0x20), // D
            ("Jump", "Jump", 0x39),                 // Space
            ("Sprint", "Sprint", 0x38),             // Left Alt
            ("Sneak", "Sneak", 0x1D),               // Left Ctrl
            ("Run", "Run", 0x2A),                   // Left Shift
            ("Toggle Always Run", "Toggle Always Run", 0x3A), // Caps Lock
            ("Auto-Move", "Auto-Move", 0x2E),       // C

            // Actions
            ("Activate", "Activate", 0x12),         // E
            ("Ready Weapon", "Ready Weapon", 0x13), // R
            ("Shout", "Shout/Power", 0x2C),         // Z

            // Menus
            ("Tween Menu", "Game Menu (Tab)", 0x0F),    // Tab
            ("Journal", "Journal", 0x24),               // J
            ("Wait", "Wait", 0x14),                     // T
            ("Favorites", "Favorites", 0x10),           // Q
            ("Quick Inventory", "Quick Inventory", 0x17), // I
            ("Quick Magic", "Quick Magic", 0x19),       // P
            ("Quick Stats", "Quick Stats", 0x35),       // /
            ("Quick Map", "Quick Map", 0x32),           // M

            // Hotkeys
            ("Hotkey1", "Hotkey 1", 0x02),   // 1
            ("Hotkey2", "Hotkey 2", 0x03),   // 2
            ("Hotkey3", "Hotkey 3", 0x04),   // 3
            ("Hotkey4", "Hotkey 4", 0x05),   // 4
            ("Hotkey5", "Hotkey 5", 0x06),   // 5
            ("Hotkey6", "Hotkey 6", 0x07),   // 6
            ("Hotkey7", "Hotkey 7", 0x08),   // 7
            ("Hotkey8", "Hotkey 8", 0x09),   // 8

            // Other
            ("Toggle POV", "Toggle POV", 0x21),     // F
            ("Quicksave", "Quicksave", 0x3F),       // F5
            ("Quickload", "Quickload", 0x43),       // F9
            ("Pause", "Pause", 0x01),               // Esc
            ("Console", "Console", 0x29),           // `
        };

        public KeyboardBindingsForm(string gameDir, string gameType)
        {
            _gameDir = gameDir;
            _gameType = gameType;
            InitializeUI();
            LoadDefaults();
        }

        private void InitializeUI()
        {
            Text = $"Keyboard Bindings - {(_gameType == "skyrim" ? "Skyrim VR" : "Fallout 4 VR")}";
            Size = new Size(1100, 680);
            MinimumSize = new Size(1000, 600);
            StartPosition = FormStartPosition.CenterParent;
            BackColor = Color.FromArgb(30, 30, 35);
            ForeColor = Color.FromArgb(220, 220, 220);
            Font = new Font("Segoe UI", 9.5f);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;

            int y = 12;
            int leftMargin = 16;

            // Title
            var lblTitle = new Label
            {
                Text = "Keyboard Bindings",
                Location = new Point(leftMargin, y),
                AutoSize = true,
                Font = new Font("Segoe UI", 14f, FontStyle.Bold),
                ForeColor = Color.FromArgb(255, 200, 40)
            };
            Controls.Add(lblTitle);
            y += 36;

            // Instructions
            var lblInstructions = new Label
            {
                Text = "Click a key on the keyboard below, then select an action to bind it to. " +
                       "Changes are saved to controlmapvr.txt and require a game restart.",
                Location = new Point(leftMargin, y),
                Size = new Size(1050, 36),
                ForeColor = Color.FromArgb(180, 180, 180)
            };
            Controls.Add(lblInstructions);
            y += 42;

            // Action selection row
            var lblAction = new Label
            {
                Text = "Selected Key:",
                Location = new Point(leftMargin, y + 4),
                AutoSize = true
            };
            Controls.Add(lblAction);

            _lblCurrentBinding = new Label
            {
                Text = "(click a key)",
                Location = new Point(leftMargin + 100, y + 4),
                Size = new Size(150, 20),
                ForeColor = Color.FromArgb(255, 200, 40),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold)
            };
            Controls.Add(_lblCurrentBinding);

            var lblBindTo = new Label
            {
                Text = "Bind to action:",
                Location = new Point(leftMargin + 270, y + 4),
                AutoSize = true
            };
            Controls.Add(lblBindTo);

            _cmbAction = new ComboBox
            {
                Location = new Point(leftMargin + 380, y),
                Width = 200,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat
            };
            _cmbAction.Items.Add("(none - unbind)");
            foreach (var action in GameActions)
            {
                _cmbAction.Items.Add(action.display);
            }
            _cmbAction.SelectedIndex = 0;
            _cmbAction.SelectedIndexChanged += CmbAction_SelectedIndexChanged;
            _cmbAction.Enabled = false;
            Controls.Add(_cmbAction);

            y += 36;

            // Keyboard panel
            _keyboardPanel = new Panel
            {
                Location = new Point(leftMargin, y),
                Size = new Size(1050, 400),
                BackColor = Color.FromArgb(25, 25, 30)
            };
            Controls.Add(_keyboardPanel);

            CreateKeyboardLayout();

            y += 410;

            // Bottom buttons
            _btnVRDefaults = CreateButton("VR Safe Defaults", leftMargin, y, 150, 36);
            _btnVRDefaults.BackColor = Color.FromArgb(40, 100, 160);
            _btnVRDefaults.Click += BtnVRDefaults_Click;
            Controls.Add(_btnVRDefaults);

            _btnResetDefaults = CreateButton("Reset to Game Defaults", leftMargin + 165, y, 180, 36);
            _btnResetDefaults.Click += BtnResetDefaults_Click;
            Controls.Add(_btnResetDefaults);

            _btnSave = CreateButton("Save controlmap.txt", leftMargin + 800, y, 180, 36);
            _btnSave.BackColor = Color.FromArgb(40, 120, 40);
            _btnSave.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
            _btnSave.Click += BtnSave_Click;
            Controls.Add(_btnSave);

            y += 46;

            _lblStatus = new Label
            {
                Text = "",
                Location = new Point(leftMargin, y),
                Size = new Size(800, 20),
                ForeColor = Color.FromArgb(100, 200, 100)
            };
            Controls.Add(_lblStatus);
        }

        private void CreateKeyboardLayout()
        {
            int startX = 10;
            int startY = 10;
            int keyW = 50;
            int keyH = 44;
            int gap = 4;

            // Row 1: Esc, F1-F12
            int x = startX;
            int y = startY;
            AddKey("Esc", "Esc", x, y, keyW, keyH); x += keyW + gap + 30;
            AddKey("F1", "F1", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F2", "F2", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F3", "F3", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F4", "F4", x, y, keyW, keyH); x += keyW + gap + 15;
            AddKey("F5", "F5", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F6", "F6", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F7", "F7", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F8", "F8", x, y, keyW, keyH); x += keyW + gap + 15;
            AddKey("F9", "F9", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F10", "F10", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F11", "F11", x, y, keyW, keyH); x += keyW + gap;
            AddKey("F12", "F12", x, y, keyW, keyH);

            // Navigation cluster
            x += keyW + gap + 20;
            AddKey("Ins", "Ins", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Home", "Home", x, y, keyW, keyH); x += keyW + gap;
            AddKey("PgUp", "PgUp", x, y, keyW, keyH);

            y += keyH + gap + 10;

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
            x = startX + 15 * (keyW + gap) + 20;
            AddKey("Del", "Del", x, y, keyW, keyH); x += keyW + gap;
            AddKey("End", "End", x, y, keyW, keyH); x += keyW + gap;
            AddKey("PgDn", "PgDn", x, y, keyW, keyH);

            y += keyH + gap;

            // Row 3: Tab Q-] \
            x = startX;
            AddKey("Tab", "Tab", x, y, (int)(keyW * 1.5), keyH); x += (int)(keyW * 1.5) + gap;
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
            x = startX + 15 * (keyW + gap) + 20;
            AddKey("Num7", "7", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num8", "8", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num9", "9", x, y, keyW, keyH);

            y += keyH + gap;

            // Row 4: Caps A-' Enter
            x = startX;
            AddKey("Caps", "Caps", x, y, (int)(keyW * 1.75), keyH); x += (int)(keyW * 1.75) + gap;
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
            AddKey("Enter", "Enter", x, y, (int)(keyW * 2.25), keyH);

            // Numpad row 2
            x = startX + 15 * (keyW + gap) + 20;
            AddKey("Num4", "4", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num5", "5", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num6", "6", x, y, keyW, keyH);

            y += keyH + gap;

            // Row 5: Shift Z-/ Shift
            x = startX;
            AddKey("LShift", "Shift", x, y, (int)(keyW * 2.25), keyH); x += (int)(keyW * 2.25) + gap;
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
            AddKey("RShift", "Shift", x, y, (int)(keyW * 2.75), keyH);

            // Arrow Up
            x = startX + 15 * (keyW + gap) + 20 + keyW + gap;
            AddKey("Up", "\u25B2", x, y, keyW, keyH);

            // Numpad row 3
            x = startX + 15 * (keyW + gap) + 20 + 3 * (keyW + gap) + 20;
            AddKey("Num1", "1", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num2", "2", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Num3", "3", x, y, keyW, keyH);

            y += keyH + gap;

            // Row 6: Ctrl Alt Space Alt Ctrl
            x = startX;
            AddKey("LCtrl", "Ctrl", x, y, (int)(keyW * 1.5), keyH); x += (int)(keyW * 1.5) + gap;
            x += keyW + gap; // Skip Win key
            AddKey("LAlt", "Alt", x, y, (int)(keyW * 1.5), keyH); x += (int)(keyW * 1.5) + gap;
            AddKey("Space", "Space", x, y, keyW * 6 + gap * 5, keyH); x += keyW * 6 + gap * 5 + gap;
            AddKey("RAlt", "Alt", x, y, (int)(keyW * 1.5), keyH); x += (int)(keyW * 1.5) + gap;
            x += keyW + gap; // Skip Win key
            AddKey("RCtrl", "Ctrl", x, y, (int)(keyW * 1.5), keyH);

            // Arrows
            x = startX + 15 * (keyW + gap) + 20;
            AddKey("Left", "\u25C0", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Down", "\u25BC", x, y, keyW, keyH); x += keyW + gap;
            AddKey("Right", "\u25B6", x, y, keyW, keyH);

            // Numpad row 4
            x = startX + 15 * (keyW + gap) + 20 + 3 * (keyW + gap) + 20;
            AddKey("Num0", "0", x, y, keyW * 2 + gap, keyH); x += keyW * 2 + gap + gap;
            AddKey("Num.", ".", x, y, keyW, keyH);
        }

        private void AddKey(string id, string label, int x, int y, int w, int h)
        {
            var btn = new Button
            {
                Text = label,
                Location = new Point(x, y),
                Size = new Size(w, h),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(40, 40, 48),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
                Cursor = Cursors.Hand,
                Tag = id
            };
            btn.FlatAppearance.BorderColor = Color.FromArgb(60, 60, 70);
            btn.FlatAppearance.BorderSize = 1;
            btn.Click += Key_Click;
            btn.Paint += Key_Paint;

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
                _selectedKeyButton.BackColor = Color.FromArgb(40, 40, 48);
                _selectedKeyButton.FlatAppearance.BorderColor = Color.FromArgb(60, 60, 70);
            }

            // Select new
            _selectedKeyId = keyId;
            _selectedKeyButton = btn;
            btn.BackColor = Color.FromArgb(60, 80, 100);
            btn.FlatAppearance.BorderColor = Color.FromArgb(100, 150, 200);

            _lblCurrentBinding.Text = keyId;
            _cmbAction.Enabled = true;

            // Find current action for this key
            int scancode = KeyScancodes.GetValueOrDefault(keyId, 0xFF);
            var currentAction = _bindings.FirstOrDefault(kvp => kvp.Value == scancode);
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

        private void Key_Paint(object? sender, PaintEventArgs e)
        {
            if (sender is not Button btn) return;
            string keyId = (string)btn.Tag;

            // Check if this key has a binding
            int scancode = KeyScancodes.GetValueOrDefault(keyId, 0xFF);
            var binding = _bindings.FirstOrDefault(kvp => kvp.Value == scancode);

            if (binding.Key != null)
            {
                // Draw binding indicator
                var action = GameActions.FirstOrDefault(a => a.id == binding.Key);
                if (action.id != null)
                {
                    using var smallFont = new Font("Segoe UI", 6.5f);
                    string shortName = GetShortActionName(action.display);
                    var textSize = e.Graphics.MeasureString(shortName, smallFont);

                    // Background for text
                    var bgRect = new RectangleF(2, btn.Height - 14, textSize.Width + 4, 12);
                    using var bgBrush = new SolidBrush(Color.FromArgb(180, 255, 180, 40));
                    e.Graphics.FillRectangle(bgBrush, bgRect);

                    using var textBrush = new SolidBrush(Color.Black);
                    e.Graphics.DrawString(shortName, smallFont, textBrush, 4, btn.Height - 13);
                }
            }
        }

        private static string GetShortActionName(string name) => name switch
        {
            "Forward" => "Fwd",
            "Back" => "Back",
            "Strafe Left" => "Left",
            "Strafe Right" => "Right",
            "Activate" => "Act",
            "Ready Weapon" => "Wpn",
            "Shout/Power" => "Shout",
            "Game Menu (Tab)" => "Menu",
            "Journal" => "Jrnl",
            "Wait" => "Wait",
            "Favorites" => "Fav",
            "Quick Inventory" => "Inv",
            "Quick Magic" => "Mag",
            "Quick Stats" => "Stats",
            "Quick Map" => "Map",
            "Toggle POV" => "POV",
            "Toggle Always Run" => "Run",
            _ when name.StartsWith("Hotkey") => name.Replace("Hotkey ", "H"),
            _ => name.Length > 5 ? name[..5] : name
        };

        private void CmbAction_SelectedIndexChanged(object? sender, EventArgs e)
        {
            if (_selectedKeyId == null) return;

            int scancode = KeyScancodes.GetValueOrDefault(_selectedKeyId, 0xFF);
            if (scancode == 0xFF) return;

            // Remove any existing binding for this scancode
            var existingKey = _bindings.FirstOrDefault(kvp => kvp.Value == scancode).Key;
            if (existingKey != null)
                _bindings.Remove(existingKey);

            // Add new binding
            if (_cmbAction.SelectedIndex > 0)
            {
                var action = GameActions[_cmbAction.SelectedIndex - 1];

                // Remove old binding for this action (if different key)
                if (_bindings.ContainsKey(action.id))
                    _bindings.Remove(action.id);

                _bindings[action.id] = scancode;
            }

            // Refresh all keys
            foreach (var btn in _keyButtons.Values)
                btn.Invalidate();

            _lblStatus.Text = "Binding updated (unsaved)";
            _lblStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private void LoadDefaults()
        {
            _bindings.Clear();
            foreach (var action in GameActions)
            {
                _bindings[action.id] = action.defaultScancode;
            }

            foreach (var btn in _keyButtons.Values)
                btn.Invalidate();
        }

        private void BtnResetDefaults_Click(object? sender, EventArgs e)
        {
            LoadDefaults();
            _lblStatus.Text = "Reset to game defaults (unsaved)";
            _lblStatus.ForeColor = Color.FromArgb(200, 180, 80);
        }

        private void BtnVRDefaults_Click(object? sender, EventArgs e)
        {
            // VR Safe Defaults: Move WASD to numpad to prevent VR keyboard conflicts
            _bindings["Forward"] = KeyScancodes["Num8"];       // Numpad 8
            _bindings["Back"] = KeyScancodes["Num5"];          // Numpad 5
            _bindings["Strafe Left"] = KeyScancodes["Num4"];   // Numpad 4
            _bindings["Strafe Right"] = KeyScancodes["Num6"];  // Numpad 6
            _bindings["Activate"] = KeyScancodes["Num0"];      // Numpad 0

            foreach (var btn in _keyButtons.Values)
                btn.Invalidate();

            _lblStatus.Text = "VR Safe Defaults applied: WASD moved to Numpad (unsaved)";
            _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
        }

        private void BtnSave_Click(object? sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(_gameDir))
            {
                MessageBox.Show("No game directory selected.", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try
            {
                string savedPath = SaveControlmap();
                _lblStatus.Text = $"Saved controlmapvr.txt! Restart the game to apply changes.";
                _lblStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save controlmap.txt: {ex.Message}", "Error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private string SaveControlmap()
        {
            // Determine the correct path based on game type
            // For MO2 mods, save to the mod folder; for manual installs, save to Data folder
            string controlsPath;

            // Check if this is an MO2 mod folder (has openvr_api.dll in root\)
            string rootDll = Path.Combine(_gameDir, "openvr_api.dll");
            if (File.Exists(rootDll))
            {
                // MO2 mod structure: save to same folder as the DLL
                // Create Interface/Controls/PC/ structure
                controlsPath = Path.Combine(Path.GetDirectoryName(_gameDir) ?? _gameDir,
                    "Interface", "Controls", "PC");
            }
            else
            {
                // Direct game folder or Data folder
                controlsPath = Path.Combine(_gameDir, "Data", "Interface", "Controls", "PC");
            }

            Directory.CreateDirectory(controlsPath);
            // VR uses controlmapvr.txt, not controlmap.txt
            string filename = "controlmapvr.txt";
            string filePath = Path.Combine(controlsPath, filename);

            using var writer = new StreamWriter(filePath);

            // Write header
            writer.WriteLine("// Generated by OC Unleashed Configurator");
            writer.WriteLine("// 1st field: User event name");
            writer.WriteLine("// 2nd: Keyboard scancode (0xff = unmapped)");
            writer.WriteLine("// 3rd: Mouse button ID");
            writer.WriteLine("// 4th: Gamepad button ID");
            writer.WriteLine("// 5th-7th: Remap flags (keyboard, mouse, gamepad)");
            writer.WriteLine("// 8th: User event flags");
            writer.WriteLine("//");
            writer.WriteLine("// Main Gameplay");

            // Write main gameplay bindings
            WriteBinding(writer, "Forward", 0x801);
            WriteBinding(writer, "Back", 0x801);
            WriteBinding(writer, "Strafe Left", 0x801);
            WriteBinding(writer, "Strafe Right", 0x801);
            writer.WriteLine("Move\t\t\t\t0xff\t\t\t\t0xff\t0x000b\t\t\t\t0\t0\t0\t0x801");
            writer.WriteLine("Look\t\t\t\t0xff\t\t\t\t0xa\t\t0x000c\t\t\t\t0\t0\t0\t0x2");
            writer.WriteLine("Left Attack/Block\t0xff\t\t\t\t0x1\t\t0x0009\t\t\t\t1\t1\t1\t0x841");
            writer.WriteLine("Right Attack/Block\t0xff\t\t\t\t0x0\t\t0x000a\t\t\t\t1\t1\t1\t0x841");
            WriteBinding(writer, "Activate", 0x804, "0xff", "0x1000");
            WriteBinding(writer, "Ready Weapon", 0x840, "0xff", "0x4000");
            WriteBinding(writer, "Tween Menu", 0x908, "0xff", "0x2000");
            WriteBinding(writer, "Toggle POV", 0x820, "0xff", "0x0080");
            writer.WriteLine("Zoom Out\t\t\t0xff\t\t\t\t0x9\t\t0xff\t\t\t\t0\t0\t0\t0x220");
            writer.WriteLine("Zoom In\t\t\t\t0xff\t\t\t\t0x8\t\t0xff\t\t\t\t0\t0\t0\t0x220");
            WriteBinding(writer, "Jump", 0xC01, "0xff", "0x8000");
            WriteBinding(writer, "Sprint", 0x801, "0xff", "0x0100");
            WriteBinding(writer, "Shout", 0x840, "0xff", "0x0200");
            WriteBinding(writer, "Sneak", 0x881, "0xff", "0x0040");
            WriteBinding(writer, "Run", 0x801);
            WriteBinding(writer, "Toggle Always Run", 0x801);
            WriteBinding(writer, "Auto-Move", 0x801);
            WriteBinding(writer, "Favorites", 0x908, "0xff", "0x0001,0x0002");

            // Hotkeys with dual bindings (number row + numpad)
            WriteHotkeyBinding(writer, "Hotkey1", 0x02, 0x4F, "0x0004");
            WriteHotkeyBinding(writer, "Hotkey2", 0x03, 0x50, "0x0008");
            WriteHotkeyBinding(writer, "Hotkey3", 0x04, 0x51, "0xff");
            WriteHotkeyBinding(writer, "Hotkey4", 0x05, 0x4B, "0xff");
            WriteHotkeyBinding(writer, "Hotkey5", 0x06, 0x4C, "0xff");
            WriteHotkeyBinding(writer, "Hotkey6", 0x07, 0x4D, "0xff");
            WriteHotkeyBinding(writer, "Hotkey7", 0x08, 0x47, "0xff");
            WriteHotkeyBinding(writer, "Hotkey8", 0x09, 0x48, "0xff");

            WriteBinding(writer, "Quicksave", 0, "0xff", "0xff", false);
            WriteBinding(writer, "Quickload", 0, "0xff", "0xff", false);
            WriteBinding(writer, "Wait", 0x808, "0xff", "0x0020");
            WriteBinding(writer, "Journal", 0x808, "0xff", "0x0010");
            WriteBinding(writer, "Pause", 0x8);
            writer.WriteLine("Screenshot\t\t\t0xb7\t\t\t\t0xff\t0xff\t\t\t\t0\t0\t0");
            writer.WriteLine("Multi-Screenshot\t0x1d+0xb7,0x9d+0xb7\t0xff\t0xff\t\t\t\t0\t0\t0");
            WriteBinding(writer, "Console", 0x10, "0xff", "0xff", false);
            writer.WriteLine("CameraPath\t\t\t0x58\t\t\t\t0xff\t0xff\t\t\t\t0\t0\t0");
            WriteBinding(writer, "Quick Inventory", 0x908);
            WriteBinding(writer, "Quick Magic", 0x908);
            WriteBinding(writer, "Quick Stats", 0x908);
            WriteBinding(writer, "Quick Map", 0x908);

            writer.WriteLine();

            // Write remaining contexts (Menu Mode, Console, etc.) with defaults
            WriteDefaultContexts(writer);

            return filePath;
        }

        private void WriteBinding(StreamWriter writer, string actionName, int flags,
            string mouse = "0xff", string gamepad = "0xff", bool canRemap = true)
        {
            int scancode = _bindings.GetValueOrDefault(actionName, 0xFF);
            string scStr = scancode != 0xFF ? $"0x{scancode:X2}" : "0xff";

            // Pad action name with tabs
            string paddedName = actionName.PadRight(20);
            string remapFlags = canRemap ? "1\t1\t0" : "0\t0\t0";

            if (flags > 0)
                writer.WriteLine($"{paddedName}{scStr}\t\t\t\t{mouse}\t{gamepad}\t\t\t\t{remapFlags}\t0x{flags:X}");
            else
                writer.WriteLine($"{paddedName}{scStr}\t\t\t\t{mouse}\t{gamepad}\t\t\t\t{remapFlags}");
        }

        private void WriteHotkeyBinding(StreamWriter writer, string actionName, int defaultRow, int defaultNumpad, string gamepad)
        {
            // For hotkeys, check if user remapped them
            int scancode = _bindings.GetValueOrDefault(actionName, defaultRow);

            // If not remapped, use dual binding (row + numpad)
            string scStr;
            if (scancode == defaultRow)
                scStr = $"0x{defaultRow:X2},0x{defaultNumpad:X2}";
            else
                scStr = $"0x{scancode:X2}";

            string paddedName = actionName.PadRight(20);
            writer.WriteLine($"{paddedName}{scStr}\t\t\t0xff\t{gamepad}\t\t\t\t0\t0\t0\t0x908");
        }

        private void WriteDefaultContexts(StreamWriter writer)
        {
            // Menu Mode
            writer.WriteLine("// Menu Mode");
            writer.WriteLine("Accept\t\t!0,Activate\t\t\t\t!0,Activate\t\t\t\t!0,Activate\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cancel\t\t!0,Tween Menu,!0,Pause\t!0,Tween Menu,!0,Pause\t!0,Tween Menu\t0\t0\t0\t0x8");
            writer.WriteLine("Up\t\t\t!0,Forward\t\t\t\t!0,Forward\t\t\t\t0x0001\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Down\t\t!0,Back\t\t\t\t\t!0,Back\t\t\t\t\t0x0002\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Left\t\t!0,Strafe Left\t\t\t!0,Strafe Left\t\t\t0x0004\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Right\t\t!0,Strafe Right\t\t\t!0,Strafe Right\t\t\t0x0008\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Left Stick\t0xff\t\t\t\t\t0xff\t\t\t\t\t0x000b\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Console\t\t0x29\t\t\t\t\t0xff\t\t\t\t\t0xff\t\t\t0\t0\t0\t0x10");
            writer.WriteLine();

            // Console
            writer.WriteLine("// Console");
            writer.WriteLine("PickPrevious\t0xff\t\t\t0x8\t\t0x0002\t0\t0\t0\t0x10");
            writer.WriteLine("PickNext\t\t0xff\t\t\t0x9\t\t0x0001\t0\t0\t0\t0x10");
            writer.WriteLine("Up\t\t\t\t0xc8\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("Down\t\t\t0xd0\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("PageUp\t\t\t0xc9\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("PageDown\t\t0xd1\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("Console\t\t\t0x29\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("NextFocus\t\t0x0f\t\t\t0xff\t0x0200\t0\t0\t0\t0x10");
            writer.WriteLine("PreviousFocus\t0x2a+0x0f,0x36+0x0f\t0xff\t0x0100\t0\t0\t0\t0x10");
            writer.WriteLine();

            // Item Menus
            writer.WriteLine("// Item Menus");
            writer.WriteLine("LeftEquip\t!0,Left Attack/Block\t!0,Left Attack/Block\t\t!0,Left Attack/Block\t\t0\t0\t0\t0x8");
            writer.WriteLine("RightEquip\t!0,Right Attack/Block\t!0,Right Attack/Block\t\t!0,Right Attack/Block\t\t0\t0\t0\t0x8");
            writer.WriteLine("Item Zoom\t0x2e\t\t\t\t\t0xff\t\t\t\t\t\t!0,Toggle POV\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Rotate\t\t0xff\t\t\t\t\t0xff\t\t\t\t\t\t0x000c\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("XButton\t\t!0,Ready Weapon\t\t\t!0,Ready Weapon\t\t\t\t!0,Ready Weapon\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("YButton\t\t!0,Toggle POV\t\t\t!0,Toggle POV\t\t\t\t!0,Jump\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cursor\t\t0xff\t\t\t\t\t0xa\t\t\t\t\t\t\t0xff\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine();

            // Additional contexts...
            writer.WriteLine("// Inventory");
            writer.WriteLine("ChargeItem\t!0,Wait\t!0,Wait\t!0,Shout\t\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Debug Text");
            writer.WriteLine("PrevPage\t0xc8\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("NextPage\t0xd0\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("PrevSubPage\t0xc9\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("NextSubPage\t0xd1\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine();

            // Favorites, Map, Stats, Cursor, Book menus
            writer.WriteLine("// Favorites menu");
            writer.WriteLine("Up\t\t\t!0,Forward\t\t\t\t\t\t\t\t!0,Forward\t\t\t\t\t\t\t\t0x0001\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Down\t\t!0,Back\t\t\t\t\t\t\t\t\t!0,Back\t\t\t\t\t\t\t\t\t0x0002\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Accept\t\t!0,Activate\t\t\t\t\t\t\t\t!0,Activate\t\t\t\t\t\t\t\t!0,Activate\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cancel\t\t!0,Favorites,!0,Tween Menu,!0,Pause\t\t!0,Favorites,!0,Tween Menu,!0,Pause\t\t!0,Tween Menu\t0\t0\t0\t0x8");
            writer.WriteLine("Left Stick\t0xff\t\t\t\t\t\t\t\t\t0xff\t\t\t\t\t\t\t\t\t0x000b\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cursor\t\t0xff\t\t\t\t\t\t\t\t\t0xa\t\t\t\t\t\t\t\t\t\t0xff\t\t\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Map Menu");
            writer.WriteLine("Cancel\t\t\t\t!0,Tween Menu,!0,Pause\t!0,Tween Menu,!0,Pause\t!0,Tween Menu\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Look\t\t\t\t0xff\t\t\t\t\t0xff\t\t\t\t\t0x000c\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Zoom In\t\t\t\t0xff\t\t\t\t\t0x8\t\t\t\t\t!0,Right Attack/Block\t\t0\t0\t0\t0x8");
            writer.WriteLine("Zoom Out\t\t\t0xff\t\t\t\t\t0x9\t\t\t\t\t!0,Left Attack/Block\t\t0\t0\t0\t0x8");
            writer.WriteLine("MapLookMode\t\t\t0xff\t\t\t\t\t0x1\t\t\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Click\t\t\t\t0xff\t\t\t\t\t0xff\t\t\t\t\t0x1000\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("PlacePlayerMarker\t0x19\t\t\t\t\t0xff\t\t\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cursor\t\t\t\t0xff\t\t\t\t\t0xa\t\t\t\t\t0x000b\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("PlayerPosition\t\t0x12\t\t\t\t\t0xff\t\t\t\t\t0x8000\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("LocalMap\t\t\t0x26\t\t\t\t\t0xff\t\t\t\t\t0x4000\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("LocalMapMoveMode\t0xff\t\t\t\t\t0x0\t\t\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Journal\t\t\t\t0x24\t\t\t\t\t0xff\t\t\t\t\t0x0010\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Up\t\t\t\t\t!0,Forward\t\t\t\t!0,Forward\t\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Down\t\t\t\t!0,Back\t\t\t\t\t!0,Back\t\t\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Left\t\t\t\t!0,Strafe Left\t\t\t!0,Strafe Left\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Right\t\t\t\t!0,Strafe Right\t\t\t!0,Strafe Right\t\t\t0xff\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Stats");
            writer.WriteLine("Rotate\t0xff\t\t0xff\t\t0x000b\t0\t0\t0\t0x8");
            writer.WriteLine("YButton\t!0,Toggle POV\t!0,Toggle POV\t!0,Jump\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Cursor");
            writer.WriteLine("Cursor\t0xff\t0xa\t0x000c\t0\t0\t0\t0x10");
            writer.WriteLine("Click\t0xff\t0x0\t0x1000\t0\t0\t0\t0x10");
            writer.WriteLine();

            writer.WriteLine("// Book");
            writer.WriteLine("PrevPage\t0xcb,0x1e\t0x0,0x9\t\t0x0004\t0\t0\t0\t0x8");
            writer.WriteLine("NextPage\t0xcd,0x20\t0x1,0x8\t\t0x0008\t0\t0\t0\t0x8");
            writer.WriteLine();

            // Additional contexts
            writer.WriteLine("// Debug overlay");
            writer.WriteLine("Console\t\t\t0x29\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("NextFocus\t\t0x0f\t\t\t\t0xff\t0x0200\t0\t0\t0\t0x10");
            writer.WriteLine("PreviousFocus\t0x2a+0x0f,0x36+0x0f\t0xff\t0x0100\t0\t0\t0\t0x10");
            writer.WriteLine("Up\t\t\t\t0xc8\t\t\t\t0xff\t0x0001\t0\t0\t0\t0x10");
            writer.WriteLine("Down\t\t\t0xd0\t\t\t\t0xff\t0x0002\t0\t0\t0\t0x10");
            writer.WriteLine("Left\t\t\t0xcb\t\t\t\t0xff\t0x0004\t0\t0\t0\t0x10");
            writer.WriteLine("Right\t\t\t0xcd\t\t\t\t0xff\t0x0008\t0\t0\t0\t0x10");
            writer.WriteLine("PageUp\t\t\t0xc9\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("PageDown\t\t0xd1\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("ToggleMinimize\t0x3f\t\t\t\t0xff\t0x0020\t0\t0\t0\t0x10");
            writer.WriteLine("ToggleMove\t\t0x3e\t\t\t\t0xff\t0x0080\t0\t0\t0\t0x10");
            writer.WriteLine("Close\t\t\t0x40\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F1\t\t\t\t0x3b\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F2\t\t\t\t0x3c\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F3\t\t\t\t0x3d\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F7\t\t\t\t0x41\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F8\t\t\t\t0x42\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F9\t\t\t\t0x43\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F10\t\t\t\t0x44\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F11\t\t\t\t0x57\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("F12\t\t\t\t0x58\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("LTrigger\t\t0xff\t\t\t\t0xff\t0x0009\t0\t0\t0\t0x10");
            writer.WriteLine("RTrigger\t\t0xff\t\t\t\t0xff\t0x000a\t0\t0\t0\t0x10");
            writer.WriteLine("Backspace\t\t0x0e\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("Enter\t\t\t0x1c\t\t\t\t0xff\t0xff\t0\t0\t0\t0x10");
            writer.WriteLine("B\t\t\t\t0xff\t\t\t\t0xff\t0x2000\t0\t0\t0\t0x10");
            writer.WriteLine("Y\t\t\t\t0xff\t\t\t\t0xff\t0x8000\t0\t0\t0\t0x10");
            writer.WriteLine("X\t\t\t\t0xff\t\t\t\t0xff\t0x4000\t0\t0\t0\t0x10");
            writer.WriteLine();

            writer.WriteLine("// Journal");
            writer.WriteLine("Zoom In\t\t0xff\t\t0x8\t\t0xff\t\t\t\t\t\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Zoom Out\t0xff\t\t0x9\t\t0xff\t\t\t\t\t\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("XButton\t\t0x2d,0x32\t0xff\t!0,Ready Weapon\t\t\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("YButton\t\t0x14\t\t0xff\t!0,Jump\t\t\t\t\t\t\t\t\t\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("TabSwitch\t0xff\t\t0xff\t!0,Left Attack/Block,!0,Right Attack/Block\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// TFC mode");
            writer.WriteLine("CameraZUp\t\t0xff\t0x8\t\t0x000a\t0\t0\t0");
            writer.WriteLine("CameraZDown\t\t0xff\t0x9\t\t0x0009\t0\t0\t0");
            writer.WriteLine("WorldZUp\t\t0xff\t0x0\t\t0x0200\t0\t0\t0");
            writer.WriteLine("WorldZDown\t\t0xff\t0x1\t\t0x0100\t0\t0\t0");
            writer.WriteLine("LockToZPlane\t0xff\t0xff\t0x4000\t0\t0\t0");
            writer.WriteLine();

            writer.WriteLine("// Debug Map Menu-like mode");
            writer.WriteLine("Look\t\t\t0xff\t0xff\t0x000c\t0\t0\t0\t0x8");
            writer.WriteLine("Zoom In\t\t\t0xff\t0x8\t\t0x000a\t0\t0\t0\t0x8");
            writer.WriteLine("Zoom Out\t\t0xff\t0x9\t\t0x0009\t0\t0\t0\t0x8");
            writer.WriteLine("Move\t\t\t0xff\t0xa\t\t0x000b\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Lockpicking");
            writer.WriteLine("RotatePick\t\t\t0xff\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t0xa\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t0x000b\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("RotateLock\t\t\t!0,Forward,!0,Back,!0,Strafe Left,!0,Strafe Right\t\t!0,Forward,!0,Back,!0,Strafe Left,!0,Strafe Right\t\t0x000c\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("DebugMode\t\t\t0x35\t\t\t\t\t\t\t\t\t\t\t\t\t\t0xff\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t0x4000\t\t\t0\t0\t0\t0x8");
            writer.WriteLine("Cancel\t\t\t\t!0,Tween Menu,!0,Pause\t\t\t\t\t\t\t\t\t!0,Tween Menu,!0,Pause\t\t\t\t\t\t\t\t\t\t\t\t\t!0,Tween Menu\t0\t0\t0\t0x8");
            writer.WriteLine();

            writer.WriteLine("// Favor");
            writer.WriteLine("Cancel\t\t!0,Tween Menu,!0,Pause\t!0,Tween Menu,!0,Pause\t!0,Tween Menu\t0\t0\t0\t0x108");
        }

        private static Button CreateButton(string text, int x, int y, int w, int h) => new()
        {
            Text = text, Location = new Point(x, y), Size = new Size(w, h),
            FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(55, 55, 65),
            ForeColor = Color.White, Cursor = Cursors.Hand
        };
    }
}
