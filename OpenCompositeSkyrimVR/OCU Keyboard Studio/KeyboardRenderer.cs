namespace OCUKeyboardStudio;

internal enum KeyboardPreviewState
{
    Lower,
    Shift,
    Caps
}

internal sealed record KeyboardTheme(
    string Name,
    string? BackgroundFile,
    bool Modern,
    Color Ink,
    Color Accent,
    Color Bright,
    Color KeyIdle,
    Color KeyHot,
    bool Outline)
{
    public float ModeButtonOffsetX { get; init; }
    public float ModeButtonOffsetY { get; init; }
    public float LockButtonOffsetX { get; init; }
    public float LockButtonOffsetY { get; init; }
    public float SizeControlOffsetX { get; init; }
    public float SizeControlOffsetY { get; init; }
    public float OpacityControlOffsetX { get; init; }
    public float OpacityControlOffsetY { get; init; }
    public float TiltControlOffsetX { get; init; }
    public float TiltControlOffsetY { get; init; }
    public KeyboardControlDesign? SizeControlDesign { get; init; }
    public KeyboardControlDesign? OpacityControlDesign { get; init; }
    public KeyboardControlDesign? TiltControlDesign { get; init; }
    public override string ToString() => Name;

    public static IReadOnlyList<KeyboardTheme> BuiltIns { get; } =
    [
        new("Modern Green", null, true, Color.FromArgb(255, 237, 240, 245), Color.FromArgb(205, 62, 190, 143), Color.FromArgb(255, 132, 242, 158), Color.FromArgb(175, 22, 26, 33), Color.FromArgb(105, 62, 190, 143), true),
        new("Modern White", null, true, Color.FromArgb(255, 237, 240, 245), Color.FromArgb(205, 190, 205, 220), Color.White, Color.FromArgb(175, 22, 26, 33), Color.FromArgb(105, 190, 205, 220), true),
        new("Modern Blue", null, true, Color.FromArgb(255, 237, 240, 245), Color.FromArgb(205, 60, 150, 245), Color.FromArgb(255, 115, 205, 255), Color.FromArgb(175, 22, 26, 33), Color.FromArgb(105, 60, 150, 245), true),
        new("Modern Amber", null, true, Color.FromArgb(255, 237, 240, 245), Color.FromArgb(205, 218, 145, 55), Color.FromArgb(255, 255, 205, 115), Color.FromArgb(175, 22, 26, 33), Color.FromArgb(105, 218, 145, 55), true),
        new("Modern Purple", null, true, Color.FromArgb(255, 237, 240, 245), Color.FromArgb(205, 155, 95, 230), Color.FromArgb(255, 215, 165, 255), Color.FromArgb(175, 22, 26, 33), Color.FromArgb(105, 155, 95, 230), true),
        new("Parchment", "parchment-bg.png", false, Color.Black, Color.FromArgb(90, 80, 55, 25), Color.FromArgb(255, 220, 200, 160), Color.Transparent, Color.FromArgb(100, 60, 35, 10), false),
        new("SkyUI Dark", "skyui-bg.png", false, Color.FromArgb(255, 235, 235, 235), Color.FromArgb(55, 255, 255, 255), Color.White, Color.Transparent, Color.FromArgb(48, 255, 255, 255), false),
        new("Dwemer", "dwemer-bg.png", false, Color.FromArgb(255, 210, 255, 240), Color.FromArgb(90, 190, 140, 70), Color.White, Color.Transparent, Color.FromArgb(90, 190, 140, 70), true)
        {
            ModeButtonOffsetX = 8,
            ModeButtonOffsetY = 6,
            LockButtonOffsetX = -18,
            LockButtonOffsetY = 2,
            SizeControlOffsetX = -1,
            SizeControlOffsetY = 28,
            OpacityControlOffsetX = 5.071f,
            OpacityControlOffsetY = 27.894f,
            TiltControlOffsetX = 4,
            TiltControlOffsetY = 55,
            SizeControlDesign = new()
            {
                ValueOffsetY = 4.142f
            },
            OpacityControlDesign = new()
            {
                Width = 89.929f,
                Height = 101.106f,
                UpOffsetY = -15.531f,
                LabelOffsetX = 1.035f,
                LabelOffsetY = -4.142f,
                ValueOffsetX = 1.035f,
                ValueOffsetY = 4.142f
            },
            TiltControlDesign = new()
            {
                ValueOffsetY = 7.248f
            }
        },
        new("Sovngarde", "sovngarde-bg.png", false, Color.White, Color.FromArgb(70, 255, 255, 255), Color.White, Color.Transparent, Color.FromArgb(65, 255, 255, 255), true)
    ];
}

internal sealed class KeyboardRenderer
{
    private sealed record PreparedArtwork(string Signature, PixelSurface Surface, PixelSurface? GlowSurface = null, int GlowOffset = 0);
    public const int TextureWidth = 1024;
    public const int TextureHeight = 560;
    public const int Padding = 6;
    public const int MarginHorizontal = 120;
    public const int MarginTop = 60;
    public const int GrabBarHeight = 52;

    public string AssetsDirectory { get; set; }
    public KeyboardTheme Theme { get; set; } = KeyboardTheme.BuiltIns[0];
    public SudoFont Font { get; set; }
    public KeyboardPreviewState State { get; set; }
    public int SelectedKeyId { get; set; } = -1;
    public bool Pressed { get; set; }
    public bool ShowGrid { get; set; } = true;
    public bool PreviewPcMode { get; set; }
    public bool PreviewHeadLocked { get; set; }
    public double AnimationTimeSeconds { get; set; }
    private PreparedArtwork? _backgroundCache;
    private PreparedArtwork? _consoleBackgroundCache;
    private readonly Dictionary<Guid, PreparedArtwork> _spriteCache = [];
    private readonly Dictionary<string, PreparedArtwork> _stateArtworkCache = new(StringComparer.Ordinal);
    private Bitmap? _spacebarImage;
    private string? _spacebarImagePath;

    public KeyboardRenderer(string assetsDirectory, SudoFont font)
    {
        AssetsDirectory = assetsDirectory;
        Font = font;
    }

    public int KeySize(KeyboardDocument document)
    {
        int availableWidth = TextureWidth - 2 * MarginHorizontal;
        return ((availableWidth - Padding) / document.Width) - Padding;
    }

    public RectangleF KeyRectangle(KeyboardDocument document, KeyboardKey key)
    {
        int keySize = KeySize(document);
        int pitch = keySize + Padding;
        int baseY = MarginTop + keySize + Padding + GrabBarHeight;
        float x = MarginHorizontal + pitch * key.X;
        float y = baseY + pitch * key.Y;
        float width = key.SpansToRight ? TextureWidth - MarginHorizontal - x : keySize * key.Width;
        return new RectangleF(x, y, width, keySize * key.Height);
    }

