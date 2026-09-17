using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// One visual system for the configurator. Keeping presentation here stops
    /// individual tabs from slowly drifting into different greys and greens.
    /// This class deliberately changes appearance only; it never reads or writes
    /// OCU settings and never changes control values or event handlers.
    /// </summary>
    internal static class ModernUiTheme
    {
        internal static readonly Color Window = Color.FromArgb(14, 17, 22);
        internal static readonly Color Surface = Color.FromArgb(22, 26, 33);
        internal static readonly Color SurfaceRaised = Color.FromArgb(27, 32, 40);
        internal static readonly Color SurfaceHover = Color.FromArgb(36, 42, 51);
        internal static readonly Color Input = Color.FromArgb(20, 24, 31);
        internal static readonly Color Border = Color.FromArgb(48, 56, 68);
        internal static readonly Color BorderSoft = Color.FromArgb(37, 43, 53);

        // This is the brighter green the configurator already used for its
        // primary Save/Apply actions, promoted to the single application accent.
        internal static readonly Color Accent = Color.FromArgb(40, 120, 40);
        internal static readonly Color AccentHover = Color.FromArgb(51, 145, 55);
        internal static readonly Color AccentPressed = Color.FromArgb(31, 96, 34);
        internal static readonly Color AccentSoft = Color.FromArgb(27, 67, 35);
        internal static readonly Color AccentText = Color.FromArgb(103, 211, 118);

        // Brighter interaction green shared by bound keyboard keys and every
        // controller-button overlay. This is intentionally separate from the
        // darker Save/Apply button green so tiny dots and key borders stay clear.
        internal static readonly Color KeyGlow = Color.FromArgb(62, 190, 143);
        internal static readonly Color KeyGlowHover = Color.FromArgb(95, 229, 176);
        internal static readonly Color KeyGlowBright = Color.FromArgb(132, 242, 158);
        internal static readonly Color KeyGlowFill = Color.FromArgb(61, 188, 144);

        internal static readonly Color TextPrimary = Color.FromArgb(237, 240, 245);
        internal static readonly Color TextSecondary = Color.FromArgb(181, 188, 199);
        internal static readonly Color TextMuted = Color.FromArgb(126, 136, 150);
        internal static readonly Color Danger = Color.FromArgb(137, 55, 55);
        internal static readonly Color DangerHover = Color.FromArgb(164, 66, 66);
        internal static readonly Color DangerPressed = Color.FromArgb(108, 43, 43);
        internal static readonly Color Warning = Color.FromArgb(194, 126, 45);

        private static readonly HashSet<Control> Styled = new();
        private static readonly HashSet<Control> DynamicContainers = new();
        private static readonly HashSet<Control> Rounded = new();

        internal static void Apply(Form form, bool windowBands = true)
        {
            form.SuspendLayout();
            form.BackColor = Window;
            form.ForeColor = TextPrimary;
            form.Font = new Font("Segoe UI", 9.5f, FontStyle.Regular);

            StyleTree(form);
            if (windowBands) InstallWindowBands(form);
            EnableDarkWindowChrome(form);

            form.ResumeLayout(performLayout: true);
            form.Invalidate(invalidateChildren: true);
        }

        internal static void StyleNavigationButton(Button button, bool selected)
        {
            button.FlatStyle = FlatStyle.Flat;
            button.UseVisualStyleBackColor = false;
            button.Cursor = Cursors.Hand;
            button.BackColor = selected ? Color.FromArgb(27, 65, 57) : Surface;
            button.ForeColor = selected ? TextPrimary : TextSecondary;
            button.Font = new Font("Segoe UI", 9.5f,
                selected ? FontStyle.Bold : FontStyle.Regular);
            button.FlatAppearance.BorderSize = selected ? 1 : 0;
            button.FlatAppearance.BorderColor = selected ? KeyGlowBright : Surface;
            button.FlatAppearance.MouseOverBackColor = selected ? Color.FromArgb(38, 92, 74) : SurfaceHover;
            button.FlatAppearance.MouseDownBackColor = selected
                ? Color.FromArgb(43, 151, 112)
                : SurfaceRaised;
            if (button is ModernPillButton pill)
            {
                pill.VisualRole = selected ? ModernButtonRole.Positive : ModernButtonRole.Neutral;
                pill.Invalidate();
            }
            else
            {
                Round(button, Math.Max(8, (button.Height - 2) / 2));
            }
        }

        private static void StyleTree(Control control)
        {
            if (Styled.Add(control))
                StyleControl(control);

            if (DynamicContainers.Add(control))
                control.ControlAdded += (_, e) =>
                {
                    if (e.Control != null)
                        StyleTree(e.Control);
                };

            foreach (Control child in control.Controls)
                StyleTree(child);
        }

        private static void StyleControl(Control control)
        {
            switch (control)
            {
                case Form form:
                    form.BackColor = Window;
                    form.ForeColor = TextPrimary;
                    break;

                case ModernKeyButton keyButton:
                    keyButton.ApplyTheme();
                    break;

                case Button button:
                    StyleButton(button);
                    break;

                case CheckBox checkBox:
                    checkBox.FlatStyle = FlatStyle.Flat;
                    checkBox.UseVisualStyleBackColor = false;
                    if (IsNeutral(checkBox.ForeColor))
                        checkBox.ForeColor = TextPrimary;
                    if (IsNeutral(checkBox.BackColor))
                        checkBox.BackColor = Color.Transparent;
                    checkBox.FlatAppearance.BorderColor = Border;
                    checkBox.FlatAppearance.MouseOverBackColor = SurfaceHover;
                    checkBox.FlatAppearance.CheckedBackColor = Accent;
                    break;

                case RadioButton radio:
                    radio.FlatStyle = FlatStyle.Flat;
                    radio.UseVisualStyleBackColor = false;
                    radio.ForeColor = TextPrimary;
                    if (IsNeutral(radio.BackColor))
                        radio.BackColor = Color.Transparent;
                    radio.FlatAppearance.BorderColor = Border;
                    radio.FlatAppearance.CheckedBackColor = Accent;
                    radio.FlatAppearance.MouseOverBackColor = SurfaceHover;
                    break;

                case ComboBox combo:
                    combo.FlatStyle = FlatStyle.Flat;
                    combo.BackColor = Input;
                    combo.ForeColor = TextPrimary;
                    ApplyNativeDarkTheme(combo);
                    break;

                case NumericUpDown numeric:
                    numeric.BorderStyle = BorderStyle.FixedSingle;
                    numeric.BackColor = Input;
                    numeric.ForeColor = numeric.Enabled ? TextPrimary : TextMuted;
                    numeric.EnabledChanged += (_, _) =>
                        numeric.ForeColor = numeric.Enabled ? TextPrimary : TextMuted;
                    ApplyNativeDarkTheme(numeric);
                    break;

                case TextBox textBox:
                    textBox.BorderStyle = BorderStyle.FixedSingle;
                    textBox.BackColor = Input;
                    textBox.ForeColor = TextPrimary;
                    ApplyNativeDarkTheme(textBox);
                    break;

                case ListBox listBox:
                    listBox.BorderStyle = BorderStyle.FixedSingle;
                    listBox.BackColor = Input;
                    listBox.ForeColor = TextPrimary;
                    ApplyNativeDarkTheme(listBox);
                    break;

                case LinkLabel link:
                    link.LinkColor = AccentText;
                    link.ActiveLinkColor = Color.FromArgb(132, 231, 145);
                    link.VisitedLinkColor = AccentText;
                    break;

                case GlowingBannerLabel banner:
                    banner.ApplyTheme();
                    break;

                case Label label:
                    StyleLabel(label);
                    break;

                case TrackBar trackBar:
                    trackBar.BackColor = control.Parent?.BackColor ?? Window;
                    trackBar.ForeColor = Accent;
                    break;

                case Panel panel:
                    StylePanel(panel);
                    break;
            }
        }

        private static void StyleButton(Button button)
        {
            Color original = button.BackColor;
            string text = button.Text.Trim();
            bool explicitlyPositive = StartsWithAny(text, "Save", "Apply", "OK", "Start");
            bool explicitlyDestructive = text.Equals("Off", StringComparison.OrdinalIgnoreCase)
                || ContainsAny(text, "Reset", "Remove", "Delete", "Restore");
            bool explicitlyNeutral = StartsWithAny(text, "Cancel", "Validate");
            bool positive = explicitlyPositive || (!explicitlyNeutral && IsPositive(original));
            bool destructive = !positive && !explicitlyNeutral
                && (explicitlyDestructive || IsDestructive(original));
            bool warning = !positive && !destructive && !explicitlyNeutral && IsWarning(original);

            button.FlatStyle = FlatStyle.Flat;
            button.UseVisualStyleBackColor = false;
            button.Cursor = Cursors.Hand;
            button.ForeColor = TextPrimary;
            button.Padding = new Padding(6, 0, 6, 0);

            if (positive)
            {
                button.BackColor = Color.FromArgb(27, 65, 57);
                button.FlatAppearance.BorderSize = 1;
                button.FlatAppearance.BorderColor = KeyGlow;
                button.FlatAppearance.MouseOverBackColor = Color.FromArgb(38, 92, 74);
                button.FlatAppearance.MouseDownBackColor = Color.FromArgb(33, 82, 66);
            }
            else if (destructive)
            {
                button.BackColor = Danger;
                button.FlatAppearance.BorderSize = 0;
                button.FlatAppearance.MouseOverBackColor = DangerHover;
                button.FlatAppearance.MouseDownBackColor = DangerPressed;
            }
            else if (warning)
            {
                button.BackColor = Color.FromArgb(112, 73, 30);
                button.FlatAppearance.BorderSize = 1;
                button.FlatAppearance.BorderColor = Warning;
                button.FlatAppearance.MouseOverBackColor = Color.FromArgb(137, 89, 35);
                button.FlatAppearance.MouseDownBackColor = Color.FromArgb(91, 59, 24);
            }
            else
            {
                button.BackColor = SurfaceRaised;
                button.FlatAppearance.BorderSize = 1;
                button.FlatAppearance.BorderColor = Border;
                button.FlatAppearance.MouseOverBackColor = SurfaceHover;
                button.FlatAppearance.MouseDownBackColor = Color.FromArgb(18, 22, 28);
            }

            if (!button.Enabled)
                button.ForeColor = TextMuted;
            button.EnabledChanged += (_, _) =>
                button.ForeColor = button.Enabled ? TextPrimary : TextMuted;

            if (button is ModernPillButton pill)
            {
                pill.VisualRole = positive ? ModernButtonRole.Positive
                    : destructive ? ModernButtonRole.Destructive
                    : warning ? ModernButtonRole.Warning
                    : ModernButtonRole.Neutral;
                pill.Invalidate();
            }
            else
            {
                int radius = positive || destructive
                    ? Math.Max(8, (button.Height - 2) / 2)
                    : button.Height <= 24 ? 6 : 8;
                Round(button, radius);
            }
        }

        private static void StyleLabel(Label label)
        {
            if (label.Text.StartsWith("OCU:", StringComparison.OrdinalIgnoreCase))
            {
                label.ForeColor = AccentText;
                label.Font = new Font("Segoe UI", 11.5f, FontStyle.Bold);
                return;
            }

            bool sectionHeading = label.Font.Bold && label.Font.Size >= 11f;
            bool oldGoldHeading = label.ForeColor.R >= 235
                && label.ForeColor.G >= 160
                && label.ForeColor.B <= 90;
            if (sectionHeading || oldGoldHeading)
            {
                label.ForeColor = AccentText;
                label.Font = new Font("Segoe UI", Math.Max(10.5f, label.Font.Size), FontStyle.Bold);
                return;
            }

            if (!IsNeutral(label.ForeColor))
                return; // Preserve semantic success, warning, error and device colours.

            int brightness = (label.ForeColor.R + label.ForeColor.G + label.ForeColor.B) / 3;
            label.ForeColor = brightness < 150 ? TextMuted
                : brightness < 215 ? TextSecondary
                : TextPrimary;
        }

        private static void StylePanel(Panel panel)
        {
            if (panel.Name.StartsWith("Modern", StringComparison.Ordinal))
                return;

            if (panel.Height <= 2)
            {
                panel.BackColor = BorderSoft;
                return;
            }

            if (!IsNeutral(panel.BackColor))
                return;

            bool rootTab = panel.Parent is Form && panel.Top >= 70;
            bool card = !rootTab && panel.Parent is Panel
                && panel.Width >= 240 && panel.Height >= 48;

            panel.BackColor = rootTab ? Window : card ? Surface : SurfaceRaised;
            if (card)
            {
                panel.Paint += (_, e) =>
                {
                    using var pen = new Pen(BorderSoft);
                    Rectangle rect = panel.ClientRectangle;
                    rect.Width -= 1;
                    rect.Height -= 1;
                    if (rect.Width > 0 && rect.Height > 0)
                        e.Graphics.DrawRectangle(pen, rect);
                };
            }
        }

        private static void InstallWindowBands(Form form)
        {
            var accentLine = new Panel
            {
                Name = "ModernAccentLine",
                Location = new Point(0, 0),
                Size = new Size(form.ClientSize.Width, 3),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
                BackColor = Accent,
                TabStop = false
            };
            var header = new Panel
            {
                Name = "ModernHeaderBand",
                Location = new Point(0, 3),
                Size = new Size(form.ClientSize.Width, 41),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
                BackColor = Surface,
                TabStop = false
            };
            var navigation = new Panel
            {
                Name = "ModernNavigationBand",
                Location = new Point(8, 48),
                Size = new Size(Math.Max(1, form.ClientSize.Width - 16), 38),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right,
                BackColor = Surface,
                TabStop = false
            };

            form.Controls.Add(accentLine);
            form.Controls.Add(header);
            form.Controls.Add(navigation);
            navigation.SendToBack();
            header.SendToBack();
            accentLine.BringToFront();

            foreach (Control control in form.Controls)
            {
                if (control is Label label && label.Top < 44)
                    label.BackColor = Surface;
            }
        }

        private static bool IsNeutral(Color color)
        {
            if (color == Color.Transparent || color.A == 0)
                return true;
            int high = Math.Max(color.R, Math.Max(color.G, color.B));
            int low = Math.Min(color.R, Math.Min(color.G, color.B));
            return high - low <= 18;
        }

        private static bool IsPositive(Color color) =>
            color.G >= 90 && color.G > color.R * 1.65 && color.G > color.B * 1.35;

        private static bool IsDestructive(Color color) =>
            color.R >= 70 && color.R > color.G * 1.45 && color.R > color.B * 1.20;

        private static bool IsWarning(Color color) =>
            color.R >= 80 && color.G >= 45 && color.R > color.G * 1.25
            && color.G > color.B * 1.30;

        private static bool StartsWithAny(string text, params string[] prefixes)
        {
            foreach (string prefix in prefixes)
            {
                if (text.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }

        private static bool ContainsAny(string text, params string[] fragments)
        {
            foreach (string fragment in fragments)
            {
                if (text.Contains(fragment, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }

        private static void Round(Control control, int radius)
        {
            void ApplyRegion()
            {
                if (control.Width <= 1 || control.Height <= 1 || control.IsDisposed)
                    return;
                using var path = RoundedRectangle(
                    new Rectangle(0, 0, control.Width, control.Height), radius);
                Region? old = control.Region;
                control.Region = new Region(path);
                old?.Dispose();
            }

            ApplyRegion();
            if (Rounded.Add(control))
                control.Resize += (_, _) => ApplyRegion();
        }

        private static GraphicsPath RoundedRectangle(Rectangle bounds, int radius)
        {
            int diameter = Math.Max(2, radius * 2);
            int right = bounds.Right - 1;
            int bottom = bounds.Bottom - 1;
            var path = new GraphicsPath();
            path.AddArc(bounds.Left, bounds.Top, diameter, diameter, 180, 90);
            path.AddArc(right - diameter, bounds.Top, diameter, diameter, 270, 90);
            path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0, 90);
            path.AddArc(bounds.Left, bottom - diameter, diameter, diameter, 90, 90);
            path.CloseFigure();
            return path;
        }

        private static void EnableDarkWindowChrome(Form form)
        {
            void ApplyChrome()
            {
                try
                {
                    int enabled = 1;
                    if (DwmSetWindowAttribute(form.Handle, 20, ref enabled, sizeof(int)) != 0)
                        DwmSetWindowAttribute(form.Handle, 19, ref enabled, sizeof(int));

                    // DWMWCP_ROUND: Windows 11 rounds the outer window while the
                    // OS keeps ownership of resizing, snapping and accessibility.
                    int rounded = 2;
                    DwmSetWindowAttribute(form.Handle, 33, ref rounded, sizeof(int));
                }
                catch
                {
                    // Older Windows versions simply retain their normal chrome.
                }
            }

            if (form.IsHandleCreated)
                ApplyChrome();
            else
                form.HandleCreated += (_, _) => ApplyChrome();
        }

        private static void ApplyNativeDarkTheme(Control control)
        {
            void ApplyTheme()
            {
                try
                {
                    SetWindowTheme(control.Handle, "DarkMode_Explorer", null);
                    foreach (Control child in control.Controls)
                        SetWindowTheme(child.Handle, "DarkMode_Explorer", null);
                }
                catch
                {
                    // Theme names vary by Windows build; our explicit colours
                    // remain in place if the native dark theme is unavailable.
                }
            }

            if (control.IsHandleCreated)
                ApplyTheme();
            else
                control.HandleCreated += (_, _) => ApplyTheme();
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(
            IntPtr hwnd, int attribute, ref int value, int valueSize);

        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        private static extern int SetWindowTheme(
            IntPtr hwnd, string? subAppName, string? subIdList);
    }
}
