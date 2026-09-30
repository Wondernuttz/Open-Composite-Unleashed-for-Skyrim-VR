using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace OCUKeyboardStudio;

internal sealed class PixelSurface
{
    public int Width { get; }
    public int Height { get; }
    public byte[] Bgra { get; }

    public PixelSurface(int width, int height)
    {
        Width = width;
        Height = height;
        Bgra = new byte[width * height * 4];
    }

    public static PixelSurface FromBitmap(Bitmap source)
    {
        using var converted = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb);
        using (Graphics graphics = Graphics.FromImage(converted))
        {
            graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
            graphics.DrawImage(source,
                new Rectangle(0, 0, converted.Width, converted.Height),
                new Rectangle(0, 0, source.Width, source.Height),
                GraphicsUnit.Pixel);
        }

        var result = new PixelSurface(converted.Width, converted.Height);
        BitmapData data = converted.LockBits(
            new Rectangle(0, 0, converted.Width, converted.Height),
            ImageLockMode.ReadOnly,
            PixelFormat.Format32bppArgb);
        try
        {
            for (int y = 0; y < converted.Height; y++)
                Marshal.Copy(data.Scan0 + y * data.Stride, result.Bgra, y * converted.Width * 4, converted.Width * 4);
        }
        finally
        {
            converted.UnlockBits(data);
        }
        return result;
    }

    public Bitmap ToBitmap()
    {
        var bitmap = new Bitmap(Width, Height, PixelFormat.Format32bppArgb);
        BitmapData data = bitmap.LockBits(
            new Rectangle(0, 0, Width, Height),
            ImageLockMode.WriteOnly,
            PixelFormat.Format32bppArgb);
        try
        {
            for (int y = 0; y < Height; y++)
                Marshal.Copy(Bgra, y * Width * 4, data.Scan0 + y * data.Stride, Width * 4);
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
        return bitmap;
    }

    public void BlendPixel(int x, int y, Color color, byte coverage = 255)
    {
        if ((uint)x >= (uint)Width || (uint)y >= (uint)Height || coverage == 0 || color.A == 0)
            return;

        int index = (y * Width + x) * 4;
        int sourceAlpha = (color.A * coverage + 127) / 255;
        if (sourceAlpha == 0)
            return;
        if (sourceAlpha == 255)
        {
            Bgra[index] = color.B;
            Bgra[index + 1] = color.G;
            Bgra[index + 2] = color.R;
            Bgra[index + 3] = 255;
            return;
        }

        int inverse = 255 - sourceAlpha;
        int destinationAlpha = (Bgra[index + 3] * inverse + 127) / 255;
        int outputAlpha = sourceAlpha + destinationAlpha;
        if (outputAlpha == 0)
            return;

        static byte BlendChannel(byte source, byte destination, int sourceAlpha, int destinationAlpha, int outputAlpha)
            => (byte)((source * sourceAlpha + destination * destinationAlpha + outputAlpha / 2) / outputAlpha);

        Bgra[index] = BlendChannel(color.B, Bgra[index], sourceAlpha, destinationAlpha, outputAlpha);
        Bgra[index + 1] = BlendChannel(color.G, Bgra[index + 1], sourceAlpha, destinationAlpha, outputAlpha);
        Bgra[index + 2] = BlendChannel(color.R, Bgra[index + 2], sourceAlpha, destinationAlpha, outputAlpha);
        Bgra[index + 3] = (byte)outputAlpha;
    }

    public void FillRounded(Rectangle rectangle, int radius, Color color)
    {
        if (rectangle.Width <= 0 || rectangle.Height <= 0 || color.A == 0)
            return;
        radius = Math.Clamp(radius, 0, Math.Min(rectangle.Width, rectangle.Height) / 2);

        for (int y = rectangle.Top; y < rectangle.Bottom; y++)
        {
            for (int x = rectangle.Left; x < rectangle.Right; x++)
            {
                if (IsInsideRounded(rectangle, radius, x, y))
                    BlendPixel(x, y, color);
            }
        }
    }

    public void StrokeRounded(Rectangle rectangle, int radius, int width, Color color)
    {
        if (rectangle.Width <= 0 || rectangle.Height <= 0 || width <= 0 || color.A == 0)
            return;

        radius = Math.Clamp(radius, 0, Math.Min(rectangle.Width, rectangle.Height) / 2);
        width = Math.Clamp(width, 1, Math.Max(1, Math.Min(rectangle.Width, rectangle.Height) / 2));
        Rectangle inner = Rectangle.Inflate(rectangle, -width, -width);
        int innerRadius = Math.Max(0, radius - width);

        for (int y = rectangle.Top; y < rectangle.Bottom; y++)
        {
            for (int x = rectangle.Left; x < rectangle.Right; x++)
            {
                if (!IsInsideRounded(rectangle, radius, x, y))
                    continue;
                if (inner.Width > 0 && inner.Height > 0 && IsInsideRounded(inner, innerRadius, x, y))
                    continue;
                BlendPixel(x, y, color);
            }
        }
    }

    private static bool IsInsideRounded(Rectangle rectangle, int radius, int x, int y)
    {
        if (x < rectangle.Left || y < rectangle.Top || x >= rectangle.Right || y >= rectangle.Bottom)
            return false;
        radius = Math.Clamp(radius, 0, Math.Min(rectangle.Width, rectangle.Height) / 2);
        int left = rectangle.Left + radius;
        int right = rectangle.Right - radius - 1;
        int top = rectangle.Top + radius;
        int bottom = rectangle.Bottom - radius - 1;
        int dx = x < left ? left - x : x > right ? x - right : 0;
        int dy = y < top ? top - y : y > bottom ? y - bottom : 0;
        return dx * dx + dy * dy <= radius * radius;
    }

    public void BlendSurface(PixelSurface source, int destinationX, int destinationY, int opacityPercent = 100)
    {
        opacityPercent = Math.Clamp(opacityPercent, 0, 100);
        if (opacityPercent == 0)
            return;
        for (int y = 0; y < source.Height; y++)
        {
            int targetY = destinationY + y;
            if ((uint)targetY >= (uint)Height)
                continue;
            for (int x = 0; x < source.Width; x++)
            {
                int targetX = destinationX + x;
                if ((uint)targetX >= (uint)Width)
                    continue;
                int sourceIndex = (y * source.Width + x) * 4;
                int sourceAlpha = (source.Bgra[sourceIndex + 3] * opacityPercent + 50) / 100;
                if (sourceAlpha == 0)
                    continue;
                int targetIndex = (targetY * Width + targetX) * 4;
                int inverse = 255 - sourceAlpha;
                int destinationAlpha = (Bgra[targetIndex + 3] * inverse + 127) / 255;
                int outputAlpha = sourceAlpha + destinationAlpha;
                if (outputAlpha == 0)
                    continue;
                Bgra[targetIndex] = (byte)((source.Bgra[sourceIndex] * sourceAlpha
                    + Bgra[targetIndex] * destinationAlpha + outputAlpha / 2) / outputAlpha);
                Bgra[targetIndex + 1] = (byte)((source.Bgra[sourceIndex + 1] * sourceAlpha
                    + Bgra[targetIndex + 1] * destinationAlpha + outputAlpha / 2) / outputAlpha);
                Bgra[targetIndex + 2] = (byte)((source.Bgra[sourceIndex + 2] * sourceAlpha
                    + Bgra[targetIndex + 2] * destinationAlpha + outputAlpha / 2) / outputAlpha);
                Bgra[targetIndex + 3] = (byte)outputAlpha;
            }
        }
    }

    public void BlendBitmap(Bitmap source, Rectangle destination, int opacityPercent = 100, int edgeFadePixels = 0, float rotationDegrees = 0)
    {
        if (destination.Width <= 0 || destination.Height <= 0 || opacityPercent <= 0)
            return;
        opacityPercent = Math.Clamp(opacityPercent, 0, 100);
        edgeFadePixels = Math.Clamp(edgeFadePixels, 0, 300);
        using var converted = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb);
        using (Graphics graphics = Graphics.FromImage(converted))
        {
            graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
            graphics.DrawImage(source,
                new Rectangle(0, 0, converted.Width, converted.Height),
                new Rectangle(0, 0, source.Width, source.Height),
                GraphicsUnit.Pixel);
        }

        byte[] sourcePixels = new byte[converted.Width * converted.Height * 4];
        BitmapData data = converted.LockBits(
            new Rectangle(0, 0, converted.Width, converted.Height),
            ImageLockMode.ReadOnly,
            PixelFormat.Format32bppArgb);
        try
        {
            for (int y = 0; y < converted.Height; y++)
                Marshal.Copy(data.Scan0 + y * data.Stride, sourcePixels, y * converted.Width * 4, converted.Width * 4);
        }
        finally
        {
            converted.UnlockBits(data);
        }

        float radians = -rotationDegrees * MathF.PI / 180f;
        float cosine = MathF.Cos(radians);
        float sine = MathF.Sin(radians);
        float centerX = destination.Width / 2f;
        float centerY = destination.Height / 2f;
        for (int y = 0; y < destination.Height; y++)
        {
            for (int x = 0; x < destination.Width; x++)
            {
                float dx = x + 0.5f - centerX;
                float dy = y + 0.5f - centerY;
                float localX = cosine * dx - sine * dy + centerX;
                float localY = sine * dx + cosine * dy + centerY;
                if (localX < 0 || localY < 0 || localX >= destination.Width || localY >= destination.Height)
                    continue;
                float sampleX = localX * converted.Width / destination.Width - 0.5f;
                float sampleY = localY * converted.Height / destination.Height - 0.5f;
                int floorX = (int)MathF.Floor(sampleX);
                int floorY = (int)MathF.Floor(sampleY);
                int x0 = Math.Clamp(floorX, 0, converted.Width - 1);
                int y0 = Math.Clamp(floorY, 0, converted.Height - 1);
                int x1 = Math.Clamp(floorX + 1, 0, converted.Width - 1);
                int y1 = Math.Clamp(floorY + 1, 0, converted.Height - 1);
                float fx = Math.Clamp(sampleX - floorX, 0f, 1f);
                float fy = Math.Clamp(sampleY - floorY, 0f, 1f);
                float w00 = (1f - fx) * (1f - fy);
                float w10 = fx * (1f - fy);
                float w01 = (1f - fx) * fy;
                float w11 = fx * fy;
                int i00 = (y0 * converted.Width + x0) * 4;
                int i10 = (y0 * converted.Width + x1) * 4;
                int i01 = (y1 * converted.Width + x0) * 4;
                int i11 = (y1 * converted.Width + x1) * 4;
                float a00 = sourcePixels[i00 + 3] / 255f;
                float a10 = sourcePixels[i10 + 3] / 255f;
                float a01 = sourcePixels[i01 + 3] / 255f;
                float a11 = sourcePixels[i11 + 3] / 255f;
                float aw00 = a00 * w00;
                float aw10 = a10 * w10;
                float aw01 = a01 * w01;
                float aw11 = a11 * w11;
                float alphaSum = aw00 + aw10 + aw01 + aw11;
                float bluePremultiplied = sourcePixels[i00] * aw00 + sourcePixels[i10] * aw10
                    + sourcePixels[i01] * aw01 + sourcePixels[i11] * aw11;
                float greenPremultiplied = sourcePixels[i00 + 1] * aw00 + sourcePixels[i10 + 1] * aw10
                    + sourcePixels[i01 + 1] * aw01 + sourcePixels[i11 + 1] * aw11;
                float redPremultiplied = sourcePixels[i00 + 2] * aw00 + sourcePixels[i10 + 2] * aw10
                    + sourcePixels[i01 + 2] * aw01 + sourcePixels[i11 + 2] * aw11;
                float edgeFactor = 1f;
                if (edgeFadePixels > 0)
                {
                    int edgeDistance = (int)MathF.Min(MathF.Min(localX, destination.Width - 1 - localX), MathF.Min(localY, destination.Height - 1 - localY));
                    edgeFactor = Math.Clamp(edgeDistance / (float)edgeFadePixels, 0f, 1f);
                }
                byte alpha = (byte)Math.Clamp(alphaSum * 255f * opacityPercent / 100f * edgeFactor, 0f, 255f);
                if (alpha == 0 || alphaSum <= 0.0001f)
                    continue;
                BlendPixel(destination.Left + x, destination.Top + y,
                    Color.FromArgb(alpha,
                        (int)Math.Clamp(redPremultiplied / alphaSum, 0f, 255f),
                        (int)Math.Clamp(greenPremultiplied / alphaSum, 0f, 255f),
                        (int)Math.Clamp(bluePremultiplied / alphaSum, 0f, 255f)));
            }
        }
    }
}