    public RectangleF TopElementRectangle(KeyboardDocument document, KeyboardTopElement element)
    {
        int textBarY = GrabBarHeight + MarginTop;
        int buttonY = textBarY - 36;
        float width = TopElementWidth(document, element);
        float height = TopElementHeight(document, element);
        return element switch
        {
            KeyboardTopElement.TextBar => new RectangleF(
                MarginHorizontal + document.TextBarOffsetX,
                textBarY + document.TextBarOffsetY,
                width, height),
            KeyboardTopElement.Mode => new RectangleF(
                MarginHorizontal + Theme.ModeButtonOffsetX + document.ModeButtonOffsetX,
                buttonY + Theme.ModeButtonOffsetY + document.ModeButtonOffsetY,
                width, height),
            _ => new RectangleF(
                TextureWidth - MarginHorizontal - 120 + Theme.LockButtonOffsetX + document.LockButtonOffsetX,
                buttonY + Theme.LockButtonOffsetY + document.LockButtonOffsetY,
                width, height)
        };
    }

    public float TopElementWidth(KeyboardDocument document, KeyboardTopElement element) => element switch
    {
        KeyboardTopElement.TextBar => document.TextBarWidth > 0 ? document.TextBarWidth : TextureWidth - MarginHorizontal * 2,
        KeyboardTopElement.Mode => document.ModeButtonWidth > 0 ? document.ModeButtonWidth : 220,
        _ => document.LockButtonWidth > 0 ? document.LockButtonWidth : 120
    };

    public float TopElementHeight(KeyboardDocument document, KeyboardTopElement element) => element switch
    {
        KeyboardTopElement.TextBar => document.TextBarHeight > 0 ? document.TextBarHeight : KeySize(document),
        KeyboardTopElement.Mode => document.ModeButtonHeight > 0 ? document.ModeButtonHeight : 32,
        _ => document.LockButtonHeight > 0 ? document.LockButtonHeight : 32
    };

    public float TopElementFontScale(KeyboardDocument document, KeyboardTopElement element) => element switch
    {
        KeyboardTopElement.TextBar => document.TextBarFontScale > 0 ? document.TextBarFontScale : 0.72f,
        KeyboardTopElement.Mode => document.ModeButtonFontScale > 0 ? document.ModeButtonFontScale : 0.75f,
        _ => document.LockButtonFontScale > 0 ? document.LockButtonFontScale : 0.75f
    };

    public bool IsPointOnTopElementContent(KeyboardDocument document, KeyboardTopElement element, PointF point)
    {
        RectangleF rectangle = TopElementRectangle(document, element);
        string text = TopElementText(element);
        float scale = TopElementFontScale(document, element);
        GetTopTextOffsets(document, element, out float offsetX, out float offsetY);
        return Font.HitTestText(text, rectangle, offsetX, offsetY, scale, point);
    }

    public RectangleF TopElementContentRectangle(KeyboardDocument document, KeyboardTopElement element)
    {
        RectangleF rectangle = TopElementRectangle(document, element);
        string text = TopElementText(element);
        float scale = TopElementFontScale(document, element);
        GetTopTextOffsets(document, element, out float offsetX, out float offsetY);
        RectangleF visible = Font.VisibleTextRectangle(text, rectangle, offsetX, offsetY, scale);
        return visible.IsEmpty ? RectangleF.Empty : RectangleF.Inflate(visible, 2, 2);
    }

    public RectangleF TopStateArtworkRectangle(KeyboardDocument document, KeyboardTopElement element)
    {
        RectangleF button = TopElementRectangle(document, element);
        if (element == KeyboardTopElement.TextBar)
            return RectangleF.Empty;
        float offsetX = element == KeyboardTopElement.Mode ? document.ModeArtworkOffsetX : document.LockArtworkOffsetX;
        float offsetY = element == KeyboardTopElement.Mode ? document.ModeArtworkOffsetY : document.LockArtworkOffsetY;
        float authoredWidth = element == KeyboardTopElement.Mode ? document.ModeArtworkWidth : document.LockArtworkWidth;
        float authoredHeight = element == KeyboardTopElement.Mode ? document.ModeArtworkHeight : document.LockArtworkHeight;
        return new RectangleF(button.Left + offsetX, button.Top + offsetY,
            authoredWidth > 0 ? authoredWidth : button.Width,
            authoredHeight > 0 ? authoredHeight : button.Height);
    }

    public bool HasTopStateArtwork(KeyboardDocument document, KeyboardTopElement element)
    {
        string? path = TopStateArtworkPath(document, element);
        return !string.IsNullOrWhiteSpace(path) && File.Exists(path);
    }

    public bool IsTopStateTextVisible(KeyboardDocument document, KeyboardTopElement element)
    {
        if (element == KeyboardTopElement.TextBar)
            return true;
        bool artwork = HasTopStateArtwork(document, element);
        return !artwork || (element == KeyboardTopElement.Mode
            ? document.ModeTextOverArtwork : document.LockTextOverArtwork);
    }

    private string TopElementText(KeyboardTopElement element) => element switch
    {
        KeyboardTopElement.TextBar => "OCU Keyboard Studio Preview",
        KeyboardTopElement.Mode => PreviewPcMode ? "PC MODE" : "VR MODE",
        _ => "LOCK"
    };

    private string? TopStateArtworkPath(KeyboardDocument document, KeyboardTopElement element) => element switch
    {
        KeyboardTopElement.Mode => PreviewPcMode
            ? document.ModePcArtworkImagePath : document.ModeVrArtworkImagePath,
        KeyboardTopElement.Lock => PreviewHeadLocked
            ? document.LockHeadArtworkImagePath : document.LockWorldArtworkImagePath,
        _ => null
    };

    private static void GetTopTextOffsets(KeyboardDocument document, KeyboardTopElement element,
        out float offsetX, out float offsetY)
    {
        offsetX = element == KeyboardTopElement.Mode ? document.ModeTextOffsetX
            : element == KeyboardTopElement.Lock ? document.LockTextOffsetX : 0;
        offsetY = element == KeyboardTopElement.Mode ? document.ModeTextOffsetY
            : element == KeyboardTopElement.Lock ? document.LockTextOffsetY : 0;
    }

    public bool HasVisibleKeyContent(KeyboardDocument document, KeyboardKey key)
    {
        if (ShowsParchmentRibbon(document, key)
            || key.Character is '\x04' or '\x05' or '\x06' or '\x07')
            return true;
        string label = State == KeyboardPreviewState.Lower ? key.Label : key.ShiftLabel;
        return !string.IsNullOrWhiteSpace(label);
    }

