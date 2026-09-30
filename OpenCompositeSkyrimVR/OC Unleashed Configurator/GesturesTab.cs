using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    // ═══════════════════════════════════════════════════════════════════════
    // GESTURES TAB
    // Draw a controller motion per hand (one box = one-hand gesture, both
    // boxes = two-hand gesture), bind it to a key, save it with a rendered
    // snapshot. Saved gestures live in <exe>\Gestures\<name>.json + .png and
    // are the file contract the in-game recognizer will read.
    // ═══════════════════════════════════════════════════════════════════════
    public partial class MainForm
    {
        private Button _btnTabGestures = null!;
        private Panel _tabGestures = null!;

        private GestureCanvas _gestureLeft = null!;
        private GestureCanvas _gestureRight = null!;
        private TextBox _txtGestureName = null!;
        private ComboBox _cmbGestureHold = null!;
        private ComboBox _cmbGestureKeyPick = null!;
        private FlowLayoutPanel _pnlGestureLibrary = null!;
        private int _gestureLibraryPage;
        private Button _gestureLibraryPrevious = null!;
        private Button _gestureLibraryNext = null!;
        private Label _gestureLibraryPageLabel = null!;
        private Label _lblGestureStatus = null!;

        // Stable ids the runtime recognizer will match against, split button by
        // button per hand. Labels follow the controller model selected on the
        // Bindings tab, so the list always matches the pictured controller.
        private static readonly (string id, string label)[] HoldOptionsTouch =
        {
            ("", "None (always listening)"),
            ("l_trigger", "L Trigger"),
            ("r_trigger", "R Trigger"),
            ("l_grip", "L Grip"),
            ("r_grip", "R Grip"),
            ("l_a", "X Button (left)"),
            ("l_b", "Y Button (left)"),
            ("r_a", "A Button (right)"),
            ("r_b", "B Button (right)"),
            ("l_stick", "L Stick Click"),
            ("r_stick", "R Stick Click"),
        };

        private static readonly (string id, string label)[] HoldOptionsKnuckles =
        {
            ("", "None (always listening)"),
            ("l_trigger", "L Trigger"),
            ("r_trigger", "R Trigger"),
            ("l_grip", "L Grip Squeeze"),
            ("r_grip", "R Grip Squeeze"),
            ("l_a", "L A Button"),
            ("l_b", "L B Button"),
            ("r_a", "R A Button"),
            ("r_b", "R B Button"),
            ("l_stick", "L Stick Click"),
            ("r_stick", "R Stick Click"),
            ("l_trackpad", "L Trackpad Click"),
            ("r_trackpad", "R Trackpad Click"),
        };

        private (string id, string label)[] CurrentHoldOptions =>
            _controllerModelKey == "knuckles" ? HoldOptionsKnuckles :
            _controllerModelKey == "frame" ? HoldOptionsTouch.Select(o =>
                (o.id, o.id == "l_a" ? "D-pad Down (left)" : o.id == "l_b" ? "D-pad Up (left)" : o.label)).ToArray() : HoldOptionsTouch;

        // Rebuild the hold dropdown for the active controller model, keeping
        // the selected id when it exists on both controllers.
        private void RefreshGestureHoldOptions()
        {
            if (_cmbGestureHold == null) return;
            string keepId = SelectedHoldId();
            _cmbGestureHold.Items.Clear();
            foreach (var opt in CurrentHoldOptions)
                _cmbGestureHold.Items.Add(opt.label);
            int idx = Array.FindIndex(CurrentHoldOptions, o => o.id == keepId);
            _cmbGestureHold.SelectedIndex = idx >= 0 ? idx : 0;
        }

        private string SelectedHoldId()
        {
            if (_cmbGestureHold == null || _cmbGestureHold.SelectedIndex < 0) return "";
            var opts = CurrentHoldOptions;
            int i = _cmbGestureHold.SelectedIndex;
            return i < opts.Length ? opts[i].id : "";
        }

        private string HoldLabelFor(string id)
        {
            // Prefer the active model's wording, fall back to the other set
            var hit = CurrentHoldOptions.FirstOrDefault(o => o.id == id);
            if (hit.label != null) return hit.label;
            hit = HoldOptionsKnuckles.FirstOrDefault(o => o.id == id);
            if (hit.label != null) return hit.label;
            hit = HoldOptionsTouch.FirstOrDefault(o => o.id == id);
            return hit.label ?? id;
        }

        // Gestures saved before the per-hand split used hand-agnostic ids.
        // Map them to the hand the gesture was drawn with (right for two-hand).
        private static string MigrateLegacyHoldId(string id, bool leftOnly)
        {
            switch (id)
            {
                case "trigger":
                case "grip":
                case "a":
                case "b":
                case "trackpad":
                    return (leftOnly ? "l_" : "r_") + id;
                case "stick":
                    return leftOnly ? "l_stick" : "r_stick";
                default:
                    return id;
            }
        }

        private static string GesturesDir => Path.Combine(AppContext.BaseDirectory, "Gestures");

        // Serialized gesture: normalized 0..1 points, one list of strokes per hand.
        private sealed class GestureData
        {
            public int Version { get; set; } = 1;
            public string Name { get; set; } = "";
            public int Vk { get; set; }
            public int Scancode { get; set; }            // DIK scancode, same table as combos
            public string KeyDisplay { get; set; } = "";
            public string HoldButton { get; set; } = ""; // "", trigger, grip, a, b, stick, trackpad
            public string Action { get; set; } = "key"; // key | spell | equip_left | equip_right
            public string SpellPlugin { get; set; } = "";
            public string SpellFormId { get; set; } = ""; // local form id, hex string
            public string SpellName { get; set; } = "";
            // Concentration spells (Flames-style streams) channel in-game for as
            // long as the gesture's hold button stays down after the match
            public bool Concentration { get; set; } = false;
            public string TrailColor { get; set; } = "cyan"; // in-game trail color (named)
            public string RuneColor { get; set; } = ""; // flash color when the shape completes ("" = same as trail)
            public double TrailWidth { get; set; } = 1.0; // in-game stroke thickness multiplier
            public List<string> TrailStyle { get; set; } = new() { "glowing" }; // transparent, smoky, wispy, glowing
            public List<List<float[]>> Left { get; set; } = new();
            public List<List<float[]>> Right { get; set; } = new();
        }

        private static readonly string[] TrailColors =
        {
            "cyan", "blue", "purple", "green", "orange", "red", "pink", "white", "gold", "black",
        };

        private ComboBox _cmbTrailColor = null!;
        private ComboBox _cmbRuneColor = null!;
        private ComboBox _cmbTrailWidth = null!;
        private static readonly (string label, double mult)[] TrailWidths =
        {
            ("Thin", 0.6), ("Normal", 1.0), ("Thick", 1.5), ("Massive", 2.2),
        };
        private CheckBox _chkTrailTransparent = null!;
        private CheckBox _chkTrailSmoky = null!;
        private CheckBox _chkTrailWispy = null!;
        private CheckBox _chkTrailGlowing = null!;
        private CheckBox _chkGestureSounds = null!;
        private ComboBox _cmbFinishSound = null!;
        private ComboBox _cmbGestureAction = null!;
        private ComboBox _cmbGestureSpell = null!;
        private Label _lblSpellCount = null!;

        private sealed class SpellEntry
        {
            public string name { get; set; } = "";
            public string plugin { get; set; } = "";
            public string formId { get; set; } = "";
            public string type { get; set; } = "";
            public string casting { get; set; } = ""; // "conc" = concentration stream, "ff" = fire-and-forget
            public bool known { get; set; } = false;
        }

        private List<SpellEntry> _spellListFull = new();
        private List<SpellEntry> _spellList = new(); // filtered view backing the picker
        private DateTime _spellListStamp = DateTime.MinValue; // last dump we parsed

        // The game (re)dumps the list at data load and on every save load, which
        // can happen while we sit open: re-read whenever the file on disk is newer.
        private void RefreshSpellListIfStale()
        {
            try
            {
                if (!File.Exists(SpellListPath)) return;
                if (File.GetLastWriteTimeUtc(SpellListPath) != _spellListStamp || _spellListFull.Count == 0)
                    LoadSpellList();
            }
            catch { }
        }
        private CheckBox _chkKnownSpellsOnly = null!;

        private static readonly string[] GestureActionIds = { "key", "spell", "equip_left", "equip_right" };

        private static string SpellListPath => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            "My Games", "Skyrim VR", "SKSE", "OCUGestureSpellList.json");

        // Loaded-game spell list, dumped by the SKSE plugin at data load and
        // re-dumped with "known" flags every time a save is loaded
        private void LoadSpellList()
        {
            _spellListFull.Clear();
            try
            {
                if (File.Exists(SpellListPath))
                {
                    var list = JsonSerializer.Deserialize<List<SpellEntry>>(File.ReadAllText(SpellListPath));
                    if (list != null)
                        _spellListFull = list.Where(s => !string.IsNullOrEmpty(s.name)).OrderBy(s => s.name).ToList();
                }
            }
            catch { /* stale or partial dump: picker just stays empty */ }
            try { _spellListStamp = File.GetLastWriteTimeUtc(SpellListPath); } catch { }

            bool anyKnown = _spellListFull.Any(s => s.known);
            bool filterKnown = _chkKnownSpellsOnly != null && _chkKnownSpellsOnly.Checked && anyKnown;
            _spellList = filterKnown ? _spellListFull.Where(s => s.known).ToList() : _spellListFull;

            _cmbGestureSpell.Items.Clear();
            foreach (var s in _spellList)
                _cmbGestureSpell.Items.Add($"{s.name}  [{s.plugin}]" + (s.type == "power" ? "  (power)" : ""));

            if (_spellListFull.Count == 0)
                _lblSpellCount.Text = "No spell list yet: launch the game once, the plugin dumps every loaded spell";
            else if (filterKnown)
                _lblSpellCount.Text = $"{_spellList.Count} spells your character knows (of {_spellListFull.Count} loaded)";
            else if (_chkKnownSpellsOnly != null && _chkKnownSpellsOnly.Checked && !anyKnown)
                _lblSpellCount.Text = $"{_spellList.Count} loaded spells (load a save in-game to tag which ones you know)";
            else
                _lblSpellCount.Text = $"{_spellList.Count} spells from your load order";
        }

        private void BuildGesturesTab()
        {
            CleanOverwriteGestureShadows();
            var container = _tabGestures;
            int leftMargin = 6;
            int rightEdge = container.ClientSize.Width - 6;
            int y = 8;

            var lblIntro = new Label
            {
                Text = "Draw ONE continuous stroke per hand (drawing again replaces it): in-game a cast is one unbroken motion while the hold button is down.\n" +
                       "Use both boxes for a two-hand gesture. Bind a key, pick the trail look, and save; the game hot-loads changes within seconds.",
                Location = new Point(leftMargin, y),
                Size = new Size(rightEdge - leftMargin, 34),
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(165, 168, 178),
            };
            container.Controls.Add(lblIntro);
            y += 40;

            // ── Draw boxes: perfect squares, left-aligned; the gesture library
            // lives in the leftover column to their right instead of a full-width
            // strip below, so the tab stays short enough for any desktop. The
            // boxes still shrink on small screens as the last resort.
            int screenH = (Screen.PrimaryScreen?.WorkingArea.Height) ?? 1080;
            int budget = screenH - container.Top - 150; // header above + footer/taskbar clearance below
            const int fixedRows = 290; // every row on this tab except the draw boxes (incl. the split-out Style row)
            // Box cap trimmed from 420 so the taller (split) trail section still clears the taskbar.
            int boxCap = 390;
            int gap = 10;
            int boxSide = Math.Max(280, Math.Min(boxCap, budget - fixedRows));
            int pairW = boxSide * 2 + gap;
            int startX = leftMargin;
            int boxTop = y;

            _gestureLeft = new GestureCanvas("LEFT HAND", Color.FromArgb(0, 195, 255))
            {
                Location = new Point(startX, y),
                Size = new Size(boxSide, boxSide),
            };
            container.Controls.Add(_gestureLeft);

            _gestureRight = new GestureCanvas("RIGHT HAND", Color.FromArgb(255, 150, 50))
            {
                Location = new Point(startX + boxSide + gap, y),
                Size = new Size(boxSide, boxSide),
            };
            container.Controls.Add(_gestureRight);

            // ── Library: paged column beside the boxes ──
            int libX = startX + pairW + 12;
            var lblLib = new Label
            {
                Text = "Library",
                Location = new Point(libX, boxTop),
                AutoSize = true,
                Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                ForeColor = Color.White,
            };
            container.Controls.Add(lblLib);
            _gestureLibraryPrevious = new ModernPillButton { Text = "‹", AccessibleName = "Previous gesture page", Location = new Point(rightEdge - 108, boxTop - 2), Size = new Size(28, 24) };
            _gestureLibraryNext = new ModernPillButton { Text = "›", AccessibleName = "Next gesture page", Location = new Point(rightEdge - 28, boxTop - 2), Size = new Size(28, 24) };
            _gestureLibraryPageLabel = new Label { Text = "1 / 1", Location = new Point(rightEdge - 78, boxTop + 2), Size = new Size(48, 20), TextAlign = ContentAlignment.MiddleCenter };
            _gestureLibraryPrevious.Click += (_, _) => { if (_gestureLibraryPage > 0) { --_gestureLibraryPage; RefreshGestureLibrary(); } };
            _gestureLibraryNext.Click += (_, _) => { ++_gestureLibraryPage; RefreshGestureLibrary(); };
            container.Controls.Add(_gestureLibraryPrevious); container.Controls.Add(_gestureLibraryPageLabel); container.Controls.Add(_gestureLibraryNext);

            _pnlGestureLibrary = new FlowLayoutPanel
            {
                Location = new Point(libX, boxTop + 24),
                // Bottom edge lines up with the Smooth Shapes row under the boxes
                Size = new Size(rightEdge - libX, boxSide + 46),
                BackColor = Color.FromArgb(24, 25, 31),
                AutoScroll = false,
                WrapContents = true,
                FlowDirection = FlowDirection.LeftToRight,
            };
            container.Controls.Add(_pnlGestureLibrary);

            y += boxSide + 6;

            // ── Clear + smooth controls under the boxes ──
            var btnClearLeft = MakeGestureButton("Clear Left", new Point(startX, y), 90);
            btnClearLeft.Click += (s, e) => { _gestureLeft.ClearStrokes(); };
            container.Controls.Add(btnClearLeft);

            var btnClearBoth = MakeGestureButton("Clear Both", new Point(startX + (pairW - 90) / 2, y), 90);
            btnClearBoth.Click += (s, e) => { _gestureLeft.ClearStrokes(); _gestureRight.ClearStrokes(); };
            container.Controls.Add(btnClearBoth);

            var btnClearRight = MakeGestureButton("Clear Right", new Point(startX + pairW - 90, y), 90);
            btnClearRight.Click += (s, e) => { _gestureRight.ClearStrokes(); };
            container.Controls.Add(btnClearRight);
            y += 30;

            var btnSmooth = MakeGestureButton("Smooth Shapes", new Point(startX, y), 120);
            btnSmooth.Click += (s, e) => { _gestureLeft.SmoothStrokes(); _gestureRight.SmoothStrokes(); };
            container.Controls.Add(btnSmooth);

            var chkAutoSmooth = new ModernCheckBox
            {
                Text = "Smooth as you draw (lines, squares, W's snap clean; curves and spirals de-jitter)",
                Location = new Point(startX + 128, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            chkAutoSmooth.CheckedChanged += (s, e) =>
            {
                _gestureLeft.AutoSmooth = chkAutoSmooth.Checked;
                _gestureRight.AutoSmooth = chkAutoSmooth.Checked;
            };
            container.Controls.Add(chkAutoSmooth);
            y += 34;

            // ── Name / key / save row ──
            var lblName = new Label
            {
                Text = "Name",
                Location = new Point(leftMargin, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblName);

            _txtGestureName = new TextBox
            {
                Location = new Point(leftMargin + 48, y),
                Size = new Size(190, 26),
                Font = new Font("Segoe UI", 9.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                BorderStyle = BorderStyle.FixedSingle,
            };
            container.Controls.Add(_txtGestureName);

            // Key dropdown: a scrollable list beats a physical keypress in VR.
            // Same key table the controller combos use.
            var lblKey = new Label
            {
                Text = "Key",
                Location = new Point(leftMargin + 250, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblKey);

            _cmbGestureKeyPick = new ComboBox
            {
                Location = new Point(leftMargin + 282, y),
                Size = new Size(120, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                MaxDropDownItems = 16,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            _cmbGestureKeyPick.Items.Add("(no key)");
            foreach (var keyName in KeyScancodes.Keys)
                _cmbGestureKeyPick.Items.Add(keyName);
            _cmbGestureKeyPick.SelectedIndex = 0;
            container.Controls.Add(_cmbGestureKeyPick);

            var lblHold = new Label
            {
                Text = "Hold while gesturing",
                Location = new Point(leftMargin + 414, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblHold);

            _cmbGestureHold = new ComboBox
            {
                Location = new Point(leftMargin + 542, y),
                Size = new Size(170, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            RefreshGestureHoldOptions();
            container.Controls.Add(_cmbGestureHold);

            var btnSave = MakeGestureButton("Save Gesture", new Point(leftMargin + 724, y), 120);
            btnSave.BackColor = Color.FromArgb(40, 120, 40);
            btnSave.Click += (s, e) => SaveCurrentGesture();
            container.Controls.Add(btnSave);

            _lblGestureStatus = new Label
            {
                Text = "",
                Location = new Point(leftMargin + 854, y + 4),
                Size = new Size(rightEdge - leftMargin - 854, 20),
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(120, 200, 120),
            };
            container.Controls.Add(_lblGestureStatus);
            y += 32;

            // ── Action: what a successful cast does ──
            var lblAction = new Label
            {
                Text = "Action",
                Location = new Point(leftMargin, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblAction);

            _cmbGestureAction = new ComboBox
            {
                Location = new Point(leftMargin + 48, y),
                Size = new Size(160, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            _cmbGestureAction.Items.AddRange(new object[]
            {
                "Press Key", "Cast Spell (instant)", "Equip Spell (left hand)", "Equip Spell (right hand)"
            });
            _cmbGestureAction.SelectedIndex = 0;
            _cmbGestureAction.SelectedIndexChanged += (s, e) =>
            {
                bool spellMode = _cmbGestureAction.SelectedIndex > 0;
                _cmbGestureSpell.Enabled = spellMode;
                _cmbGestureKeyPick.Enabled = !spellMode;
                if (spellMode) RefreshSpellListIfStale();
            };
            container.Controls.Add(_cmbGestureAction);

            _cmbGestureSpell = new ComboBox
            {
                Location = new Point(leftMargin + 216, y),
                Size = new Size(330, 26),
                DropDownStyle = ComboBoxStyle.DropDown,
                AutoCompleteMode = AutoCompleteMode.SuggestAppend,
                AutoCompleteSource = AutoCompleteSource.ListItems,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
                Enabled = false,
            };
            _cmbGestureSpell.DropDown += (s, e) => RefreshSpellListIfStale();
            container.Controls.Add(_cmbGestureSpell);

            var btnReloadSpells = MakeGestureButton("Reload", new Point(leftMargin + 552, y), 70);
            btnReloadSpells.Click += (s, e) => LoadSpellList();
            container.Controls.Add(btnReloadSpells);

            _chkKnownSpellsOnly = new ModernCheckBox
            {
                Text = "Only spells I know",
                Location = new Point(leftMargin + 630, y + 2),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
                Checked = false,
            };
            _chkKnownSpellsOnly.CheckedChanged += (s, e) => LoadSpellList();
            container.Controls.Add(_chkKnownSpellsOnly);

            _lblSpellCount = new Label
            {
                Text = "",
                Location = new Point(leftMargin + 768, y + 4),
                Size = new Size(rightEdge - leftMargin - 768, 20),
                Font = new Font("Segoe UI", 8f, FontStyle.Italic),
                ForeColor = Color.FromArgb(120, 122, 132),
            };
            container.Controls.Add(_lblSpellCount);
            LoadSpellList();
            y += 32;

            // ── Trail look: how the in-game overlay draws this gesture ──
            var lblTrail = new Label
            {
                Text = "Trail",
                Location = new Point(leftMargin, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblTrail);

            _cmbTrailColor = new ComboBox
            {
                Location = new Point(leftMargin + 48, y),
                Size = new Size(110, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            foreach (var c in TrailColors)
                _cmbTrailColor.Items.Add(char.ToUpper(c[0]) + c.Substring(1));
            _cmbTrailColor.SelectedIndex = 0;
            container.Controls.Add(_cmbTrailColor);

            var lblRune = new Label
            {
                Text = "Rune",
                Location = new Point(leftMargin + 168, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblRune);

            _cmbRuneColor = new ComboBox
            {
                Location = new Point(leftMargin + 206, y),
                Size = new Size(110, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            _cmbRuneColor.Items.Add("Same as trail");
            foreach (var c in TrailColors)
                _cmbRuneColor.Items.Add(char.ToUpper(c[0]) + c.Substring(1));
            _cmbRuneColor.SelectedIndex = 0;
            container.Controls.Add(_cmbRuneColor);

            CheckBox MakeTrailCheck(string text, int x, bool check)
            {
                var chk = new ModernCheckBox
                {
                    Text = text,
                    Location = new Point(x, y + 2),
                    AutoSize = true,
                    Font = new Font("Segoe UI", 8.5f),
                    ForeColor = Color.FromArgb(190, 192, 200),
                    Checked = check,
                };
                container.Controls.Add(chk);
                return chk;
            }
            var lblWidth = new Label
            {
                Text = "Width",
                Location = new Point(leftMargin + 330, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblWidth);

            _cmbTrailWidth = new ComboBox
            {
                Location = new Point(leftMargin + 376, y),
                Size = new Size(90, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            foreach (var (label, _) in TrailWidths)
                _cmbTrailWidth.Items.Add(label);
            _cmbTrailWidth.SelectedIndex = 1; // Normal
            container.Controls.Add(_cmbTrailWidth);

            // Style flags drop to their own row so the Trail line isn't a wall of controls.
            y += 30;
            var lblStyle = new Label
            {
                Text = "Style",
                Location = new Point(leftMargin, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 9f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblStyle);

            int styleX = leftMargin + 48;
            _chkTrailTransparent = MakeTrailCheck("Transparent", styleX, false);
            styleX = _chkTrailTransparent.Right + 8;
            _chkTrailSmoky = MakeTrailCheck("Smoky", styleX, false);
            styleX = _chkTrailSmoky.Right + 8;
            _chkTrailWispy = MakeTrailCheck("Wispy", styleX, false);
            styleX = _chkTrailWispy.Right + 8;
            _chkTrailGlowing = MakeTrailCheck("Glowing", styleX, true);
            styleX = _chkTrailGlowing.Right + 12;

            var lblTrailHint = new Label
            {
                Text = "Trail = the ribbon while you draw; Rune = the flash when the shape completes. Smoky = billowing puffs, wispy = thin tendrils.",
                Location = new Point(styleX, y + 4),
                Size = new Size(Math.Max(0, rightEdge - styleX), 20),
                Font = new Font("Segoe UI", 8f, FontStyle.Italic),
                ForeColor = Color.FromArgb(120, 122, 132),
            };
            container.Controls.Add(lblTrailHint);
            y += 30;

            // ── Cast sounds (global, saved to opencomposite.ini on Save) ──
            _chkGestureSounds = new ModernCheckBox
            {
                Text = "Cast sounds",
                Location = new Point(leftMargin, y + 2),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
                Checked = true,
            };
            container.Controls.Add(_chkGestureSounds);

            var lblFinish = new Label
            {
                Text = "Finish sound",
                Location = new Point(leftMargin + 110, y + 4),
                AutoSize = true,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(190, 192, 200),
            };
            container.Controls.Add(lblFinish);

            _cmbFinishSound = new ComboBox
            {
                Location = new Point(leftMargin + 192, y),
                Size = new Size(100, 26),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 8.5f),
                BackColor = Color.FromArgb(24, 25, 31),
                ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            _cmbFinishSound.Items.Add("Impact");
            _cmbFinishSound.Items.Add("Dark");
            _cmbFinishSound.SelectedIndex = 0;
            container.Controls.Add(_cmbFinishSound);

            var lblSoundHint = new Label
            {
                Text = "Tracing loops a hum while you draw; the finish sound plays on a successful cast. Arm a cast by raising your hand above your head while holding the button.",
                Location = new Point(leftMargin + 304, y + 4),
                Size = new Size(rightEdge - leftMargin - 304, 20),
                Font = new Font("Segoe UI", 8f, FontStyle.Italic),
                ForeColor = Color.FromArgb(120, 122, 132),
            };
            container.Controls.Add(lblSoundHint);
            y += 34;

            container.Size = new Size(container.Width, y + 6);

            RefreshGestureLibrary();
        }

        private Button MakeGestureButton(string text, Point loc, int width)
        {
            var b = new ModernPillButton
            {
                Text = text,
                Location = loc,
                Size = new Size(width, 26),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = Color.FromArgb(210, 212, 220),
                BackColor = Color.FromArgb(45, 47, 56),
                Cursor = Cursors.Hand,
            };
            b.FlatAppearance.BorderColor = Color.FromArgb(70, 75, 95);
            b.FlatAppearance.BorderSize = 1;
            return b;
        }

        private void SetGestureStatus(string msg, bool ok)
        {
            _lblGestureStatus.Text = msg;
            _lblGestureStatus.ForeColor = ok ? Color.FromArgb(120, 200, 120) : Color.FromArgb(255, 120, 100);
        }

        // ── Save ──
        private void SaveCurrentGesture()
        {
            string name = _txtGestureName.Text.Trim();
            if (name.Length == 0)
            {
                SetGestureStatus("Name the gesture first.", false);
                return;
            }
            var left = _gestureLeft.GetNormalizedStrokes();
            var right = _gestureRight.GetNormalizedStrokes();
            if (left.Count == 0 && right.Count == 0)
            {
                SetGestureStatus("Draw something first.", false);
                return;
            }

            string keyName = _cmbGestureKeyPick.SelectedIndex > 0
                ? _cmbGestureKeyPick.SelectedItem?.ToString() ?? ""
                : "";
            int scancode = keyName.Length > 0 && KeyScancodes.TryGetValue(keyName, out int sc) ? sc : 0;
            Keys vk = Keys.None;
            if (keyName.Length > 0)
                Enum.TryParse(keyName, true, out vk); // best effort; scancode is the real contract

            string actionId = GestureActionIds[Math.Max(0, _cmbGestureAction.SelectedIndex)];
            SpellEntry? spell = null;
            if (actionId != "key")
            {
                int si = _cmbGestureSpell.SelectedIndex;
                if (si < 0)
                    si = _cmbGestureSpell.Items.IndexOf(_cmbGestureSpell.Text);
                if (si >= 0 && si < _spellList.Count)
                    spell = _spellList[si];
                if (spell == null)
                {
                    SetGestureStatus("Pick a spell from the list first (launch the game once if the list is empty).", false);
                    return;
                }
            }

            var style = new List<string>();
            if (_chkTrailTransparent.Checked) style.Add("transparent");
            if (_chkTrailSmoky.Checked) style.Add("smoky");
            if (_chkTrailWispy.Checked) style.Add("wispy");
            if (_chkTrailGlowing.Checked) style.Add("glowing");

            var data = new GestureData
            {
                Name = name,
                Vk = (int)vk,
                Scancode = scancode,
                KeyDisplay = keyName,
                HoldButton = SelectedHoldId(),
                Action = actionId,
                SpellPlugin = spell?.plugin ?? "",
                SpellFormId = spell?.formId ?? "",
                SpellName = spell?.name ?? "",
                Concentration = actionId == "spell" && spell?.casting == "conc",
                TrailColor = TrailColors[Math.Max(0, _cmbTrailColor.SelectedIndex)],
                RuneColor = _cmbRuneColor.SelectedIndex <= 0 ? "" : TrailColors[_cmbRuneColor.SelectedIndex - 1],
                TrailWidth = TrailWidths[Math.Max(0, _cmbTrailWidth.SelectedIndex)].mult,
                TrailStyle = style,
                Left = left,
                Right = right,
            };

            try
            {
                Directory.CreateDirectory(GesturesDir);
                string safe = SanitizeGestureFileName(name);
                string jsonPath = Path.Combine(GesturesDir, safe + ".json");
                string pngPath = Path.Combine(GesturesDir, safe + ".png");

                File.WriteAllText(jsonPath, JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true }));

                // Square halves match the square draw boxes, so snapshots keep proportions
                using (var thumb = RenderGestureThumbnail(data, 304, 150))
                    thumb.Save(pngPath, ImageFormat.Png);

                // Mirror to the game folder: the in-game recognizer reads
                // <game>\Gestures, same dual-save pattern as opencomposite.ini
                MirrorGestureToGameDir(safe, jsonPath, pngPath, deleteInstead: false);

                string boundTo = actionId == "key"
                    ? (keyName.Length == 0 ? " (no key bound yet)" : $" to {keyName}")
                    : $" to spell {spell!.name}";
                SetGestureStatus($"Saved \"{name}\"{boundTo}", true);
                RefreshGestureLibrary();
            }
            catch (Exception ex)
            {
                SetGestureStatus($"Save failed: {ex.Message}", false);
            }
        }

        // Mirror a gesture into the place the DLL reads it from.
        // MO2-managed install (mod root\ exists): write ONLY to <mod>\root\Gestures.
        // Root Builder serves it to the game; writing to <game>\Gestures directly
        // creates a physical copy that Root Builder sweeps into MO2's overwrite
        // folder on exit, where it permanently shadows every future save here.
        // Non-MO2 install (no root\ beside the exe): write to <game>\Gestures.
        // Deletes always hit both, to clean up any legacy game-dir debris.
        // The local <exe>\Gestures stays the editor's library.
        // MO2's Root Builder sweeps unmanaged game-dir files into
        // <instance>\overwrite\Root on exit, and overwrite outranks every mod,
        // so a swept Gestures copy silently shadows the mod's forever.
        // That folder can only ever contain swept OCU debris, so it is always
        // safe to delete outright. Runs at tab build and before every mirror,
        // and never touches anything else in overwrite.
        private void CleanOverwriteGestureShadows()
        {
            try
            {
                // Under MO2 the exe lives at <instance>\mods\<modname>\
                string modDir = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\', '/');
                string modsDir = Path.GetDirectoryName(modDir) ?? "";
                if (!string.Equals(Path.GetFileName(modsDir), "mods", StringComparison.OrdinalIgnoreCase))
                    return; // not an MO2 layout
                string instanceDir = Path.GetDirectoryName(modsDir) ?? "";
                string shadow = Path.Combine(instanceDir, "overwrite", "Root", "Gestures");
                if (Directory.Exists(shadow))
                    Directory.Delete(shadow, true);
            }
            catch { /* best-effort; a locked file just means we retry next save */ }
        }

        private void MirrorGestureToGameDir(string safeName, string jsonPath, string pngPath, bool deleteInstead)
        {
            CleanOverwriteGestureShadows();
            var targets = new List<string>();
            string rootDir = Path.Combine(AppContext.BaseDirectory, "root");
            bool haveModRoot = Directory.Exists(rootDir);
            if (haveModRoot)
                targets.Add(Path.Combine(rootDir, "Gestures"));
            if ((!haveModRoot || deleteInstead) && !string.IsNullOrEmpty(_gameDir) && Directory.Exists(_gameDir))
                targets.Add(Path.Combine(_gameDir, "Gestures"));

            foreach (string gdir in targets)
            {
                try
                {
                    string gJson = Path.Combine(gdir, safeName + ".json");
                    string gPng = Path.Combine(gdir, safeName + ".png");
                    if (deleteInstead)
                    {
                        if (File.Exists(gJson)) File.Delete(gJson);
                        if (File.Exists(gPng)) File.Delete(gPng);
                        continue;
                    }
                    Directory.CreateDirectory(gdir);
                    File.Copy(jsonPath, gJson, true);
                    File.Copy(pngPath, gPng, true);
                }
                catch { /* best-effort; the local library save already succeeded */ }
            }
        }

        private static string SanitizeGestureFileName(string name)
        {
            var sb = new System.Text.StringBuilder();
            foreach (char c in name)
            {
                if (char.IsLetterOrDigit(c) || c == '-' || c == '_') sb.Append(c);
                else if (c == ' ') sb.Append('_');
            }
            return sb.Length > 0 ? sb.ToString() : "gesture";
        }

        // ── Thumbnail: both hands side by side, same visual style as the canvases ──
        private Bitmap RenderGestureThumbnail(GestureData data, int width, int height)
        {
            var bmp = new Bitmap(width, height);
            using var g = Graphics.FromImage(bmp);
            g.SmoothingMode = SmoothingMode.AntiAlias;

            int half = (width - 4) / 2;
            var leftRect = new Rectangle(0, 0, half, height);
            var rightRect = new Rectangle(half + 4, 0, half, height);

            GestureCanvas.RenderScene(g, leftRect, DenormalizeStrokes(data.Left, leftRect),
                Color.FromArgb(0, 195, 255), "L", null);
            GestureCanvas.RenderScene(g, rightRect, DenormalizeStrokes(data.Right, rightRect),
                Color.FromArgb(255, 150, 50), "R", null);
            return bmp;
        }

        private static List<List<PointF>> DenormalizeStrokes(List<List<float[]>> strokes, Rectangle rect)
        {
            var result = new List<List<PointF>>();
            foreach (var stroke in strokes)
            {
                var pts = new List<PointF>();
                foreach (var p in stroke)
                {
                    if (p.Length < 2) continue;
                    pts.Add(new PointF(rect.X + p[0] * rect.Width, rect.Y + p[1] * rect.Height));
                }
                if (pts.Count >= 2) result.Add(pts);
            }
            return result;
        }

        // ── Library cards ──
        private void RefreshGestureLibrary()
        {
            _gestureLibraryPrevious.Enabled = false;
            _gestureLibraryNext.Enabled = false;
            _gestureLibraryPageLabel.Text = "1 / 1";
            foreach (Control c in _pnlGestureLibrary.Controls.Cast<Control>().ToList())
            {
                if (c.Tag is Image img) img.Dispose();
                c.Dispose();
            }
            _pnlGestureLibrary.Controls.Clear();

            if (!Directory.Exists(GesturesDir))
            {
                AddLibraryHint();
                return;
            }

            var files = Directory.GetFiles(GesturesDir, "*.json").OrderBy(f => f).ToArray();
            if (files.Length == 0)
            {
                AddLibraryHint();
                return;
            }

            int pageCount = Math.Max(1, (files.Length + 1) / 2);
            _gestureLibraryPage = Math.Clamp(_gestureLibraryPage, 0, pageCount - 1);
            _gestureLibraryPrevious.Enabled = _gestureLibraryPage > 0;
            _gestureLibraryNext.Enabled = _gestureLibraryPage + 1 < pageCount;
            _gestureLibraryPageLabel.Text = $"{_gestureLibraryPage + 1} / {pageCount}";
            foreach (string jsonPath in files.Skip(_gestureLibraryPage * 2).Take(2))
            {
                GestureData? data = null;
                try { data = JsonSerializer.Deserialize<GestureData>(File.ReadAllText(jsonPath)); }
                catch { /* skip corrupt files */ }
                if (data == null) continue;

                var card = MakeGestureCard(jsonPath, data);
                DpiLayout.AddLogicalControl(_pnlGestureLibrary, card);
            }
        }

        private void AddLibraryHint()
        {
            _pnlGestureLibrary.Controls.Add(new Label
            {
                Text = "No saved gestures yet. Draw one above and hit Save Gesture.",
                AutoSize = true,
                Margin = new Padding(10),
                Font = new Font("Segoe UI", 9f, FontStyle.Italic),
                ForeColor = Color.FromArgb(120, 122, 132),
            });
        }

        private Panel MakeGestureCard(string jsonPath, GestureData data)
        {
            string pngPath = Path.ChangeExtension(jsonPath, ".png");

            var card = new Panel
            {
                Size = new Size(186, 158),
                Margin = new Padding(6),
                BackColor = Color.FromArgb(33, 35, 43),
                Cursor = Cursors.Hand,
            };

            Image? thumb = null;
            if (File.Exists(pngPath))
            {
                // Load via memory so the PNG file is never locked (delete must work)
                try { using var ms = new MemoryStream(File.ReadAllBytes(pngPath)); thumb = Image.FromStream(ms); }
                catch { thumb = null; }
            }

            var pic = new PictureBox
            {
                Location = new Point(5, 5),
                Size = new Size(176, 88),
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(18, 19, 24),
                Image = thumb,
                Cursor = Cursors.Hand,
            };
            card.Tag = thumb; // disposed on library refresh
            card.Controls.Add(pic);

            var lblName = new Label
            {
                Text = data.Name,
                Location = new Point(6, 97),
                Size = new Size(174, 18),
                Font = new Font("Segoe UI", 9f, FontStyle.Bold),
                ForeColor = Color.White,
                AutoEllipsis = true,
            };
            card.Controls.Add(lblName);

            bool twoHand = data.Left.Count > 0 && data.Right.Count > 0;
            string holdId = MigrateLegacyHoldId(data.HoldButton, data.Left.Count > 0 && data.Right.Count == 0);
            string holdLabel = HoldLabelFor(holdId);
            string act = data.Action ?? "key";
            string bindingText = act == "key"
                ? (data.KeyDisplay.Length > 0 ? $"Key: {data.KeyDisplay}" : "Key: unbound")
                : $"Spell: {(string.IsNullOrEmpty(data.SpellName) ? data.SpellFormId : data.SpellName)}"
                  + (act == "equip_left" ? " (L)" : act == "equip_right" ? " (R)" : "");
            var lblInfo = new Label
            {
                Text = bindingText
                     + (twoHand ? "  |  two-hand" : data.Left.Count > 0 ? "  |  left" : "  |  right")
                     + (data.HoldButton.Length > 0 ? $"  |  hold {holdLabel}" : ""),
                Location = new Point(6, 116),
                Size = new Size(174, 16),
                Font = new Font("Segoe UI", 8f),
                ForeColor = data.KeyDisplay.Length > 0 ? Color.FromArgb(140, 200, 255) : Color.FromArgb(255, 170, 90),
            };
            card.Controls.Add(lblInfo);

            var btnDelete = new ModernPillButton
            {
                Text = "Delete",
                Location = new Point(112, 134),
                Size = new Size(68, 20),
                FlatStyle = FlatStyle.Flat,
                Font = new Font("Segoe UI", 7.5f),
                ForeColor = Color.FromArgb(220, 140, 130),
                BackColor = Color.FromArgb(45, 35, 38),
                Cursor = Cursors.Hand,
            };
            btnDelete.FlatAppearance.BorderSize = 0;
            btnDelete.Click += (s, e) =>
            {
                if (MessageBox.Show($"Delete gesture \"{data.Name}\"?", "Delete Gesture",
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                    return;
                try
                {
                    File.Delete(jsonPath);
                    if (File.Exists(pngPath)) File.Delete(pngPath);
                    MirrorGestureToGameDir(Path.GetFileNameWithoutExtension(jsonPath), jsonPath, pngPath, deleteInstead: true);
                }
                catch (Exception ex) { SetGestureStatus($"Delete failed: {ex.Message}", false); return; }
                SetGestureStatus($"Deleted \"{data.Name}\".", true);
                RefreshGestureLibrary();
            };
            card.Controls.Add(btnDelete);

            var lblLoad = new Label
            {
                Text = "click to load",
                Location = new Point(6, 137),
                Size = new Size(100, 15),
                Font = new Font("Segoe UI", 7.5f, FontStyle.Italic),
                ForeColor = Color.FromArgb(110, 112, 122),
            };
            card.Controls.Add(lblLoad);

            void Load(object? s, EventArgs e) => LoadGestureIntoEditor(data);
            card.Click += Load;
            pic.Click += Load;
            lblName.Click += Load;
            lblInfo.Click += Load;
            lblLoad.Click += Load;

            return card;
        }

        private void LoadGestureIntoEditor(GestureData data)
        {
            _txtGestureName.Text = data.Name;

            int keyIdx = 0;
            if (data.KeyDisplay.Length > 0)
            {
                keyIdx = _cmbGestureKeyPick.Items.IndexOf(data.KeyDisplay);
                if (keyIdx < 0) keyIdx = 0;
            }
            _cmbGestureKeyPick.SelectedIndex = keyIdx;

            int actionIdx = Array.IndexOf(GestureActionIds, data.Action ?? "key");
            _cmbGestureAction.SelectedIndex = actionIdx >= 0 ? actionIdx : 0;
            if (actionIdx > 0)
            {
                int spellIdx = _spellList.FindIndex(s =>
                    string.Equals(s.plugin, data.SpellPlugin, StringComparison.OrdinalIgnoreCase)
                    && string.Equals(s.formId, data.SpellFormId, StringComparison.OrdinalIgnoreCase));
                if (spellIdx < 0 && _chkKnownSpellsOnly.Checked)
                {
                    // Saved spell may be outside the known filter: widen and retry
                    _chkKnownSpellsOnly.Checked = false; // triggers LoadSpellList
                    spellIdx = _spellList.FindIndex(s =>
                        string.Equals(s.plugin, data.SpellPlugin, StringComparison.OrdinalIgnoreCase)
                        && string.Equals(s.formId, data.SpellFormId, StringComparison.OrdinalIgnoreCase));
                }
                if (spellIdx >= 0)
                    _cmbGestureSpell.SelectedIndex = spellIdx;
                else
                    _cmbGestureSpell.Text = data.SpellName; // list refresh may re-find it
            }

            string holdId = MigrateLegacyHoldId(data.HoldButton, data.Left.Count > 0 && data.Right.Count == 0);
            int holdIdx = Array.FindIndex(CurrentHoldOptions, o => o.id == holdId);
            _cmbGestureHold.SelectedIndex = holdIdx >= 0 ? holdIdx : 0;

            int colorIdx = Array.IndexOf(TrailColors, (data.TrailColor ?? "cyan").ToLowerInvariant());
            _cmbTrailColor.SelectedIndex = colorIdx >= 0 ? colorIdx : 0;
            int runeIdx = Array.IndexOf(TrailColors, (data.RuneColor ?? "").ToLowerInvariant());
            _cmbRuneColor.SelectedIndex = runeIdx >= 0 ? runeIdx + 1 : 0; // slot 0 = "Same as trail"
            int widthIdx = 1; // Normal
            for (int i = 0; i < TrailWidths.Length; i++)
                if (Math.Abs(TrailWidths[i].mult - data.TrailWidth) < 0.05)
                    widthIdx = i;
            _cmbTrailWidth.SelectedIndex = widthIdx;
            var style = data.TrailStyle ?? new List<string>();
            _chkTrailTransparent.Checked = style.Contains("transparent");
            _chkTrailSmoky.Checked = style.Contains("smoky");
            _chkTrailWispy.Checked = style.Contains("wispy");
            _chkTrailGlowing.Checked = style.Count == 0 || style.Contains("glowing");

            _gestureLeft.SetNormalizedStrokes(data.Left);
            _gestureRight.SetNormalizedStrokes(data.Right);
            SetGestureStatus($"Loaded \"{data.Name}\". Redraw or rename, then Save Gesture.", true);
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // GestureCanvas: double-buffered draw surface. Strokes render as glowing
    // smoothed curves with direction arrows, over a dark dotted backdrop.
    // ═══════════════════════════════════════════════════════════════════════
    internal sealed class GestureCanvas : Panel
    {
        private readonly List<List<PointF>> _strokes = new();
        private List<PointF>? _activeStroke;
        private readonly string _label;
        private readonly Color _accent;

        public GestureCanvas(string label, Color accent)
        {
            _label = label;
            _accent = accent;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint
                | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            BackColor = Color.FromArgb(18, 19, 24);
            Cursor = Cursors.Cross;

            MouseDown += (s, e) =>
            {
                if (e.Button != MouseButtons.Left) return;
                // One continuous stroke per hand: in-game, releasing the hold
                // button ends the cast, so a gesture must be drawable in one
                // go. Starting a new stroke replaces the previous attempt.
                _strokes.Clear();
                _activeStroke = new List<PointF> { e.Location };
                _strokes.Add(_activeStroke);
                Invalidate();
            };
            MouseMove += (s, e) =>
            {
                if (_activeStroke == null) return;
                var last = _activeStroke[^1];
                float dx = e.X - last.X, dy = e.Y - last.Y;
                if (dx * dx + dy * dy < 9f) return; // 3px minimum spacing
                _activeStroke.Add(e.Location);
                Invalidate();
            };
            MouseUp += (s, e) =>
            {
                if (_activeStroke != null && _activeStroke.Count < 2)
                {
                    _strokes.Remove(_activeStroke); // ignore stray clicks
                }
                else if (_activeStroke != null && AutoSmooth)
                {
                    int idx = _strokes.IndexOf(_activeStroke);
                    if (idx >= 0) _strokes[idx] = BeautifyStroke(_activeStroke);
                }
                _activeStroke = null;
                Invalidate();
            };
        }

        [System.ComponentModel.Browsable(false)]
        [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
        public bool AutoSmooth { get; set; }

        public void ClearStrokes()
        {
            _strokes.Clear();
            _activeStroke = null;
            Invalidate();
        }

        public void SmoothStrokes()
        {
            for (int i = 0; i < _strokes.Count; i++)
                _strokes[i] = BeautifyStroke(_strokes[i]);
            Invalidate();
        }

        // ═══════════════ Shape beautifier ═══════════════
        // Order of attempts: straight line, axis-aligned rectangle, angular
        // polyline (W's, zigzags, triangles), circle, then generic de-jitter
        // (spirals, waves, anything curvy).
        private static List<PointF> BeautifyStroke(List<PointF> pts)
        {
            if (pts.Count < 3) return pts;

            float minX = pts.Min(p => p.X), maxX = pts.Max(p => p.X);
            float minY = pts.Min(p => p.Y), maxY = pts.Max(p => p.Y);
            float diag = (float)Math.Sqrt((maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY));
            if (diag < 8f) return pts;

            var corners = RdpSimplify(pts, Math.Max(3f, diag * 0.045f));

            // Straight line: RDP collapsed everything to the endpoints
            if (corners.Count == 2)
                return SnapLine(corners[0], corners[1]);

            bool closed = Dist(pts[0], pts[^1]) < diag * 0.15f;

            // Axis-aligned rectangle / square
            if (closed && corners.Count >= 4 && corners.Count <= 6)
            {
                var loop = new List<PointF>(corners);
                if (Dist(loop[0], loop[^1]) < diag * 0.15f) loop.RemoveAt(loop.Count - 1);
                if (loop.Count == 4 && IsAxisAlignedQuad(loop))
                {
                    float rMinX = loop.Min(p => p.X), rMaxX = loop.Max(p => p.X);
                    float rMinY = loop.Min(p => p.Y), rMaxY = loop.Max(p => p.Y);
                    return RectangleStroke(rMinX, rMinY, rMaxX, rMaxY, loop);
                }
            }

            // Angular polyline vs curve: count sharp turns at interior corners
            if (corners.Count >= 3 && corners.Count <= 9)
            {
                int interior = corners.Count - 2, sharp = 0;
                for (int i = 1; i < corners.Count - 1; i++)
                    if (TurnAngleDeg(corners[i - 1], corners[i], corners[i + 1]) > 40f)
                        sharp++;
                if (interior > 0 && sharp * 2 >= interior)
                {
                    var snapped = new List<PointF>(corners);
                    if (closed) snapped[^1] = snapped[0];
                    return snapped;
                }
            }

            // Circle
            if (closed && FitCircle(pts, out PointF center, out float radius)
                && CircleFitIsGood(pts, center, radius))
                return CircleStroke(center, radius, pts);

            // Curvy: spirals, waves. Resample evenly, then relax the jitter out.
            return SmoothCurve(pts);
        }

        private static float Dist(PointF a, PointF b)
        {
            float dx = a.X - b.X, dy = a.Y - b.Y;
            return (float)Math.Sqrt(dx * dx + dy * dy);
        }

        private static float TurnAngleDeg(PointF a, PointF b, PointF c)
        {
            float v1x = b.X - a.X, v1y = b.Y - a.Y, v2x = c.X - b.X, v2y = c.Y - b.Y;
            float l1 = (float)Math.Sqrt(v1x * v1x + v1y * v1y), l2 = (float)Math.Sqrt(v2x * v2x + v2y * v2y);
            if (l1 < 0.01f || l2 < 0.01f) return 0f;
            float dot = (v1x * v2x + v1y * v2y) / (l1 * l2);
            dot = Math.Clamp(dot, -1f, 1f);
            return (float)(Math.Acos(dot) * 180.0 / Math.PI);
        }

        // Snap a 2-point line to the nearest 45 degree angle when close (10 deg)
        private static List<PointF> SnapLine(PointF a, PointF b)
        {
            float dx = b.X - a.X, dy = b.Y - a.Y;
            float len = (float)Math.Sqrt(dx * dx + dy * dy);
            if (len < 1f) return new List<PointF> { a, b };
            double ang = Math.Atan2(dy, dx) * 180.0 / Math.PI;
            double snapped = Math.Round(ang / 45.0) * 45.0;
            if (Math.Abs(ang - snapped) <= 10.0)
            {
                double rad = snapped * Math.PI / 180.0;
                var mid = new PointF((a.X + b.X) / 2f, (a.Y + b.Y) / 2f);
                float hx = (float)(Math.Cos(rad) * len / 2), hy = (float)(Math.Sin(rad) * len / 2);
                a = new PointF(mid.X - hx, mid.Y - hy);
                b = new PointF(mid.X + hx, mid.Y + hy);
            }
            return new List<PointF> { a, b };
        }

        private static bool IsAxisAlignedQuad(List<PointF> loop)
        {
            for (int i = 0; i < 4; i++)
            {
                var p1 = loop[i];
                var p2 = loop[(i + 1) % 4];
                double ang = Math.Abs(Math.Atan2(p2.Y - p1.Y, p2.X - p1.X) * 180.0 / Math.PI);
                double dev = Math.Min(Math.Min(Math.Abs(ang - 0), Math.Abs(ang - 90)), Math.Abs(ang - 180));
                if (dev > 14.0) return false;
            }
            return true;
        }

        // Perfect rectangle starting at the corner nearest the user's first point,
        // preserving their draw direction (winding).
        private static List<PointF> RectangleStroke(float minX, float minY, float maxX, float maxY, List<PointF> loop)
        {
            var cw = new[]
            {
                new PointF(minX, minY), new PointF(maxX, minY),
                new PointF(maxX, maxY), new PointF(minX, maxY),
            };
            // Winding of the drawn loop via the shoelace sum
            float area = 0;
            for (int i = 0; i < loop.Count; i++)
            {
                var p1 = loop[i];
                var p2 = loop[(i + 1) % loop.Count];
                area += p1.X * p2.Y - p2.X * p1.Y;
            }
            bool clockwise = area < 0; // screen coords, y down

            int start = 0;
            float best = float.MaxValue;
            for (int i = 0; i < 4; i++)
            {
                float d = Dist(cw[i], loop[0]);
                if (d < best) { best = d; start = i; }
            }

            var result = new List<PointF>();
            for (int i = 0; i <= 4; i++)
            {
                int idx = clockwise ? (start + i) % 4 : ((start - i) % 4 + 4) % 4;
                result.Add(cw[idx]);
            }
            return result;
        }

        // Kasa algebraic circle fit
        private static bool FitCircle(List<PointF> pts, out PointF center, out float radius)
        {
            center = default;
            radius = 0;
            int n = pts.Count;
            if (n < 6) return false;
            double mx = pts.Average(p => (double)p.X), my = pts.Average(p => (double)p.Y);
            double suu = 0, svv = 0, suv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
            foreach (var p in pts)
            {
                double u = p.X - mx, v = p.Y - my;
                suu += u * u; svv += v * v; suv += u * v;
                suuu += u * u * u; svvv += v * v * v;
                suvv += u * v * v; svuu += v * u * u;
            }
            double det = suu * svv - suv * suv;
            if (Math.Abs(det) < 1e-6) return false;
            double uc = (0.5 * ((suuu + suvv) * svv - (svvv + svuu) * suv)) / det;
            double vc = (0.5 * ((svvv + svuu) * suu - (suuu + suvv) * suv)) / det;
            double r2 = uc * uc + vc * vc + (suu + svv) / n;
            if (r2 <= 0) return false;
            center = new PointF((float)(uc + mx), (float)(vc + my));
            radius = (float)Math.Sqrt(r2);
            return radius > 6f;
        }

        private static bool CircleFitIsGood(List<PointF> pts, PointF c, float r)
        {
            double meanDev = pts.Average(p => Math.Abs(Dist(p, c) - r));
            if (meanDev > r * 0.13) return false;
            // Require most of a full turn so arcs and U shapes stay as curves
            double span = 0;
            double prev = Math.Atan2(pts[0].Y - c.Y, pts[0].X - c.X);
            foreach (var p in pts.Skip(1))
            {
                double a = Math.Atan2(p.Y - c.Y, p.X - c.X);
                double d = a - prev;
                while (d > Math.PI) d -= 2 * Math.PI;
                while (d < -Math.PI) d += 2 * Math.PI;
                span += d;
                prev = a;
            }
            return Math.Abs(span) > 5.0;
        }

        private static List<PointF> CircleStroke(PointF c, float r, List<PointF> original)
        {
            double startAng = Math.Atan2(original[0].Y - c.Y, original[0].X - c.X);
            // Preserve the direction the user drew in
            double a1 = Math.Atan2(original[Math.Min(4, original.Count - 1)].Y - c.Y,
                                   original[Math.Min(4, original.Count - 1)].X - c.X);
            double d0 = a1 - startAng;
            while (d0 > Math.PI) d0 -= 2 * Math.PI;
            while (d0 < -Math.PI) d0 += 2 * Math.PI;
            double dir = d0 >= 0 ? 1.0 : -1.0;

            const int segments = 48;
            var result = new List<PointF>(segments + 1);
            for (int i = 0; i <= segments; i++)
            {
                double a = startAng + dir * 2.0 * Math.PI * i / segments;
                result.Add(new PointF(c.X + (float)(Math.Cos(a) * r), c.Y + (float)(Math.Sin(a) * r)));
            }
            return result;
        }

        // Even resample along arc length, then a few relaxation passes.
        // Endpoints stay pinned so the gesture start/end never drifts.
        private static List<PointF> SmoothCurve(List<PointF> pts)
        {
            const float spacing = 6f;
            var resampled = new List<PointF> { pts[0] };
            float carry = 0f;
            for (int i = 1; i < pts.Count; i++)
            {
                var a = resampled[^1];
                var b = pts[i];
                float seg = Dist(a, b) + carry;
                if (seg < spacing) { carry = seg; continue; }
                carry = 0f;
                resampled.Add(b);
            }
            if (resampled.Count < 3) return pts;
            if (Dist(resampled[^1], pts[^1]) > 0.5f) resampled.Add(pts[^1]);

            for (int pass = 0; pass < 3; pass++)
            {
                for (int i = 1; i < resampled.Count - 1; i++)
                {
                    resampled[i] = new PointF(
                        (resampled[i - 1].X + resampled[i].X * 2f + resampled[i + 1].X) / 4f,
                        (resampled[i - 1].Y + resampled[i].Y * 2f + resampled[i + 1].Y) / 4f);
                }
            }
            return resampled;
        }

        // Ramer-Douglas-Peucker simplification
        private static List<PointF> RdpSimplify(List<PointF> pts, float epsilon)
        {
            var keep = new bool[pts.Count];
            keep[0] = keep[^1] = true;
            RdpRecurse(pts, 0, pts.Count - 1, epsilon, keep);
            var result = new List<PointF>();
            for (int i = 0; i < pts.Count; i++)
                if (keep[i]) result.Add(pts[i]);
            return result;
        }

        private static void RdpRecurse(List<PointF> pts, int a, int b, float eps, bool[] keep)
        {
            if (b <= a + 1) return;
            float maxD = -1f;
            int idx = -1;
            for (int i = a + 1; i < b; i++)
            {
                float d = PerpendicularDistance(pts[i], pts[a], pts[b]);
                if (d > maxD) { maxD = d; idx = i; }
            }
            if (maxD > eps && idx > 0)
            {
                keep[idx] = true;
                RdpRecurse(pts, a, idx, eps, keep);
                RdpRecurse(pts, idx, b, eps, keep);
            }
        }

        private static float PerpendicularDistance(PointF p, PointF a, PointF b)
        {
            float dx = b.X - a.X, dy = b.Y - a.Y;
            float len2 = dx * dx + dy * dy;
            if (len2 < 0.0001f) return Dist(p, a);
            float t = ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2;
            t = Math.Clamp(t, 0f, 1f);
            return Dist(p, new PointF(a.X + t * dx, a.Y + t * dy));
        }

        public List<List<float[]>> GetNormalizedStrokes()
        {
            var result = new List<List<float[]>>();
            float w = Math.Max(1, ClientSize.Width), h = Math.Max(1, ClientSize.Height);
            foreach (var stroke in _strokes)
            {
                if (stroke.Count < 2) continue;
                result.Add(stroke.Select(p => new[] { p.X / w, p.Y / h }).ToList());
            }
            return result;
        }

        public void SetNormalizedStrokes(List<List<float[]>> strokes)
        {
            _strokes.Clear();
            _activeStroke = null;
            float w = Math.Max(1, ClientSize.Width), h = Math.Max(1, ClientSize.Height);
            foreach (var stroke in strokes)
            {
                var pts = stroke.Where(p => p.Length >= 2)
                    .Select(p => new PointF(p[0] * w, p[1] * h)).ToList();
                if (pts.Count >= 2) _strokes.Add(pts);
            }
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            RenderScene(e.Graphics, ClientRectangle, _strokes, _accent, _label,
                _strokes.Count == 0 ? "draw here with the mouse" : null);
        }

        // Shared renderer: used by the live canvas and by saved-gesture thumbnails.
        internal static void RenderScene(Graphics g, Rectangle rect, List<List<PointF>> strokes,
            Color accent, string label, string? emptyHint)
        {
            // Background: vertical gradient + faint dot grid
            using (var bg = new LinearGradientBrush(rect,
                Color.FromArgb(27, 29, 38), Color.FromArgb(15, 16, 20), LinearGradientMode.Vertical))
                g.FillRectangle(bg, rect);

            using (var dot = new SolidBrush(Color.FromArgb(26, 255, 255, 255)))
            {
                for (int gy = rect.Y + 12; gy < rect.Bottom; gy += 24)
                    for (int gx = rect.X + 12; gx < rect.Right; gx += 24)
                        g.FillRectangle(dot, gx, gy, 1.4f, 1.4f);
            }

            // Border with a whisper of the accent color
            using (var border = new Pen(Color.FromArgb(90, accent), 1f))
                g.DrawRectangle(border, rect.X, rect.Y, rect.Width - 1, rect.Height - 1);

            // Strokes: glow pass, mid pass, bright core, then direction arrows
            foreach (var stroke in strokes)
            {
                if (stroke.Count < 2) continue;
                var pts = stroke.ToArray();

                using (var glow = new Pen(Color.FromArgb(36, accent), 11f)
                    { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                    g.DrawCurve(glow, pts, 0.35f);
                using (var mid = new Pen(Color.FromArgb(85, accent), 5.5f)
                    { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                    g.DrawCurve(mid, pts, 0.35f);
                using (var core = new Pen(accent, 2.2f)
                    { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                    g.DrawCurve(core, pts, 0.35f);

                // Start marker: bright dot with white center
                using (var startGlow = new SolidBrush(Color.FromArgb(120, accent)))
                    g.FillEllipse(startGlow, pts[0].X - 5.5f, pts[0].Y - 5.5f, 11f, 11f);
                using (var startCore = new SolidBrush(Color.White))
                    g.FillEllipse(startCore, pts[0].X - 2f, pts[0].Y - 2f, 4f, 4f);

                // Direction arrows along the path every ~60px of arc length
                float since = 0f;
                for (int i = 1; i < pts.Length; i++)
                {
                    float dx = pts[i].X - pts[i - 1].X, dy = pts[i].Y - pts[i - 1].Y;
                    float seg = (float)Math.Sqrt(dx * dx + dy * dy);
                    since += seg;
                    if (since >= 60f && seg > 0.01f)
                    {
                        since = 0f;
                        DrawArrowHead(g, pts[i], dx / seg, dy / seg, 9f, Color.FromArgb(230, accent));
                    }
                }

                // End arrowhead, larger and brighter
                var pe = pts[^1];
                var pp = pts[^2];
                float edx = pe.X - pp.X, edy = pe.Y - pp.Y;
                float elen = (float)Math.Sqrt(edx * edx + edy * edy);
                if (elen > 0.01f)
                    DrawArrowHead(g, pe, edx / elen, edy / elen, 13f, Color.White);
            }

            // Stroke order badges when the gesture has breaks (multiple strokes)
            if (strokes.Count(s => s.Count >= 2) > 1)
            {
                int number = 0;
                foreach (var stroke in strokes)
                {
                    if (stroke.Count < 2) continue;
                    number++;

                    // Place the badge just behind the stroke start, opposite the
                    // initial draw direction, clamped inside the box
                    var p0 = stroke[0];
                    var p1 = stroke[Math.Min(3, stroke.Count - 1)];
                    float dx = p1.X - p0.X, dy = p1.Y - p0.Y;
                    float len = (float)Math.Sqrt(dx * dx + dy * dy);
                    float bx = p0.X - (len > 0.01f ? dx / len : 0f) * 16f;
                    float by = p0.Y - (len > 0.01f ? dy / len : -1f) * 16f;
                    bx = Math.Clamp(bx, rect.X + 9, rect.Right - 9);
                    by = Math.Clamp(by, rect.Y + 9, rect.Bottom - 9);

                    using var fill = new SolidBrush(Color.FromArgb(230, 30, 32, 40));
                    using var ring = new Pen(Color.FromArgb(200, accent), 1.2f);
                    g.FillEllipse(fill, bx - 8f, by - 8f, 16f, 16f);
                    g.DrawEllipse(ring, bx - 8f, by - 8f, 16f, 16f);

                    using var f = new Font("Segoe UI", 7.5f, FontStyle.Bold);
                    string txt = number.ToString();
                    var ts = g.MeasureString(txt, f);
                    using var tb = new SolidBrush(Color.White);
                    g.DrawString(txt, f, tb, bx - ts.Width / 2f, by - ts.Height / 2f + 0.5f);
                }
            }

            // Hand label, bottom left
            if (!string.IsNullOrEmpty(label))
            {
                using var f = new Font("Segoe UI", 8f, FontStyle.Bold);
                using var b = new SolidBrush(Color.FromArgb(80, accent));
                g.DrawString(label, f, b, rect.X + 8, rect.Bottom - 22);
            }

            // Empty-state hint, centered
            if (emptyHint != null)
            {
                using var f = new Font("Segoe UI", 9.5f, FontStyle.Italic);
                using var b = new SolidBrush(Color.FromArgb(70, 255, 255, 255));
                var size = g.MeasureString(emptyHint, f);
                g.DrawString(emptyHint, f, b,
                    rect.X + (rect.Width - size.Width) / 2f,
                    rect.Y + (rect.Height - size.Height) / 2f);
            }
        }

        private static void DrawArrowHead(Graphics g, PointF at, float ux, float uy, float size, Color color)
        {
            float px = -uy, py = ux; // perpendicular
            var tip = new PointF(at.X + ux * size, at.Y + uy * size);
            var left = new PointF(at.X - ux * size * 0.4f - px * size * 0.6f, at.Y - uy * size * 0.4f - py * size * 0.6f);
            var right = new PointF(at.X - ux * size * 0.4f + px * size * 0.6f, at.Y - uy * size * 0.4f + py * size * 0.6f);
            using var b = new SolidBrush(color);
            g.FillPolygon(b, new[] { tip, left, right });
        }
    }
}
