using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    public class ComboEntry
    {
        public string ButtonString { get; set; } = "";
        public string Mode { get; set; } = "press";
        public int TimingMs { get; set; } = 500;
        public int Scancode { get; set; } = 0x02;

        public string ToIniValue()
        {
            return $"{ButtonString},{Mode},{TimingMs},0x{Scancode:x2}";
        }

        public static ComboEntry? FromIniValue(string value)
        {
            var parts = value.Split(',');
            if (parts.Length < 4) return null;

            string scHex = parts[3].Trim();
            if (!scHex.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) return null;
            if (!int.TryParse(scHex[2..], System.Globalization.NumberStyles.HexNumber, null, out int sc)) return null;

            int timing = 0;
            int.TryParse(parts[2].Trim(), out timing);

            return new ComboEntry
            {
                ButtonString = parts[0].Trim(),
                Mode = parts[1].Trim().ToLowerInvariant(),
                TimingMs = timing,
                Scancode = sc
            };
        }

        public string GetDisplaySummary(Dictionary<string, int> keyScancodes)
        {
            string buttons = string.Join(" + ", ButtonString.Split('+').Select(FormatButton));
            string modeStr = Mode switch
            {
                "press" => "Press",
                "double_tap" => "Double Tap",
                "triple_tap" => "Triple Tap",
                "quadruple_tap" => "Quad Tap",
                "long_press" => "Long Press",
                _ => Mode
            };
            string keyName = keyScancodes.FirstOrDefault(kv => kv.Value == Scancode).Key ?? $"0x{Scancode:x2}";
            return $"{buttons}  \u2192  {modeStr}  \u2192  {keyName}";
        }

        private static string FormatButton(string b)
        {
            return b.Trim() switch
            {
                "left_stick" => "L Stick",
                "right_stick" => "R Stick",
                "left_stick_up" => "L Stick \u25B2",
                "left_stick_down" => "L Stick \u25BC",
                "left_stick_left" => "L Stick \u25C0",
                "left_stick_right" => "L Stick \u25B6",
                "right_stick_up" => "R Stick \u25B2",
                "right_stick_down" => "R Stick \u25BC",
                "right_stick_left" => "R Stick \u25C0",
                "right_stick_right" => "R Stick \u25B6",
                "left_grip" => "L Grip",
                "right_grip" => "R Grip",
                "left_trigger" => "L Trigger",
                "right_trigger" => "R Trigger",
                "x" => "X",
                "y" => "Y",
                "a" => "A",
                "b" => "B",
                _ => b.Trim()
            };
        }
    }

    public class ComboEditForm : Form
    {
        // Result
        public ComboEntry? Result { get; private set; }

        // Controller image
        private PictureBox _picController = null!;
        private Image? _controllerImage;

        // Button toggle state
        private readonly HashSet<string> _selectedButtons = new();
        private string? _hoveredButton = null;

        // Mode
        private RadioButton _rdoPress = null!;
        private RadioButton _rdoDoubleTap = null!;
        private RadioButton _rdoTripleTap = null!;
        private RadioButton _rdoLongPress = null!;

        // Timing
        private NumericUpDown _nudTiming = null!;
        private Label _lblTiming = null!;

        // Target key
        private ComboBox _cmbKey = null!;

        // Target action (alternative to keyboard key) — context picker + action picker.
        // User picks "Main Gameplay" → "Hotkey3" and the form looks up that action's
        // keyboard scancode in the current controlmapvr.txt to use as the saved value.
        private RadioButton _rdoTargetKey = null!;
        private RadioButton _rdoTargetAction = null!;
        private ComboBox _cmbActionContext = null!;
        private ComboBox _cmbAction = null!;
        private Label _lblActionContext = null!;
        private Label _lblActionPick = null!;
        private Label _lblKey = null!;
        private Label _lblKeyHint = null!;

        // Buttons
        private Button _btnOk = null!;
        private Button _btnCancel = null!;
        private Label _lblPreview = null!;

        // Key scancodes (passed from MainForm)
        private readonly Dictionary<string, int> _keyScancodes;
        // Per-context action lists from controlmapvr.txt: context name → list of
        // (actionName, keyboardScancode) for actions that have a non-0xff keyboard
        // binding (only those can be combo targets).
        private readonly Dictionary<string, List<(string action, int scancode)>> _actionsByContext;

        // Button positions on the controller image
        // Stick directions are small circles around the stick center
        private static readonly Dictionary<string, (string display, PointF pos, bool isStickDir)> ComboButtons = new()
        {
            // Face buttons, grips, triggers
            { "x",             ("X",          new PointF(0.308f, 0.254f), false) },
            { "y",             ("Y",          new PointF(0.353f, 0.189f), false) },
            { "left_trigger",  ("L Trig",     new PointF(0.455f, 0.106f), false) },
            { "left_grip",     ("L Grip",     new PointF(0.383f, 0.512f), false) },
            { "a",             ("A",          new PointF(0.670f, 0.254f), false) },
            { "b",             ("B",          new PointF(0.627f, 0.189f), false) },
            { "right_trigger", ("R Trig",     new PointF(0.537f, 0.106f), false) },
            { "right_grip",    ("R Grip",     new PointF(0.605f, 0.515f), false) },

            // Left stick: click + 4 directions (0.060 vertical, 0.045 horizontal)
            { "left_stick",       ("L Click",  new PointF(0.272f, 0.153f), false) },
            { "left_stick_up",    ("\u25B2",   new PointF(0.272f, 0.153f - 0.060f), true) },
            { "left_stick_down",  ("\u25BC",   new PointF(0.272f, 0.153f + 0.060f), true) },
            { "left_stick_left",  ("\u25C0",   new PointF(0.272f - 0.045f, 0.153f), true) },
            { "left_stick_right", ("\u25B6",   new PointF(0.272f + 0.045f, 0.153f), true) },

            // Right stick: click + 4 directions (0.060 vertical, 0.045 horizontal)
            { "right_stick",       ("R Click",  new PointF(0.713f, 0.147f), false) },
            { "right_stick_up",    ("\u25B2",   new PointF(0.713f, 0.147f - 0.060f), true) },
            { "right_stick_down",  ("\u25BC",   new PointF(0.713f, 0.147f + 0.060f), true) },
            { "right_stick_left",  ("\u25C0",   new PointF(0.713f - 0.045f, 0.147f), true) },
            { "right_stick_right", ("\u25B6",   new PointF(0.713f + 0.045f, 0.147f), true) },
        };

        public ComboEditForm(Dictionary<string, int> keyScancodes,
                             Dictionary<string, List<(string action, int scancode)>>? actionsByContext = null,
                             ComboEntry? existing = null)
        {
            _keyScancodes = keyScancodes;
            _actionsByContext = actionsByContext ?? new Dictionary<string, List<(string action, int scancode)>>();
            LoadControllerImage();
            InitializeUI();

            if (existing != null)
                LoadExisting(existing);
        }

        private void LoadControllerImage()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.controllers.png");
            if (stream != null)
                _controllerImage = Image.FromStream(stream);
        }

        private void InitializeUI()
        {
            Text = "Edit Controller Combo";
            Size = new Size(750, 580);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            BackColor = Color.FromArgb(30, 30, 35);
            ForeColor = Color.White;
            Font = new Font("Segoe UI", 9.5f);

            int leftMargin = 12;
            int y = 10;

            // Section: Select Buttons
            var lblButtons = new Label
            {
                Text = "Select buttons (click on controller image):",
                Location = new Point(leftMargin, y),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold)
            };
            Controls.Add(lblButtons);
            y += 24;

            // Controller image
            _picController = new PictureBox
            {
                Location = new Point(leftMargin, y),
                Size = new Size(710, 290),
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(40, 40, 48),
                Image = _controllerImage,
                Cursor = Cursors.Hand
            };
            _picController.Paint += PicController_Paint;
            _picController.MouseMove += PicController_MouseMove;
            _picController.MouseClick += PicController_MouseClick;
            Controls.Add(_picController);
            y += 298;

            // Preview label
            _lblPreview = new Label
            {
                Text = "",
                Location = new Point(leftMargin, y),
                Size = new Size(710, 20),
                ForeColor = Color.FromArgb(255, 200, 40),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
                TextAlign = ContentAlignment.MiddleCenter
            };
            Controls.Add(_lblPreview);
            y += 26;

            // Mode section
            var lblMode = new Label
            {
                Text = "Activation Mode:",
                Location = new Point(leftMargin, y + 2),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold)
            };
            Controls.Add(lblMode);

            int modeX = leftMargin + 140;
            _rdoPress = MakeRadio("Press (hold)", modeX, y, 110);
            _rdoDoubleTap = MakeRadio("Double Tap", modeX + 115, y, 100);
            _rdoTripleTap = MakeRadio("Triple Tap", modeX + 220, y, 100);
            _rdoLongPress = MakeRadio("Long Press", modeX + 325, y, 100);
            _rdoPress.Checked = true;
            Controls.AddRange(new Control[] { _rdoPress, _rdoDoubleTap, _rdoTripleTap, _rdoLongPress });

            _rdoPress.CheckedChanged += (s, e) => UpdateTimingVisibility();
            _rdoDoubleTap.CheckedChanged += (s, e) => UpdateTimingVisibility();
            _rdoTripleTap.CheckedChanged += (s, e) => UpdateTimingVisibility();
            _rdoLongPress.CheckedChanged += (s, e) => UpdateTimingVisibility();
            y += 30;

            // Timing
            _lblTiming = new Label
            {
                Text = "Timing (ms):",
                Location = new Point(leftMargin + 140, y + 3),
                AutoSize = true,
                ForeColor = Color.White,
                Visible = false
            };
            Controls.Add(_lblTiming);

            _nudTiming = new NumericUpDown
            {
                Location = new Point(leftMargin + 240, y),
                Width = 80,
                Minimum = 100, Maximum = 3000, Increment = 50, Value = 500,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                Visible = false
            };
            Controls.Add(_nudTiming);
            y += 34;

            // Fire as: Key | Action — radio toggle. Default is Key (existing behavior).
            var lblFireAs = new Label
            {
                Text = "Fire as:",
                Location = new Point(leftMargin, y + 3),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold)
            };
            Controls.Add(lblFireAs);

            _rdoTargetKey = new RadioButton
            {
                Text = "Keyboard key",
                Location = new Point(leftMargin + 90, y + 1),
                AutoSize = true,
                Checked = true,
                ForeColor = Color.White
            };
            _rdoTargetKey.CheckedChanged += (s, e) => UpdateTargetVisibility();
            Controls.Add(_rdoTargetKey);

            _rdoTargetAction = new RadioButton
            {
                Text = "Skyrim action",
                Location = new Point(leftMargin + 230, y + 1),
                AutoSize = true,
                ForeColor = Color.White
            };
            _rdoTargetAction.CheckedChanged += (s, e) => UpdateTargetVisibility();
            // If we have no actions data (failed to harvest from controlmap), disable
            // the option so the user isn't stuck with an empty action list.
            _rdoTargetAction.Enabled = _actionsByContext.Count > 0;
            Controls.Add(_rdoTargetAction);
            y += 28;

            // Target key (existing behavior — visible when "Keyboard key" radio is selected)
            _lblKey = new Label
            {
                Text = "Fire Key:",
                Location = new Point(leftMargin, y + 3),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold)
            };
            Controls.Add(_lblKey);

            _cmbKey = new ComboBox
            {
                Location = new Point(leftMargin + 140, y),
                Width = 200,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 9f)
            };
            foreach (var kv in _keyScancodes.OrderBy(kv => kv.Value))
                _cmbKey.Items.Add($"{kv.Key}  (0x{kv.Value:x2})");
            if (_cmbKey.Items.Count > 0) _cmbKey.SelectedIndex = 0;
            Controls.Add(_cmbKey);

            _lblKeyHint = new Label
            {
                Text = "Skyrim reads this as a keyboard press",
                Location = new Point(leftMargin + 350, y + 3),
                AutoSize = true,
                ForeColor = Color.FromArgb(130, 130, 130),
                Font = new Font("Segoe UI", 8.5f, FontStyle.Italic)
            };
            Controls.Add(_lblKeyHint);

            // Target action — context picker + action picker, occupies the same row
            // as the Fire Key dropdown. Visibility toggled by the radio.
            _lblActionContext = new Label
            {
                Text = "Context:",
                Location = new Point(leftMargin, y + 3),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                Visible = false
            };
            Controls.Add(_lblActionContext);

            _cmbActionContext = new ComboBox
            {
                Location = new Point(leftMargin + 140, y),
                Width = 180,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 9f),
                Visible = false
            };
            foreach (var ctx in _actionsByContext.Keys)
                _cmbActionContext.Items.Add(ctx);
            if (_cmbActionContext.Items.Count > 0) _cmbActionContext.SelectedIndex = 0;
            _cmbActionContext.SelectedIndexChanged += (s, e) => RebuildActionList();
            Controls.Add(_cmbActionContext);

            _lblActionPick = new Label
            {
                Text = "Action:",
                Location = new Point(leftMargin + 332, y + 3),
                AutoSize = true,
                ForeColor = Color.FromArgb(180, 200, 255),
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                Visible = false
            };
            Controls.Add(_lblActionPick);

            _cmbAction = new ComboBox
            {
                Location = new Point(leftMargin + 392, y),
                Width = 200,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 9f),
                Visible = false
            };
            Controls.Add(_cmbAction);
            RebuildActionList();
            y += 40;

            // OK / Cancel
            _btnOk = new Button
            {
                Text = "OK",
                Location = new Point(ClientSize.Width / 2 - 110, y),
                Size = new Size(100, 32),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(40, 120, 40),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 9.5f, FontStyle.Bold),
                DialogResult = DialogResult.OK
            };
            _btnOk.FlatAppearance.BorderSize = 0;
            _btnOk.Click += BtnOk_Click;
            Controls.Add(_btnOk);

            _btnCancel = new Button
            {
                Text = "Cancel",
                Location = new Point(ClientSize.Width / 2 + 10, y),
                Size = new Size(100, 32),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(70, 50, 50),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 9.5f),
                DialogResult = DialogResult.Cancel
            };
            _btnCancel.FlatAppearance.BorderSize = 0;
            Controls.Add(_btnCancel);

            AcceptButton = _btnOk;
            CancelButton = _btnCancel;
        }

        private void LoadExisting(ComboEntry entry)
        {
            // Select buttons
            foreach (string btn in entry.ButtonString.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                _selectedButtons.Add(btn);

            // Set mode
            switch (entry.Mode)
            {
                case "double_tap": _rdoDoubleTap.Checked = true; break;
                case "triple_tap": _rdoTripleTap.Checked = true; break;
                case "long_press": _rdoLongPress.Checked = true; break;
                default: _rdoPress.Checked = true; break;
            }

            _nudTiming.Value = Math.Clamp(entry.TimingMs, 100, 3000);

            // Find key in combo box
            string hex = $"0x{entry.Scancode:x2}";
            for (int i = 0; i < _cmbKey.Items.Count; i++)
            {
                if (_cmbKey.Items[i]!.ToString()!.Contains(hex))
                {
                    _cmbKey.SelectedIndex = i;
                    break;
                }
            }

            UpdatePreview();
            _picController.Invalidate();
        }

        private void UpdateTimingVisibility()
        {
            bool showTiming = !_rdoPress.Checked;
            _lblTiming.Visible = showTiming;
            _nudTiming.Visible = showTiming;
        }

        private RadioButton MakeRadio(string text, int x, int y, int width)
        {
            return new RadioButton
            {
                Text = text,
                Location = new Point(x, y),
                Size = new Size(width, 24),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 9f),
                FlatStyle = FlatStyle.Flat
            };
        }

        // ── Controller Image Drawing ──

        private (float drawW, float drawH, float offX, float offY) GetImageBounds()
        {
            if (_controllerImage == null) return (0, 0, 0, 0);
            float imgW = _picController.Width;
            float imgH = _picController.Height;
            float imgAspect = (float)_controllerImage.Width / _controllerImage.Height;
            float boxAspect = imgW / imgH;

            if (boxAspect > imgAspect)
                return (imgH * imgAspect, imgH, (imgW - imgH * imgAspect) / 2, 0);
            else
                return (imgW, imgW / imgAspect, 0, (imgH - imgW / imgAspect) / 2);
        }

        private void PicController_Paint(object? sender, PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var (drawW, drawH, offX, offY) = GetImageBounds();

            foreach (var kvp in ComboButtons)
            {
                float cx = offX + kvp.Value.pos.X * drawW;
                float cy = offY + kvp.Value.pos.Y * drawH;

                bool isSelected = _selectedButtons.Contains(kvp.Key);
                bool isHovered = kvp.Key == _hoveredButton;
                bool isDir = kvp.Value.isStickDir;

                if (isDir)
                {
                    // Draw directional triangles for stick directions
                    var triPts = GetDirectionTriangle(kvp.Key, cx, cy);

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

                    // Ghost circle (always visible)
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
                }

                // Label on hover or selection (non-directional buttons only)
                if ((isHovered || isSelected) && !isDir)
                {
                    using var font = new Font("Segoe UI", 7f, FontStyle.Bold);
                    using var textBrush = new SolidBrush(Color.White);
                    var sf = new StringFormat { Alignment = StringAlignment.Center };
                    g.DrawString(kvp.Value.display, font, textBrush, cx, cy + 14f + 2, sf);
                }
            }
        }

        private string? HitTest(float fx, float fy)
        {
            string? closest = null;
            float closestDist = float.MaxValue;
            foreach (var kvp in ComboButtons)
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

        private static PointF[] GetDirectionTriangle(string key, float cx, float cy)
        {
            // Triangle size: 5px tip-to-base height, 7px base width
            float h = 5f, w = 3.5f;
            if (key.EndsWith("_up"))
                return new[] { new PointF(cx, cy - h), new PointF(cx - w, cy + h), new PointF(cx + w, cy + h) };
            if (key.EndsWith("_down"))
                return new[] { new PointF(cx, cy + h), new PointF(cx - w, cy - h), new PointF(cx + w, cy - h) };
            if (key.EndsWith("_left"))
                return new[] { new PointF(cx - h, cy), new PointF(cx + h, cy - w), new PointF(cx + h, cy + w) };
            // _right
            return new[] { new PointF(cx + h, cy), new PointF(cx - h, cy - w), new PointF(cx - h, cy + w) };
        }

        private void PicController_MouseMove(object? sender, MouseEventArgs e)
        {
            var (drawW, drawH, offX, offY) = GetImageBounds();
            if (drawW <= 0 || drawH <= 0) return;
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            string? hit = HitTest(fx, fy);
            if (hit != _hoveredButton)
            {
                _hoveredButton = hit;
                _picController.Invalidate();
            }
        }

        private void PicController_MouseClick(object? sender, MouseEventArgs e)
        {
            var (drawW, drawH, offX, offY) = GetImageBounds();
            if (drawW <= 0 || drawH <= 0) return;
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            string? hit = HitTest(fx, fy);
            if (hit != null)
            {
                if (_selectedButtons.Contains(hit))
                    _selectedButtons.Remove(hit);
                else
                    _selectedButtons.Add(hit);

                UpdatePreview();
                _picController.Invalidate();
            }
        }

        private void UpdatePreview()
        {
            if (_selectedButtons.Count == 0)
            {
                _lblPreview.Text = "Click buttons on the controller to select them";
                _lblPreview.ForeColor = Color.FromArgb(130, 130, 130);
            }
            else
            {
                string buttons = string.Join(" + ", _selectedButtons.Select(b =>
                    ComboButtons.TryGetValue(b, out var info) ? info.display : b));
                _lblPreview.Text = buttons;
                _lblPreview.ForeColor = Color.FromArgb(255, 200, 40);
            }
        }

        // Toggle visibility of the Key vs Action picker based on the radio.
        private void UpdateTargetVisibility()
        {
            bool action = _rdoTargetAction.Checked;
            _cmbKey.Visible = !action;
            _lblKey.Visible = !action;
            _lblKeyHint.Visible = !action;
            _cmbActionContext.Visible = action;
            _cmbAction.Visible = action;
            _lblActionContext.Visible = action;
            _lblActionPick.Visible = action;
        }

        // Repopulate the action dropdown based on the currently-selected context.
        private void RebuildActionList()
        {
            _cmbAction.Items.Clear();
            string ctx = _cmbActionContext.SelectedItem?.ToString() ?? "";
            if (_actionsByContext.TryGetValue(ctx, out var list))
            {
                foreach (var (action, _) in list)
                    _cmbAction.Items.Add(action);
            }
            if (_cmbAction.Items.Count > 0) _cmbAction.SelectedIndex = 0;
        }

        private void BtnOk_Click(object? sender, EventArgs e)
        {
            if (_selectedButtons.Count == 0)
            {
                MessageBox.Show("Please select at least one button on the controller.",
                    "No Buttons Selected", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                DialogResult = DialogResult.None;
                return;
            }

            // Validate target based on which mode is active.
            bool actionMode = _rdoTargetAction.Checked;
            if (actionMode)
            {
                if (_cmbAction.SelectedIndex < 0)
                {
                    MessageBox.Show("Please pick a context and an action.",
                        "No Action Selected", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    DialogResult = DialogResult.None;
                    return;
                }
            }
            else
            {
                if (_cmbKey.SelectedIndex < 0)
                {
                    MessageBox.Show("Please select a target key.",
                        "No Key Selected", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    DialogResult = DialogResult.None;
                    return;
                }
            }

            // Build result
            string mode;
            if (_rdoDoubleTap.Checked) mode = "double_tap";
            else if (_rdoTripleTap.Checked) mode = "triple_tap";
            else if (_rdoLongPress.Checked) mode = "long_press";
            else mode = "press";

            int scancode = 0x02;
            if (actionMode)
            {
                // Look up the chosen action's keyboard scancode in the picked
                // context. The combo internally still stores a scancode — Skyrim
                // sees a keyboard press, controlmap routes it to the action.
                string ctx = _cmbActionContext.SelectedItem?.ToString() ?? "";
                string actionName = _cmbAction.SelectedItem?.ToString() ?? "";
                if (_actionsByContext.TryGetValue(ctx, out var list))
                {
                    var found = list.FirstOrDefault(t => t.action == actionName);
                    if (!string.IsNullOrEmpty(found.action))
                        scancode = found.scancode;
                }
            }
            else
            {
                // Parse scancode from combo box item text (existing keyboard path)
                string itemText = _cmbKey.Items[_cmbKey.SelectedIndex]!.ToString()!;
                int parenIdx = itemText.IndexOf("(0x");
                if (parenIdx >= 0)
                {
                    string hexPart = itemText[(parenIdx + 3)..].TrimEnd(')');
                    if (int.TryParse(hexPart, System.Globalization.NumberStyles.HexNumber, null, out int parsed))
                        scancode = parsed;
                }
            }

            // Sort buttons: left-hand first, then right-hand
            var sorted = _selectedButtons.OrderBy(b => b switch
            {
                "left_grip" => 0, "left_trigger" => 1, "left_stick" => 2,
                "left_stick_up" => 3, "left_stick_down" => 4, "left_stick_left" => 5, "left_stick_right" => 6,
                "x" => 7, "y" => 8,
                "right_grip" => 9, "right_trigger" => 10, "right_stick" => 11,
                "right_stick_up" => 12, "right_stick_down" => 13, "right_stick_left" => 14, "right_stick_right" => 15,
                "a" => 16, "b" => 17,
                _ => 18
            });

            Result = new ComboEntry
            {
                ButtonString = string.Join("+", sorted),
                Mode = mode,
                TimingMs = _rdoPress.Checked ? 0 : (int)_nudTiming.Value,
                Scancode = scancode
            };
        }
    }
}