    public RectangleF KeyContentRectangle(KeyboardDocument document, KeyboardKey key)
    {
        RectangleF plate = KeyRectangle(document, key);
        if (ShowsParchmentRibbon(document, key))
        {
            float scale = Math.Clamp(key.LabelScale, 0.25f, 3f);
            float width = plate.Width * scale;
            float height = plate.Height * scale;
            float ribbonHoverOffset = key.Id == SelectedKeyId && Pressed ? 2 : 0;
            return new RectangleF(
                plate.Left + (plate.Width - width) / 2f + key.LabelOffsetX,
                plate.Top + (plate.Height - height) / 2f + key.LabelOffsetY + ribbonHoverOffset,
                width, height);
        }
        if (key.Character is '\x04' or '\x05' or '\x06' or '\x07')
        {
            float scale = Math.Clamp(key.LabelScale, 0.25f, 3f);
            float width = Math.Max(4f, 20f * scale);
            float height = Math.Max(4f, 16f * scale);
            float arrowHoverOffset = key.Id == SelectedKeyId && Pressed ? 2 : 0;
            return new RectangleF(
                plate.Left + (plate.Width - width) / 2f + key.LabelOffsetX,
                plate.Top + (plate.Height - height) / 2f + key.LabelOffsetY + arrowHoverOffset,
                width, height);
        }
        string label = State == KeyboardPreviewState.Lower ? key.Label : key.ShiftLabel;
        float hoverOffset = key.Id == SelectedKeyId && Pressed ? 2 : 0;
        RectangleF visible = Font.VisibleTextRectangle(label, plate, key.LabelOffsetX, key.LabelOffsetY + hoverOffset, key.LabelScale);
        return visible.IsEmpty ? new RectangleF(plate.Left + plate.Width / 2f - 2, plate.Top + plate.Height / 2f - 2, 4, 4)
            : RectangleF.Inflate(visible, 2, 2);
    }

    public bool IsPointOnKeyContent(KeyboardDocument document, KeyboardKey key, PointF point)
    {
        RectangleF plate = KeyRectangle(document, key);
        if (ShowsParchmentRibbon(document, key))
        {
            RectangleF ribbon = KeyContentRectangle(document, key);
            if (!ribbon.Contains(point))
                return false;
            Bitmap? image = SpacebarImage();
            if (image is null)
                return true;
            int sourceX = Math.Clamp((int)((point.X - ribbon.Left) / Math.Max(1f, ribbon.Width) * image.Width), 0, image.Width - 1);
            int sourceY = Math.Clamp((int)((point.Y - ribbon.Top) / Math.Max(1f, ribbon.Height) * image.Height), 0, image.Height - 1);
            Color sample = image.GetPixel(sourceX, sourceY);
            return sample.A >= 250 && (sample.R + sample.G + sample.B) / 3 < 180;
        }
        if (key.Character is '\x04' or '\x05' or '\x06' or '\x07')
            return KeyContentRectangle(document, key).Contains(point);
        string label = State == KeyboardPreviewState.Lower ? key.Label : key.ShiftLabel;
        float hoverOffset = key.Id == SelectedKeyId && Pressed ? 2 : 0;
        return Font.HitTestText(label, plate, key.LabelOffsetX, key.LabelOffsetY + hoverOffset, key.LabelScale, point);
    }

    public Bitmap Render(KeyboardDocument document)
    {
        PixelSurface surface = CreateBackground(document);
        DrawSprites(surface, document);
        DrawTopControls(surface, document);

        foreach (KeyboardKey key in document.Keys)
        {
            RectangleF raw = KeyRectangle(document, key);
            var rectangle = Rectangle.Round(raw);
            bool selected = key.Id == SelectedKeyId;
            bool active = (key.Character == '\x01' && State == KeyboardPreviewState.Shift)
                || (key.Character == '\x02' && State == KeyboardPreviewState.Caps)
                || key.Character == '\x1E' && State == KeyboardPreviewState.Caps;

            DrawKeyPlate(surface, document, rectangle, active, selected);
            DrawKeyContent(surface, document, key, rectangle, selected);
        }

        DrawRuntimeControls(surface, document);

        return surface.ToBitmap();
    }

    public Bitmap RenderBackgroundExact(KeyboardDocument document)
        => CreateBackground(document).ToBitmap();

    public Bitmap RenderConsolePreview(KeyboardDocument document, string sampleText = "player.setav speedmult 100")
    {
        const int width = 1024;
        const int height = 120;
        const int titleHeight = 60;
        var surface = new PixelSurface(width, height);
        Color background = InputFill(document);
        Color border = InputOutline(document);
        Color titleFill = document.InputFillOverrideEnabled
            ? document.InputFillColor : ConsoleTitleFillForTheme(Theme);
        int borderWidth = InputOutlineWidth(document, 2);

        surface.FillRounded(new Rectangle(0, 0, width, height), 0, background);
        surface.FillRounded(new Rectangle(borderWidth, borderWidth,
            width - borderWidth * 2, titleHeight), 0, titleFill);
        if (!string.IsNullOrWhiteSpace(document.ConsoleInputBackgroundImagePath)
            && File.Exists(document.ConsoleInputBackgroundImagePath))
        {
            string signature = ArtworkSignature(document.ConsoleInputBackgroundImagePath,
                width, height, 0, 0);
            if (_consoleBackgroundCache is null || _consoleBackgroundCache.Signature != signature)
                _consoleBackgroundCache = new PreparedArtwork(signature,
                    PrepareArtwork(document.ConsoleInputBackgroundImagePath, width, height, 0, 0));
            surface.BlendSurface(_consoleBackgroundCache.Surface, 0, 0, 100);
        }
        if (borderWidth > 0)
        {
            surface.StrokeRounded(new Rectangle(0, 0, width, height), 0, borderWidth, border);
            surface.FillRounded(new Rectangle(borderWidth, borderWidth + titleHeight,
                width - borderWidth * 2, borderWidth), 0, border);
        }

        Rectangle titleBounds = ConsoleTitleBounds(document);
        Rectangle textBounds = ConsoleTextBounds(document, sampleText);
        Color ink = document.CustomStyleEnabled ? document.FontColor : ConsoleInkForTheme(Theme);
        DrawStyledTextAt(surface, document, "INPUT", titleBounds.X, titleBounds.Y, 1, ink);
        DrawStyledTextAt(surface, document, sampleText, textBounds.X, textBounds.Y, 1, ink);
        return surface.ToBitmap();
    }

    internal Rectangle ConsoleTitleBounds(KeyboardDocument document)
    {
        const int width = 1024;
        const int titleHeight = 60;
        int borderWidth = InputOutlineWidth(document, 2);
        return new Rectangle(
            (width - Font.Width("INPUT")) / 2 + (int)Math.Round(document.InputTitleOffsetX),
            borderWidth + (titleHeight - Font.LineHeight) / 2 + (int)Math.Round(document.InputTitleOffsetY),
            Math.Max(1, Font.Width("INPUT")), Math.Max(1, Font.LineHeight));
    }

