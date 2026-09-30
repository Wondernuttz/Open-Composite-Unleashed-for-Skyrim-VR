using System.Drawing;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

internal sealed class IndexSpellWheelHelpDialog : Form
{
    internal IndexSpellWheelHelpDialog()
    {
        Text = "Spell Wheel / Index setup";
        Font = new Font("Segoe UI", 9.5f);
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        ShowInTaskbar = false;

        var content = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 1,
            Dock = DockStyle.Fill,
            Padding = new Padding(22)
        };
        Controls.Add(content);

        void AddText(string text, bool heading = false)
        {
            content.Controls.Add(new Label
            {
                Text = text,
                AutoSize = true,
                MaximumSize = new Size(540, 0),
                Margin = new Padding(0, 0, 0, 12),
                Font = new Font("Segoe UI", heading ? 11f : 9.5f,
                    heading ? FontStyle.Bold : FontStyle.Regular),
                ForeColor = heading ? ModernUiTheme.TextPrimary : ModernUiTheme.TextSecondary
            });
        }

        AddText("Open Spell Wheel with an Index trackpad press", heading: true);
        AddText("Optional setup for versions of Spell Wheel that offer the exact MCM option below (verified in 1.5.11). This help changes no settings.");
        AddText("1. In OCU", heading: true);
        AddText("Enable 'Use trackpad press for VRIK gestures', then Save settings and restart Skyrim VR. Having VRIK installed alone does not require this option.");
        AddText("2. In MCM > Spell Wheel > General", heading: true);
        AddText("For each desired hand, under Main Hand Buttons or Secondary Hand Buttons:\n" +
            "Button: VRIK Index Touchpad Press\n" +
            "Button Combination: -Empty- (unless you want a modifier).");
        AddText("Press the physical pad to open the wheel; simply touching it is not enough. This pairing avoids using face-button touch to open the wheel.");
        AddText("This OCU mode takes over upper/lower trackpad assignments. Turn it off to use those assignments again; your saved assignments are retained. HIGGS grip touch works independently.");

        var close = new ModernPillButton
        {
            Text = "Close",
            Size = new Size(100, 32),
            Anchor = AnchorStyles.Right,
            Margin = Padding.Empty,
            DialogResult = DialogResult.OK
        };
        content.Controls.Add(close);
        AcceptButton = close;
        CancelButton = close;
        ModernUiTheme.Apply(this, windowBands: false);
        // Measure wrapped help text before fixing its scrollable content size.
        AutoSize = false;
        ClientSize = new Size(584, content.GetPreferredSize(new Size(584, 0)).Height);
        DpiLayout.Popup(this);
    }
}
