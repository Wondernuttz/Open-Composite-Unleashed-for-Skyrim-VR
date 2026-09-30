using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace OCUKeyboardStudio;

internal sealed record FontImportManifest(string DisplayName, string ConfigName, string SourceFile);

internal sealed record FontImportResult(
    string DisplayName,
    string ConfigName,
    string MetadataPath,
    string TexturePath,
    string SourceFontPath,
    IReadOnlyList<char> FallbackCharacters,
    IReadOnlyList<char> UnsupportedCharacters);

internal static class FontImporter
{
    private const float PixelEmSize = 40f; // 30pt at the 96-DPI atlas baseline.
    private const int AtlasWidth = 512;
    private const int GlyphPadding = 2;

    public static FontImportResult Import(string sourceFontPath, KeyboardDocument document,
        string outputDirectory, SudoFont fallback, string? existingConfigName = null)
    {
        if (!File.Exists(sourceFontPath))
            throw new FileNotFoundException("The selected font file no longer exists.", sourceFontPath);
        Directory.CreateDirectory(outputDirectory);

        using var collection = new PrivateFontCollection();
        collection.AddFontFile(sourceFontPath);
        FontFamily family = collection.Families.FirstOrDefault()
            ?? throw new InvalidDataException("Windows could not find a font family in this TTF/OTF file.");
        FontStyle style = PreferredStyle(family);
        string displayName = family.Name;
        string configName = existingConfigName ?? $"custom_{Sanitize(displayName)}";
        string metadataPath = Path.Combine(outputDirectory, $"{configName}-30.sfn");
        string texturePath = Path.Combine(outputDirectory, $"{configName}-30-texture.png");
        string sourceExtension = Path.GetExtension(sourceFontPath).ToLowerInvariant();
        if (sourceExtension is not (".ttf" or ".otf"))
            sourceExtension = ".ttf";
        string storedSourcePath = Path.Combine(outputDirectory, $"{configName}-source{sourceExtension}");

        SortedSet<char> required = document.RequiredFontCharacters();
        var rasters = new List<FontGlyphRaster>();
        var fallbackCharacters = new List<char>();
        var unsupportedCharacters = new List<char>();

        int lineHeight = Math.Max(1, (int)Math.Ceiling(
            family.GetLineSpacing(style) / (double)family.GetEmHeight(style) * PixelEmSize));
        using var font = new Font(family, PixelEmSize, style, GraphicsUnit.Pixel);
        using var support = new GdiGlyphSupport(font);
        foreach (char character in required)
        {
            if (!char.IsSurrogate(character) && support.Contains(character))
            {
                rasters.Add(Rasterize(character, family, style, font));
                continue;
            }
            if (fallback.TryGetGlyphRaster(character, out FontGlyphRaster fallbackRaster))
            {
                rasters.Add(fallbackRaster);
                fallbackCharacters.Add(character);
                lineHeight = Math.Max(lineHeight, fallback.LineHeight);
            }
            else
            {
                unsupportedCharacters.Add(character);
            }
        }

        WriteFont(metadataPath, texturePath, lineHeight, rasters);
        if (!Path.GetFullPath(sourceFontPath).Equals(Path.GetFullPath(storedSourcePath), StringComparison.OrdinalIgnoreCase))
            File.Copy(sourceFontPath, storedSourcePath, overwrite: true);
        var manifest = new FontImportManifest(displayName, configName, Path.GetFileName(storedSourcePath));
        File.WriteAllText(ManifestPath(outputDirectory, configName),
            JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));