    internal Rectangle ConsoleTextBounds(KeyboardDocument document, string sampleText = "player.setav speedmult 100")
    {
        const int height = 120;
        const int titleHeight = 60;
        const int padding = 8;
        int borderWidth = InputOutlineWidth(document, 2);
        int contentTop = borderWidth + titleHeight + borderWidth;
        int contentHeight = height - contentTop - borderWidth;
        return new Rectangle(
            padding + borderWidth + (int)Math.Round(document.InputTextOffsetX),
            contentTop + (contentHeight - Font.LineHeight) / 2 + (int)Math.Round(document.InputTextOffsetY),
            Math.Max(1, Font.Width(sampleText)), Math.Max(1, Font.LineHeight));
    }

    private PixelSurface CreateBackground(KeyboardDocument document)
    {
        if (!string.IsNullOrWhiteSpace(document.BackgroundImagePath) && File.Exists(document.BackgroundImagePath))
        {
            var surface = new PixelSurface(TextureWidth, TextureHeight);
            int width = Math.Max(1, (int)Math.Round(document.BackgroundWidth));
            int height = Math.Max(1, (int)Math.Round(document.BackgroundHeight));
            string signature = ArtworkSignature(document.BackgroundImagePath, width, height,
                document.BackgroundEdgeFade, document.BackgroundRotation) + $"|round={document.BackgroundRoundness}";
            if (_backgroundCache is null || _backgroundCache.Signature != signature)
                _backgroundCache = new PreparedArtwork(signature, PrepareArtwork(document.BackgroundImagePath,
                    width, height, document.BackgroundEdgeFade, document.BackgroundRotation,
                    Math.Clamp(document.BackgroundRoundness, 0, 100) * Math.Min(width, height) / 200));
            int opacity = AnimatedOpacity(document.BackgroundOpacity,
                document.BackgroundBreatheEnabled, document.BackgroundBreatheMinPercent,
                document.BackgroundBreathePeriodSeconds, document.BackgroundBreathePhaseDegrees);
            surface.BlendSurface(_backgroundCache.Surface,
                (int)Math.Round(document.BackgroundX), (int)Math.Round(document.BackgroundY), opacity);
            return surface;
        }

        if (Theme.Modern)
        {
            var surface = new PixelSurface(TextureWidth, TextureHeight);
            surface.FillRounded(new Rectangle(0, 0, TextureWidth, TextureHeight), 22, Color.FromArgb(235, 48, 56, 68));
            surface.FillRounded(new Rectangle(2, 2, TextureWidth - 4, TextureHeight - 4), 20, Color.FromArgb(235, 14, 17, 22));
            return surface;
        }

        var background = new Bitmap(TextureWidth, TextureHeight);
        using (Graphics graphics = Graphics.FromImage(background))
        {
            graphics.Clear(Color.Transparent);
            string path = Path.Combine(AssetsDirectory, Theme.BackgroundFile ?? "");
            if (File.Exists(path))
            {
                using var image = new Bitmap(path);
                int x = (TextureWidth - image.Width) / 2;
                graphics.DrawImage(image,
                    new Rectangle(x, 0, image.Width, image.Height),
                    new Rectangle(0, 0, image.Width, image.Height),
                    GraphicsUnit.Pixel);
            }
            else
            {
                graphics.Clear(Color.FromArgb(235, 14, 17, 22));
            }
        }
        PixelSurface result = PixelSurface.FromBitmap(background);
        background.Dispose();
        return result;
    }

    private void DrawSprites(PixelSurface surface, KeyboardDocument document)
    {
        foreach (KeyboardSprite sprite in document.Sprites)
        {
            if (string.IsNullOrWhiteSpace(sprite.SourcePath) || !File.Exists(sprite.SourcePath))
                continue;
            int width = Math.Max(1, (int)Math.Round(sprite.Width));
            int height = Math.Max(1, (int)Math.Round(sprite.Height));
            string signature = ArtworkSignature(sprite.SourcePath, width, height, sprite.EdgeFade, sprite.Rotation)
                + $"|glow={sprite.GlowEnabled}|{sprite.GlowColor.ToArgb()}|{sprite.GlowRadius}";
            if (!_spriteCache.TryGetValue(sprite.Id, out PreparedArtwork? prepared) || prepared.Signature != signature)
            {
                PixelSurface art = PrepareArtwork(sprite.SourcePath, width, height, sprite.EdgeFade, sprite.Rotation);
                prepared = new PreparedArtwork(signature, art,
                    sprite.GlowEnabled ? CreateGlowSurface(art, sprite.GlowColor, sprite.GlowRadius) : null,
                    sprite.GlowEnabled ? sprite.GlowRadius : 0);
                _spriteCache[sprite.Id] = prepared;
            }
            if (sprite.GlowEnabled && prepared.GlowSurface is not null)
            {
                int glowOpacity = AnimatedOpacity(sprite.GlowStrength, sprite.BreatheEnabled, sprite.BreatheMinPercent,
                    sprite.BreathePeriodSeconds, sprite.BreathePhaseDegrees);
                surface.BlendSurface(prepared.GlowSurface,
                    (int)Math.Round(sprite.X) - prepared.GlowOffset,
                    (int)Math.Round(sprite.Y) - prepared.GlowOffset, glowOpacity);
            }
            surface.BlendSurface(prepared.Surface, (int)Math.Round(sprite.X), (int)Math.Round(sprite.Y), sprite.Opacity);
        }
    }

    private static string ArtworkSignature(string path, int width, int height, int edgeFade, float rotation)
        => $"{Path.GetFullPath(path)}|{File.GetLastWriteTimeUtc(path).Ticks}|{width}|{height}|{edgeFade}|{rotation:R}";

    private static PixelSurface PrepareArtwork(string path, int width, int height, int edgeFade, float rotation, int roundness = 0)
    {
        var prepared = new PixelSurface(width, height);
        using var image = new Bitmap(path);
        // A rounded background must feather inward from the rounded boundary.
        // Fading the source rectangle first leaves opaque corners until a later
        // hard cut, which is visibly wrong for pill-shaped artwork.
        prepared.BlendBitmap(image, new Rectangle(0, 0, width, height), 100,
            roundness > 0 ? 0 : edgeFade, rotation);
        ApplyRoundedMask(prepared, roundness, edgeFade);
        return prepared;
    }

