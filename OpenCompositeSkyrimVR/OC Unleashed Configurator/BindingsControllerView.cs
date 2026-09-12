using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    // ═══════════════════════════════════════════════════════════════════════
    // BINDINGS TAB: controller photo switcher + draggable dot calibration.
    //
    // The hex codes are the same for every controller model because OCU
    // translates all of them to the same OpenVR legacy button ids. What
    // changes per model is only the photo and where the dots sit on it.
    // The controlmap writer stamps every VR column pair (Vive 4/5,
    // Oculus 6/7, WMR 8/9) so bindings apply no matter which controller
    // type the runtime reports (Index reports "knuckles" and reads the
    // Vive columns, which is why bindings used to silently not apply).
    //
    // Move Dots mode: drag any dot to the right spot on the photo; the
    // layout persists to ControllerDotLayouts.json beside the EXE so we
    // can bake the final coordinates into defaults later.
    // ═══════════════════════════════════════════════════════════════════════
    public partial class MainForm
    {
        private Image? _knucklesImage;
        private Image? _psvr2Image;
        private ComboBox _cmbControllerModel = null!;
        private CheckBox _chkMoveDots = null!;
        private string _controllerModelKey = "touch";
        private string? _dragCtrlButton;

        // Trackpad swipe shortcut controls (Settings tab, Index-only)
        private Label _lblTrackpadSwipe = null!;
        private ComboBox _cmbTrackpadSwipe = null!;
        private Label _lblTrackpadSwipeHint = null!;

        // Active layout the paint/hit-test/click handlers use. Starts as a
        // copy of the Touch defaults; swapped when the model changes.
        private Dictionary<string, (string display, PointF pos, bool isStickDir)> _activeControllerButtons = new();

        // Index knuckles defaults, baked from the hand-calibrated layout
        // (ControllerDotLayouts.json, 2026-09-11). The JSON override still
        // wins if the user recalibrates with Move Dots.
        private static readonly Dictionary<string, (string display, PointF pos, bool isStickDir)> ControllerButtonsKnuckles = new()
        {
            // Left controller
            { "left_stick",  ("L Stick Click", new PointF(0.236f, 0.101f), false) },
            { "x_button",    ("A Button",      new PointF(0.332f, 0.167f), false) },
            { "y_button",    ("B Button",      new PointF(0.347f, 0.111f), false) },
            { "l_trigger",   ("L Trigger",     new PointF(0.412f, 0.061f), false) },
            { "l_grip",      ("L Grip Squeeze", new PointF(0.327f, 0.472f), false) },
            // Right controller
            { "right_stick", ("R Stick Click", new PointF(0.751f, 0.111f), false) },
            { "a_button",    ("A Button",      new PointF(0.657f, 0.178f), false) },
            { "b_button",    ("B Button",      new PointF(0.639f, 0.125f), false) },
            { "r_trigger",   ("R Trigger",     new PointF(0.589f, 0.072f), false) },
            { "r_grip",      ("R Grip Squeeze", new PointF(0.671f, 0.469f), false) },
            // Index trackpad: the touch oval under the stick, Touch has no
            // equivalent. Click acts as A (lower half) / B-Menu (upper half),
            // or as the VRIK gesture input when VRIK Knuckles trackpad mode is on.
            { "l_trackpad_upper", ("L Trackpad Upper", new PointF(0.3082449f, 0.09549072f), false) },
            { "l_trackpad_lower", ("L Trackpad Lower", new PointF(0.27628574f, 0.17771883f), false) },
            { "r_trackpad_upper", ("R Trackpad Upper", new PointF(0.68056935f, 0.1193634f), false) },
            { "r_trackpad_lower", ("R Trackpad Lower", new PointF(0.70773464f, 0.17771883f), false) },
            // Left stick directions
            { "left_stick_up",    ("L Stick Up",    new PointF(0.238f, 0.056f), true) },
            { "left_stick_down",  ("L Stick Down",  new PointF(0.235f, 0.146f), true) },
            { "left_stick_left",  ("L Stick Left",  new PointF(0.209f, 0.103f), true) },
            { "left_stick_right", ("L Stick Right", new PointF(0.262f, 0.103f), true) },
            // Right stick directions
            { "right_stick_up",    ("R Stick Up",    new PointF(0.749f, 0.074f), true) },
            { "right_stick_down",  ("R Stick Down",  new PointF(0.752f, 0.156f), true) },
            { "right_stick_left",  ("R Stick Left",  new PointF(0.719f, 0.109f), true) },
            { "right_stick_right", ("R Stick Right", new PointF(0.781f, 0.111f), true) },
        };

        // PlayStation VR2 Sense defaults for the front-facing controller
        // artwork, calibrated by the user on 2026-09-11.
        // The logical ids intentionally remain Oculus-compatible:
        // Square/Triangle are left primary/secondary and Cross/Circle are
        // right primary/secondary. XR_KHR_generic_controller translates the
        // physical PSVR2 inputs to those same ids at runtime.
        private static readonly Dictionary<string, (string display, PointF pos, bool isStickDir)> ControllerButtonsPsvr2 = new()
        {
            // Left Sense controller
            { "left_stick",  ("L Stick Click",  new PointF(0.30901858f, 0.060190395f), false) },
            { "x_button",    ("Square Button",   new PointF(0.363f, 0.186f), false) },
            { "y_button",    ("Triangle Button", new PointF(0.379f, 0.099f), false) },
            { "l_trigger",   ("L2 Trigger",      new PointF(0.405f, 0.245f), false) },
            { "l_grip",      ("L1 Grip",         new PointF(0.322f, 0.618f), false) },
            // Right Sense controller
            { "right_stick", ("R Stick Click", new PointF(0.6896552f, 0.06288036f), false) },
            { "a_button",    ("Cross Button",  new PointF(0.6366048f, 0.1893088f), false) },
            { "b_button",    ("Circle Button", new PointF(0.621f, 0.101f), false) },
            { "r_trigger",   ("R2 Trigger",    new PointF(0.595f, 0.245f), false) },
            { "r_grip",      ("R1 Grip",       new PointF(0.678f, 0.618f), false) },
            // Stick directions are deliberately compact around the visible caps.
            { "left_stick_up",    ("L Stick Up",    new PointF(0.30901858f, 0.006391052f), true) },
            { "left_stick_down",  ("L Stick Down",  new PointF(0.30901858f, 0.1166797f), true) },
            { "left_stick_left",  ("L Stick Left",  new PointF(0.28249338f, 0.065570325f), true) },
            { "left_stick_right", ("L Stick Right", new PointF(0.33421752f, 0.06288036f), true) },
            { "right_stick_up",    ("R Stick Up",    new PointF(0.69098145f, 0.009081019f), true) },
            { "right_stick_down",  ("R Stick Down",  new PointF(0.69098145f, 0.11129977f), true) },
            { "right_stick_left",  ("R Stick Left",  new PointF(0.66445625f, 0.065570325f), true) },
            { "right_stick_right", ("R Stick Right", new PointF(0.7161804f, 0.065570325f), true) },
        };

        private static string DotLayoutPath => Path.Combine(AppContext.BaseDirectory, "ControllerDotLayouts.json");
        private static string UiStatePath => Path.Combine(AppContext.BaseDirectory, "ConfiguratorUI.json");

        // The controller choice persists beside the EXE so an Index owner sees
        // their controller every launch until they switch back.
        private static string LoadUiModelChoice()
        {
            try
            {
                if (!File.Exists(UiStatePath)) return "touch";
                var state = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(UiStatePath));
                if (state != null && state.TryGetValue("controllerModel", out var model) &&
                    model is "knuckles" or "psvr2")
                    return model;
                return "touch";
            }
            catch { return "touch"; }
        }

        private bool SaveUiModelChoice()
        {
            try
            {
                File.WriteAllText(UiStatePath, JsonSerializer.Serialize(
                    new Dictionary<string, string> { ["controllerModel"] = _controllerModelKey },
                    new JsonSerializerOptions { WriteIndented = true }));
                return true;
            }
            catch { return false; }
        }

        private void LoadKnucklesImage()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.knuckles.png");
            if (stream != null)
                _knucklesImage = Image.FromStream(stream);
        }

        private void LoadPsvr2Image()
        {
            var assembly = Assembly.GetExecutingAssembly();
            using var stream = assembly.GetManifestResourceStream("OpenCompositeConfigurator.Resources.psvr2_sense.png");
            if (stream != null)
                _psvr2Image = Image.FromStream(stream);
        }

        private Image? ActiveControllerImage => _controllerModelKey switch
        {
            "knuckles" when _knucklesImage != null => _knucklesImage,
            "psvr2" when _psvr2Image != null => _psvr2Image,
            _ => _controllerImage,
        };

        internal static bool IsTrackpadButton(string? id) => TrackpadRegion(id) >= 0;

        // Grips and triggers are big physical targets, they keep the full
        // Oculus circle size even on knuckles.
        private static bool IsFullSizeDot(string key) => key.Contains("grip") || key.Contains("trigger");
        private static bool IsStickClickDot(string key) => key is "left_stick" or "right_stick";

        // Knuckles and PSVR2 face dots are smaller because their inputs are
        // packed more tightly in the source artwork.
        private bool UsesCompactDots => _controllerModelKey is "knuckles" or "psvr2";
        private float DotRadiusFor(string key)
        {
            if (IsFullSizeDot(key)) return 14f;
            return _controllerModelKey switch
            {
                "psvr2" when IsStickClickDot(key) => 11f,
                "psvr2" => 9f,
                "knuckles" => 7f,
                _ => 14f,
            };
        }
        private float HitRadiusFor(string key, bool isStickDir)
        {
            if (IsTrackpadButton(key)) return 0.018f;
            if (IsFullSizeDot(key)) return isStickDir ? 0.025f : 0.04f;
            return _controllerModelKey switch
            {
                "psvr2" when IsStickClickDot(key) => 0.036f,
                "psvr2" => isStickDir ? 0.022f : 0.032f,
                "knuckles" => isStickDir ? 0.018f : 0.026f,
                _ => isStickDir ? 0.025f : 0.04f,
            };
        }

        // Row under the controller photo: model dropdown + dot calibration toggle
        private void BuildControllerSwitcherRow(Control container, int x, int y, int width)
        {
            var lblModel = new Label
            {
                Text = "Controller",
                Location = new Point(x, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblModel);

            _cmbControllerModel = new ComboBox
            {
                Location = new Point(x + 66, y),
                Width = 180,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = Color.FromArgb(50, 50, 55),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f),
            };
            _cmbControllerModel.Items.Add("Oculus / Quest Touch");
            _cmbControllerModel.Items.Add("Valve Index Knuckles");
            _cmbControllerModel.Items.Add("PlayStation VR2 Sense");
            _cmbControllerModel.SelectedIndex = 0; // default Meta / Quest Touch

            // Explicit Save button so the choice only sticks when the user commits it.
            // (Auto-saving on every dropdown change made an accidental flick to Index
            //  persist and greet Meta owners with knuckles on the next launch.)
            var btnSaveController = new ModernPillButton
            {
                Text = "Save",
                Location = new Point(x + 252, y),
                Size = new Size(66, 24),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(40, 120, 40),
                ForeColor = Color.White,
                Font = new Font("Segoe UI", 8.5f),
            };
            btnSaveController.FlatAppearance.BorderColor = Color.FromArgb(70, 145, 70);

            _cmbControllerModel.SelectedIndexChanged += (s, e) =>
            {
                // Live-update the picture on change, but do NOT persist until Save is clicked.
                string model = _cmbControllerModel.SelectedIndex switch
                {
                    1 => "knuckles",
                    2 => "psvr2",
                    _ => "touch",
                };
                ApplyControllerModel(model);
                btnSaveController.Text = "Save";
            };
            container.Controls.Add(_cmbControllerModel);

            btnSaveController.Click += (s, e) =>
            {
                if (SaveUiModelChoice())
                {
                    AcceptTrackedControlAsSaved(_cmbControllerModel);
                    btnSaveController.Text = "Saved";
                }
                else
                {
                    btnSaveController.Text = "Retry";
                    _lblKbStatus.Text = "Controller picture choice could not be saved.";
                    _lblKbStatus.ForeColor = Color.FromArgb(255, 100, 100);
                }
            };
            container.Controls.Add(btnSaveController);

            // Restore the persisted choice (fires the change handler when non-Touch).
            // No saved file => stays on the Meta/Touch default above.
            _cmbControllerModel.SelectedIndex = LoadUiModelChoice() switch
            {
                "knuckles" => 1,
                "psvr2" => 2,
                _ => 0,
            };

            _chkMoveDots = new ModernCheckBox
            {
                Text = "Move dots (drag to calibrate, saves on release)",
                Location = new Point(x + 326, y + 2),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(_chkMoveDots);
        }

        private void ApplyControllerModel(string key)
        {
            _controllerModelKey = key;
            var defaults = key switch
            {
                "knuckles" => ControllerButtonsKnuckles,
                "psvr2" => ControllerButtonsPsvr2,
                _ => ControllerButtons,
            };
            _activeControllerButtons = defaults.ToDictionary(kv => kv.Key, kv => kv.Value);
            ApplyDotOverrides(key);

            _selectedCtrlButton = null;
            _hoveredCtrlButton = null;
            _dragCtrlButton = null;
            if (_picBindingsController != null)
            {
                _picBindingsController.Image = ActiveControllerImage;
                _picBindingsController.Invalidate();
            }
            // Mirror the photo on the Settings tab controller picture too, and
            // force a repaint so the shortcut circles jump to the new layout
            // immediately (the Image swap alone was not refreshing the overlay).
            if (_picControllers != null)
            {
                _picControllers.Image = ActiveControllerImage;
                _picControllers.Invalidate();
                _picControllers.Update();
            }

            // The trackpad swipe shortcut only exists on Index knuckles
            bool knuckles = key == "knuckles";
            if (_lblTrackpadSwipe != null) _lblTrackpadSwipe.Visible = knuckles;
            if (_cmbTrackpadSwipe != null) _cmbTrackpadSwipe.Visible = knuckles;
            if (_lblTrackpadSwipeHint != null) _lblTrackpadSwipeHint.Visible = knuckles;

            // Gestures tab: hold-button options follow the controller model
            RefreshGestureHoldOptions();
            RefreshWalkActivationOptions();
        }

        // VR Keyboard Shortcut picture (Settings tab): model-aware positions
        // for the clickable stick/A/B/X/Y dots, fed from the same calibrated
        // layout the Bindings tab uses.
        private Dictionary<string, PointF[]> ShortcutButtonPositions()
        {
            if (_controllerModelKey == "touch")
                return ButtonPositions;

            var map = new Dictionary<string, PointF[]>();
            void Add(string shortcutKey, string bindingsKey)
            {
                if (_activeControllerButtons.TryGetValue(bindingsKey, out var entry))
                    map[shortcutKey] = new[] { entry.pos };
            }
            Add("left_stick", "left_stick");
            Add("x", "x_button");
            Add("y", "y_button");
            Add("right_stick", "right_stick");
            Add("a", "a_button");
            Add("b", "b_button");
            return map;
        }

        private float ShortcutDotRadius => UsesCompactDots ? 7f : 10f;
        private float ShortcutHitRadius => UsesCompactDots ? 0.03f : 0.06f;

        // Push the current model (photo + calibrated positions) into the combo
        // editor popup so the VR keyboard shortcut picker mirrors this tab.
        private void SyncComboEditorModel()
        {
            ComboEditForm.ControllerModelKey = _controllerModelKey;
            if (_controllerModelKey == "touch")
            {
                ComboEditForm.ModelPositionOverrides = null;
                return;
            }
            var map = new Dictionary<string, PointF>();
            foreach (var kv in _activeControllerButtons)
            {
                string comboKey = kv.Key switch
                {
                    "x_button" => "x",
                    "y_button" => "y",
                    "l_trigger" => "left_trigger",
                    "l_grip" => "left_grip",
                    "a_button" => "a",
                    "b_button" => "b",
                    "r_trigger" => "right_trigger",
                    "r_grip" => "right_grip",
                    _ => kv.Key, // sticks + directions share names; trackpads have no combo entry
                };
                map[comboKey] = kv.Value.pos;
            }
            ComboEditForm.ModelPositionOverrides = map;
        }

        // ── Dot layout persistence ──

        private void ApplyDotOverrides(string key)
        {
            try
            {
                if (!File.Exists(DotLayoutPath)) return;
                var all = JsonSerializer.Deserialize<Dictionary<string, Dictionary<string, float[]>>>(
                    File.ReadAllText(DotLayoutPath));
                if (all == null || !all.TryGetValue(key, out var layout)) return;

                foreach (var kv in layout)
                {
                    if (kv.Value.Length < 2) continue;
                    // Preserve calibrated old trackpad centers when splitting them.
                    if (kv.Key is "l_trackpad" or "r_trackpad")
                    {
                        foreach (string half in new[] { "upper", "lower" })
                        {
                            string splitKey = kv.Key + "_" + half;
                            if (!layout.ContainsKey(splitKey) && _activeControllerButtons.TryGetValue(splitKey, out var split))
                                _activeControllerButtons[splitKey] = (split.display,
                                    new PointF(kv.Value[0], kv.Value[1] + (half == "upper" ? -0.020f : 0.020f)), false);
                        }
                    }
                    if (_activeControllerButtons.TryGetValue(kv.Key, out var entry))
                        _activeControllerButtons[kv.Key] = (entry.display, new PointF(kv.Value[0], kv.Value[1]), entry.isStickDir);
                }
            }
            catch { /* corrupt layout file: fall back to defaults */ }
        }

        private void SaveDotOverrides()
        {
            try
            {
                Dictionary<string, Dictionary<string, float[]>> all = new();
                if (File.Exists(DotLayoutPath))
                {
                    try
                    {
                        all = JsonSerializer.Deserialize<Dictionary<string, Dictionary<string, float[]>>>(
                            File.ReadAllText(DotLayoutPath)) ?? new();
                    }
                    catch { all = new(); }
                }

                all[_controllerModelKey] = _activeControllerButtons.ToDictionary(
                    kv => kv.Key, kv => new[] { kv.Value.pos.X, kv.Value.pos.Y });

                File.WriteAllText(DotLayoutPath, JsonSerializer.Serialize(all, new JsonSerializerOptions { WriteIndented = true }));
                _lblKbStatus.Text = $"Dot layout saved for {_controllerModelKey} ({Path.GetFileName(DotLayoutPath)})";
                _lblKbStatus.ForeColor = Color.FromArgb(100, 200, 100);
            }
            catch (Exception ex)
            {
                _lblKbStatus.Text = $"Dot layout save failed: {ex.Message}";
                _lblKbStatus.ForeColor = Color.FromArgb(255, 100, 100);
            }
        }

        // ── Drag handlers (wired from BuildKeyboardTab) ──

        private void PicBindingsController_MouseDown(object? sender, MouseEventArgs e)
        {
            if (!_chkMoveDots.Checked || e.Button != MouseButtons.Left) return;
            var (drawW, drawH, offX, offY) = GetBindingsImageBounds();
            float fx = (e.X - offX) / drawW;
            float fy = (e.Y - offY) / drawH;

            string? closest = null;
            float best = float.MaxValue;
            foreach (var kvp in _activeControllerButtons)
            {
                if (!IsControllerButtonVisible(kvp.Key)) continue;
                float dx = fx - kvp.Value.pos.X, dy = fy - kvp.Value.pos.Y;
                float d = (float)Math.Sqrt(dx * dx + dy * dy);
                if (d < 0.05f && d < best) { best = d; closest = kvp.Key; }
            }
            _dragCtrlButton = closest;
        }

        private void PicBindingsController_MouseUp(object? sender, MouseEventArgs e)
        {
            if (_dragCtrlButton != null)
            {
                _dragCtrlButton = null;
                SaveDotOverrides();
            }
        }

        // Returns true when the move handled a drag (caller skips hover logic)
        private bool HandleDotDragMove(MouseEventArgs e)
        {
            if (_dragCtrlButton == null || !_chkMoveDots.Checked) return false;
            var (drawW, drawH, offX, offY) = GetBindingsImageBounds();
            float fx = Math.Clamp((e.X - offX) / drawW, 0f, 1f);
            float fy = Math.Clamp((e.Y - offY) / drawH, 0f, 1f);
            if (_activeControllerButtons.TryGetValue(_dragCtrlButton, out var entry))
            {
                _activeControllerButtons[_dragCtrlButton] = (entry.display, new PointF(fx, fy), entry.isStickDir);
                _picBindingsController.Invalidate();
            }
            return true;
        }
    }
}
