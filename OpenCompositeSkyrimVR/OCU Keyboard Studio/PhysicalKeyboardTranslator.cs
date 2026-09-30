using System.Runtime.InteropServices;
using System.Text;

namespace OCUKeyboardStudio;

internal readonly record struct PhysicalKeyAssignment(char Normal, char Shifted);

internal static class PhysicalKeyboardTranslator
{
    internal static bool TryTranslate(Keys key, out PhysicalKeyAssignment assignment)
    {
        assignment = key switch
        {
            Keys.Tab => new('\t', '\t'),
            Keys.Enter => new('\n', '\n'),
            Keys.Back => new('\b', '\b'),
            Keys.Space => new(' ', ' '),
            Keys.Escape => new('\x0E', '\x0E'),
            Keys.End => new('\x1D', '\x1D'),
            Keys.ControlKey or Keys.LControlKey or Keys.RControlKey => new('\x1E', '\x1E'),
            Keys.ShiftKey or Keys.LShiftKey or Keys.RShiftKey => new('\x01', '\x01'),
            Keys.CapsLock => new('\x02', '\x02'),
            Keys.PrintScreen => new('\x1F', '\x1F'),
            Keys.Up => new('\x04', '\x04'),
            Keys.Down => new('\x05', '\x05'),
            Keys.Left => new('\x06', '\x06'),
            Keys.Right => new('\x07', '\x07'),
            >= Keys.F1 and <= Keys.F12 => new((char)('\x10' + key - Keys.F1), (char)('\x10' + key - Keys.F1)),
            _ => default
        };
        if (assignment != default)
            return true;

        char? normal = TranslatePrintable(key, shifted: false);
        char? shifted = TranslatePrintable(key, shifted: true);
        if (normal is null && shifted is null)
            return false;
        char resolvedNormal = normal ?? shifted.GetValueOrDefault();
        char resolvedShifted = shifted ?? normal.GetValueOrDefault();
        assignment = new PhysicalKeyAssignment(resolvedNormal, resolvedShifted);
        return true;
    }

    private static char? TranslatePrintable(Keys key, bool shifted)
    {
        IntPtr layout = GetKeyboardLayout(0);
        uint virtualKey = (uint)key;
        uint scanCode = MapVirtualKeyEx(virtualKey, 0, layout);
        var keyboardState = new byte[256];
        if (shifted)
            keyboardState[(int)Keys.ShiftKey] = 0x80;
        var result = new StringBuilder(8);
        int count = ToUnicodeEx(virtualKey, scanCode, keyboardState, result, result.Capacity, 4, layout);
        if (count <= 0 || result.Length == 0 || char.IsControl(result[0]))
            return null;
        return result[0];
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetKeyboardLayout(uint threadId);

    [DllImport("user32.dll")]
    private static extern uint MapVirtualKeyEx(uint code, uint mapType, IntPtr keyboardLayout);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int ToUnicodeEx(uint virtualKey, uint scanCode, byte[] keyboardState,
        [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder buffer, int bufferSize,
        uint flags, IntPtr keyboardLayout);
}