    private static void ApplyRoundedMask(PixelSurface surface, int roundness, int edgeFade)
    {
        int radius = Math.Clamp(roundness, 0, Math.Min(surface.Width, surface.Height) / 2);
        if (radius == 0)
            return;
        float halfWidth = surface.Width / 2f;
        float halfHeight = surface.Height / 2f;
        float innerHalfWidth = halfWidth - radius;
        float innerHalfHeight = halfHeight - radius;
        float fade = Math.Max(0, edgeFade);
        for (int y = 0; y < surface.Height; y++)
        {
            for (int x = 0; x < surface.Width; x++)
            {
                float qx = Math.Abs(x + 0.5f - halfWidth) - innerHalfWidth;
                float qy = Math.Abs(y + 0.5f - halfHeight) - innerHalfHeight;
                float outside = MathF.Sqrt(MathF.Max(qx, 0) * MathF.Max(qx, 0)
                    + MathF.Max(qy, 0) * MathF.Max(qy, 0));
                float inside = MathF.Min(MathF.Max(qx, qy), 0);
                float signedDistance = outside + inside - radius;
                int alphaIndex = (y * surface.Width + x) * 4 + 3;
                if (signedDistance >= 0)
                {
                    surface.Bgra[alphaIndex] = 0;
                    continue;
                }
                if (fade > 0)
                {
                    float factor = Math.Clamp(-signedDistance / fade, 0, 1);
                    surface.Bgra[alphaIndex] = (byte)Math.Round(surface.Bgra[alphaIndex] * factor);
                }
            }
        }
    }

    private static PixelSurface CreateGlowSurface(PixelSurface source, Color color, int requestedRadius)
    {
        int radius = Math.Clamp(requestedRadius, 1, 48);
        int width = source.Width + radius * 2;
        int height = source.Height + radius * 2;
        float[] horizontal = new float[width * height];
        float[] blurred = new float[width * height];
        for (int y = 0; y < source.Height; y++)
        {
            int sourceRow = y * source.Width;
            int targetRow = (y + radius) * width + radius;
            for (int x = 0; x < source.Width; x++)
                horizontal[targetRow + x] = source.Bgra[(sourceRow + x) * 4 + 3] / 255f;
        }

        float[] pass = new float[width * height];
        int diameter = radius * 2 + 1;
        for (int y = 0; y < height; y++)
        {
            float sum = 0;
            int row = y * width;
            for (int x = -radius; x < width; x++)
            {
                if (x + radius < width) sum += horizontal[row + x + radius];
                if (x - radius - 1 >= 0) sum -= horizontal[row + x - radius - 1];
                if (x >= 0) pass[row + x] = sum / diameter;
            }
        }
        for (int x = 0; x < width; x++)
        {
            float sum = 0;
            for (int y = -radius; y < height; y++)
            {
                if (y + radius < height) sum += pass[(y + radius) * width + x];
                if (y - radius - 1 >= 0) sum -= pass[(y - radius - 1) * width + x];
                if (y >= 0) blurred[y * width + x] = sum / diameter;
            }
        }

        var glow = new PixelSurface(width, height);
        for (int index = 0; index < blurred.Length; index++)
        {
            byte alpha = (byte)Math.Clamp((int)Math.Round(Math.Sqrt(blurred[index]) * color.A), 0, 255);
            int output = index * 4;
            glow.Bgra[output] = color.B;
            glow.Bgra[output + 1] = color.G;
            glow.Bgra[output + 2] = color.R;
            glow.Bgra[output + 3] = alpha;
        }
        return glow;
    }

    private Color Ink(KeyboardDocument document) => document.CustomStyleEnabled ? document.FontColor : Theme.Ink;
    private Color KeyAccent(KeyboardDocument document) => document.CustomStyleEnabled ? document.KeyColor : Theme.Accent;
    internal static Color ConsoleBorderForTheme(KeyboardTheme theme) => theme.Name switch
    {
        "Parchment" => Color.FromArgb(220, 60, 40, 20),
        "SkyUI Dark" => Color.FromArgb(80, 255, 255, 255),
        "Dwemer" => Color.FromArgb(220, 190, 140, 70),
        "Sovngarde" => Color.FromArgb(85, 255, 255, 255),
        _ => Color.FromArgb(190, theme.Accent.R, theme.Accent.G, theme.Accent.B)
    };

    internal static Color ConsoleBackgroundForTheme(KeyboardTheme theme) => theme.Name switch
    {
        "Parchment" => Color.FromArgb(200, 220, 195, 160),
        "SkyUI Dark" => Color.FromArgb(215, 12, 12, 12),
        "Dwemer" => Color.FromArgb(225, 38, 30, 20),
        "Sovngarde" => Color.FromArgb(215, 10, 14, 30),
        _ => Color.FromArgb(235, 14, 17, 22)
    };

    private static Color ConsoleTitleFillForTheme(KeyboardTheme theme) => theme.Name switch
    {
        "Parchment" => Color.FromArgb(100, 80, 55, 25),
        "SkyUI Dark" => Color.FromArgb(70, 255, 255, 255),
        "Dwemer" => Color.FromArgb(0, 190, 140, 70),
        "Sovngarde" => Color.FromArgb(75, 255, 255, 255),
        _ => Color.FromArgb(175, theme.Accent.R, theme.Accent.G, theme.Accent.B)
    };

    private static Color ConsoleInkForTheme(KeyboardTheme theme) => theme.Name switch
    {
        "Parchment" => Color.FromArgb(255, 30, 15, 5),
        _ => theme.Ink
    };

    private Color InputOutline(KeyboardDocument document) => document.InputOutlineOverrideEnabled
        ? document.InputOutlineColor
        : document.CustomStyleEnabled ? document.KeyColor : ConsoleBorderForTheme(Theme);
    private int InputOutlineWidth(KeyboardDocument document, int themeFallback) =>
        !document.InputOutlineVisible ? 0 : document.InputOutlineOverrideEnabled
            ? Math.Clamp(document.InputOutlineWidth, 0, 8)
            : document.CustomStyleEnabled ? Math.Clamp(document.PlateOutlineWidth, 0, 8) : themeFallback;
    private Color InputFill(KeyboardDocument document) => document.InputFillOverrideEnabled
        ? document.InputFillColor : ConsoleBackgroundForTheme(Theme);
    private Color PlateFill(KeyboardDocument document) => document.CustomStyleEnabled ? document.PlateFillColor : Theme.KeyIdle;
    private Color Glow(KeyboardDocument document) => document.CustomStyleEnabled ? document.GlowColor : Theme.Bright;
    private Color Hover(KeyboardDocument document) => document.CustomStyleEnabled ? document.HoverColor : Theme.Bright;
    private bool Outline(KeyboardDocument document) => document.CustomStyleEnabled ? document.OutlineEnabled : Theme.Outline;

    private void DrawStyledText(PixelSurface surface, KeyboardDocument document, string text, Rectangle box,
        float offsetX, float offsetY, float scale, Color color)
    {
        (Color? outline, Color? glow, int glowRadius) = TextEffects(document);
        Font.DrawTextCentered(surface, text, box, offsetX, offsetY, scale, color,
            outline, glow, glowRadius);
    }

