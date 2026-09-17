using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        private const string TreadmillCalibrationHint =
            "After loading, face forward. Hold both thumbstick clicks for 2 seconds, or press the direction-sensor button.";
        private string CurrentTreadmillCalibrationHint => _chkTreadmillControllerCalibration.Checked
            ? TreadmillCalibrationHint : "After loading, face forward and press the direction-sensor button to calibrate.";
        private CheckBox _chkTreadmillEnabled = null!;
        private CheckBox _chkTreadmillControllerCalibration = null!;
        private NumericUpDown _nudTreadmillPort = null!;
        private NumericUpDown _nudTreadmillFullSpeed = null!;
        private Label _lblTreadmillGateway = null!;
        private Label _lblPhysicalTrackerConfig = null!;
        private Label _lblTreadmillConnection = null!;
        private BodySilhouettePanel _treadmillSilhouette = null!;
        private Form? _treadmillReaderDialog;
        private TreadmillReaderPreferences _treadmillReaderPreferences =
            TreadmillReaderPreferences.Load(TreadmillReaderPreferences.StoragePath);
        private CheckBox _chkPhysicalTrackers = null!;
        private Label _lblTrackerHelp = null!;
        private readonly List<CheckBox> _physicalTrackerRoles = new();
        private bool _loadingTrackerRoles, _trackerRolesEdited;
        private string _loadedTrackerRoles = "waist,left_foot,right_foot";
        private static readonly string[] PhysicalTrackerRoles = {
            "waist", "left_foot", "right_foot"
        };

        private sealed record ReaderOutput(string Line, long ReceivedAt);
        private sealed class ReaderOutputBuffer
        {
            internal ReaderOutput Latest = new("", 0);
            internal void Receive(string line) => System.Threading.Volatile.Write(
                ref Latest, new ReaderOutput(line, Environment.TickCount64));
        }

        private readonly record struct ReaderStatus(bool Connected, string Message);

        private void SetTreadmillConnection(bool connected, string message)
        {
            if (IsDisposed || _treadmillSilhouette.IsDisposed) return;
            _treadmillSilhouette.SetTreadmillConnected(connected);
            _lblTreadmillConnection.Text = message;
            _lblTreadmillConnection.ForeColor = connected ? ModernUiTheme.AccentText : ModernUiTheme.TextMuted;
        }

        private void BuildTreadmillTab()
        {
            var container = _tabTreadmill;
            // Explicit text prevents UI Automation from borrowing a label from
            // the preceding hidden page when identifying this panel.
            container.AccessibleName = "KAT VR / Trackers";
            container.AccessibleDescription = "Treadmill reader setup, controller calibration and runtime body tracker status.";
            int right = container.ClientSize.Width - 20;
            container.Controls.Add(MakeSectionLabel("KAT VR / Trackers", 12, 12));
            var save = MakeButton("Save opencomposite.ini", right - 310, 10, 200, 30);
            save.BackColor = ModernUiTheme.Accent;
            save.Click += BtnSave_Click;
            container.Controls.Add(save);
            var reload = MakeButton("Reload", right - 100, 10, 90, 30);
            reload.Click += BtnReload_Click;
            container.Controls.Add(reload);

            _treadmillSilhouette = new BodySilhouettePanel
            {
                Location = new Point(12, 58), Size = new Size(284, 426),
                ReferenceOnly = true, RuntimeMonitor = true, BackColor = ModernUiTheme.SurfaceRaised,
                AccessibleName = "Eleven-point body layout and KAT treadmill diagram",
            };
            container.Controls.Add(_treadmillSilhouette);
            _lblTreadmillConnection = new Label
            {
                Location = new Point(16, 492), Size = new Size(280, 28),
                Text = "KAT: not checked — start the reader",
                ForeColor = ModernUiTheme.TextMuted, TextAlign = ContentAlignment.TopCenter,
            };
            container.Controls.Add(_lblTreadmillConnection);
            container.Controls.Add(new Label
            {
                Location = new Point(16, 524), Size = new Size(280, 66),
                Text = "Waist/feet: green = tracked, amber = inferred\nOther body dots are a reference\nKAT platform = fresh walking input",
                ForeColor = ModernUiTheme.TextMuted, TextAlign = ContentAlignment.TopCenter,
            });

            const int x = 320;
            int y = BuildTreadmillPanel(container, x, 58, right - x);
            container.Controls.Add(MakeSectionLabel("Body trackers — SkyrimVR FBT", x, y + 8));
            _chkPhysicalTrackers = MakeCheckBox("Enable runtime body trackers (restart Skyrim)", x, y + 38);
            _chkPhysicalTrackers.AutoSize = true;
            container.Controls.Add(_chkPhysicalTrackers);
            _chkPhysicalTrackers.CheckedChanged += (_, _) => {
                foreach (var role in _physicalTrackerRoles) role.Enabled = _chkPhysicalTrackers.Checked;
            };
            for (int i = 0; i < PhysicalTrackerRoles.Length; ++i)
            {
                var (column, row) = i switch { 0 => (0, 0), 1 => (0, 1), _ => (1, 1) };
                var role = MakeCheckBox(PhysicalTrackerRoles[i].Replace('_', ' '),
                    x + column * ((right - x) / 2), y + 70 + row * 24);
                role.AutoSize = true;
                role.CheckedChanged += (_, _) => { if (!_loadingTrackerRoles) _trackerRolesEdited = true; };
                _physicalTrackerRoles.Add(role);
                container.Controls.Add(role);
            }
            _lblPhysicalTrackerConfig = new Label
            {
                Location = new Point(x, y + 132), Size = new Size(right - x, 40),
                ForeColor = ModernUiTheme.TextSecondary,
                Text = "Start Skyrim to check body poses",
            };
            container.Controls.Add(_lblPhysicalTrackerConfig);
            _lblTrackerHelp = new Label
            {
                Location = new Point(x, y + 180), Size = new Size(right - x, 116),
                Text = "Current FBT uses waist and two feet. For ankle-mounted trackers, assign left/right foot in your tracker software.\n"
                    + "Controllers already track your hands. Extra knee, elbow and chest trackers are not current FBT inputs.\n"
                    + "Avatar movement also needs SkyrimVR FBT with its VRIK/HIGGS/PLANCK requirements.\n"
                    + "KAT Reader supplies walking input. Loco MoCap requires a working Gateway tracker driver.\n"
                    + "The monitor reads game poses and does not start SteamVR.",
                ForeColor = ModernUiTheme.TextSecondary,
            };
            container.Controls.Add(_lblTrackerHelp);
            container.Height = Math.Max(650, y + 312);

            var poseTimer = new System.Windows.Forms.Timer { Interval = 250 };
            void RefreshPoses()
            {
                var status = BodyTrackerMonitor.Read();
                _treadmillSilhouette.SetRuntimeTrackerState(status.Valid & 7u, status.Tracked & 7u);
                _lblPhysicalTrackerConfig.Text = status.Valid != 0 && (status.Valid & 7u) == 0
                    ? "Runtime reports other roles; waiting for waist/foot poses" : status.Message;
            }
            poseTimer.Tick += (_, _) => RefreshPoses();
            container.VisibleChanged += (_, _) => {
                poseTimer.Enabled = container.Visible;
                if (container.Visible) RefreshPoses();
                else _treadmillSilhouette.SetRuntimeTrackerState(0, 0);
            };
            Disposed += (_, _) => poseTimer.Dispose();

            var gatewayTimer = new System.Windows.Forms.Timer { Interval = 5000 };
            gatewayTimer.Tick += (_, _) => RefreshTreadmillGatewayStatus();
            container.VisibleChanged += (_, _) =>
            {
                gatewayTimer.Enabled = container.Visible;
                if (container.Visible) RefreshTreadmillGatewayStatus();
            };
            Disposed += (_, _) => gatewayTimer.Dispose();

            Shown += (_, _) =>
            {
                // Autostart follows the loaded INI, never unrelated unsaved controls.
                var decision = _treadmillReaderPreferences.AutoStart(
                    ParseBool(_ini.Get("", "treadmillEnabled", "false")), TreadmillReaderExecutable);
                if (decision.Start) ShowTreadmillReaderSetup(startAutomatically: true);
                else if (decision.Status.Length > 0) SetTreadmillConnection(false, decision.Status);
            };
            Disposed += (_, _) => _treadmillReaderDialog?.Dispose();
        }

        private void RefreshTreadmillGatewayStatus()
        {
            bool running = false;
            try
            {
                foreach (string name in new[] { "KAT Gateway", "KATGateway", "KAT Gateway Core" })
                {
                    var processes = Process.GetProcessesByName(name);
                    running |= processes.Length > 0;
                    foreach (var process in processes) process.Dispose();
                }
                _lblTreadmillGateway.Text = running
                    ? "Gateway process found. Open Reader setup to check treadmill data."
                    : "Gateway process not found. Start Gateway, then open Reader setup.";
                _lblTreadmillGateway.ForeColor = ModernUiTheme.TextMuted;
            }
            catch (Exception ex) when (ex is InvalidOperationException || ex is System.ComponentModel.Win32Exception)
            {
                _lblTreadmillGateway.Text = "Gateway process status unavailable. Check treadmill data in Reader setup.";
                _lblTreadmillGateway.ForeColor = ModernUiTheme.TextMuted;
            }
        }

        private int BuildTreadmillPanel(Control parent, int x, int y, int width)
        {
            var panel = new Panel { Location = new Point(x, y), Size = new Size(width, 350),
                BackColor = ModernUiTheme.SurfaceRaised };
            _chkTreadmillEnabled = MakeCheckBox("KAT treadmill input (experimental)", 16, 16);
            _chkTreadmillEnabled.AutoSize = true;
            panel.Controls.Add(_chkTreadmillEnabled);
            panel.Controls.Add(new Label { Location = new Point(16, 48), Size = new Size(width - 32, 38),
                ForeColor = ModernUiTheme.TextSecondary,
                Text = "Enable input, save, then restart Skyrim. Start Gateway with your treadmill connected." });
            panel.Controls.Add(MakeLabel("Local port:", 16, 94, 75));
            _nudTreadmillPort = new NumericUpDown { Location = new Point(97, 90), Width = 82,
                Minimum = 1024, Maximum = 65535, Value = 9020 };
            panel.Controls.Add(_nudTreadmillPort);
            panel.Controls.Add(MakeLabel("Full-stick speed (m/s):", 210, 94, 150));
            _nudTreadmillFullSpeed = new NumericUpDown { Location = new Point(368, 90), Width = 70,
                Minimum = 0.1m, Maximum = 10m, DecimalPlaces = 1, Increment = 0.1m, Value = 3m };
            panel.Controls.Add(_nudTreadmillFullSpeed);
            var setup = MakeButton("Reader setup...", 16, 134, 150, 32);
            setup.Click += (_, _) => ShowTreadmillReaderSetup();
            panel.Controls.Add(setup);
            _lblTreadmillGateway = new Label { Location = new Point(182, 138), Size = new Size(width - 198, 46),
                ForeColor = ModernUiTheme.TextMuted, Text = "Gateway process status: checking when this page opens." };
            panel.Controls.Add(_lblTreadmillGateway);
            _chkTreadmillControllerCalibration = MakeCheckBox("Controller calibration (both thumbstick clicks)", 16, 190);
            _chkTreadmillControllerCalibration.AutoSize = true;
            panel.Controls.Add(_chkTreadmillControllerCalibration);
            var calibrationTip = new ToolTip { AutoPopDelay = 20000, InitialDelay = 350 };
            calibrationTip.SetToolTip(_chkTreadmillControllerCalibration,
                "Stand still with both sticks centered. Hold both thumbstick clicks for 2 seconds, then release.\n"
                + "Your normal click bindings still work. PICO O buttons and other runtime recenter controls are separate.\n"
                + "A known headset recenter keeps treadmill alignment. The direction-sensor button remains available.");
            Disposed += (_, _) => calibrationTip.Dispose();
            var calibrationHint = new Label { Location = new Point(16, 220), Size = new Size(width - 32, 44),
                ForeColor = ModernUiTheme.KeyGlowBright, Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                Text = CurrentTreadmillCalibrationHint };
            _chkTreadmillControllerCalibration.CheckedChanged += (_, _) => calibrationHint.Text = CurrentTreadmillCalibrationHint;
            panel.Controls.Add(calibrationHint);
            panel.Controls.Add(new Label { Location = new Point(16, 274), Size = new Size(width - 32, 72),
                ForeColor = ModernUiTheme.TextSecondary,
                Text = "Stand still with both sticks centered; release after calibrating. The sensor button still works.\n"
                    + "Use headset-directed movement. Keep the Configurator running while playing.\n"
                    + "A higher full-stick speed reduces sensitivity." });
            parent.Controls.Add(panel);
            return y + panel.Height + 8;
        }

        private void LoadTreadmillSettings()
        {
            _chkTreadmillEnabled.Checked = ParseBool(_ini.Get("", "treadmillEnabled", "false"));
            _chkTreadmillControllerCalibration.Checked = ParseBool(_ini.Get("", "treadmillControllerCalibration", "true"));
            if (int.TryParse(_ini.Get("", "treadmillPort", "9020"), out int port))
                _nudTreadmillPort.Value = Math.Clamp(port, 1024, 65535);
            if (float.TryParse(_ini.Get("", "treadmillFullSpeed", "3.0"), NumberStyles.Float,
                    CultureInfo.InvariantCulture, out float speed) && float.IsFinite(speed))
                _nudTreadmillFullSpeed.Value = (decimal)Math.Clamp(speed, 0.1f, 10f);
            _loadingTrackerRoles = true;
            _chkPhysicalTrackers.Checked = ParseBool(_ini.Get("input", "bodyTrackersEnabled", "true"));
            _loadedTrackerRoles = _ini.Get("input", "bodyTrackerRoles", "waist,left_foot,right_foot");
            var roles = _loadedTrackerRoles.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < PhysicalTrackerRoles.Length; ++i)
            {
                _physicalTrackerRoles[i].Checked = _loadedTrackerRoles.Trim().Equals("all", StringComparison.OrdinalIgnoreCase)
                    || roles.Contains(PhysicalTrackerRoles[i], StringComparer.OrdinalIgnoreCase);
                _physicalTrackerRoles[i].Enabled = _chkPhysicalTrackers.Checked;
            }
            _trackerRolesEdited = false;
            _loadingTrackerRoles = false;
        }

        private void SaveTreadmillSettings()
        {
            _ini.Set("", "treadmillEnabled", _chkTreadmillEnabled.Checked ? "true" : "false");
            _ini.Set("", "treadmillControllerCalibration", _chkTreadmillControllerCalibration.Checked ? "true" : "false");
            _ini.Set("", "treadmillPort", _nudTreadmillPort.Value.ToString(CultureInfo.InvariantCulture));
            _ini.Set("", "treadmillFullSpeed", _nudTreadmillFullSpeed.Value.ToString("0.0", CultureInfo.InvariantCulture));
            _ini.Set("input", "bodyTrackersEnabled", _chkPhysicalTrackers.Checked ? "true" : "false");
            if (_trackerRolesEdited)
            {
                var selected = PhysicalTrackerRoles.Where((_, i) => _physicalTrackerRoles[i].Checked);
                // Retain future/custom role names when editing the known roles.
                var other = _loadedTrackerRoles.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)
                    .Where(r => !r.Equals("all", StringComparison.OrdinalIgnoreCase)
                        && !PhysicalTrackerRoles.Contains(r, StringComparer.OrdinalIgnoreCase));
                _loadedTrackerRoles = string.Join(',', selected.Concat(other));
                _trackerRolesEdited = false;
            }
            _ini.Set("input", "bodyTrackerRoles", _loadedTrackerRoles);
        }

        private static string TreadmillReaderExecutable =>
            Path.Combine(AppContext.BaseDirectory, "Tools", "KATReader", "KATReader.exe");

        private void ShowTreadmillReaderSetup(bool startAutomatically = false)
        {
            if (_treadmillReaderDialog is { IsDisposed: false } existing)
            {
                if (existing.WindowState == FormWindowState.Minimized) existing.WindowState = FormWindowState.Normal;
                existing.Show(); existing.Activate(); return;
            }
            var dialog = new Form { Text = "KAT Reader setup — experimental", ClientSize = new Size(780, 396),
                StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog,
                MaximizeBox = false, MinimizeBox = true, BackColor = ModernUiTheme.Window, ForeColor = ModernUiTheme.TextPrimary,
                AutoScaleMode = AutoScaleMode.Dpi };
            dialog.Controls.Add(new Label { Location = new Point(18, 24), Size = new Size(744, 70),
                Text = "Start Gateway with your treadmill connected. Select KATNativeSDK.dll from that installed Gateway.\n"
                    + "Your SDK choice is remembered on this PC. Closing setup leaves the reader running; closing the Configurator stops it." });
            var sdk = new TextBox { Location = new Point(18, 104), Width = 608, ReadOnly = true,
                Text = _treadmillReaderPreferences.SdkPath };
            var choose = MakeButton("Choose SDK...", 638, 100, 124, 32);
            dialog.Controls.Add(sdk);
            dialog.Controls.Add(choose);
            var autoStart = MakeCheckBox("Start reader with Configurator when KAT input is enabled", 18, 142);
            autoStart.AutoSize = true;
            autoStart.Checked = _treadmillReaderPreferences.StartWithConfigurator;
            dialog.Controls.Add(autoStart);
            var start = MakeButton("Start reader", 18, 188, 140, 34);
            start.BackColor = ModernUiTheme.Accent;
            var stop = MakeButton("Stop reader", 170, 188, 140, 34);
            stop.Enabled = false;
            var calibrationHint = new Label { Location = new Point(18, 232), Size = new Size(744, 38),
                ForeColor = ModernUiTheme.KeyGlowBright, Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                Text = CurrentTreadmillCalibrationHint };
            EventHandler updateCalibrationHint = (_, _) => calibrationHint.Text = CurrentTreadmillCalibrationHint;
            _chkTreadmillControllerCalibration.CheckedChanged += updateCalibrationHint;
            dialog.Controls.Add(calibrationHint);
            var status = new Label { Location = new Point(18, 278), Size = new Size(744, 100),
                Text = "Not started. Reader output here confirms its data, not delivery to Skyrim.\n"
                    + "Enable KAT treadmill input, save, and restart Skyrim before testing movement." };
            dialog.Controls.Add(start);
            dialog.Controls.Add(stop);
            dialog.Controls.Add(status);
            Process? reader = null;
            ReaderOutputBuffer? readerOutput = null;
            void SaveReaderPreferences()
            {
                _treadmillReaderPreferences = new(sdk.Text, autoStart.Checked);
                if (!_treadmillReaderPreferences.TrySave(TreadmillReaderPreferences.StoragePath, out string error))
                    status.Text = error;
            }
            choose.Click += (_, _) => {
                using var picker = new OpenFileDialog { Title = "Select the SDK from your installed KAT Gateway",
                    Filter = "KAT native SDK|KATNativeSDK.dll", CheckFileExists = true };
                if (TreadmillReaderPreferences.IsSdkPath(sdk.Text) && File.Exists(sdk.Text))
                    picker.FileName = sdk.Text;
                if (picker.ShowDialog(dialog) == DialogResult.OK)
                {
                    if (!TreadmillReaderPreferences.IsSdkPath(picker.FileName))
                    {
                        status.Text = "Choose KATNativeSDK.dll from your installed Gateway.";
                        return;
                    }
                    sdk.Text = picker.FileName;
                    SaveReaderPreferences();
                }
            };
            autoStart.CheckedChanged += (_, _) => SaveReaderPreferences();
            void StopReader()
            {
                readerOutput = null;
                if (reader != null) {
                    try { if (!reader.HasExited) reader.Kill(); } catch (InvalidOperationException) { }
                    catch (System.ComponentModel.Win32Exception) { }
                    reader.Dispose(); reader = null;
                }
                if (!dialog.IsDisposed)
                {
                    start.Enabled = true; stop.Enabled = false; choose.Enabled = true;
                    status.Text = "Reader stopped. Treadmill movement expires automatically in Skyrim.";
                    status.ForeColor = ModernUiTheme.TextSecondary;
                }
                SetTreadmillConnection(false, "KAT: reader stopped");
            }
            void StartReader()
            {
                if (reader != null) return;
                string exe = TreadmillReaderExecutable;
                if (!File.Exists(exe)) { status.Text = "The optional KAT Reader is not installed at Tools\\KATReader\\KATReader.exe."; return; }
                if (!TreadmillReaderPreferences.IsSdkPath(sdk.Text) || !File.Exists(sdk.Text)) {
                    status.Text = "Choose KATNativeSDK.dll from the installed Gateway first."; return;
                }
                try {
                    // Each process owns its buffer, so late callbacks from a stopped
                    // reader cannot revive the indicator in a replacement session.
                    var output = new ReaderOutputBuffer();
                    readerOutput = output;
                    SetTreadmillConnection(false, "KAT: waiting for treadmill data");
                    var info = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true,
                        RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = Path.GetDirectoryName(exe)! };
                    info.ArgumentList.Add("--sdk"); info.ArgumentList.Add(sdk.Text);
                    info.ArgumentList.Add("--port"); info.ArgumentList.Add(_nudTreadmillPort.Value.ToString(CultureInfo.InvariantCulture));
                    info.ArgumentList.Add("--stdout");
                    reader = new Process { StartInfo = info };
                    reader.OutputDataReceived += (_, e) => {
                        if (e.Data != null) output.Receive(e.Data);
                    };
                    reader.ErrorDataReceived += (_, e) => { if (e.Data != null) output.Receive(e.Data); };
                    reader.Start(); reader.BeginOutputReadLine(); reader.BeginErrorReadLine();
                    start.Enabled = false; stop.Enabled = true; choose.Enabled = false;
                    status.Text = "Reader process started. Waiting for output...";
                } catch (Exception ex) {
                    StopReader(); status.Text = "Reader could not start: " + ex.Message;
                    SetTreadmillConnection(false, "KAT: reader could not start — open Reader setup");
                }
            }
            start.Click += (_, _) => StartReader();
            stop.Click += (_, _) => StopReader();
            var timer = new System.Windows.Forms.Timer { Interval = 100 };
            timer.Tick += (_, _) => {
                if (reader == null || readerOutput == null) return;
                var output = System.Threading.Volatile.Read(ref readerOutput.Latest);
                string line = output.Line;
                if (reader.HasExited) {
                    int code = reader.ExitCode; StopReader(); status.Text = $"Reader exited ({code}).\n{line}";
                    SetTreadmillConnection(false, "KAT: reader exited");
                } else if (line.Length > 0) {
                    long age = Environment.TickCount64 - output.ReceivedAt;
                    var report = EvaluateTreadmillReaderOutput(line, age);
                    status.Text = report.Message;
                    status.ForeColor = report.Connected ? ModernUiTheme.AccentText : ModernUiTheme.TextSecondary;
                    SetTreadmillConnection(report.Connected,
                        report.Connected ? "KAT: connected — fresh data" : "KAT: no live connection data");
                }
            };
            timer.Start();
            dialog.FormClosing += (_, e) =>
            {
                // Setup is a monitor, not the lifetime owner of an active reader.
                if (e.CloseReason == CloseReason.UserClosing && reader != null && !IsDisposed && !Disposing)
                {
                    e.Cancel = true;
                    dialog.Hide();
                }
            };
            dialog.Disposed += (_, _) => {
                _chkTreadmillControllerCalibration.CheckedChanged -= updateCalibrationHint;
                timer.Stop(); timer.Dispose(); StopReader();
                if (ReferenceEquals(_treadmillReaderDialog, dialog)) _treadmillReaderDialog = null;
            };
            ModernUiTheme.Apply(dialog, windowBands: false);
            _treadmillReaderDialog = dialog;
            if (startAutomatically) StartReader();
            else dialog.Show(this);
        }

        private static string FormatTreadmillReaderOutput(string line, long ageMs)
            => EvaluateTreadmillReaderOutput(line, ageMs).Message;

        private static ReaderStatus EvaluateTreadmillReaderOutput(string line, long ageMs)
        {
            try {
                using var json = System.Text.Json.JsonDocument.Parse(line);
                var root = json.RootElement;
                if (!root.GetProperty("connected").GetBoolean()) return new(false, "Reader reports: treadmill disconnected. No movement is sent.");
                if (ageMs < 0 || ageMs >= 250) return new(false, "No fresh treadmill sample. Connection is unconfirmed and movement is stopped.");
                var velocity = root.GetProperty("velocity");
                if (velocity.GetArrayLength() != 3) return new(false, "Invalid treadmill velocity data. Connection is unconfirmed.");
                double x = velocity[0].GetDouble(), y = velocity[1].GetDouble(), z = velocity[2].GetDouble();
                double speed = Math.Sqrt(x * x + z * z);
                if (!double.IsFinite(x) || !double.IsFinite(y) || !double.IsFinite(z) || !double.IsFinite(speed))
                    return new(false, "Invalid treadmill velocity data. Connection is unconfirmed.");
                return new(true, $"Fresh treadmill sample — speed {speed:0.00} m/s.\n"
                    + "Calibrate in loaded Skyrim using the controls above.\n"
                    + "This checks the reader; confirm movement and direction in the headset.");
            } catch (Exception ex) when (ex is System.Text.Json.JsonException || ex is InvalidOperationException
                || ex is KeyNotFoundException || ex is FormatException) {
                return new(false, "Reader: " + line);
            }
        }
    }
}
