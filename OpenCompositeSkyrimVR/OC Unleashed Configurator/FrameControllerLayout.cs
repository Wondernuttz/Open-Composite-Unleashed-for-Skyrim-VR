using System.Collections.Generic;
using System.Drawing;
using System.Reflection;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        private Image? _frameImage;

        private void LoadFrameImage()
        {
            using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("OpenCompositeConfigurator.Resources.frame.png");
            if (stream != null) _frameImage = Image.FromStream(stream);
        }

        // D-pad directions are buttons, not stick axes. Down/Up keep the legacy
        // left primary/secondary assignments. Every other Frame input is independent.
        private static readonly Dictionary<string, (string display, PointF pos, bool isStickDir)> ControllerButtonsFrame = new()
        {
            { "left_stick", ("L Stick Click", new(0.3325088f, 0.33628318f), false) },
            { "x_button", ("D-pad Down", new(0.156f, 0.36f), false) },
            { "y_button", ("D-pad Up", new(0.156f, 0.229f), false) },
            { "frame_dpad_left", ("D-pad Left", new(0.102f, 0.296f), false) },
            { "frame_dpad_right", ("D-pad Right", new(0.212f, 0.296f), false) },
            { "frame_view", ("View", new(0.257f, 0.196f), false) },
            { "l_trigger", ("L Trigger", new(0.43203256f, 0.16224189f), false) },
            { "frame_l_bumper", ("L Bumper", new(0.31308952f, 0.050147492f), false) },
            { "l_grip", ("L Grip", new(0.351f, 0.611f), false) },
            { "right_stick", ("R Stick Click", new(0.6626364f, 0.3421829f), false) },
            { "a_button", ("A Button", new(0.848f, 0.366f), false) },
            { "b_button", ("B Button", new(0.904f, 0.296f), false) },
            { "frame_x", ("X Button", new(0.791f, 0.296f), false) },
            { "frame_y", ("Y Button", new(0.848f, 0.23f), false) },
            { "frame_menu", ("Menu", new(0.747f, 0.199f), false) },
            { "r_trigger", ("R Trigger", new(0.5728223f, 0.16519174f), false) },
            { "frame_r_bumper", ("R Bumper", new(0.6917653f, 0.056047197f), false) },
            { "r_grip", ("R Grip", new(0.644f, 0.611f), false) },
            { "left_stick_up", ("L Stick Up", new(0.3373636f, 0.25368732f), true) },
            { "left_stick_down", ("L Stick Down", new(0.33979103f, 0.43067846f), true) },
            { "left_stick_left", ("L Stick Left", new(0.26454133f, 0.3480826f), true) },
            { "left_stick_right", ("L Stick Right", new(0.40047625f, 0.34513274f), true) },
            { "right_stick_up", ("R Stick Up", new(0.6650638f, 0.27433628f), true) },
            { "right_stick_down", ("R Stick Down", new(0.6650638f, 0.4218289f), true) },
            { "right_stick_left", ("R Stick Left", new(0.5970964f, 0.34513274f), true) },
            { "right_stick_right", ("R Stick Right", new(0.7306038f, 0.3421829f), true) },
        };

        // Must match LegacyControllerActions::FrameButtonIds (5, 6, 3, 35).
        private static readonly Dictionary<string, (string hexRight, string hexLeft)> FrameButtonHex = new()
        {
            { "frame_dpad_left", ("", "0x05") }, { "frame_dpad_right", ("", "0x06") },
            { "frame_x", ("0x05", "") }, { "frame_y", ("0x06", "") },
            { "frame_l_bumper", ("", "0x03") }, { "frame_r_bumper", ("0x03", "") },
            { "frame_view", ("", "0x23") }, { "frame_menu", ("0x23", "") },
        };

        private void PaintFrameRearLabels(Graphics g, float drawW, float drawH, float offX, float offY)
        {
            if (_controllerModelKey != "frame") return;
            using var font = new Font("Segoe UI", 8.5f);
            using var brush = new SolidBrush(ModernUiTheme.TextMuted);
            using var format = new StringFormat { Alignment = StringAlignment.Center };
            // The supplied front view hides the rear triggers/bumpers; label their
            // callout dots explicitly instead of pretending they're face buttons.
            foreach (var id in new[] { "frame_l_bumper", "l_trigger", "frame_r_bumper", "r_trigger" })
            {
                var entry = _activeControllerButtons[id];
                g.DrawString(entry.display, font, brush, offX + entry.pos.X * drawW,
                    offY + entry.pos.Y * drawH + DotRadiusFor(id) + 3, format);
            }
        }
    }
}