        return new FontImportResult(displayName, configName, metadataPath, texturePath, storedSourcePath,
            fallbackCharacters, unsupportedCharacters);
    }

    public static FontImportManifest? ReadManifest(string directory, string configName)
    {
        string path = ManifestPath(directory, configName);
        if (!File.Exists(path))
            return null;
        try
        {
            return JsonSerializer.Deserialize<FontImportManifest>(File.ReadAllText(path));
        }
        catch
        {
            return null;
        }
    }

    private static string ManifestPath(string directory, string configName)
        => Path.Combine(directory, $"{configName}-font.json");

    private static FontStyle PreferredStyle(FontFamily family)
    {
        foreach (FontStyle style in new[] { FontStyle.Regular, FontStyle.Bold, FontStyle.Italic, FontStyle.Bold | FontStyle.Italic })
            if (family.IsStyleAvailable(style)) return style;
        throw new InvalidDataException($"'{family.Name}' does not expose a renderable Regular, Bold, or Italic face.");
    }

    private static string Sanitize(string name)
    {
        var output = new StringBuilder();
        foreach (char character in name.ToLowerInvariant())
        {
            if (char.IsAsciiLetterOrDigit(character)) output.Append(character);
            else if (output.Length > 0 && output[^1] != '_') output.Append('_');
        }
        string result = output.ToString().Trim('_');
        return string.IsNullOrWhiteSpace(result) ? "imported_font" : result;
    }

    private static FontGlyphRaster Rasterize(char character, FontFamily family, FontStyle style, Font font)
    {
        using StringFormat format = (StringFormat)StringFormat.GenericTypographic.Clone();
        format.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
        int advance;
        using (var measureBitmap = new Bitmap(2, 2, PixelFormat.Format32bppArgb))
        using (Graphics graphics = Graphics.FromImage(measureBitmap))
            advance = Math.Max(1, (int)Math.Ceiling(graphics.MeasureString(character.ToString(), font,
                new PointF(0, 0), format).Width));

        using var path = new GraphicsPath();
        path.AddString(character.ToString(), family, (int)style, PixelEmSize, new PointF(0, 0), format);
        RectangleF bounds = path.GetBounds();
        if (bounds.IsEmpty || bounds.Width <= 0 || bounds.Height <= 0)
            return new FontGlyphRaster(character, 0, 0, 0, 0, ToShort(advance), []);

        int nominalLeft = (int)Math.Floor(bounds.Left);
        int nominalTop = (int)Math.Floor(bounds.Top);
        int temporaryWidth = Math.Max(1, (int)Math.Ceiling(bounds.Right) - nominalLeft + GlyphPadding * 2);
        int temporaryHeight = Math.Max(1, (int)Math.Ceiling(bounds.Bottom) - nominalTop + GlyphPadding * 2);
        using var temporary = new Bitmap(temporaryWidth, temporaryHeight, PixelFormat.Format32bppArgb);
        using (Graphics graphics = Graphics.FromImage(temporary))
        {
            graphics.Clear(Color.Transparent);
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.TranslateTransform(GlyphPadding - nominalLeft, GlyphPadding - nominalTop);
            using var brush = new SolidBrush(Color.White);
            graphics.FillPath(brush, path);
        }

        byte[] temporaryAlpha = ReadAlpha(temporary);
        int minX = temporaryWidth, minY = temporaryHeight, maxX = -1, maxY = -1;
        for (int y = 0; y < temporaryHeight; y++)
        {
            for (int x = 0; x < temporaryWidth; x++)
            {
                if (temporaryAlpha[y * temporaryWidth + x] == 0) continue;
                minX = Math.Min(minX, x); minY = Math.Min(minY, y);
                maxX = Math.Max(maxX, x); maxY = Math.Max(maxY, y);
            }
        }
        if (maxX < minX || maxY < minY)
            return new FontGlyphRaster(character, 0, 0, 0, 0, ToShort(advance), []);

        int width = maxX - minX + 1;
        int height = maxY - minY + 1;
        byte[] alpha = new byte[width * height];
        for (int y = 0; y < height; y++)
            Array.Copy(temporaryAlpha, (minY + y) * temporaryWidth + minX, alpha, y * width, width);
        int xOffset = nominalLeft + minX - GlyphPadding;
        int yOffset = nominalTop + minY - GlyphPadding;
        return new FontGlyphRaster(character, ToShort(width), ToShort(height),
            ToShort(xOffset), ToShort(yOffset), ToShort(advance), alpha);
    }

    private static byte[] ReadAlpha(Bitmap bitmap)
    {
        var alpha = new byte[bitmap.Width * bitmap.Height];
        BitmapData data = bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height),
            ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            byte[] row = new byte[bitmap.Width * 4];
            for (int y = 0; y < bitmap.Height; y++)
            {
                Marshal.Copy(data.Scan0 + y * data.Stride, row, 0, row.Length);
                for (int x = 0; x < bitmap.Width; x++)
                    alpha[y * bitmap.Width + x] = row[x * 4 + 3];
            }
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
        return alpha;
    }

    private static void WriteFont(string metadataPath, string texturePath, int lineHeight,
        IReadOnlyList<FontGlyphRaster> rasters)
    {
        var placements = new List<(FontGlyphRaster Raster, int X, int Y)>();
        int x = GlyphPadding, y = GlyphPadding, rowHeight = 0;
        foreach (FontGlyphRaster raster in rasters)
        {
            if (raster.Width <= 0 || raster.Height <= 0)
            {
                placements.Add((raster, 0, 0));
                continue;
            }
            if (x + raster.Width + GlyphPadding > AtlasWidth)
            {
                x = GlyphPadding;
                y += rowHeight + GlyphPadding;
                rowHeight = 0;
            }
            placements.Add((raster, x, y));
            x += raster.Width + GlyphPadding;
            rowHeight = Math.Max(rowHeight, raster.Height);
        }
        int usedHeight = y + rowHeight + GlyphPadding;
        int atlasHeight = 64;
        while (atlasHeight < usedHeight) atlasHeight *= 2;
        if (atlasHeight > 4096)
            throw new InvalidDataException("This keyboard's required glyphs do not fit in OCU's font atlas.");

        using (var atlas = new Bitmap(AtlasWidth, atlasHeight, PixelFormat.Format32bppArgb))
        {
            BitmapData data = atlas.LockBits(new Rectangle(0, 0, atlas.Width, atlas.Height),
                ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            try
            {
                byte[] pixels = new byte[atlas.Width * atlas.Height * 4];
                foreach ((FontGlyphRaster raster, int glyphX, int glyphY) in placements)
                {
                    for (int gy = 0; gy < raster.Height; gy++)
                    {
                        for (int gx = 0; gx < raster.Width; gx++)
                        {
                            int target = ((glyphY + gy) * atlas.Width + glyphX + gx) * 4;
                            pixels[target] = 255; pixels[target + 1] = 255; pixels[target + 2] = 255;
                            pixels[target + 3] = raster.Alpha[gy * raster.Width + gx];
                        }
                    }
                }
                for (int row = 0; row < atlas.Height; row++)
                    Marshal.Copy(pixels, row * atlas.Width * 4, data.Scan0 + row * data.Stride, atlas.Width * 4);
            }
            finally
            {
                atlas.UnlockBits(data);
            }
            atlas.Save(texturePath, ImageFormat.Png);
        }

        using var stream = new FileStream(metadataPath, FileMode.Create, FileAccess.Write, FileShare.None);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: false);
        writer.Write((byte)0x0B);
        writer.Write(Encoding.ASCII.GetBytes("SudoFont1.1"));
        writer.Write((ushort)0);
        writer.Write((uint)6);
        writer.Write((ushort)Math.Clamp(lineHeight, 1, ushort.MaxValue));
        writer.Write((ushort)AtlasWidth);
        writer.Write((ushort)atlasHeight);
        writer.Write((ushort)1);
        writer.Write((uint)(2 + placements.Count * 16));
        writer.Write((ushort)placements.Count);
        foreach ((FontGlyphRaster raster, int glyphX, int glyphY) in placements)
        {
            writer.Write((ushort)raster.Character);
            writer.Write((ushort)glyphX);
            writer.Write((ushort)glyphY);
            writer.Write(unchecked((ushort)raster.Width));
            writer.Write(unchecked((ushort)raster.Height));
            writer.Write(unchecked((ushort)raster.XOffset));
            writer.Write(unchecked((ushort)raster.YOffset));
            writer.Write(unchecked((ushort)raster.XAdvance));
        }
        writer.Write((ushort)999);
    }

    private static short ToShort(int value) => (short)Math.Clamp(value, short.MinValue, short.MaxValue);

    private sealed class GdiGlyphSupport : IDisposable
    {
        private readonly Bitmap _bitmap = new(2, 2, PixelFormat.Format32bppArgb);
        private readonly Graphics _graphics;
        private readonly IntPtr _hdc;
        private readonly IntPtr _font;
        private readonly IntPtr _previous;

        public GdiGlyphSupport(Font font)
        {
            _graphics = Graphics.FromImage(_bitmap);
            _hdc = _graphics.GetHdc();
            _font = font.ToHfont();
            _previous = SelectObject(_hdc, _font);
        }

        public bool Contains(char character)
        {
            ushort[] indices = new ushort[1];
            uint result = GetGlyphIndicesW(_hdc, character.ToString(), 1, indices, 1);
            return result != uint.MaxValue && indices[0] != ushort.MaxValue;
        }

        public void Dispose()
        {
            SelectObject(_hdc, _previous);
            DeleteObject(_font);
            _graphics.ReleaseHdc(_hdc);
            _graphics.Dispose();
            _bitmap.Dispose();
        }

        [DllImport("gdi32.dll")]
        private static extern IntPtr SelectObject(IntPtr hdc, IntPtr h);

        [DllImport("gdi32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool DeleteObject(IntPtr hObject);

        [DllImport("gdi32.dll", CharSet = CharSet.Unicode)]
        private static extern uint GetGlyphIndicesW(IntPtr hdc, string text, int count,
            [Out] ushort[] glyphIndices, uint flags);
    }
}