    private void DrawStyledTextAt(PixelSurface surface, KeyboardDocument document, string text,
        float x, float y, float scale, Color color)
    {
        (Color? outline, Color? glow, int glowRadius) = TextEffects(document);
        Font.DrawTextAt(surface, text, x, y, scale, color, outline, glow, glowRadius);
    }

    private (Color? Outline, Color? Glow, int GlowRadius) TextEffects(KeyboardDocument document)
    {
        Color? outline = Outline(document)
            ? document.CustomStyleEnabled ? document.FontOutlineColor : Color.FromArgb(220, 8, 11, 15)
            : null;
        Color? glow = null;
        int glowRadius = 0;
        if (document.CustomStyleEnabled && document.FontGlowEnabled && document.FontGlowStrength > 0)
        {
            double breathe = document.FontBreatheEnabled
                ? BreatheMultiplier(document.FontBreatheMinPercent, document.FontBreathePeriodSeconds,
                    document.FontBreathePhaseDegrees)
                : 1.0;
            int alpha = Math.Clamp((int)Math.Round(document.FontGlowColor.A
                * document.FontGlowStrength / 100.0 * breathe), 0, 255);
            glow = Color.FromArgb(alpha, document.FontGlowColor);
            glowRadius = Math.Clamp(document.FontGlowRadius, 1, 8);
        }
        return (outline, glow, glowRadius);
    }

    private void DrawTopControls(PixelSurface surface, KeyboardDocument document)
    {
        Rectangle mode = Rectangle.Round(TopElementRectangle(document, KeyboardTopElement.Mode));
        Rectangle lockButton = Rectangle.Round(TopElementRectangle(document, KeyboardTopElement.Lock));
        Rectangle textBar = Rectangle.Round(TopElementRectangle(document, KeyboardTopElement.TextBar));
        if (document.TopButtonPlatesEnabled)
        {
            Color? activeModeFill = document.CustomStyleEnabled
                ? Color.FromArgb(Math.Clamp(document.KeyColor.A / 2, 36, 140), document.KeyColor)
                : null;
            DrawPlate(surface, document, mode, PreviewPcMode, PreviewPcMode ? activeModeFill : null);
            DrawPlate(surface, document, lockButton, PreviewHeadLocked,
                PreviewHeadLocked ? activeModeFill : null);
        }
        string? modeArtwork = TopStateArtworkPath(document, KeyboardTopElement.Mode);
        string? lockArtwork = TopStateArtworkPath(document, KeyboardTopElement.Lock);
        Rectangle modeArtworkRectangle = Rectangle.Round(TopStateArtworkRectangle(document, KeyboardTopElement.Mode));
        Rectangle lockArtworkRectangle = Rectangle.Round(TopStateArtworkRectangle(document, KeyboardTopElement.Lock));
        bool drewModeArtwork = DrawStateArtwork(surface, modeArtwork, modeArtworkRectangle, "mode");
        bool drewLockArtwork = DrawStateArtwork(surface, lockArtwork, lockArtworkRectangle, "lock");
        if (!drewModeArtwork || document.ModeTextOverArtwork)
            DrawStyledText(surface, document, PreviewPcMode ? "PC MODE" : "VR MODE", mode,
                document.ModeTextOffsetX, document.ModeTextOffsetY,
                TopElementFontScale(document, KeyboardTopElement.Mode), Ink(document));
        if (!drewLockArtwork || document.LockTextOverArtwork)
            DrawStyledText(surface, document, "LOCK", lockButton,
                document.LockTextOffsetX, document.LockTextOffsetY,
                TopElementFontScale(document, KeyboardTopElement.Lock), Ink(document));
        if (document.InputBarPlateEnabled)
            DrawPlate(surface, document, textBar, false);
        Color ink = Ink(document);
        DrawStyledText(surface, document, "OCU Keyboard Studio Preview", textBar, 0, 0,
            TopElementFontScale(document, KeyboardTopElement.TextBar), Color.FromArgb(170, ink));
    }