internal sealed record FontGlyphRaster(
    char Character,
    short Width,
    short Height,
    short XOffset,
    short YOffset,
    short XAdvance,
    byte[] Alpha);

internal sealed class SudoFont
{
    internal readonly record struct Glyph(
        char Character,
        short PackedX,
        short PackedY,
        short PackedWidth,
        short PackedHeight,
        short XOffset,
        short YOffset,
        short XAdvance);

    private readonly Dictionary<char, Glyph> _glyphs = [];
    private readonly byte[] _atlasAlpha;
    private readonly int _atlasWidth;
    private readonly int _atlasHeight;

    public int LineHeight { get; }
    public string MetadataPath { get; }
    public string TexturePath { get; }

    public SudoFont(string metadataPath, string texturePath)
    {
        MetadataPath = metadataPath;
        TexturePath = texturePath;

        using var stream = File.OpenRead(metadataPath);
        using var reader = new BinaryReader(stream);
        byte[] expected = [0x0B, .. System.Text.Encoding.ASCII.GetBytes("SudoFont1.1")];
        byte[] actual = reader.ReadBytes(expected.Length);
        if (!actual.SequenceEqual(expected))
            throw new InvalidDataException($"'{Path.GetFileName(metadataPath)}' is not a SudoFont 1.1 metadata file.");

        int originalWidth = 0;
        int originalHeight = 0;
        while (reader.BaseStream.Position < reader.BaseStream.Length)
        {
            ushort sectionId = reader.ReadUInt16();
            if (sectionId == 999)
                break;
            uint sectionSize = reader.ReadUInt32();
            long sectionEnd = reader.BaseStream.Position + sectionSize;

            if (sectionId == 0)
            {
                LineHeight = reader.ReadUInt16();
                originalWidth = reader.ReadUInt16();
                originalHeight = reader.ReadUInt16();
            }
            else if (sectionId == 1)
            {
                ushort count = reader.ReadUInt16();
                for (int index = 0; index < count; index++)
                {
                    var glyph = new Glyph(
                        (char)reader.ReadUInt16(),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()),
                        unchecked((short)reader.ReadUInt16()));
                    _glyphs[glyph.Character] = glyph;
                }
            }
            reader.BaseStream.Position = sectionEnd;
        }

