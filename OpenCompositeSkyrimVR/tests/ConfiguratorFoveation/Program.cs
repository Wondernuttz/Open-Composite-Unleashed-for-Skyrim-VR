using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static class Program
{
    const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    static int checks;
    static T Field<T>(MainForm form, string name) => (T)typeof(MainForm).GetField(name, Private)!.GetValue(form)!;
    static object? Call(MainForm form, string name, params object[] args) => typeof(MainForm).GetMethod(name, Private)!.Invoke(form, args);
    static EyeFoveationSettings Capture(MainForm form) => (EyeFoveationSettings)Call(form, "CaptureEyeFoveationSettings")!;
    static void Check(bool ok, string message) { ++checks; if (!ok) throw new Exception(message); }
    static EyeFoveationEditor Editor(MainForm form) => (EyeFoveationEditor)Call(form, "CreateEyeFoveationEditor")!;
    static void Modal(EyeFoveationEditor dialog, bool apply)
    {
        // Exercise the real modal message loop and production button handlers,
        // without showing a test window or running MainForm.OnShown/deployment.
        dialog.Opacity = 0;
        using var timer = new Timer { Interval = 40 };
        timer.Tick += (_, _) => { timer.Stop(); if (apply) dialog.AcceptButton!.PerformClick(); else dialog.CancelButton!.PerformClick(); };
        dialog.Shown += (_, _) => timer.Start();
        Check(dialog.ShowDialog() == (apply ? DialogResult.OK : DialogResult.Cancel), "Wrong modal result");
    }
    static void Render(EyeFoveationEditor form, string path, Size size, float scale = 1)
    {
        form.Preview.Animate = false;
        form.ClientSize = size;
        if (scale != 1) {
            var fonts = Descendants(form).Select(c => (Control: c, Font: c.Font)).ToArray();
            form.Scale(new SizeF(scale, scale));
            foreach (var item in fonts) item.Control.Font = new Font(item.Font.FontFamily, item.Font.SizeInPoints * scale, item.Font.Style);
        }
        form.Opacity = 0; form.Show(); Application.DoEvents(); form.PerformLayout();
        using var bitmap = new Bitmap(form.Width, form.Height);
        form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size));
        bitmap.Save(path);
        if (Path.GetFileName(path).StartsWith("eye-editor-adjusted-")) {
            var scroll = (Panel)form.PeripheralMask.Parent!.Parent!;
            scroll.AutoScrollPosition = new Point(0, Math.Max(0, form.PeripheralMaskRadius.Parent!.Top - 20));
            Application.DoEvents();
            var fieldPosition = scroll.PointToClient(form.PeripheralMaskRadius.PointToScreen(Point.Empty));
            Check(fieldPosition.Y >= 0 && fieldPosition.Y + form.PeripheralMaskRadius.Height <= scroll.ClientSize.Height,
                "Blackout controls cannot scroll into view at scale " + scale);
            using var scrolled = new Bitmap(form.Width, form.Height);
            form.DrawToBitmap(scrolled, new Rectangle(Point.Empty, scrolled.Size));
            scrolled.Save(Path.Combine(Path.GetDirectoryName(path)!, Path.GetFileNameWithoutExtension(path) + "-scrolled.png"));
        }
        form.Hide();
        var colors = new System.Collections.Generic.HashSet<int>();
        for (int y = 80; y < bitmap.Height - 60; y += 13) for (int x = 40; x < bitmap.Width / 2; x += 13) colors.Add(bitmap.GetPixel(x, y).ToArgb());
        Check(colors.Count > 20, "Empty or unpainted preview render");
        Check(form.Inner.Visible == form.Mid.Visible, "Ring control visibility diverged");
        Check(form.Preview.Width > 300 && form.Preview.Height > 350, "Preview shrank below readable size");
        Check(form.Preview.PlotBounds().Width > 150 * scale, "Screen-height constraints shrank the photo excessively at scale " + scale);
        Check(form.AcceptButton is Button b && b.Right <= b.Parent!.ClientSize.Width, "Apply button clipped");
        foreach (var field in new[] { form.HorizontalScale, form.HorizontalOffset, form.VerticalOffset, form.PeripheralMaskRadius })
            Check(field.Width > 55 * scale && field.Right <= field.Parent!.ClientSize.Width,
                "Geometry field clipped at scale " + scale);
        Check(form.BlackoutCull.Width >= form.BlackoutCull.GetPreferredSize(Size.Empty).Width,
            "Experimental blackout label clipped at scale " + scale);
    }
    static System.Collections.Generic.IEnumerable<Control> Descendants(Control root)
    {
        yield return root;
        foreach (Control child in root.Controls) foreach (Control item in Descendants(child)) yield return item;
    }
    static void PhotoChecks(string output)
    {
        Check(EyeFoveationPhoto.CenterCrop(new Size(2816, 1536)) == new Rectangle(640, 0, 1536, 1536), "Photo crop stretched or off-center");
        var plot = new RectangleF(0, 0, 400, 400); var gaze = new PointF(200, 200);
        Check(EyeFoveationPhoto.RingBounds(plot, gaze, .20m) == new RectangleF(160, 160, 80, 80), "Center geometry does not match runtime UV boundary");
        Check(EyeFoveationPhoto.RingBounds(plot, gaze, .40m) == new RectangleF(120, 120, 160, 160), "Middle geometry does not match runtime UV boundary");
        var wideBounds = EyeFoveationPhoto.RingBounds(plot, gaze, .20m, 1.5m);
        Check(Math.Abs(wideBounds.X - 140) < .001f && Math.Abs(wideBounds.Width - 120) < .001f && wideBounds.Y == 160 && wideBounds.Height == 80,
            "Width adjustment changed ring height or center");
        using var photo = new EyeFoveationPhoto();
        var defaults = new EyeFoveationSettings();
        Bitmap render(EyeFoveationSettings settings, PointF at, bool effect = true) {
            var bitmap = new Bitmap(400, 400);
            using var g = Graphics.FromImage(bitmap); photo.Draw(g, plot, at, settings, effect);
            return bitmap;
        }
        bool same(Bitmap a, Bitmap b) {
            for (int y = 3; y < 397; y += 7) for (int x = 3; x < 397; x += 7)
                if (a.GetPixel(x, y) != b.GetPixel(x, y)) return false;
            return true;
        }
        using var original = render(defaults, gaze, false);
        using var reduced = render(defaults, gaze);
        Check(!same(original, reduced), "Selected coarse photo rates do not change any sampled pixels");
        Check(original.GetPixel(205, 205) == reduced.GetPixel(205, 205), "Full-density center degraded");
        foreach (var settings in new[] { defaults with { Enabled = false }, defaults with { Backend = 3 },
            defaults with { MidRate = "1x1", OuterRate = "1x1" } }) {
            using var untouched = render(settings, gaze);
            Check(same(original, untouched), "Disabled/effects-only/full-rate preview altered photo");
        }
        var halfX = photo.ImageForRate(400, "2x1"); var halfY = photo.ImageForRate(400, "1x2");
        bool columns = true, rows = true, horizontalVaries = false, verticalVaries = false;
        for (int y = 10; y < 390; y += 14) for (int x = 10; x < 390; x += 14) {
            columns &= halfX.GetPixel(x, y) == halfX.GetPixel(x + 1, y);
            rows &= halfY.GetPixel(x, y) == halfY.GetPixel(x, y + 1);
            horizontalVaries |= halfY.GetPixel(x, y) != halfY.GetPixel(x + 1, y);
            verticalVaries |= halfX.GetPixel(x, y) != halfX.GetPixel(x, y + 1);
        }
        Check(columns && rows && horizontalVaries && verticalVaries, "Photo rate axes reversed or not represented");
        using var moved = render(defaults, new PointF(100, 200));
        Check(!same(reduced, moved), "Moving gaze did not move quality regions");
        Check(reduced.GetPixel(350, 350) == moved.GetPixel(350, 350), "Gaze movement shifted the image or outer sampling grid");
        Check(ReferenceEquals(halfX, photo.ImageForRate(400, "2x1")), "Gaze movement rebuilt rate cache");
        var adjusted = defaults with { HorizontalScale = 1.5m, HorizontalOffset = .1m, VerticalOffset = -.05m };
        Check(EyeFoveationPhoto.AdjustedGaze(plot, gaze, adjusted) == new PointF(240, 180), "Preview offset direction or units wrong");
        Check(EyeFoveationPhoto.AdjustedGaze(plot, new PointF(390, 5), adjusted) == new PointF(400, 0), "Adjusted gaze crossed an eye boundary");
        using var wider = render(defaults with { HorizontalScale = 1.5m }, gaze);
        Check(!same(reduced, wider), "Width adjustment did not move the photo quality boundary");
        using var shifted = render(adjusted, gaze);
        Check(!same(reduced, shifted), "Offsets did not move the preview quality regions");
        Check(shifted.GetPixel(350, 350) == reduced.GetPixel(350, 350), "Offsets shifted the photo or its sample grid");
        var blackout = adjusted with { PeripheralMask = true, PeripheralMaskRadius = .6m };
        using var blacked = render(blackout, gaze);
        Check(blacked.GetPixel(0, 0).ToArgb() == Color.Black.ToArgb(), "Peripheral blackout did not paint the outer view black");
        Check(blacked.GetPixel(240, 180) == shifted.GetPixel(240, 180), "Peripheral blackout altered the adjusted center");
        using var disabledMask = render(blackout with { Enabled = false }, gaze);
        Check(same(original, disabledMask), "Disabled eye tracking left blackout active in preview");
        using var shapedMask = render(blackout with { HorizontalScale = .5m }, gaze);
        Check(!same(blacked, shapedMask), "Blackout did not use the adjusted ellipse width");
        shifted.Save(Path.Combine(output, "pugdragon-adjusted-gaze.png"));
        blacked.Save(Path.Combine(output, "pugdragon-adjusted-blackout.png"));
        foreach (bool middle in new[] { false, true }) foreach (bool outer in new[] { false, true }) {
            var visibility = defaults with { MiddleBlackout = middle, OuterBlackout = outer };
            using var zones = render(visibility, gaze);
            Check(zones.GetPixel(200, 200) == reduced.GetPixel(200, 200), "Ring blackout covered the full-quality center");
            Check(zones.GetPixel(260, 200) == (middle ? Color.FromArgb(255, 0, 0, 0) : reduced.GetPixel(260, 200)), "Middle blackout did not independently cover only its band");
            Check(zones.GetPixel(330, 200) == (outer ? Color.FromArgb(255, 0, 0, 0) : reduced.GetPixel(330, 200)), "Outer blackout did not independently cover beyond the middle boundary");
            using var off = render(visibility with { Enabled = false }, gaze);
            Check(same(original, off), "Disabled eye tracking left a ring blackout active");
            using var cutoffOff = render(visibility with { PeripheralMaskRadius = .4m }, gaze);
            Check(same(zones, cutoffOff), "Disabled outer cutoff invented a third quality transition");
            using var cutoffOn = render(visibility with { PeripheralMaskRadius = .7m, PeripheralMask = true }, gaze);
            Check(cutoffOn.GetPixel(350, 200).ToArgb() == Color.Black.ToArgb(), "Finite outer cutoff did not combine with independent ring blackouts");
            Check(cutoffOn.GetPixel(260, 200) == zones.GetPixel(260, 200), "Outer cutoff changed the middle blackout state");
            zones.Save(Path.Combine(output, $"pugdragon-middle-{middle}-outer-{outer}.png"));
        }
        using var edge = render(defaults with { Radii = new(.8m, 1.5m) }, PointF.Empty);
        Check(edge.GetPixel(399, 399).A == 255, "Large off-center rings left unpainted photo pixels");
        original.Save(Path.Combine(output, "dragonpug-full-detail.png"));
        reduced.Save(Path.Combine(output, "dragonpug-performance-detail.png"));
    }
    [STAThread]
    static int Main(string[] args)
    {
        try {
            string output = Path.GetFullPath(args[0]); Directory.CreateDirectory(output);
            bool perMonitor = args.Contains("--per-monitor");
            Application.SetHighDpiMode(perMonitor ? HighDpiMode.PerMonitorV2 : HighDpiMode.DpiUnaware);
            Application.EnableVisualStyles();
            PhotoChecks(output);
            using var main = new MainForm(); // Never shown; no install or live-file actions.
            var ini = Field<IniFile>(main, "_ini"); string path = Path.Combine(output, "foveation-fixture.ini");
            void load(string contents) { File.WriteAllText(path, contents); ini.Load(path); Call(main, "ReadFromIni"); }
            void saveReload() { Call(main, "WriteToIni", false); ini.Save(path); ini.Load(path); Call(main, "ReadFromIni"); }
            var defaults = new EyeFoveationSettings();
            Check(defaults.HorizontalScale == 1m && defaults.HorizontalOffset == 0m && defaults.VerticalOffset == 0m &&
                !defaults.PeripheralMask && defaults.PeripheralMaskRadius == 1m && !defaults.MiddleBlackout && !defaults.OuterBlackout &&
                !defaults.BlackoutCull && !defaults.CanCullBlackout,
                "Fresh shape/position/blackout defaults changed the old view");
            Check(defaults.Radii == new FoveationRadii(.20m, .40m) &&
                defaults.EffectiveRates.SequenceEqual(new[] { "1x1", "2x2", "4x2" }) && defaults.Enabled &&
                defaults.Backend == 0 && !defaults.DebugRings && defaults.CustomRates && !defaults.Compatibility && defaults.FavorHorizontal,
                "Fresh defaults differ from the requested Performance profile");
            Check(FoveationProfiles.EyePresetNames.SequenceEqual(new[] { "Quality", "Balanced", "Performance", "Custom", "Aggressive" }),
                "Eye preset names or stable indices changed");
            var presetCases = new[] {
                (Index: 0, Name: "Quality", Inner: .30m, Mid: .65m, MidRate: "1x2", OuterRate: "2x2"),
                (Index: 1, Name: "Balanced", Inner: .22m, Mid: .45m, MidRate: "2x2", OuterRate: "4x2"),
                (Index: 2, Name: "Performance", Inner: .20m, Mid: .40m, MidRate: "2x2", OuterRate: "4x2"),
                (Index: 4, Name: "Aggressive", Inner: .20m, Mid: .40m, MidRate: "2x2", OuterRate: "4x4")
            };
            var unrelated = defaults with { Enabled = false, Backend = 2, DebugRings = true, FavorHorizontal = false,
                Radii = new(.77m, 1.11m), CustomRates = false, Compatibility = true,
                InnerRate = "1x2", MidRate = "4x4", OuterRate = "2x4", HorizontalScale = 1.6m,
                HorizontalOffset = .125m, VerticalOffset = -.075m, PeripheralMask = true, PeripheralMaskRadius = 1.4m,
                MiddleBlackout = true, OuterBlackout = true, BlackoutCull = true };
            foreach (var sample in presetCases) {
                var expected = unrelated with { Radii = new(sample.Inner, sample.Mid), CustomRates = true, Compatibility = false,
                    InnerRate = "1x1", MidRate = sample.MidRate, OuterRate = sample.OuterRate };
                if (sample.Index == 4) expected = expected with {
                    HorizontalScale = 1m, HorizontalOffset = 0m, VerticalOffset = 0m,
                    PeripheralMask = true, PeripheralMaskRadius = .82m,
                    MiddleBlackout = false, OuterBlackout = false, BlackoutCull = false
                };
                Check(unrelated.WithPreset(sample.Index) == expected, sample.Name + " did not apply its complete profile");
                Check(expected.PresetIndex == sample.Index, sample.Name + " was not detected exactly");
                using var editor = new EyeFoveationEditor(unrelated);
                editor.Preset.SelectedIndex = sample.Index;
                Check(editor.ReadDraft() == expected && editor.Preview.Settings == expected,
                    sample.Name + " popup/preview disagrees with requested sizes, rates or cap");
                Check(editor.Preset.SelectedItem?.ToString() == sample.Name, sample.Name + " has wrong popup label");
                Call(main, "ApplyEyeFoveationSettings", editor.ReadDraft()); saveReload();
                Check(Capture(main) == expected && Field<ComboBox>(main, "_cboVrsEyePreset").SelectedIndex == sample.Index,
                    sample.Name + " failed page Save/reload");
                foreach (var altered in new[] {
                    expected with { Radii = expected.Radii with { Inner = expected.Radii.Inner + .01m } },
                    expected with { Radii = expected.Radii with { Mid = expected.Radii.Mid + .01m } },
                    expected with { InnerRate = "1x2" }, expected with { MidRate = "4x4" },
                    expected with { OuterRate = "1x1" }, expected with { CustomRates = false }, expected with { Compatibility = true }
                }) {
                    Check(altered.PresetIndex == 3, sample.Name + " detection ignored an altered boundary, rate or policy");
                    editor.SetDraft(altered);
                    Check(editor.Preset.SelectedIndex == 3 && editor.ReadDraft() == altered,
                        sample.Name + " popup relabeled or overwrote an edited profile");
                }
                editor.SetDraft(expected with { Enabled = true, DebugRings = false });
                Render(editor, Path.Combine(output, "eye-preset-" + sample.Name.ToLowerInvariant() + ".png"), new Size(1040, 850));
                if (sample.Index == 4) foreach (var altered in new[] {
                    expected with { HorizontalScale = 1.1m }, expected with { HorizontalOffset = .01m },
                    expected with { VerticalOffset = -.01m }, expected with { PeripheralMask = false },
                    expected with { PeripheralMaskRadius = .83m }, expected with { MiddleBlackout = true },
                    expected with { OuterBlackout = true }, expected with { BlackoutCull = true }
                }) {
                    editor.SetDraft(altered);
                    Check(editor.Preset.SelectedIndex == 3 && editor.ReadDraft() == altered,
                        "Edited Aggressive geometry/blackout was not retained as Custom");
                    editor.Preset.SelectedIndex = 4;
                    Check(editor.ReadDraft() == expected && editor.Preview.Settings == expected,
                        "Selecting Aggressive did not restore its saved geometry and cutoff");
                }
            }
            Check(unrelated.WithPreset(3) == unrelated && unrelated.WithPreset(-1) == unrelated && unrelated.WithPreset(5) == unrelated,
                "Custom or invalid preset selection changed stored tuning");
            foreach (var fixedPreset in new[] { (Index: 0, Inner: .70m, Mid: .85m), (Index: 1, Inner: .60m, Mid: .80m), (Index: 2, Inner: .50m, Mid: .70m) })
                Check(FoveationProfiles.Preset(false, fixedPreset.Index) == new FoveationRadii(fixedPreset.Inner, fixedPreset.Mid),
                    "Eye preset ladder changed fixed preset " + fixedPreset.Index);
            foreach (string alias in new[] { "true", "1", "yes", "on", "enabled", "ON", " Enabled " }) {
                load($"vrsEyeTracked={alias}\nvrsEyeCustomRates={alias}\nvrsEyeCompatibilityMode={alias}\nvrsFavorHorizontal={alias}\nfoveationDebugRings={alias}\n" +
                    $"vrsEyeMiddleBlackout={alias}\nvrsEyeOuterBlackout={alias}\nvrsEyePeripheralMask={alias}\nvrsEyeBlackoutCull={alias}\n");
                var value = Capture(main);
                Check(value.Enabled && value.CustomRates && value.Compatibility && value.FavorHorizontal && value.DebugRings &&
                    value.MiddleBlackout && value.OuterBlackout && value.PeripheralMask && value.BlackoutCull, "Boolean alias lost: " + alias);
                saveReload(); Check(Capture(main) == value, "Boolean alias failed save/reload: " + alias);
            }
            foreach (string section in new[] { "", "[general]\n", "[vrs]\n" }) {
                load(section);
                Check(Capture(main) == defaults, "Fresh config is not complete uncapped Performance");
                Check(Field<ComboBox>(main, "_cboVrsEyePreset").SelectedIndex == 2, "Performance not detected on page");
                Check(Field<CheckBox>(main, "_chkVrsCompatibilityMode").Checked, "Fixed default cap changed");
                Check(Field<NumericUpDown>(main, "_nudVrsInnerRadius").Value == .70m, "Fixed default size changed");
                saveReload(); Check(Capture(main) == defaults, "Performance save/reload changed settings");
            }
            foreach (string section in new[] { "", "[default]\n", "[vrs]\n" }) {
                load(section + "vrsEyeHorizontalScale=1.625\nvrsEyeHorizontalOffset=.125\nvrsEyeVerticalOffset=-.075\n" +
                    "vrsEyePeripheralMask=true\nvrsEyePeripheralMaskRadius=.75\nvrsEyeMiddleBlackout=true\nvrsEyeOuterBlackout=false\n");
                var shaped = defaults with { HorizontalScale = 1.625m, HorizontalOffset = .125m, VerticalOffset = -.075m,
                    PeripheralMask = true, PeripheralMaskRadius = .75m, MiddleBlackout = true };
                Check(Capture(main) == shaped, "Geometry-only settings changed legacy profile defaults or units");
                saveReload(); Check(Capture(main) == shaped, "Shape/position/blackout failed INI roundtrip");
                load(section + "vrsEyeHorizontalScale=9\nvrsEyeHorizontalOffset=-1\nvrsEyeVerticalOffset=1\n" +
                    "vrsEyePeripheralMaskRadius=.1\n");
                Check(Capture(main) == (defaults with { HorizontalScale = 2m, HorizontalOffset = -.25m, VerticalOffset = .25m,
                    PeripheralMaskRadius = defaults.Radii.Mid }), "Out-of-range geometry was not clamped or mask entered middle ring");
                foreach (string invalid in new[] { "NaN", "Infinity", "not-a-number", "" }) {
                    load(section + "vrsEyeHorizontalScale=" + invalid + "\nvrsEyeHorizontalOffset=" + invalid +
                        "\nvrsEyeVerticalOffset=" + invalid + "\nvrsEyePeripheralMaskRadius=" + invalid + "\n");
                    Check(Capture(main) == defaults, "Invalid geometry did not restore neutral defaults: " + invalid);
                }
                load(section + "vrsEyeCustomRates=true\nvrsEyeCompatibilityMode=false\n");
                var partial = Capture(main);
                Check(partial.MidRate == "2x1" && partial.OuterRate == "2x2", "Partial older profile adopted new missing-rate defaults");
                saveReload(); Check(Capture(main) == partial, "Partial legacy rates changed during save/reload");
                load(section + "vrsEyeCustomRates=true\nvrsEyeCompatibilityMode=false\nvrsEyeInnerRadius=.20\nvrsEyeMidRadius=.40\n" +
                    "vrsEyeInnerRate=1x1\nvrsEyeMidRate=2x1\nvrsEyeOuterRate=2x2\n");
                var formerNormal = Capture(main);
                Check(formerNormal.PresetIndex == 3 && formerNormal.EffectiveRates.SequenceEqual(new[] { "1x1", "2x1", "2x2" }),
                    "Old explicit Normal was replaced or mislabeled as a named preset");
                load(section + "vrsEyeCustomRates=true\nvrsEyeCompatibilityMode=false\nvrsEyeInnerRadius=.20\nvrsEyeMidRadius=.40\n" +
                    "vrsEyeInnerRate=1x1\nvrsEyeMidRate=2x2\nvrsEyeOuterRate=4x4\n");
                var savedAggressive = Capture(main);
                Check(savedAggressive.PresetIndex == 3 && savedAggressive.OuterRate == "4x4" && !savedAggressive.PeripheralMask,
                    "Older 4x4 tuning without the cutoff must remain Custom without enabling blackout");
                saveReload(); Check(Capture(main) == savedAggressive, "Reading older 4x4 tuning altered its settings");
                load(section + "vrsEyeCustomRates=true\nvrsEyeCompatibilityMode=false\nvrsEyeInnerRadius=.20\nvrsEyeMidRadius=.40\n" +
                    "vrsEyeInnerRate=1x1\nvrsEyeMidRate=2x2\nvrsEyeOuterRate=4x4\nvrsEyePeripheralMask=true\nvrsEyePeripheralMaskRadius=.82\n");
                var currentAggressive = Capture(main);
                Check(currentAggressive.PresetIndex == 4 && currentAggressive.PeripheralMaskRadius == .82m,
                    "Saved Aggressive cutoff was not recognized");
                saveReload(); Check(Capture(main) == currentAggressive, "Saved Aggressive cutoff changed on roundtrip");
                ini.Reset(); Call(main, "ReadFromIni");
                Check(Capture(main) == defaults, "Master-reset reload did not restore Performance defaults");
                saveReload(); Check(Capture(main) == defaults, "Reset defaults did not survive save/reload");
            }
            load("[vrs]\nvrsEyeBlackoutCull=true\nvrsEyeTracked=false\nfoveatedBackend=effects\n");
            var inactiveCull = Capture(main);
            Check(inactiveCull.BlackoutCull && !inactiveCull.CanCullBlackout,
                "Saved experimental option was erased while unavailable");
            saveReload(); Check(Capture(main) == inactiveCull, "Inactive experimental option failed roundtrip");
            ini.Reset(); Call(main, "ReadFromIni");
            Check(Capture(main) == defaults, "Full reset retained experimental blackout skipping");
            foreach (string legacyKey in new[] { "vrsInnerRadius=.35", "vrsMidRadius=.65", "vrsEyeInnerRadius=.35", "vrsEyeMidRadius=.65",
                "vrsCompatibilityMode=true", "vrsEyeCompatibilityMode=false", "vrsFavorHorizontal=false", "vrsEyeInnerRate=1x1", "vrsEyeMidRate=1x2", "vrsEyeOuterRate=4x4" }) {
                load(legacyKey + "\n"); var old = Capture(main);
                Check(!old.CustomRates, "Old implicit layout was replaced: " + legacyKey);
                Check(old.Compatibility == !legacyKey.StartsWith("vrsEyeCompatibilityMode=false"), "Old implicit cap was lost: " + legacyKey);
                saveReload(); Check(Capture(main) == old, "Old tuning changed on roundtrip: " + legacyKey);
            }
            load("vrsFixedInnerRadius=.65\nvrsFixedMidRadius=.85\n");
            Check(Capture(main) == defaults, "Fixed-only sizing must not block new eye defaults");
            foreach (string explicitCustom in new[] { "true", "false" }) {
                load("vrsEyeCustomRates=" + explicitCustom + "\n");
                Check(Capture(main).CustomRates == (explicitCustom == "true") && Capture(main).Compatibility,
                    "Explicit custom switch must preserve old implicit cap");
            }
            foreach (string cap in new[] { "true", "false" }) {
                load("vrsCompatibilityMode=" + cap + "\n");
                Check(Capture(main).Compatibility == (cap == "true"), "Explicit legacy cap not inherited");
                load("vrsCompatibilityMode=" + cap + "\nvrsEyeCompatibilityMode=" + (cap == "true" ? "false" : "true") + "\n");
                Check(Capture(main).Compatibility != (cap == "true"), "Explicit eye cap lost to fixed cap");
            }
            const string customized = "vrsEnabled=true\nvrsFixedInnerRadius=.66\nvrsFixedMidRadius=.88\nvrsCompatibilityMode=true\n" +
                "vrsEyeTracked=true\nfoveatedBackend=rdm\nvrsEyeInnerRadius=.31\nvrsEyeMidRadius=.57\nvrsEyeCustomRates=true\n" +
                "vrsEyeInnerRate=1x2\nvrsEyeMidRate=2x4\nvrsEyeOuterRate=4x4\nvrsEyeCompatibilityMode=true\nvrsFavorHorizontal=false\n" +
                "aswLocoScale=.75\naswRotationScale=.45\nfoveationDebugRings=true\n" +
                "vrsEyeHorizontalScale=1.6\nvrsEyeHorizontalOffset=.12\nvrsEyeVerticalOffset=-.08\nvrsEyePeripheralMask=true\nvrsEyePeripheralMaskRadius=.9\n" +
                "vrsEyeMiddleBlackout=true\nvrsEyeOuterBlackout=false\nvrsEyeBlackoutCull=true\n";
            load(customized); var before = Capture(main); string diskBefore = File.ReadAllText(path);
            var openButton = Descendants(main).OfType<EyeFoveationButton>().Single();
            Check(openButton.AccessibleName == "Edit eye-tracked foveation", "Main-page eye button missing");
            foreach (string field in new[] { "_chkVrsEyeTracked", "_chkFoveationDebugRings", "_cboFoveatedBackend",
                "_cboVrsEyePreset", "_nudVrsEyeInnerRadius", "_nudVrsEyeMidRadius", "_chkVrsEyeCustomRates",
                "_chkVrsEyeCompatibilityMode", "_chkVrsFavorHorizontal", "_cboVrsEyeInnerRate", "_cboVrsEyeMidRate", "_cboVrsEyeOuterRate",
                "_nudVrsEyeHorizontalScale", "_nudVrsEyeHorizontalOffset", "_nudVrsEyeVerticalOffset",
                "_chkVrsEyePeripheralMask", "_nudVrsEyePeripheralMaskRadius", "_chkVrsEyeMiddleBlackout", "_chkVrsEyeOuterBlackout", "_chkVrsEyeBlackoutCull" })
                Check(Field<Control>(main, field).Parent?.Name == "EyeFoveationState", "Duplicate eye field remains on Video: " + field);
            Check(Field<Control>(main, "_nudVrsInnerRadius").Parent == openButton.Parent &&
                Field<Control>(main, "_chkVrsCompatibilityMode").Parent == openButton.Parent, "Fixed controls were moved out of Video");
            foreach (bool apply in new[] { false, true }) {
                main.EyeEditorFactory = settings => {
                    var dialog = new EyeFoveationEditor(settings) { Opacity = 0 };
                    dialog.Shown += (_, _) => dialog.BeginInvoke(new Action(() => {
                        dialog.Preset.SelectedIndex = 4;
                        if (apply) dialog.AcceptButton!.PerformClick(); else dialog.CancelButton!.PerformClick();
                    }));
                    return dialog;
                };
                typeof(Button).GetMethod("OnClick", Private)!.Invoke(openButton, new object[] { EventArgs.Empty });
                Check(Capture(main) == (apply ? before.WithPreset(4) : before), "Main-page button modal result not applied correctly");
                Check(File.ReadAllText(path) == diskBefore, "Main-page popup wrote INI before Save");
            }
            Check(Field<Label>(main, "_lblVrsEffectiveRates").Text.Contains("0.20") &&
                Field<Label>(main, "_lblVrsEffectiveRates").Text.Contains("0.40"), "Video summary did not reflect applied Aggressive");
            main.EyeEditorFactory = settings => new EyeFoveationEditor(settings);
            load(customized);
            using (var canceled = Editor(main)) {
                Check(canceled.ReadDraft() == before, "Popup did not receive current page state");
                canceled.Preset.SelectedIndex = 4; canceled.Backend.SelectedIndex = 1; canceled.DebugRings.Checked = false;
                canceled.HorizontalScale.Value = 75m; canceled.HorizontalOffset.Value = -10m; canceled.VerticalOffset.Value = 20m;
                canceled.PeripheralMask.Checked = false; canceled.PeripheralMaskRadius.Value = 1.3m;
                canceled.MiddleBlackout.Checked = false; canceled.OuterBlackout.Checked = true;
                canceled.BlackoutCull.Checked = false;
                Modal(canceled, false); Call(main, "ApplyEyeFoveationEditorResult", canceled);
                Check(Capture(main) == before, "Cancel mutated main page");
                Check(canceled.AcceptedSettings == null, "Cancel exposed an accepted result");
            }
            Check(File.ReadAllText(path) == diskBefore, "Cancel wrote to disk");
            using (var accepted = Editor(main)) {
                accepted.Preset.SelectedIndex = 4;
                Check(accepted.ReadDraft().EffectiveRates.SequenceEqual(new[] { "1x1", "2x2", "4x4" }), "Aggressive selection remained capped");
                Check(!accepted.Cap.Checked && accepted.CustomRates.Checked, "Aggressive did not select explicit rates/cap policy");
                Check(accepted.Preview.Settings == accepted.ReadDraft(), "Preview not using live draft");
                accepted.Cap.Checked = true;
                Check(accepted.Preset.SelectedIndex == 3, "Capped Aggressive mislabeled as Aggressive");
                Check(accepted.Preview.Settings.EffectiveRates[2] == "1x2", "Cap ignored shared vertical preference");
                accepted.Cap.Checked = false;
                accepted.Inner.Value = .60m; accepted.Mid.Value = .30m;
                Check(accepted.Mid.Value >= accepted.Inner.Value, "Editor permitted reversed boundaries");
                accepted.Preset.SelectedIndex = 4;
                Modal(accepted, true); Call(main, "ApplyEyeFoveationEditorResult", accepted);
                Check(Capture(main) == before.WithPreset(4), "Apply did not update exactly the intended settings");
            }
            Check(File.ReadAllText(path) == diskBefore, "Apply bypassed page Save and wrote disk");
            saveReload();
            Check(Capture(main) == before.WithPreset(4), "Applied Aggressive failed INI roundtrip");
            Check(ini.Get("", "vrsFixedInnerRadius", "") == "0.66" && ini.Get("", "vrsFixedMidRadius", "") == "0.88" &&
                ini.Get("", "vrsCompatibilityMode", "") == "true" && ini.Get("", "vrsEnabled", "") == "true", "Eye edit changed fixed fallback");
            Check(ini.Get("", "aswLocoScale", "") == "0.75" && ini.Get("", "aswRotationScale", "") == "0.45", "Eye edit changed DAPA tuning");
            using (var shapeEditor = Editor(main)) {
                var originalDraft = shapeEditor.ReadDraft();
                Check(!Descendants(shapeEditor).OfType<ComboBox>().Any(c => c.AccessibleName == "Preview eye"), "Eye selector remains in the single-gaze preview");
                typeof(Button).GetMethod("OnClick", Private)!.Invoke(shapeEditor.ClearAdjustments, new object[] { EventArgs.Empty });
                Check(shapeEditor.ReadDraft() == originalDraft.ClearAdjustments(), "Clear adjustments reset more than shape and position");
                Check(shapeEditor.Preview.Settings == originalDraft.ClearAdjustments(), "Clear adjustments did not update preview");
                shapeEditor.HorizontalScale.Value = 75m; shapeEditor.HorizontalOffset.Value = -15m; shapeEditor.VerticalOffset.Value = 20m;
                shapeEditor.PeripheralMask.Checked = false;
                Check(shapeEditor.PeripheralMaskRadius.Enabled, "Optional outer boundary must remain editable when the cutoff is disabled");
                shapeEditor.PeripheralMask.Checked = true; shapeEditor.PeripheralMaskRadius.Value = .1m;
                Check(shapeEditor.PeripheralMaskRadius.Value == shapeEditor.Mid.Value, "Blackout boundary entered the middle ring");
                var acceptedShape = originalDraft with { HorizontalScale = .75m, HorizontalOffset = -.15m, VerticalOffset = .20m,
                    PeripheralMaskRadius = originalDraft.Radii.Mid };
                Modal(shapeEditor, true); Call(main, "ApplyEyeFoveationEditorResult", shapeEditor);
                Check(Capture(main) == acceptedShape, "Geometry Apply changed unrelated tuning or used wrong percent units");
                saveReload(); Check(Capture(main) == acceptedShape, "Applied shape failed INI save/reload");
            }
            foreach (bool middle in new[] { false, true }) foreach (bool outer in new[] { false, true }) {
                using var editor = new EyeFoveationEditor(defaults);
                editor.MiddleBlackout.Checked = middle; editor.OuterBlackout.Checked = outer;
                var visibility = defaults with { MiddleBlackout = middle, OuterBlackout = outer };
                Check(editor.ReadDraft() == visibility && editor.Preview.Settings == visibility, "Blackout switches changed saved rates or unrelated tuning");
                Check(editor.InnerRate.Enabled && editor.MidRate.Enabled == !middle && editor.OuterRate.Enabled == !outer,
                    "A blackout checkbox disabled the wrong rate control");
                Check(editor.Preset.SelectedIndex == defaults.PresetIndex, "Visibility selection changed the shading preset");
                Check(editor.PeripheralMaskRadius.Parent == editor.Mid.Parent && editor.MiddleBlackout.Parent == editor.Mid.Parent &&
                    editor.OuterBlackout.Parent == editor.Mid.Parent, "Blackout controls and optional outer boundary are not in the ring grid");
                var grid = (TableLayoutPanel)editor.Mid.Parent!;
                Check(grid.ColumnCount == 4 && grid.GetColumn(editor.MiddleBlackout) == 1 && grid.GetColumn(editor.OuterBlackout) == 1 &&
                    grid.GetColumn(editor.PeripheralMaskRadius) == 2 && grid.GetColumn(editor.OuterRate) == 3,
                    "Ring grid does not use Ring | Blackout | Boundary | Rate");
                Modal(editor, true); Call(main, "ApplyEyeFoveationEditorResult", editor); saveReload();
                Check(Capture(main) == visibility, "Independent blackout selection failed modal Apply and INI roundtrip");
            }
            foreach (bool eyeEnabled in new[] { false, true }) foreach (int backend in new[] { 0, 1, 2, 3 })
                for (int flags = 0; flags < 8; ++flags) {
                    var requested = defaults with { Enabled = eyeEnabled, Backend = backend, BlackoutCull = true,
                        MiddleBlackout = (flags & 1) != 0, OuterBlackout = (flags & 2) != 0, PeripheralMask = (flags & 4) != 0 };
                    using var editor = new EyeFoveationEditor(requested);
                    bool canCull = eyeEnabled && backend != 3 && flags != 0;
                    Check(editor.BlackoutCull.Enabled == canCull && requested.CanCullBlackout == canCull,
                        "Experimental skipping enabled for the wrong eye/backend/mask combination");
                    Check(editor.BlackoutCull.Checked && editor.ReadDraft() == requested,
                        "Disabling experimental control erased its saved value or changed rates");
                    editor.EyeEnabled.Checked = !eyeEnabled;
                    Check(editor.BlackoutCull.Checked && editor.BlackoutCull.Enabled == (!eyeEnabled && backend != 3 && flags != 0),
                        "Eye toggle did not refresh experimental availability while preserving opt-in");
                }
            using (var editor = new EyeFoveationEditor(defaults with { OuterBlackout = true })) {
                editor.BlackoutCull.Checked = true;
                var requested = defaults with { OuterBlackout = true, BlackoutCull = true };
                Check(editor.Preview.Settings == requested && editor.Preset.SelectedIndex == defaults.PresetIndex,
                    "Experimental toggle changed the preset or failed to update the snapshot");
                Modal(editor, true); Call(main, "ApplyEyeFoveationEditorResult", editor); saveReload();
                Check(Capture(main) == requested, "Experimental opt-in failed modal Apply and Save/reload");
            }
            foreach (bool cap in new[] { false, true }) foreach (bool horizontal in new[] { false, true }) foreach (string rate in FoveationProfiles.RateChoices) {
                var input = defaults with { Compatibility = cap, FavorHorizontal = horizontal, InnerRate = rate, MidRate = rate, OuterRate = rate };
                using var editor = new EyeFoveationEditor(input);
                Check(editor.ReadDraft() == input, "Popup changed stored rate choices");
                string expected = FoveationProfiles.EffectiveRate(rate, cap, horizontal);
                Check(editor.Preview.Settings.EffectiveRates.All(x => x == expected), "Preview cap mismatch across rings");
            }
            foreach (int backend in new[] { 0, 1, 2, 3 }) {
                using var editor = new EyeFoveationEditor(defaults with { Backend = backend, HorizontalScale = 1.5m });
                Check(editor.ReadDraft().Backend == backend, "Backend failed roundtrip");
                Check(editor.ReadDraft().HorizontalScale == 1.5m && editor.HorizontalScale.Enabled == (backend != 3), "Backend lost requested width or exposed unsupported effects-only shape");
                Check(editor.Preview.Settings.EffectiveHorizontalScale == (backend == 3 ? 1m : 1.5m), "Effects-only preview disagrees with circular API shape");
                Check(editor.HorizontalOffset.Enabled && editor.VerticalOffset.Enabled, "Effects-only incorrectly disabled supported offsets");
                if (backend == 3) Check(editor.Status.Text.Contains("do not apply") && !editor.OuterRate.Enabled, "Effects-only preview pretended hardware rates apply");
            }
            foreach (var sample in new (int Backend, bool Custom, bool Cap, bool Caveat)[] {
                (0, false, false, true), (2, false, false, true),
                (1, false, false, false), (3, false, false, false), (2, true, false, false),
                (2, false, true, false), (0, true, false, false) }) {
                using var editor = new EyeFoveationEditor(defaults with {
                    Backend = sample.Backend, CustomRates = sample.Custom, Compatibility = sample.Cap });
                Check(editor.Status.Text.Contains("far edge") == sample.Caveat,
                    "Legacy Density Mask caveat shown for the wrong rate/backend state");
            }
            // Exercise production merge=true, including its real disk-path
            // resolution. The fixture executable's own output folder is the
            // only fake mod root; never use the user's installed mod directory.
            string executableDirectory = Path.GetDirectoryName(Application.ExecutablePath)!;
            Check(Path.GetFileNameWithoutExtension(Application.ExecutablePath) == "ConfiguratorFoveation", "Merge fixture must run from its own test executable");
            Directory.CreateDirectory(Path.Combine(executableDirectory, "root"));
            Directory.CreateDirectory(Path.Combine(executableDirectory, "interface"));
            string mergePath = Path.Combine(executableDirectory, "root", "opencomposite.ini");
            string[] owned = { "vrsEyeTracked", "foveatedBackend", "foveationDebugRings", "vrsEyeInnerRadius", "vrsEyeMidRadius",
                "vrsEyeCustomRates", "vrsEyeInnerRate", "vrsEyeMidRate", "vrsEyeOuterRate", "vrsEyeCompatibilityMode", "vrsFavorHorizontal",
                "vrsEyeHorizontalScale", "vrsEyeHorizontalOffset", "vrsEyeVerticalOffset", "vrsEyePeripheralMask", "vrsEyePeripheralMaskRadius",
                "vrsEyeMiddleBlackout", "vrsEyeOuterBlackout", "vrsEyeBlackoutCull" };
            string duplicates = "";
            foreach (string section in new[] { "", "[default]\n", "[general]\n", "[vrs]\n" })
                duplicates += section + "vrsEyeTracked=false\nfoveatedBackend=rdm\nfoveationDebugRings=true\nvrsEyeInnerRadius=.31\nvrsEyeMidRadius=.57\n" +
                    "vrsEyeCustomRates=false\nvrsEyeInnerRate=1x2\nvrsEyeMidRate=4x4\nvrsEyeOuterRate=2x4\nvrsEyeCompatibilityMode=true\nvrsFavorHorizontal=false\n" +
                    "vrsEyeHorizontalScale=1.5\nvrsEyeHorizontalOffset=.1\nvrsEyeVerticalOffset=-.1\nvrsEyePeripheralMask=true\nvrsEyePeripheralMaskRadius=.9\n" +
                    "vrsEyeMiddleBlackout=true\nvrsEyeOuterBlackout=true\nvrsEyeBlackoutCull=true\n";
            duplicates += "; keep merge sentinel\ncustomSentinel=untouched\naswLocoScale=.75\nvrsFixedInnerRadius=.66\nvrsFixedMidRadius=.88\nvrsCompatibilityMode=true\n";
            File.WriteAllText(mergePath, duplicates); ini.Load(mergePath); Call(main, "ReadFromIni");
            var merged = Capture(main).WithPreset(4) with { Backend = 1, Enabled = true, DebugRings = false,
                HorizontalScale = .85m, HorizontalOffset = -.2m, VerticalOffset = .05m, PeripheralMask = false, PeripheralMaskRadius = 1.2m,
                MiddleBlackout = false, OuterBlackout = false, BlackoutCull = false };
            Call(main, "ApplyEyeFoveationSettings", merged);
            Call(main, "WriteToIni", true); ini.Save(mergePath); ini.Load(mergePath); Call(main, "ReadFromIni");
            Check(Capture(main) == merged, "Production merge Save resurrected a legacy eye setting");
            string savedMerge = File.ReadAllText(mergePath);
            foreach (string key in owned)
                Check(System.Text.RegularExpressions.Regex.Matches(savedMerge, "^" + key + "=", System.Text.RegularExpressions.RegexOptions.Multiline | System.Text.RegularExpressions.RegexOptions.IgnoreCase).Count == 1,
                    "Production merge left duplicate owned key: " + key);
            Check(savedMerge.Contains("; keep merge sentinel") && savedMerge.Contains("customSentinel=untouched"), "Merge removed unrelated data/comments");
            Check(Field<NumericUpDown>(main, "_nudAswLocoScale").Value == .75m && Field<NumericUpDown>(main, "_nudVrsInnerRadius").Value == .66m,
                "Production merge changed unrelated DAPA/fixed tuning");
            File.Copy(mergePath, Path.Combine(output, "production-merge-result.ini"), true);
            using (var editor = new EyeFoveationEditor(defaults)) {
                Render(editor, Path.Combine(output, "eye-editor-defaults.png"), new Size(1040, 850));
                Check(editor.Preview.PlotBounds().Width == editor.Preview.PlotBounds().Height, "Photo preview stretches circles into ovals");
            }
            using (var editor = new EyeFoveationEditor(before)) Render(editor, Path.Combine(output, "eye-editor-capped-custom.png"), new Size(1000, 790));
            using (var editor = new EyeFoveationEditor(defaults with { Backend = 2, CustomRates = false }))
                Render(editor, Path.Combine(output, "eye-editor-legacy-rdm.png"), new Size(1000, 790));
            using (var editor = new EyeFoveationEditor(defaults)) Render(editor, Path.Combine(output, "eye-editor-small.png"), new Size(880, 700));
            using (var editor = new EyeFoveationEditor(defaults)) Render(editor, Path.Combine(output, "eye-editor-scaled-150.png"), new Size(1000, 790), 1.5f);
            using (var editor = new EyeFoveationEditor(before with { Compatibility = false }))
                Render(editor, Path.Combine(output, "eye-editor-adjusted-100.png"), new Size(1040, 850));
            using (var editor = new EyeFoveationEditor(before with { Compatibility = false })) {
                Render(editor, Path.Combine(output, "eye-editor-adjusted-150.png"), new Size(1000, 790), 1.5f);
            }
            using (var editor = new EyeFoveationEditor(before with { Compatibility = false }))
                Render(editor, Path.Combine(output, "eye-editor-adjusted-200.png"), new Size(1000, 790), 2f);
            using (var bitmap = new Bitmap(openButton.Width, openButton.Height)) {
                openButton.DrawToBitmap(bitmap, new Rectangle(Point.Empty, bitmap.Size)); bitmap.Save(Path.Combine(output, "eye-editor-button.png"));
                Check(((ModernPillButton)(Button)openButton).VisualRole == ModernButtonRole.Positive,
                    "Eye button is not using the shared green action-button theme");
            }
            var video = openButton.Parent!;
            var oldParent = video.Parent!; var oldBounds = video.Bounds; var oldDock = video.Dock;
            bool oldVisible = video.Visible;
            // Render the real page in a fixture host, never MainForm.OnShown.
            // A hidden main form's child panel otherwise produces an empty bitmap.
            using (var host = new Form { ClientSize = video.Size, Opacity = 0, ShowInTaskbar = false }) {
                video.Parent = host; video.Dock = DockStyle.Fill; video.Visible = true;
                host.Show(); Application.DoEvents();
                using var screenshot = new Bitmap(video.Width, video.Height);
                video.DrawToBitmap(screenshot, new Rectangle(Point.Empty, screenshot.Size));
                int top = Math.Max(0, openButton.Top - 7);
                int bottom = Math.Min(video.Height, video.Controls.OfType<Button>().Where(b => b.Text == "Save opencomposite.ini").Max(b => b.Bottom) + 8);
                using var crop = screenshot.Clone(new Rectangle(0, top, screenshot.Width, bottom - top), screenshot.PixelFormat);
                crop.Save(Path.Combine(output, "video-foveation-clean.png"));
                Check(Field<Control>(main, "_nudVrsInnerRadius").Visible && !Field<Control>(main, "_nudVrsEyeInnerRadius").Visible,
                    "Video visibility does not separate fixed controls and popup-owned eye settings");
                video.Parent = oldParent; video.Dock = oldDock; video.Bounds = oldBounds; video.Visible = oldVisible;
                host.Hide();
            }
            Console.WriteLine($"PASS {checks} checks ({(perMonitor ? "PerMonitorV2" : "production DpiUnaware")}): main-page button and actual modal Apply/Cancel, shared page/model/INI roundtrip, complete eye preset ladder, Performance defaults, old implicit-rate migration, boolean aliases, fixed/DAPA preservation, all requested rates and caps, single-gaze preview offsets, eye width, peripheral blackout, Clear adjustments, effects-only semantics, resize/synthetic100/150/200%-scale bitmap artifacts.");
            return 0;
        } catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