    private bool DrawStateArtwork(PixelSurface surface, string? path, Rectangle rectangle, string cacheSlot)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)
            || rectangle.Width <= 0 || rectangle.Height <= 0)
            return false;
        string signature = ArtworkSignature(path, rectangle.Width, rectangle.Height, 0, 0);
        if (!_stateArtworkCache.TryGetValue(cacheSlot, out PreparedArtwork? cached)
            || cached.Signature != signature)
        {
            cached = new PreparedArtwork(signature,
                PrepareArtwork(path, rectangle.Width, rectangle.Height, 0, 0));
            _stateArtworkCache[cacheSlot] = cached;
        }
        surface.BlendSurface(cached.Surface, rectangle.X, rectangle.Y, 100);
        return true;
    }

    private void DrawKeyPlate(PixelSurface surface, KeyboardDocument document, Rectangle rectangle, bool active, bool selected)
    {
        if (!document.KeyPlatesEnabled)
            return;

        bool modern = Theme.Modern || document.CustomStyleEnabled;
        if (modern)
        {
            Color glowBase = Glow(document);
            int glowStrength = document.CustomStyleEnabled
                ? (document.GlowEnabled ? document.GlowStrength : 0)
                : 35;
            if (document.CustomStyleEnabled && document.KeyBreatheEnabled)
                glowStrength = (int)Math.Round(glowStrength * BreatheMultiplier(
                    document.KeyBreatheMinPercent, document.KeyBreathePeriodSeconds,
                    document.KeyBreathePhaseDegrees));
            if (glowStrength > 0)
            {
                int glowAlpha = Math.Clamp((selected || active ? 12 : 5) + glowStrength / 4, 0, 80);
                Color glow = Color.FromArgb(glowAlpha, glowBase.R, glowBase.G, glowBase.B);
                int glowRadius = document.CustomStyleEnabled ? Math.Clamp(document.GlowRadius, 1, 8) : 4;
                for (int spread = glowRadius; spread >= 1; spread--)
                    surface.StrokeRounded(Rectangle.Inflate(rectangle, spread, spread), EffectiveRoundness(document, rectangle) + spread, 1, glow);
            }
            int outlineWidth = document.CustomStyleEnabled ? document.PlateOutlineWidth : 2;
            if (outlineWidth > 0)
                surface.StrokeRounded(rectangle, EffectiveRoundness(document, rectangle), outlineWidth, KeyAccent(document));
            Rectangle inner = Rectangle.Inflate(rectangle, -outlineWidth, -outlineWidth);
            Color hot = document.CustomStyleEnabled && document.HoverEnabled
                ? Color.FromArgb(Math.Clamp(document.HoverStrength * 2, 0, 200), Hover(document))
                : Theme.KeyHot;
            surface.FillRounded(inner, Math.Max(0, EffectiveRoundness(document, rectangle) - outlineWidth), selected || active ? hot : PlateFill(document));
        }
        else
        {
            DrawPlate(surface, document, rectangle, selected || active);
        }
    }

    private int EffectiveRoundness(KeyboardDocument document, Rectangle rectangle)
        => Math.Min(document.CustomStyleEnabled ? document.KeyRoundness : 14, rectangle.Height / 2);

    private void DrawPlate(PixelSurface surface, KeyboardDocument document, Rectangle rectangle, bool hot,
        Color? fillOverride = null, Color? borderOverride = null, int? outlineWidthOverride = null)
    {
        Color border = borderOverride ?? KeyAccent(document);
        int roundness = Theme.Modern || document.CustomStyleEnabled ? EffectiveRoundness(document, rectangle) : 2;
        int outlineWidth = Math.Min(
            outlineWidthOverride ?? (document.CustomStyleEnabled ? document.PlateOutlineWidth : 1),
            Math.Max(0, Math.Min(rectangle.Width, rectangle.Height) / 2));
        if (outlineWidth > 0)
            surface.StrokeRounded(rectangle, roundness, outlineWidth, border);
        Rectangle inner = Rectangle.Inflate(rectangle, -outlineWidth, -outlineWidth);
        Color hotColor = document.CustomStyleEnabled && document.HoverEnabled
            ? Color.FromArgb(Math.Clamp(document.HoverStrength * 2, 0, 200), Hover(document))
            : Theme.KeyHot;
        surface.FillRounded(inner, Theme.Modern || document.CustomStyleEnabled ? Math.Max(0, roundness - 1) : 1,
            hot ? hotColor : (fillOverride ?? PlateFill(document)));
    }

    private void DrawKeyContent(PixelSurface surface, KeyboardDocument document, KeyboardKey key, Rectangle rectangle, bool selected)
    {
        bool isArrow = key.Character is '\x04' or '\x05' or '\x06' or '\x07';
        if (isArrow)
        {
            Rectangle arrowRectangle = Rectangle.Round(KeyContentRectangle(document, key));
            DrawArrow(surface, arrowRectangle, key.Character, selected ? Hover(document) : Ink(document));
            return;
        }
        if (key.Character == ' ' && string.IsNullOrEmpty(key.Label))
        {
            if (ShowsParchmentRibbon(document, key))
                DrawSpacebar(surface, Rectangle.Round(KeyContentRectangle(document, key)), selected ? Hover(document) : Ink(document));
            return;
        }

        string label = State == KeyboardPreviewState.Lower ? key.Label : key.ShiftLabel;
        float hoverOffset = selected && Pressed ? 2 : 0;
        DrawStyledText(surface, document, label, rectangle,
            key.LabelOffsetX,
            key.LabelOffsetY + hoverOffset,
            key.LabelScale,
            selected && (!document.CustomStyleEnabled || document.HoverEnabled) ? Hover(document) : Ink(document));
    }

    private void DrawSpacebar(PixelSurface surface, Rectangle rectangle, Color color)
    {
        Bitmap? image = SpacebarImage();
        if (image is null)
        {
            int y = rectangle.Top + rectangle.Height / 2;
            for (int x = rectangle.Left + rectangle.Width / 4; x < rectangle.Right - rectangle.Width / 4; x++)
                surface.BlendPixel(x, y, color);
            return;
        }

        for (int y = 0; y < rectangle.Height; y++)
        {
            int sourceY = Math.Clamp(y * image.Height / Math.Max(1, rectangle.Height), 0, image.Height - 1);
            for (int x = 0; x < rectangle.Width; x++)
            {
                int sourceX = Math.Clamp(x * image.Width / Math.Max(1, rectangle.Width), 0, image.Width - 1);
                Color sample = image.GetPixel(sourceX, sourceY);
                if (sample.A >= 250 && (sample.R + sample.G + sample.B) / 3 < 180)
                    surface.BlendPixel(rectangle.Left + x, rectangle.Top + y, color);
            }
        }
    }

    private bool ShowsParchmentRibbon(KeyboardDocument document, KeyboardKey key)
        => document.ParchmentRibbonEnabled
            && Theme.Name.Equals("Parchment", StringComparison.OrdinalIgnoreCase)
            && key.Character == ' ' && string.IsNullOrEmpty(key.Label);

    private Bitmap? SpacebarImage()
    {
        string path = Path.Combine(AssetsDirectory, "spacebar.png");
        if (_spacebarImage is not null && string.Equals(_spacebarImagePath, path, StringComparison.OrdinalIgnoreCase))
            return _spacebarImage;
        _spacebarImage?.Dispose();
        _spacebarImage = File.Exists(path) ? new Bitmap(path) : null;
        _spacebarImagePath = path;
        return _spacebarImage;
    }

    private static void DrawArrow(PixelSurface surface, Rectangle rectangle, char direction, Color color)
    {
        int centerX = rectangle.Left + rectangle.Width / 2;
        int centerY = rectangle.Top + rectangle.Height / 2;
        int triangleWidth = Math.Max(4, rectangle.Width);
        int triangleHeight = Math.Max(4, rectangle.Height);
        if (direction is '\x04' or '\x05')
        {
            for (int row = 0; row < triangleHeight; row++)
            {
                int halfWidth = (int)((1f - row / (float)(triangleHeight - 1)) * triangleWidth / 2);
                int y = direction == '\x04' ? centerY + triangleHeight / 2 - row : centerY - triangleHeight / 2 + row;
                for (int x = -halfWidth; x <= halfWidth; x++)
                    surface.BlendPixel(centerX + x, y, color);
            }
        }
        else
        {
            for (int column = 0; column < triangleWidth; column++)
            {
                int halfHeight = (int)((1f - column / (float)(triangleWidth - 1)) * triangleHeight / 2);
                int x = direction == '\x06' ? centerX + triangleWidth / 2 - column : centerX - triangleWidth / 2 + column;
                for (int y = -halfHeight; y <= halfHeight; y++)
                    surface.BlendPixel(x, centerY + y, color);
            }
        }
    }

    public RectangleF RuntimeControlRectangle(KeyboardDocument document, KeyboardRuntimeControl control)
    {
        KeyboardControlDesign design = document.GetControlDesign(control);
        return control switch
        {
            KeyboardRuntimeControl.Size => new RectangleF(39 + document.SizeControlOffsetX, 224 + document.SizeControlOffsetY, design.Width, design.Height),
            KeyboardRuntimeControl.Opacity => new RectangleF(918 + document.OpacityControlOffsetX, 60 + document.OpacityControlOffsetY, design.Width, design.Height),
            _ => new RectangleF(918 + document.TiltControlOffsetX, 270 + document.TiltControlOffsetY, design.Width, design.Height)
        };
    }

    public RectangleF RuntimeControlPartRectangle(KeyboardDocument document, KeyboardRuntimeControl control, KeyboardControlPart part)
    {
        RectangleF group = RuntimeControlRectangle(document, control);
        KeyboardControlDesign design = document.GetControlDesign(control);
        return part switch
        {
            KeyboardControlPart.UpArrow => new RectangleF(
                group.Left + (group.Width - design.UpWidth) / 2f + design.UpOffsetX,
                group.Top + design.UpOffsetY, design.UpWidth, design.UpHeight),
            KeyboardControlPart.DownArrow => new RectangleF(
                group.Left + (group.Width - design.DownWidth) / 2f + design.DownOffsetX,
                group.Bottom - design.DownHeight + design.DownOffsetY, design.DownWidth, design.DownHeight),
            KeyboardControlPart.Label => ControlTextRectangle(document, control, label: true),
            KeyboardControlPart.Value => ControlTextRectangle(document, control, label: false),
            _ => group
        };
    }

    public bool IsPointOnRuntimeControlText(KeyboardDocument document, KeyboardRuntimeControl control, bool label, PointF point)
    {
        RectangleF group = RuntimeControlRectangle(document, control);
        KeyboardControlDesign design = document.GetControlDesign(control);
        RectangleF row = new(group.Left, group.Top + (label ? 21 : 45), group.Width, 25);
        string text = ControlText(control, label);
        return Font.HitTestText(text, row,
            label ? design.LabelOffsetX : design.ValueOffsetX,
            label ? design.LabelOffsetY : design.ValueOffsetY,
            label ? design.LabelScale : design.ValueScale, point);
    }

    private RectangleF ControlTextRectangle(KeyboardDocument document, KeyboardRuntimeControl control, bool label)
    {
        RectangleF group = RuntimeControlRectangle(document, control);
        KeyboardControlDesign design = document.GetControlDesign(control);
        RectangleF row = new(group.Left, group.Top + (label ? 21 : 45), group.Width, 25);
        RectangleF visible = Font.VisibleTextRectangle(ControlText(control, label), row,
            label ? design.LabelOffsetX : design.ValueOffsetX,
            label ? design.LabelOffsetY : design.ValueOffsetY,
            label ? design.LabelScale : design.ValueScale);
        return visible.IsEmpty ? new RectangleF(row.Left + row.Width / 2 - 2, row.Top + row.Height / 2 - 2, 4, 4)
            : RectangleF.Inflate(visible, 1, 1);
    }

    private static string ControlText(KeyboardRuntimeControl control, bool label) => (control, label) switch
    {
        (KeyboardRuntimeControl.Size, true) => "size",
        (KeyboardRuntimeControl.Size, false) => "100%",
        (KeyboardRuntimeControl.Opacity, true) => "opac",
        (KeyboardRuntimeControl.Opacity, false) => "30%",
        (KeyboardRuntimeControl.Tilt, true) => "tilt",
        _ => "23"
    };

    private void DrawRuntimeControls(PixelSurface surface, KeyboardDocument document)
    {
        DrawRuntimeControl(surface, document, KeyboardRuntimeControl.Size, "size", "100%");
        DrawRuntimeControl(surface, document, KeyboardRuntimeControl.Opacity, "opac", "30%");
        DrawRuntimeControl(surface, document, KeyboardRuntimeControl.Tilt, "tilt", "23");
    }

    private void DrawRuntimeControl(PixelSurface surface, KeyboardDocument document,
        KeyboardRuntimeControl control, string label, string value)
    {
        KeyboardControlDesign design = document.GetControlDesign(control);
        Rectangle box = Rectangle.Round(RuntimeControlRectangle(document, control));
        Color ink = Ink(document);
        DrawControlArrow(surface, document, Rectangle.Round(RuntimeControlPartRectangle(document, control, KeyboardControlPart.UpArrow)), false, ink);
        DrawStyledText(surface, document, label, new Rectangle(box.Left, box.Top + 21, box.Width, 25),
            design.LabelOffsetX, design.LabelOffsetY, design.LabelScale, ink);
        DrawStyledText(surface, document, value, new Rectangle(box.Left, box.Top + 45, box.Width, 25),
            design.ValueOffsetX, design.ValueOffsetY, design.ValueScale, ink);
        DrawControlArrow(surface, document, Rectangle.Round(RuntimeControlPartRectangle(document, control, KeyboardControlPart.DownArrow)), true, ink);
    }

    private void DrawControlArrow(PixelSurface surface, KeyboardDocument document, Rectangle rectangle, bool down, Color fallback)
    {
        int width = Math.Max(1, rectangle.Width);
        int height = Math.Max(1, rectangle.Height);
        var arrow = new PixelSurface(width, height);
        if (!string.IsNullOrWhiteSpace(document.ControlArrowImagePath) && File.Exists(document.ControlArrowImagePath))
        {
            using var image = new Bitmap(document.ControlArrowImagePath);
            arrow.BlendBitmap(image, new Rectangle(0, 0, width, height), 100, 0,
                document.ControlArrowRotation + (down ? 180 : 0));
        }
        else
        {
            DrawArrow(arrow, Rectangle.Inflate(new Rectangle(0, 0, width, height), -2, -2),
                down ? '\x05' : '\x04', fallback);
        }

        if (document.ControlArrowGlowEnabled && document.ControlArrowGlowStrength > 0)
        {
            int radius = Math.Clamp(document.ControlArrowGlowRadius, 1, 48);
            PixelSurface glow = CreateGlowSurface(arrow, document.ControlArrowGlowColor, radius);
            int glowOpacity = AnimatedOpacity(document.ControlArrowGlowStrength,
                document.ControlArrowBreatheEnabled, document.ControlArrowBreatheMinPercent,
                document.ControlArrowBreathePeriodSeconds, document.ControlArrowBreathePhaseDegrees);
            surface.BlendSurface(glow, rectangle.Left - radius, rectangle.Top - radius, glowOpacity);
        }
        surface.BlendSurface(arrow, rectangle.Left, rectangle.Top, 100);
    }

    private int AnimatedOpacity(int baseOpacity, bool enabled, int minimumPercent, float periodSeconds, float phaseDegrees)
        => Math.Clamp((int)Math.Round(baseOpacity * (enabled
            ? BreatheMultiplier(minimumPercent, periodSeconds, phaseDegrees)
            : 1.0)), 0, 100);

    private double BreatheMultiplier(int minimumPercent, float periodSeconds, float phaseDegrees)
    {
        double minimum = Math.Clamp(minimumPercent, 0, 100) / 100.0;
        double period = Math.Clamp(periodSeconds, 0.5f, 10f);
        double angle = AnimationTimeSeconds * Math.PI * 2.0 / period
            + phaseDegrees * Math.PI / 180.0;
        double wave = 0.5 - 0.5 * Math.Cos(angle);
        return minimum + (1.0 - minimum) * wave;
    }
}
