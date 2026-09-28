using System.Drawing;
using System.Drawing.Text;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

// WinForms' disabled GDI+ label paints an embossed system-colour shadow.
// On our dark panels that becomes black text; draw a single muted-grey pass.
internal sealed class ModernLabel : Label
{
    protected override void OnPaint(PaintEventArgs e)
    {
        if (Enabled)
        {
            base.OnPaint(e);
            return;
        }

        var bounds = ClientRectangle;
        bounds.X += Padding.Left;
        bounds.Y += Padding.Top;
        bounds.Width -= Padding.Horizontal;
        bounds.Height -= Padding.Vertical;
        if (bounds.Width <= 0 || bounds.Height <= 0) return;

        using var format = new StringFormat
        {
            Alignment = TextAlign is ContentAlignment.TopCenter or ContentAlignment.MiddleCenter or ContentAlignment.BottomCenter
                ? StringAlignment.Center
                : TextAlign is ContentAlignment.TopRight or ContentAlignment.MiddleRight or ContentAlignment.BottomRight
                    ? StringAlignment.Far : StringAlignment.Near,
            LineAlignment = TextAlign is ContentAlignment.MiddleLeft or ContentAlignment.MiddleCenter or ContentAlignment.MiddleRight
                ? StringAlignment.Center
                : TextAlign is ContentAlignment.BottomLeft or ContentAlignment.BottomCenter or ContentAlignment.BottomRight
                    ? StringAlignment.Far : StringAlignment.Near,
            HotkeyPrefix = !UseMnemonic ? HotkeyPrefix.None : ShowKeyboardCues ? HotkeyPrefix.Show : HotkeyPrefix.Hide,
            Trimming = AutoEllipsis ? StringTrimming.EllipsisCharacter : StringTrimming.Character
        };
        if (RightToLeft == RightToLeft.Yes) format.FormatFlags |= StringFormatFlags.DirectionRightToLeft;
        using var brush = new SolidBrush(ModernUiTheme.TextMuted);
        e.Graphics.DrawString(Text, Font, brush, bounds, format);
    }
}
