using System.Drawing;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

public partial class MainForm
{
    private void InitializeEyeFoveationState(Control parent)
    {
        // The popup owns the visible eye controls. Keep the existing state fields
        // attached to a hidden panel so save/load and disposal share one path.
        var state = new Panel { Name = "EyeFoveationState", Visible = false, TabStop = false, Size = Size.Empty };
        parent.Controls.Add(state);
        T keep<T>(T control) where T : Control { state.Controls.Add(control); return control; }
        ComboBox choice(string[] values, int selected = 0)
        {
            var control = keep(new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList });
            control.Items.AddRange(values);
            control.SelectedIndex = selected;
            return control;
        }
        NumericUpDown radius(decimal maximum) => keep(new NumericUpDown {
            DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.10m, Maximum = maximum
        });
        _chkVrsEyeTracked = keep(new CheckBox { Checked = true });
        _chkFoveationDebugRings = keep(new CheckBox());
        _cboFoveatedBackend = choice(EyeFoveationSettings.BackendNames);
        _cboVrsEyePreset = choice(FoveationProfiles.EyePresetNames, FoveationProfiles.DefaultEyePreset);
        _nudVrsEyeInnerRadius = radius(1.00m);
        _nudVrsEyeMidRadius = radius(1.50m);
        _chkVrsEyeCustomRates = keep(new CheckBox());
        _chkVrsEyeCompatibilityMode = keep(new CheckBox());
        _chkVrsFavorHorizontal = keep(new CheckBox { Checked = true });
        _cboVrsEyeInnerRate = choice(FoveationProfiles.RateChoices);
        _cboVrsEyeMidRate = choice(FoveationProfiles.RateChoices);
        _cboVrsEyeOuterRate = choice(FoveationProfiles.RateChoices);
        _cboVrsEyeInnerRate.SelectedItem = EyeFoveationSettings.DefaultInnerRate;
        _cboVrsEyeMidRate.SelectedItem = EyeFoveationSettings.DefaultMidRate;
        _cboVrsEyeOuterRate.SelectedItem = EyeFoveationSettings.DefaultOuterRate;

        _cboVrsEyePreset.SelectedIndexChanged += (_, _) => {
            if (_updatingVrsPreset || _isLoading) return;
            ApplyEyeFoveationSettings(CaptureEyeFoveationSettings().WithPreset(_cboVrsEyePreset.SelectedIndex));
        };
        void radiiChanged(object? sender, System.EventArgs e)
        {
            if (_updatingVrsPreset || _isLoading) return;
            _updatingVrsPreset = true;
            try {
                if (_nudVrsEyeMidRadius.Value < _nudVrsEyeInnerRadius.Value)
                    _nudVrsEyeMidRadius.Value = _nudVrsEyeInnerRadius.Value;
                _cboVrsEyePreset.SelectedIndex = CaptureEyeFoveationSettings().PresetIndex;
            } finally { _updatingVrsPreset = false; }
            UpdateFoveationControls();
        }
        _nudVrsEyeInnerRadius.ValueChanged += radiiChanged;
        _nudVrsEyeMidRadius.ValueChanged += radiiChanged;
        _cboFoveatedBackend.SelectedIndexChanged += (_, _) => UpdateFoveationControls();
        _chkVrsEyeCompatibilityMode.CheckedChanged += (_, _) => UpdateFoveationControls();
        _chkVrsFavorHorizontal.CheckedChanged += (_, _) => UpdateFoveationControls();
        _chkVrsEyeCustomRates.CheckedChanged += (_, _) => UpdateFoveationControls();
        _chkFoveationDebugRings.CheckedChanged += (_, _) => UpdateFoveationControls();
        _chkVrsEyeTracked.CheckedChanged += (_, _) => { UpdateFoveationControls(); CheckPotatoMode(); };
        foreach (var rate in new[] { _cboVrsEyeInnerRate, _cboVrsEyeMidRate, _cboVrsEyeOuterRate })
            rate.SelectedIndexChanged += (_, _) => UpdateFoveationControls();
    }

    private EyeFoveationSettings CaptureEyeFoveationSettings() => new() {
        Enabled = _chkVrsEyeTracked.Checked, Backend = _cboFoveatedBackend.SelectedIndex,
        DebugRings = _chkFoveationDebugRings.Checked,
        Radii = new(_nudVrsEyeInnerRadius.Value, _nudVrsEyeMidRadius.Value),
        CustomRates = _chkVrsEyeCustomRates.Checked, Compatibility = _chkVrsEyeCompatibilityMode.Checked,
        FavorHorizontal = _chkVrsFavorHorizontal.Checked,
        InnerRate = _cboVrsEyeInnerRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultInnerRate,
        MidRate = _cboVrsEyeMidRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultMidRate,
        OuterRate = _cboVrsEyeOuterRate.SelectedItem?.ToString() ?? EyeFoveationSettings.DefaultOuterRate
    };

    private void ApplyEyeFoveationSettings(EyeFoveationSettings settings)
    {
        bool previous = _updatingVrsPreset;
        _updatingVrsPreset = true;
        try {
            _chkVrsEyeTracked.Checked = settings.Enabled;
            _cboFoveatedBackend.SelectedIndex = settings.Backend;
            _chkFoveationDebugRings.Checked = settings.DebugRings;
            _nudVrsEyeInnerRadius.Value = settings.Radii.Inner;
            _nudVrsEyeMidRadius.Value = settings.Radii.Mid;
            _chkVrsEyeCustomRates.Checked = settings.CustomRates;
            _chkVrsEyeCompatibilityMode.Checked = settings.Compatibility;
            _chkVrsFavorHorizontal.Checked = settings.FavorHorizontal;
            _cboVrsEyeInnerRate.SelectedItem = settings.InnerRate;
            _cboVrsEyeMidRate.SelectedItem = settings.MidRate;
            _cboVrsEyeOuterRate.SelectedItem = settings.OuterRate;
            _cboVrsEyePreset.SelectedIndex = settings.PresetIndex;
        } finally { _updatingVrsPreset = previous; }
        UpdateFoveationControls();
    }

    // The factory also allows the modal workflow to be tested without showing
    // windows or triggering the main form's installation/startup actions.
    internal System.Func<EyeFoveationSettings, EyeFoveationEditor> EyeEditorFactory = settings => new(settings);
    private EyeFoveationEditor CreateEyeFoveationEditor() => EyeEditorFactory(CaptureEyeFoveationSettings());
    private void OpenEyeFoveationEditor()
    {
        using var editor = CreateEyeFoveationEditor();
        editor.ShowDialog(this);
        ApplyEyeFoveationEditorResult(editor);
    }
    private void ApplyEyeFoveationEditorResult(EyeFoveationEditor editor)
    {
        if (editor.DialogResult == DialogResult.OK && editor.AcceptedSettings is { } settings) {
            ApplyEyeFoveationSettings(settings);
            _lblVideoStatus.Text = "Eye foveation updated. Save opencomposite.ini, then restart Skyrim.";
        }
    }
}