        using var atlas = new Bitmap(texturePath);
        _atlasWidth = atlas.Width;
        _atlasHeight = atlas.Height;
        if (originalWidth != 0 && (originalWidth != atlas.Width || originalHeight != atlas.Height))
            throw new InvalidDataException("SudoFont metadata and texture dimensions do not match.");
        _atlasAlpha = ReadAlpha(atlas);
    }

    private int Advance(char character)
    {
        if (_glyphs.TryGetValue(character, out Glyph glyph) && glyph.XAdvance > 0)
            return glyph.XAdvance;
        // OCU's bundled SFN atlases intentionally carry no painted glyph for a
        // blank. Keep word spacing and caret placement even when U+0020 is not
        // present in the atlas; this is a renderer fallback, not a file-format
        // addition, so existing .kb and .ocukb designs remain unchanged.
        return character == ' ' ? Math.Max(4, LineHeight / 3) : 0;
    }

    public int Width(string text) => text.Sum(Advance);

    public bool ContainsGlyph(char character) => _glyphs.ContainsKey(character);

    public IReadOnlyCollection<char> Characters => _glyphs.Keys;

    public bool TryGetGlyphRaster(char character, out FontGlyphRaster raster)
    {
        if (!_glyphs.TryGetValue(character, out Glyph glyph))
        {
            raster = null!;
            return false;
        }
        int width = Math.Max(0, (int)glyph.PackedWidth);
        int height = Math.Max(0, (int)glyph.PackedHeight);
        byte[] alpha = new byte[width * height];
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                alpha[y * width + x] = AtlasAlpha(glyph.PackedX + x, glyph.PackedY + y);
        raster = new FontGlyphRaster(character, glyph.PackedWidth, glyph.PackedHeight,
            glyph.XOffset, glyph.YOffset, glyph.XAdvance, alpha);
        return true;
    }

    public RectangleF VisibleTextRectangle(string text, RectangleF box, float offsetX, float offsetY, float scale)
    {
        if (!TryMeasureVisible(text, out int minX, out int minY, out int maxX, out int maxY))
            return RectangleF.Empty;
        scale = Math.Clamp(scale, 0.25f, 3f);
        float width = (maxX - minX) * scale;
        float height = (maxY - minY) * scale;
        return new RectangleF(
            box.Left + (box.Width - width) * 0.5f + offsetX,
            box.Top + (box.Height - height) * 0.5f + offsetY,
            width, height);
    }

    public bool HitTestText(string text, RectangleF box, float offsetX, float offsetY, float scale, PointF point)
    {
        if (string.IsNullOrEmpty(text) || !TryMeasureVisible(text, out int minX, out int minY, out int maxX, out int maxY))
            return false;
        scale = Math.Clamp(scale, 0.25f, 3f);
        float visualWidth = (maxX - minX) * scale;
        float visualHeight = (maxY - minY) * scale;
        float originX = box.Left + (box.Width - visualWidth) * 0.5f + offsetX - minX * scale;
        float originY = box.Top + (box.Height - visualHeight) * 0.5f + offsetY - minY * scale;
        int cursor = 0;
        foreach (char character in text)
        {
            if (!_glyphs.TryGetValue(character, out Glyph glyph))
            {
                cursor += Advance(character);
                continue;
            }
            float glyphLeft = originX + (cursor + glyph.XOffset) * scale;
            float glyphTop = originY + glyph.YOffset * scale;
            if (point.X >= glyphLeft && point.X < glyphLeft + glyph.PackedWidth * scale
                && point.Y >= glyphTop && point.Y < glyphTop + glyph.PackedHeight * scale)
            {
                float sourceX = (point.X - glyphLeft) / scale;
                float sourceY = (point.Y - glyphTop) / scale;
                if (SampleAlpha(glyph, sourceX, sourceY) >= 48)
                    return true;
            }
            cursor += Advance(character);
        }
        return false;
    }

    private bool TryMeasureVisible(string text, out int minX, out int minY, out int maxX, out int maxY)
    {
        minX = int.MaxValue;
        minY = int.MaxValue;
        maxX = int.MinValue;
        maxY = int.MinValue;
        int cursor = 0;
        foreach (char character in text)
        {
            if (!_glyphs.TryGetValue(character, out Glyph glyph))
            {
                cursor += Advance(character);
                continue;
            }
            if (glyph.PackedWidth > 0 && glyph.PackedHeight > 0)
            {
                minX = Math.Min(minX, cursor + glyph.XOffset);
                minY = Math.Min(minY, glyph.YOffset);
                maxX = Math.Max(maxX, cursor + glyph.XOffset + glyph.PackedWidth);
                maxY = Math.Max(maxY, glyph.YOffset + glyph.PackedHeight);
            }
            cursor += Advance(character);
        }
        return minX != int.MaxValue;
    }

    public void DrawTextCentered(PixelSurface surface, string text, Rectangle box, float offsetX, float offsetY,
        float scale, Color color, bool outline)
        => DrawTextCentered(surface, text, box, offsetX, offsetY, scale, color,
            outline ? Color.FromArgb(220, 8, 11, 15) : null, null, 0);

    public void DrawTextCentered(PixelSurface surface, string text, Rectangle box, float offsetX, float offsetY,
        float scale, Color color, Color? outlineColor, Color? glowColor, int glowRadius)
    {
        if (string.IsNullOrEmpty(text))
            return;
        scale = Math.Clamp(scale, 0.25f, 3f);

        var runs = new List<(Glyph Glyph, int Cursor)>();
        int cursor = 0;
        int minX = int.MaxValue;
        int minY = int.MaxValue;
        int maxX = int.MinValue;
        int maxY = int.MinValue;
        foreach (char character in text)
        {
            if (!_glyphs.TryGetValue(character, out Glyph glyph))
            {
                cursor += Advance(character);
                continue;
            }
            if (glyph.PackedWidth > 0 && glyph.PackedHeight > 0)
            {
                runs.Add((glyph, cursor));
                minX = Math.Min(minX, cursor + glyph.XOffset);
                minY = Math.Min(minY, glyph.YOffset);
                maxX = Math.Max(maxX, cursor + glyph.XOffset + glyph.PackedWidth);
                maxY = Math.Max(maxY, glyph.YOffset + glyph.PackedHeight);
            }
            cursor += Advance(character);
        }
        if (runs.Count == 0)
            return;

        float visualWidth = (maxX - minX) * scale;
        float visualHeight = (maxY - minY) * scale;
        float originX = box.Left + (box.Width - visualWidth) * 0.5f + offsetX - minX * scale;
        float originY = box.Top + (box.Height - visualHeight) * 0.5f + offsetY - minY * scale;

        if (glowColor is Color glow && glow.A > 0 && glowRadius > 0)
        {
            int outerRadius = Math.Clamp(glowRadius, 1, 8);
            int innerRadius = Math.Max(1, outerRadius / 2);
            DrawGlowRing(surface, runs, originX, originY, scale, glow, outerRadius,
                innerRadius == outerRadius ? 1f : 0.55f);
            if (innerRadius != outerRadius)
                DrawGlowRing(surface, runs, originX, originY, scale, glow, innerRadius, 0.82f);
        }

        if (outlineColor is Color outline && outline.A > 0)
        {
            (int X, int Y)[] stamps = [(-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, -1), (-1, 1), (1, 1)];
            foreach ((int x, int y) in stamps)
                DrawRuns(surface, runs, originX + x, originY + y, scale, outline);
        }
        DrawRuns(surface, runs, originX, originY, scale, color);
    }

    public void DrawTextAt(PixelSurface surface, string text, float originX, float originY,
        float scale, Color color, Color? outlineColor, Color? glowColor, int glowRadius)
    {
        if (string.IsNullOrEmpty(text))
            return;
        scale = Math.Clamp(scale, 0.25f, 3f);

        var runs = new List<(Glyph Glyph, int Cursor)>();
        int cursor = 0;
        foreach (char character in text)
        {
            if (_glyphs.TryGetValue(character, out Glyph glyph)
                && glyph.PackedWidth > 0 && glyph.PackedHeight > 0)
                runs.Add((glyph, cursor));
            cursor += Advance(character);
        }
        if (runs.Count == 0)
            return;

        if (glowColor is Color glow && glow.A > 0 && glowRadius > 0)
        {
            int outerRadius = Math.Clamp(glowRadius, 1, 8);
            int innerRadius = Math.Max(1, outerRadius / 2);
            DrawGlowRing(surface, runs, originX, originY, scale, glow, outerRadius,
                innerRadius == outerRadius ? 1f : 0.55f);
            if (innerRadius != outerRadius)
                DrawGlowRing(surface, runs, originX, originY, scale, glow, innerRadius, 0.82f);
        }
        if (outlineColor is Color outline && outline.A > 0)
        {
            (int X, int Y)[] stamps = [(-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, -1), (-1, 1), (1, 1)];
            foreach ((int x, int y) in stamps)
                DrawRuns(surface, runs, originX + x, originY + y, scale, outline);
        }
        DrawRuns(surface, runs, originX, originY, scale, color);
    }

    private void DrawGlowRing(PixelSurface surface, IReadOnlyList<(Glyph Glyph, int Cursor)> runs,
        float originX, float originY, float scale, Color color, int radius, float alphaScale)
    {
        Color ring = Color.FromArgb(Math.Clamp((int)Math.Round(color.A * alphaScale), 0, 255), color);
        (int X, int Y)[] stamps =
        [
            (-radius, 0), (radius, 0), (0, -radius), (0, radius),
            (-radius, -radius), (radius, -radius), (-radius, radius), (radius, radius)
        ];
        foreach ((int x, int y) in stamps)
            DrawRuns(surface, runs, originX + x, originY + y, scale, ring);
    }

    private void DrawRuns(PixelSurface surface, IReadOnlyList<(Glyph Glyph, int Cursor)> runs,
        float originX, float originY, float scale, Color color)
    {
        foreach ((Glyph glyph, int cursor) in runs)
        {
            int outputWidth = Math.Max(1, (int)MathF.Ceiling(glyph.PackedWidth * scale));
            int outputHeight = Math.Max(1, (int)MathF.Ceiling(glyph.PackedHeight * scale));
            int drawX = (int)MathF.Round(originX + (cursor + glyph.XOffset) * scale);
            int drawY = (int)MathF.Round(originY + glyph.YOffset * scale);
            for (int y = 0; y < outputHeight; y++)
            {
                float sourceY = (y + 0.5f) / scale - 0.5f;
                for (int x = 0; x < outputWidth; x++)
                {
                    float sourceX = (x + 0.5f) / scale - 0.5f;
                    byte coverage = SampleAlpha(glyph, sourceX, sourceY);
                    surface.BlendPixel(drawX + x, drawY + y, color, coverage);
                }
            }
        }
    }

    private byte SampleAlpha(Glyph glyph, float x, float y)
    {
        int x0 = Math.Clamp((int)MathF.Floor(x), 0, glyph.PackedWidth - 1);
        int y0 = Math.Clamp((int)MathF.Floor(y), 0, glyph.PackedHeight - 1);
        int x1 = Math.Min(x0 + 1, glyph.PackedWidth - 1);
        int y1 = Math.Min(y0 + 1, glyph.PackedHeight - 1);
        float fx = Math.Clamp(x - MathF.Floor(x), 0, 1);
        float fy = Math.Clamp(y - MathF.Floor(y), 0, 1);
        byte a00 = AtlasAlpha(glyph.PackedX + x0, glyph.PackedY + y0);
        byte a10 = AtlasAlpha(glyph.PackedX + x1, glyph.PackedY + y0);
        byte a01 = AtlasAlpha(glyph.PackedX + x0, glyph.PackedY + y1);
        byte a11 = AtlasAlpha(glyph.PackedX + x1, glyph.PackedY + y1);
        float top = a00 + (a10 - a00) * fx;
        float bottom = a01 + (a11 - a01) * fx;
        return (byte)Math.Clamp((int)MathF.Round(top + (bottom - top) * fy), 0, 255);
    }

    private byte AtlasAlpha(int x, int y)
        => (uint)x < (uint)_atlasWidth && (uint)y < (uint)_atlasHeight ? _atlasAlpha[y * _atlasWidth + x] : (byte)0;

    private static byte[] ReadAlpha(Bitmap bitmap)
    {
        using var converted = new Bitmap(bitmap.Width, bitmap.Height, PixelFormat.Format32bppArgb);
        using (Graphics graphics = Graphics.FromImage(converted))
        {
            graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
            graphics.DrawImage(bitmap,
                new Rectangle(0, 0, converted.Width, converted.Height),
                new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                GraphicsUnit.Pixel);
        }
        byte[] alpha = new byte[bitmap.Width * bitmap.Height];
        BitmapData data = converted.LockBits(
            new Rectangle(0, 0, converted.Width, converted.Height),
            ImageLockMode.ReadOnly,
            PixelFormat.Format32bppArgb);
        try
        {
            byte[] row = new byte[converted.Width * 4];
            for (int y = 0; y < converted.Height; y++)
            {
                Marshal.Copy(data.Scan0 + y * data.Stride, row, 0, row.Length);
                for (int x = 0; x < converted.Width; x++)
                    alpha[y * converted.Width + x] = row[x * 4 + 3];
            }
        }
        finally
        {
            converted.UnlockBits(data);
        }
        return alpha;
    }
}
