using System.Globalization;
using System.Text;

namespace OCUKeyboardStudio;

internal enum KeyboardRuntimeControl
{
    Size,
    Opacity,
    Tilt
}

internal enum KeyboardTopElement
{
    TextBar,
    Mode,
    Lock
}

internal enum KeyboardControlPart
{
    Group,
    UpArrow,
    Label,
    Value,
    DownArrow
}

internal sealed class KeyboardControlDesign
{
    public float Width { get; set; } = 92;
    public float Height { get; set; } = 98;
    public float UpOffsetX { get; set; }
    public float UpOffsetY { get; set; }
    public float UpWidth { get; set; } = 24;
    public float UpHeight { get; set; } = 20;
    public float DownOffsetX { get; set; }
    public float DownOffsetY { get; set; }
    public float DownWidth { get; set; } = 24;
    public float DownHeight { get; set; } = 20;
    public float LabelOffsetX { get; set; }
    public float LabelOffsetY { get; set; }
    public float LabelScale { get; set; } = 0.58f;
    public float ValueOffsetX { get; set; }
    public float ValueOffsetY { get; set; }
    public float ValueScale { get; set; } = 0.58f;

    public KeyboardControlDesign Clone() => (KeyboardControlDesign)MemberwiseClone();
}

internal sealed class KeyboardKey
{
    public int Id { get; set; }
    public char Character { get; set; }
    public char ShiftCharacter { get; set; }
    public float X { get; set; }
    public float Y { get; set; }
    public float Width { get; set; } = 1f;
    public float Height { get; set; } = 1f;
    public string Label { get; set; } = "";
    public string ShiftLabel { get; set; } = "";
    public float LabelOffsetX { get; set; }
    public float LabelOffsetY { get; set; }
    public float LabelScale { get; set; } = 1f;
    public bool SpansToRight { get; set; }

    public KeyboardKey Clone() => (KeyboardKey)MemberwiseClone();
}

internal sealed class KeyboardSprite
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string? SourcePath { get; set; }
    public string? FileName { get; set; }
    public float X { get; set; } = 120;
    public float Y { get; set; } = 70;
    public float Width { get; set; } = 320;
    public float Height { get; set; } = 80;
    public int Opacity { get; set; } = 100;
    public int EdgeFade { get; set; }
    public float Rotation { get; set; }
    public bool GlowEnabled { get; set; }
    public Color GlowColor { get; set; } = Color.FromArgb(255, 132, 242, 158);
    public int GlowStrength { get; set; } = 55;
    public int GlowRadius { get; set; } = 12;
    public bool BreatheEnabled { get; set; }
    public int BreatheMinPercent { get; set; } = 35;
    public float BreathePeriodSeconds { get; set; } = 2f;
    public float BreathePhaseDegrees { get; set; }

    public KeyboardSprite Clone() => (KeyboardSprite)MemberwiseClone();
    public override string ToString() => Path.GetFileName(SourcePath ?? FileName ?? "Sprite");
}

internal sealed class KeyboardDocument
{
    // Uses the reserved upper end of the existing sprite filename allow-list so
    // older format-v1 Studio builds can still import new .ocukb packages. They
    // ignore the metadata comment and harmlessly leave this asset unused.
    public const string ModeVrArtworkPortableName = "OCUKeyboardSprite9995.png";
    public const string ModePcArtworkPortableName = "OCUKeyboardSprite9996.png";
    public const string LockWorldArtworkPortableName = "OCUKeyboardSprite9997.png";
    public const string LockHeadArtworkPortableName = "OCUKeyboardSprite9998.png";
    public const string ConsoleInputBackgroundPortableName = "OCUKeyboardSprite9999.png";

    public int Width { get; set; } = 15;
    public List<KeyboardKey> Keys { get; } = [];
    public string? SourcePath { get; set; }
    public bool IsDirty { get; set; }

    // Portable design identity and optional visual overrides. Theme/font live
    // in the layout so shared keyboard mods never have to replace a user's INI.
    public string BaseTheme { get; set; } = "parchment";
    public string FontName { get; set; } = "parchment";
    public string? CustomFontMetadataPath { get; set; }
    public string? CustomFontTexturePath { get; set; }
    public bool CustomStyleEnabled { get; set; }
    public bool CustomStyleInitialized { get; set; }
    public bool KeyPlatesEnabled { get; set; } = true;
    public bool TopButtonPlatesEnabled { get; set; } = true;
    public bool InputBarPlateEnabled { get; set; } = true;
    public bool ParchmentRibbonEnabled { get; set; } = true;
    public Color FontColor { get; set; } = Color.FromArgb(255, 237, 240, 245);
    public Color FontOutlineColor { get; set; } = Color.FromArgb(220, 8, 11, 15);
    public Color FontGlowColor { get; set; } = Color.FromArgb(255, 132, 242, 158);
    public bool FontGlowEnabled { get; set; }
    public int FontGlowStrength { get; set; } = 45;
    public int FontGlowRadius { get; set; } = 3;
    public bool FontBreatheEnabled { get; set; }
    public int FontBreatheMinPercent { get; set; } = 35;
    public float FontBreathePeriodSeconds { get; set; } = 2f;
    public float FontBreathePhaseDegrees { get; set; }
    public Color KeyColor { get; set; } = Color.FromArgb(205, 62, 190, 143);
    public Color PlateFillColor { get; set; } = Color.FromArgb(175, 22, 26, 33);
    public int PlateOutlineWidth { get; set; } = 2;
    public bool InputFillOverrideEnabled { get; set; }
    public Color InputFillColor { get; set; } = Color.FromArgb(235, 14, 17, 22);
    public bool InputOutlineOverrideEnabled { get; set; }
    public bool InputOutlineVisible { get; set; } = true;
    public Color InputOutlineColor { get; set; } = Color.FromArgb(205, 62, 190, 143);
    public int InputOutlineWidth { get; set; } = 2;
    public float InputTitleOffsetX { get; set; }
    public float InputTitleOffsetY { get; set; }
    public float InputTextOffsetX { get; set; }
    public float InputTextOffsetY { get; set; }
    public string? ConsoleInputBackgroundImagePath { get; set; }
    public string? ConsoleInputBackgroundFileName { get; set; }
    public string? ModeVrArtworkImagePath { get; set; }
    public string? ModeVrArtworkFileName { get; set; }
    public string? ModePcArtworkImagePath { get; set; }
    public string? ModePcArtworkFileName { get; set; }
    public string? LockWorldArtworkImagePath { get; set; }
    public string? LockWorldArtworkFileName { get; set; }
    public string? LockHeadArtworkImagePath { get; set; }
    public string? LockHeadArtworkFileName { get; set; }
    public bool ModeTextOverArtwork { get; set; }
    public bool LockTextOverArtwork { get; set; }
    public Color GlowColor { get; set; } = Color.FromArgb(255, 132, 242, 158);
    public Color HoverColor { get; set; } = Color.FromArgb(255, 132, 242, 158);
    public bool GlowEnabled { get; set; } = true;
    public int GlowStrength { get; set; } = 45;
    public int GlowRadius { get; set; } = 4;
    public bool HoverEnabled { get; set; } = true;
    public int HoverStrength { get; set; } = 55;
    public bool OutlineEnabled { get; set; } = true;
    public int KeyRoundness { get; set; } = 14;
    public bool KeyBreatheEnabled { get; set; }
    public int KeyBreatheMinPercent { get; set; } = 35;
    public float KeyBreathePeriodSeconds { get; set; } = 2f;
    public float KeyBreathePhaseDegrees { get; set; }

    // Artwork sources are authoring paths. The serialized layout references
    // portable filenames copied beside it or into an MO2 root folder.
    public string? BackgroundImagePath { get; set; }
    public string? BackgroundFileName { get; set; }
    public float BackgroundX { get; set; }
    public float BackgroundY { get; set; }
    public float BackgroundWidth { get; set; } = 1024;
    public float BackgroundHeight { get; set; } = 560;
    public int BackgroundOpacity { get; set; } = 100;
    public int BackgroundEdgeFade { get; set; }
    public float BackgroundRotation { get; set; }
    public int BackgroundRoundness { get; set; }
    public bool BackgroundBreatheEnabled { get; set; }
    public int BackgroundBreatheMinPercent { get; set; } = 35;
    public float BackgroundBreathePeriodSeconds { get; set; } = 2f;
    public float BackgroundBreathePhaseDegrees { get; set; }
    public List<KeyboardSprite> Sprites { get; } = [];
    public string? ControlArrowImagePath { get; set; }
    public string? ControlArrowFileName { get; set; }
    public float ControlArrowRotation { get; set; }
    public bool ControlArrowGlowEnabled { get; set; }
    public Color ControlArrowGlowColor { get; set; } = Color.FromArgb(255, 132, 242, 158);
    public int ControlArrowGlowStrength { get; set; } = 55;
    public int ControlArrowGlowRadius { get; set; } = 6;
    public bool ControlArrowBreatheEnabled { get; set; }
    public int ControlArrowBreatheMinPercent { get; set; } = 35;
    public float ControlArrowBreathePeriodSeconds { get; set; } = 2f;
    public float ControlArrowBreathePhaseDegrees { get; set; }

    // Legacy single-overlay fields are parsed and migrated into Sprites.
    public string? OverlayImagePath { get; set; }
    public string? OverlayFileName { get; set; }
    public float OverlayX { get; set; } = 120;
    public float OverlayY { get; set; } = 70;
    public float OverlayWidth { get; set; } = 784;
    public float OverlayHeight { get; set; } = 80;
    public int OverlayOpacity { get; set; } = 100;

    // Exact pixel offsets from OCU's native control positions.
    public float SizeControlOffsetX { get; set; }
    public float SizeControlOffsetY { get; set; }
    public float OpacityControlOffsetX { get; set; }
    public float OpacityControlOffsetY { get; set; }
    public float TiltControlOffsetX { get; set; }
    public float TiltControlOffsetY { get; set; }
    public float TextBarOffsetX { get; set; }
    public float TextBarOffsetY { get; set; }
    public float TextBarWidth { get; set; }
    public float TextBarHeight { get; set; }
    public float TextBarFontScale { get; set; }
    public float ModeButtonOffsetX { get; set; }
    public float ModeButtonOffsetY { get; set; }
    public float ModeButtonWidth { get; set; }
    public float ModeButtonHeight { get; set; }
    public float ModeButtonFontScale { get; set; }
    public float ModeArtworkOffsetX { get; set; }
    public float ModeArtworkOffsetY { get; set; }
    public float ModeArtworkWidth { get; set; }
    public float ModeArtworkHeight { get; set; }
    public float ModeTextOffsetX { get; set; }
    public float ModeTextOffsetY { get; set; }
    public float LockButtonOffsetX { get; set; }
    public float LockButtonOffsetY { get; set; }
    public float LockButtonWidth { get; set; }
    public float LockButtonHeight { get; set; }
    public float LockButtonFontScale { get; set; }
    public float LockArtworkOffsetX { get; set; }
    public float LockArtworkOffsetY { get; set; }
    public float LockArtworkWidth { get; set; }
    public float LockArtworkHeight { get; set; }
    public float LockTextOffsetX { get; set; }
    public float LockTextOffsetY { get; set; }
    public KeyboardControlDesign SizeControlDesign { get; set; } = new();
    public KeyboardControlDesign OpacityControlDesign { get; set; } = new();
    public KeyboardControlDesign TiltControlDesign { get; set; } = new();

    public KeyboardDocument Clone()
    {
        var clone = new KeyboardDocument
        {
            Width = Width,
            SourcePath = SourcePath,
            IsDirty = IsDirty,
            BaseTheme = BaseTheme,
            FontName = FontName,
            CustomFontMetadataPath = CustomFontMetadataPath,
            CustomFontTexturePath = CustomFontTexturePath,
            CustomStyleEnabled = CustomStyleEnabled,
            CustomStyleInitialized = CustomStyleInitialized,
            KeyPlatesEnabled = KeyPlatesEnabled,
            TopButtonPlatesEnabled = TopButtonPlatesEnabled,
            InputBarPlateEnabled = InputBarPlateEnabled,
            ParchmentRibbonEnabled = ParchmentRibbonEnabled,
            FontColor = FontColor,
            FontOutlineColor = FontOutlineColor,
            FontGlowColor = FontGlowColor,
            FontGlowEnabled = FontGlowEnabled,
            FontGlowStrength = FontGlowStrength,
            FontGlowRadius = FontGlowRadius,
            FontBreatheEnabled = FontBreatheEnabled,
            FontBreatheMinPercent = FontBreatheMinPercent,
            FontBreathePeriodSeconds = FontBreathePeriodSeconds,
            FontBreathePhaseDegrees = FontBreathePhaseDegrees,
            KeyColor = KeyColor,
            PlateFillColor = PlateFillColor,
            PlateOutlineWidth = PlateOutlineWidth,
            InputFillOverrideEnabled = InputFillOverrideEnabled,
            InputFillColor = InputFillColor,
            InputOutlineOverrideEnabled = InputOutlineOverrideEnabled,
            InputOutlineVisible = InputOutlineVisible,
            InputOutlineColor = InputOutlineColor,
            InputOutlineWidth = InputOutlineWidth,
            InputTitleOffsetX = InputTitleOffsetX,
            InputTitleOffsetY = InputTitleOffsetY,
            InputTextOffsetX = InputTextOffsetX,
            InputTextOffsetY = InputTextOffsetY,
            ConsoleInputBackgroundImagePath = ConsoleInputBackgroundImagePath,
            ConsoleInputBackgroundFileName = ConsoleInputBackgroundFileName,
            ModeVrArtworkImagePath = ModeVrArtworkImagePath,
            ModeVrArtworkFileName = ModeVrArtworkFileName,
            ModePcArtworkImagePath = ModePcArtworkImagePath,
            ModePcArtworkFileName = ModePcArtworkFileName,
            LockWorldArtworkImagePath = LockWorldArtworkImagePath,
            LockWorldArtworkFileName = LockWorldArtworkFileName,
            LockHeadArtworkImagePath = LockHeadArtworkImagePath,
            LockHeadArtworkFileName = LockHeadArtworkFileName,
            ModeTextOverArtwork = ModeTextOverArtwork,
            LockTextOverArtwork = LockTextOverArtwork,
            GlowColor = GlowColor,
            HoverColor = HoverColor,
            GlowEnabled = GlowEnabled,
            GlowStrength = GlowStrength,
            GlowRadius = GlowRadius,
            HoverEnabled = HoverEnabled,
            HoverStrength = HoverStrength,
            OutlineEnabled = OutlineEnabled,
            KeyRoundness = KeyRoundness,
            KeyBreatheEnabled = KeyBreatheEnabled,
            KeyBreatheMinPercent = KeyBreatheMinPercent,
            KeyBreathePeriodSeconds = KeyBreathePeriodSeconds,
            KeyBreathePhaseDegrees = KeyBreathePhaseDegrees,
            BackgroundImagePath = BackgroundImagePath,
            BackgroundFileName = BackgroundFileName,
            BackgroundX = BackgroundX,
            BackgroundY = BackgroundY,
            BackgroundWidth = BackgroundWidth,
            BackgroundHeight = BackgroundHeight,
            BackgroundOpacity = BackgroundOpacity,
            BackgroundEdgeFade = BackgroundEdgeFade,
            BackgroundRotation = BackgroundRotation,
            BackgroundRoundness = BackgroundRoundness,
            BackgroundBreatheEnabled = BackgroundBreatheEnabled,
            BackgroundBreatheMinPercent = BackgroundBreatheMinPercent,
            BackgroundBreathePeriodSeconds = BackgroundBreathePeriodSeconds,
            BackgroundBreathePhaseDegrees = BackgroundBreathePhaseDegrees,
            ControlArrowImagePath = ControlArrowImagePath,
            ControlArrowFileName = ControlArrowFileName,
            ControlArrowRotation = ControlArrowRotation,
            ControlArrowGlowEnabled = ControlArrowGlowEnabled,
            ControlArrowGlowColor = ControlArrowGlowColor,
            ControlArrowGlowStrength = ControlArrowGlowStrength,
            ControlArrowGlowRadius = ControlArrowGlowRadius,
            ControlArrowBreatheEnabled = ControlArrowBreatheEnabled,
            ControlArrowBreatheMinPercent = ControlArrowBreatheMinPercent,
            ControlArrowBreathePeriodSeconds = ControlArrowBreathePeriodSeconds,
            ControlArrowBreathePhaseDegrees = ControlArrowBreathePhaseDegrees,
            OverlayImagePath = OverlayImagePath,
            OverlayFileName = OverlayFileName,
            OverlayX = OverlayX,
            OverlayY = OverlayY,
            OverlayWidth = OverlayWidth,
            OverlayHeight = OverlayHeight,
            OverlayOpacity = OverlayOpacity,
            SizeControlOffsetX = SizeControlOffsetX,
            SizeControlOffsetY = SizeControlOffsetY,
            OpacityControlOffsetX = OpacityControlOffsetX,
            OpacityControlOffsetY = OpacityControlOffsetY,
            TiltControlOffsetX = TiltControlOffsetX,
            TiltControlOffsetY = TiltControlOffsetY,
            TextBarOffsetX = TextBarOffsetX,
            TextBarOffsetY = TextBarOffsetY,
            TextBarWidth = TextBarWidth,
            TextBarHeight = TextBarHeight,
            TextBarFontScale = TextBarFontScale,
            ModeButtonOffsetX = ModeButtonOffsetX,
            ModeButtonOffsetY = ModeButtonOffsetY,
            ModeButtonWidth = ModeButtonWidth,
            ModeButtonHeight = ModeButtonHeight,
            ModeButtonFontScale = ModeButtonFontScale,
            ModeArtworkOffsetX = ModeArtworkOffsetX,
            ModeArtworkOffsetY = ModeArtworkOffsetY,
            ModeArtworkWidth = ModeArtworkWidth,
            ModeArtworkHeight = ModeArtworkHeight,
            ModeTextOffsetX = ModeTextOffsetX,
            ModeTextOffsetY = ModeTextOffsetY,
            LockButtonOffsetX = LockButtonOffsetX,
            LockButtonOffsetY = LockButtonOffsetY,
            LockButtonWidth = LockButtonWidth,
            LockButtonHeight = LockButtonHeight,
            LockButtonFontScale = LockButtonFontScale,
            LockArtworkOffsetX = LockArtworkOffsetX,
            LockArtworkOffsetY = LockArtworkOffsetY,
            LockArtworkWidth = LockArtworkWidth,
            LockArtworkHeight = LockArtworkHeight,
            LockTextOffsetX = LockTextOffsetX,
            LockTextOffsetY = LockTextOffsetY,
            SizeControlDesign = SizeControlDesign.Clone(),
            OpacityControlDesign = OpacityControlDesign.Clone(),
            TiltControlDesign = TiltControlDesign.Clone()
        };
        clone.Keys.AddRange(Keys.Select(key => key.Clone()));
        clone.Sprites.AddRange(Sprites.Select(sprite => sprite.Clone()));
        return clone;
    }

    // History deliberately ignores SourcePath and IsDirty, but it must include
    // authoring-only asset paths that Serialize() converts to portable names.
    // This lets the editor discard click-only/no-op history entries without
    // accidentally treating two different source images or fonts as equal.
    public bool IsEquivalentForHistory(KeyboardDocument other)
    {
        if (!string.Equals(Serialize(), other.Serialize(), StringComparison.Ordinal)
            || CustomStyleInitialized != other.CustomStyleInitialized
            || !string.Equals(CustomFontMetadataPath, other.CustomFontMetadataPath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(CustomFontTexturePath, other.CustomFontTexturePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(BackgroundImagePath, other.BackgroundImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(BackgroundFileName, other.BackgroundFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ConsoleInputBackgroundImagePath, other.ConsoleInputBackgroundImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ConsoleInputBackgroundFileName, other.ConsoleInputBackgroundFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ModeVrArtworkImagePath, other.ModeVrArtworkImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ModeVrArtworkFileName, other.ModeVrArtworkFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ModePcArtworkImagePath, other.ModePcArtworkImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ModePcArtworkFileName, other.ModePcArtworkFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(LockWorldArtworkImagePath, other.LockWorldArtworkImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(LockWorldArtworkFileName, other.LockWorldArtworkFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(LockHeadArtworkImagePath, other.LockHeadArtworkImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(LockHeadArtworkFileName, other.LockHeadArtworkFileName, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ControlArrowImagePath, other.ControlArrowImagePath, StringComparison.OrdinalIgnoreCase)
            || !string.Equals(ControlArrowFileName, other.ControlArrowFileName, StringComparison.OrdinalIgnoreCase)
            || Sprites.Count != other.Sprites.Count)
            return false;

        for (int index = 0; index < Sprites.Count; index++)
        {
            KeyboardSprite left = Sprites[index];
            KeyboardSprite right = other.Sprites[index];
            if (left.Id != right.Id
                || !string.Equals(left.SourcePath, right.SourcePath, StringComparison.OrdinalIgnoreCase)
                || !string.Equals(left.FileName, right.FileName, StringComparison.OrdinalIgnoreCase))
                return false;
        }
        return true;
    }

    public bool HasBreathingEffects => (CustomStyleEnabled && GlowEnabled && KeyBreatheEnabled)
        || (CustomStyleEnabled && FontGlowEnabled && FontBreatheEnabled)
        || (BackgroundBreatheEnabled && (!string.IsNullOrWhiteSpace(BackgroundImagePath) || !string.IsNullOrWhiteSpace(BackgroundFileName)))
        || (ControlArrowGlowEnabled && ControlArrowBreatheEnabled)
        || Sprites.Any(sprite => sprite.GlowEnabled && sprite.BreatheEnabled);

    public void EnableCustomStyleFromTheme(KeyboardTheme theme)
    {
        if (!CustomStyleInitialized)
        {
            FontColor = theme.Ink;
            FontOutlineColor = Color.FromArgb(220, 8, 11, 15);
            FontGlowColor = theme.Bright;
            FontGlowEnabled = false;
            FontBreatheEnabled = false;
            KeyColor = theme.Accent;
            PlateFillColor = theme.KeyIdle;
            PlateOutlineWidth = theme.Modern ? 2 : 1;
            GlowColor = theme.Bright;
            GlowEnabled = theme.Modern;
            GlowStrength = theme.Modern ? 35 : 0;
            GlowRadius = 4;
            // Preserve the Base Theme's exact hover RGB when the first edited
            // appearance value promotes the theme into editable overrides.
            HoverColor = Color.FromArgb(255, theme.KeyHot.R, theme.KeyHot.G, theme.KeyHot.B);
            HoverEnabled = true;
            HoverStrength = Math.Clamp((int)Math.Round(theme.KeyHot.A / 2.0), 0, 100);
            OutlineEnabled = theme.Outline;
            KeyRoundness = theme.Modern ? 14 : 2;
            KeyBreatheEnabled = false;
            CustomStyleInitialized = true;
        }
        CustomStyleEnabled = true;
    }

    // Font coverage follows what this keyboard actually displays. We do not
    // reject a decorative font for Unicode characters or hypothetical keys
    // that are not present in the open layout.
    public SortedSet<char> RequiredFontCharacters()
    {
        var required = new SortedSet<char>();
        foreach (KeyboardKey key in Keys)
        {
            foreach (char character in key.Label)
                if (!char.IsControl(character)) required.Add(character);
            foreach (char character in key.ShiftLabel)
                if (!char.IsControl(character)) required.Add(character);
        }
        return required;
    }

    public KeyboardControlDesign GetControlDesign(KeyboardRuntimeControl control) => control switch
    {
        KeyboardRuntimeControl.Size => SizeControlDesign,
        KeyboardRuntimeControl.Opacity => OpacityControlDesign,
        _ => TiltControlDesign
    };

    private KeyboardControlDesign GetControlDesign(string name) => name.ToLowerInvariant() switch
    {
        "size" => SizeControlDesign,
        "opacity" => OpacityControlDesign,
        "tilt" => TiltControlDesign,
        _ => throw new FormatException($"Unknown runtime control '{name}'.")
    };

    public void ApplyThemeControlLayout(KeyboardTheme theme)
    {
        SizeControlOffsetX = theme.SizeControlOffsetX;
        SizeControlOffsetY = theme.SizeControlOffsetY;
        OpacityControlOffsetX = theme.OpacityControlOffsetX;
        OpacityControlOffsetY = theme.OpacityControlOffsetY;
        TiltControlOffsetX = theme.TiltControlOffsetX;
        TiltControlOffsetY = theme.TiltControlOffsetY;
        SizeControlDesign = theme.SizeControlDesign?.Clone() ?? new KeyboardControlDesign();
        OpacityControlDesign = theme.OpacityControlDesign?.Clone() ?? new KeyboardControlDesign();
        TiltControlDesign = theme.TiltControlDesign?.Clone() ?? new KeyboardControlDesign();
    }

    public static KeyboardDocument Load(string path)
    {
        var document = Parse(File.ReadAllText(path, Encoding.UTF8));
        document.SourcePath = path;
        string directory = Path.GetDirectoryName(path) ?? "";
        if (!string.IsNullOrWhiteSpace(document.BackgroundFileName))
        {
            string candidate = Path.Combine(directory, document.BackgroundFileName);
            if (File.Exists(candidate))
                document.BackgroundImagePath = candidate;
        }
        if (!string.IsNullOrWhiteSpace(document.ConsoleInputBackgroundFileName))
        {
            string candidate = Path.Combine(directory, document.ConsoleInputBackgroundFileName);
            if (File.Exists(candidate))
                document.ConsoleInputBackgroundImagePath = candidate;
        }
        ResolvePortableArtwork(directory, document.ModeVrArtworkFileName,
            path => document.ModeVrArtworkImagePath = path);
        ResolvePortableArtwork(directory, document.ModePcArtworkFileName,
            path => document.ModePcArtworkImagePath = path);
        ResolvePortableArtwork(directory, document.LockWorldArtworkFileName,
            path => document.LockWorldArtworkImagePath = path);
        ResolvePortableArtwork(directory, document.LockHeadArtworkFileName,
            path => document.LockHeadArtworkImagePath = path);
        if (!string.IsNullOrWhiteSpace(document.OverlayFileName))
        {
            string candidate = Path.Combine(directory, document.OverlayFileName);
            if (File.Exists(candidate))
                document.OverlayImagePath = candidate;
        }
        foreach (KeyboardSprite sprite in document.Sprites)
        {
            if (string.IsNullOrWhiteSpace(sprite.FileName))
                continue;
            string candidate = Path.Combine(directory, sprite.FileName);
            if (File.Exists(candidate))
                sprite.SourcePath = candidate;
        }
        if (!string.IsNullOrWhiteSpace(document.ControlArrowFileName))
        {
            string candidate = Path.Combine(directory, document.ControlArrowFileName);
            if (File.Exists(candidate))
                document.ControlArrowImagePath = candidate;
        }
        if (document.FontName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase))
        {
            string metadata = Path.Combine(directory, "OCUKeyboardFont.sfn");
            string texture = Path.Combine(directory, "OCUKeyboardFont.png");
            if (File.Exists(metadata) && File.Exists(texture))
            {
                document.CustomFontMetadataPath = metadata;
                document.CustomFontTexturePath = texture;
            }
        }
        document.IsDirty = false;
        return document;
    }

    public static KeyboardDocument Parse(string contents)
    {
        var document = new KeyboardDocument();
        KeyboardKey? last = null;

        KeyboardKey AddKey(char character, char shifted, float x, float y)
        {
            var key = new KeyboardKey
            {
                Id = document.Keys.Count,
                Character = character,
                ShiftCharacter = shifted,
                X = x,
                Y = y,
                Label = character.ToString(),
                ShiftLabel = shifted.ToString()
            };
            document.Keys.Add(key);
            last = key;
            return key;
        }

        foreach (string rawLine in contents.Replace("\r\n", "\n").Split('\n'))
        {
            if (TryParseOcuCommentMetadata(document, rawLine))
                continue;
            if (rawLine.TrimStart().StartsWith('#'))
                continue;
            List<string> tokens = Tokenize(rawLine);
            if (tokens.Count == 0 || tokens[0].StartsWith('#'))
                continue;

            string op = tokens[0];
            if (op.Equals("width", StringComparison.OrdinalIgnoreCase))
            {
                Require(tokens, 2, rawLine);
                document.Width = int.Parse(tokens[1], CultureInfo.InvariantCulture);
                continue;
            }

            if (TryParseVisualDirective(document, op, tokens, rawLine))
                continue;

            if (op.Equals("key", StringComparison.OrdinalIgnoreCase))
            {
                Require(tokens, 3, rawLine);
                char character = DecodeCharacter(tokens[1]);
                char shifted = DecodeCharacter(tokens[2]);
                float x;
                float y;
                if (tokens.Count >= 5)
                {
                    x = ParseFloat(tokens[3]);
                    y = ParseFloat(tokens[4]);
                }
                else
                {
                    if (last is null)
                        throw new FormatException("The first key must include x and y coordinates.");
                    x = last.X + last.Width;
                    y = last.Y;
                }

                KeyboardKey key = AddKey(character, shifted, x, y);
                if (tokens.Count >= 7)
                {
                    key.Width = ParseFloat(tokens[5]);
                    key.Height = ParseFloat(tokens[6]);
                }
                continue;
            }

            if (op.Equals("bank", StringComparison.OrdinalIgnoreCase))
            {
                Require(tokens, 3, rawLine);
                if (last is null)
                    throw new FormatException("A bank cannot be the first key in a layout.");
                string lower = tokens[1];
                string upper = tokens[2];
                if (lower.Length != upper.Length)
                    throw new FormatException($"Bank lower/upper length mismatch: '{lower}' and '{upper}'.");
                foreach ((char character, char shifted) in lower.Zip(upper))
                {
                    float x = last.X + last.Width;
                    float y = last.Y;
                    AddKey(character, shifted, x, y);
                }
                continue;
            }

            if (op.StartsWith('.'))
            {
                if (last is null)
                    throw new FormatException($"Property '{op}' has no preceding key.");
                string property = op[1..].ToLowerInvariant();
                switch (property)
                {
                    case "label":
                        last.Label = tokens.Count > 1 ? tokens[1] : "";
                        last.ShiftLabel = last.Label;
                        break;
                    case "label_shift":
                        last.ShiftLabel = tokens.Count > 1 ? tokens[1] : "";
                        break;
                    case "label_offset":
                        Require(tokens, 3, rawLine);
                        last.LabelOffsetX = ParseFloat(tokens[1]);
                        last.LabelOffsetY = ParseFloat(tokens[2]);
                        break;
                    case "label_scale":
                        Require(tokens, 2, rawLine);
                        last.LabelScale = Math.Clamp(ParseFloat(tokens[1]), 0.25f, 3f);
                        break;
                    case "spans_to_right":
                        last.SpansToRight = tokens.Count < 2 || !tokens[1].Equals("false", StringComparison.OrdinalIgnoreCase);
                        break;
                    default:
                        throw new FormatException($"Unknown key property '{op}'.");
                }
                continue;
            }

            throw new FormatException($"Unknown keyboard layout instruction '{op}'.");
        }

        if (document.Width <= 0)
            throw new FormatException("The layout width must be greater than zero.");
        if (document.Keys.Count == 0)
            throw new FormatException("The layout has no keys.");
        if (document.Sprites.Count == 0 && !string.IsNullOrWhiteSpace(document.OverlayFileName))
        {
            document.Sprites.Add(new KeyboardSprite
            {
                FileName = document.OverlayFileName,
                X = document.OverlayX,
                Y = document.OverlayY,
                Width = document.OverlayWidth,
                Height = document.OverlayHeight,
                Opacity = document.OverlayOpacity
            });
        }
        return document;
    }

    public void Save(string path)
    {
        File.WriteAllText(path, Serialize(), new UTF8Encoding(false));
        SourcePath = path;
        IsDirty = false;
    }

    public string Serialize()
    {
        var output = new StringBuilder();
        output.AppendLine("# OCU Keyboard Studio custom keyboard");
        output.AppendLine("# Label offsets are pixels in OCU's 1024x560 keyboard texture.");
        output.AppendLine();
        output.AppendLine($"width {Width.ToString(CultureInfo.InvariantCulture)}");
        output.AppendLine($"base_theme {SanitizeDesignName(BaseTheme, "parchment")}");
        output.AppendLine($"font {SanitizeDesignName(FontName, "parchment")}");
        output.AppendLine($"key_plates {KeyPlatesEnabled.ToString().ToLowerInvariant()}");
        output.AppendLine($"top_button_plates {TopButtonPlatesEnabled.ToString().ToLowerInvariant()}");
        output.AppendLine($"input_bar_plate {InputBarPlateEnabled.ToString().ToLowerInvariant()}");
        output.AppendLine($"parchment_ribbon {ParchmentRibbonEnabled.ToString().ToLowerInvariant()}");
        if (InputFillOverrideEnabled)
            output.AppendLine($"# ocu_input_fill_color {FormatColor(InputFillColor)}");
        if (!InputOutlineVisible)
            output.AppendLine("# ocu_input_outline_enabled false");
        if (InputOutlineOverrideEnabled)
        {
            // Comment metadata keeps this layout valid in older OCU builds;
            // those builds ignore it and retain the ordinary plate outline.
            output.AppendLine($"# ocu_input_outline_color {FormatColor(InputOutlineColor)}");
            output.AppendLine($"# ocu_input_outline_width {Math.Clamp(InputOutlineWidth, 0, 8)}");
        }
        if (Math.Abs(InputTitleOffsetX) > 0.0001f || Math.Abs(InputTitleOffsetY) > 0.0001f)
            output.AppendLine($"# ocu_input_title_offset {FormatFloat(InputTitleOffsetX)} {FormatFloat(InputTitleOffsetY)}");
        if (Math.Abs(InputTextOffsetX) > 0.0001f || Math.Abs(InputTextOffsetY) > 0.0001f)
            output.AppendLine($"# ocu_input_text_offset {FormatFloat(InputTextOffsetX)} {FormatFloat(InputTextOffsetY)}");
        if (!string.IsNullOrWhiteSpace(ConsoleInputBackgroundImagePath)
            || !string.IsNullOrWhiteSpace(ConsoleInputBackgroundFileName))
            output.AppendLine($"# ocu_console_input_background {ConsoleInputBackgroundPortableName}");
        AppendStateArtworkMetadata(output, "mode_vr", ModeVrArtworkImagePath, ModeVrArtworkFileName,
            ModeVrArtworkPortableName);
        AppendStateArtworkMetadata(output, "mode_pc", ModePcArtworkImagePath, ModePcArtworkFileName,
            ModePcArtworkPortableName);
        AppendStateArtworkMetadata(output, "lock_world", LockWorldArtworkImagePath, LockWorldArtworkFileName,
            LockWorldArtworkPortableName);
        AppendStateArtworkMetadata(output, "lock_head", LockHeadArtworkImagePath, LockHeadArtworkFileName,
            LockHeadArtworkPortableName);
        if (ModeTextOverArtwork)
            output.AppendLine("# ocu_top_state_text mode true");
        if (LockTextOverArtwork)
            output.AppendLine("# ocu_top_state_text lock true");
        if (Math.Abs(ModeArtworkOffsetX) > 0.0001f || Math.Abs(ModeArtworkOffsetY) > 0.0001f
            || ModeArtworkWidth > 0 || ModeArtworkHeight > 0)
            output.AppendLine($"# ocu_top_state_art_design mode {FormatFloat(ModeArtworkOffsetX)} {FormatFloat(ModeArtworkOffsetY)} {FormatFloat(ModeArtworkWidth)} {FormatFloat(ModeArtworkHeight)}");
        if (Math.Abs(LockArtworkOffsetX) > 0.0001f || Math.Abs(LockArtworkOffsetY) > 0.0001f
            || LockArtworkWidth > 0 || LockArtworkHeight > 0)
            output.AppendLine($"# ocu_top_state_art_design lock {FormatFloat(LockArtworkOffsetX)} {FormatFloat(LockArtworkOffsetY)} {FormatFloat(LockArtworkWidth)} {FormatFloat(LockArtworkHeight)}");
        if (Math.Abs(ModeTextOffsetX) > 0.0001f || Math.Abs(ModeTextOffsetY) > 0.0001f)
            output.AppendLine($"# ocu_top_state_text_offset mode {FormatFloat(ModeTextOffsetX)} {FormatFloat(ModeTextOffsetY)}");
        if (Math.Abs(LockTextOffsetX) > 0.0001f || Math.Abs(LockTextOffsetY) > 0.0001f)
            output.AppendLine($"# ocu_top_state_text_offset lock {FormatFloat(LockTextOffsetX)} {FormatFloat(LockTextOffsetY)}");
        output.AppendLine();

        if (CustomStyleEnabled)
        {
            output.AppendLine("style custom");
            output.AppendLine($"font_color {FormatColor(FontColor)}");
            output.AppendLine($"font_outline_color {FormatColor(FontOutlineColor)}");
            output.AppendLine($"font_glow_color {FormatColor(FontGlowColor)}");
            output.AppendLine($"font_glow_enabled {FontGlowEnabled.ToString().ToLowerInvariant()}");
            output.AppendLine($"font_glow_strength {Math.Clamp(FontGlowStrength, 0, 100)}");
            output.AppendLine($"font_glow_radius {Math.Clamp(FontGlowRadius, 1, 8)}");
            output.Append("font_breathe ").Append(FontBreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(Math.Clamp(FontBreatheMinPercent, 0, 100)).Append(' ')
                .Append(FormatFloat(Math.Clamp(FontBreathePeriodSeconds, 0.5f, 10f))).Append(' ')
                .AppendLine(FormatFloat(NormalizeRotation(FontBreathePhaseDegrees)));
            output.AppendLine($"key_color {FormatColor(KeyColor)}");
            output.AppendLine($"plate_fill_color {FormatColor(PlateFillColor)}");
            output.AppendLine($"plate_outline_width {Math.Clamp(PlateOutlineWidth, 0, 8)}");
            output.AppendLine($"glow_color {FormatColor(GlowColor)}");
            output.AppendLine($"hover_color {FormatColor(HoverColor)}");
            output.AppendLine($"glow_enabled {GlowEnabled.ToString().ToLowerInvariant()}");
            output.AppendLine($"glow_strength {Math.Clamp(GlowStrength, 0, 100)}");
            output.AppendLine($"glow_radius {Math.Clamp(GlowRadius, 1, 8)}");
            output.AppendLine($"hover_enabled {HoverEnabled.ToString().ToLowerInvariant()}");
            output.AppendLine($"hover_strength {Math.Clamp(HoverStrength, 0, 100)}");
            output.AppendLine($"label_outline {OutlineEnabled.ToString().ToLowerInvariant()}");
            output.AppendLine($"key_roundness {Math.Clamp(KeyRoundness, 0, 24)}");
            output.Append("key_breathe ").Append(KeyBreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(Math.Clamp(KeyBreatheMinPercent, 0, 100)).Append(' ')
                .Append(FormatFloat(Math.Clamp(KeyBreathePeriodSeconds, 0.5f, 10f))).Append(' ')
                .AppendLine(FormatFloat(NormalizeRotation(KeyBreathePhaseDegrees)));
            output.AppendLine();
        }
        if (!string.IsNullOrWhiteSpace(BackgroundImagePath) || !string.IsNullOrWhiteSpace(BackgroundFileName))
        {
            output.AppendLine("background OCUKeyboardBackground.png");
            output.Append("background_rect ")
                .Append(FormatFloat(BackgroundX)).Append(' ')
                .Append(FormatFloat(BackgroundY)).Append(' ')
                .Append(FormatFloat(BackgroundWidth)).Append(' ')
                .Append(FormatFloat(BackgroundHeight)).Append(' ')
                .Append(Math.Clamp(BackgroundOpacity, 0, 100)).Append(' ')
                .Append(Math.Clamp(BackgroundEdgeFade, 0, 300)).Append(' ')
                .Append(FormatFloat(NormalizeRotation(BackgroundRotation))).Append(' ')
                .Append(BackgroundBreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(Math.Clamp(BackgroundBreatheMinPercent, 0, 100)).Append(' ')
                .Append(FormatFloat(Math.Clamp(BackgroundBreathePeriodSeconds, 0.5f, 10f))).Append(' ')
                .Append(FormatFloat(NormalizeRotation(BackgroundBreathePhaseDegrees))).Append(' ')
                .AppendLine(Math.Clamp(BackgroundRoundness, 0, 100).ToString(CultureInfo.InvariantCulture));
        }
        for (int index = 0; index < Sprites.Count; index++)
        {
            KeyboardSprite sprite = Sprites[index];
            output.Append("sprite ").Append(SpriteFileName(index)).Append(' ')
                .Append(FormatFloat(sprite.X)).Append(' ')
                .Append(FormatFloat(sprite.Y)).Append(' ')
                .Append(FormatFloat(Math.Max(1, sprite.Width))).Append(' ')
                .Append(FormatFloat(Math.Max(1, sprite.Height))).Append(' ')
                .Append(Math.Clamp(sprite.Opacity, 0, 100)).Append(' ')
                .Append(Math.Clamp(sprite.EdgeFade, 0, 300)).Append(' ')
                .Append(FormatFloat(NormalizeRotation(sprite.Rotation))).Append(' ')
                .Append(sprite.BreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(Math.Clamp(sprite.BreatheMinPercent, 0, 100)).Append(' ')
                .Append(FormatFloat(Math.Clamp(sprite.BreathePeriodSeconds, 0.5f, 10f))).Append(' ')
                .Append(FormatFloat(NormalizeRotation(sprite.BreathePhaseDegrees))).Append(' ')
                .Append(sprite.GlowEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(FormatColor(sprite.GlowColor)).Append(' ')
                .Append(Math.Clamp(sprite.GlowStrength, 0, 100)).Append(' ')
                .AppendLine(Math.Clamp(sprite.GlowRadius, 1, 48).ToString(CultureInfo.InvariantCulture));
        }
        if (!string.IsNullOrWhiteSpace(BackgroundImagePath) || !string.IsNullOrWhiteSpace(BackgroundFileName) || Sprites.Count > 0)
            output.AppendLine();

        output.AppendLine($"control_offset size {FormatFloat(SizeControlOffsetX)} {FormatFloat(SizeControlOffsetY)}");
        output.AppendLine($"control_offset opacity {FormatFloat(OpacityControlOffsetX)} {FormatFloat(OpacityControlOffsetY)}");
        output.AppendLine($"control_offset tilt {FormatFloat(TiltControlOffsetX)} {FormatFloat(TiltControlOffsetY)}");
        output.AppendLine($"top_offset textbar {FormatFloat(TextBarOffsetX)} {FormatFloat(TextBarOffsetY)}");
        output.AppendLine($"top_offset mode {FormatFloat(ModeButtonOffsetX)} {FormatFloat(ModeButtonOffsetY)}");
        output.AppendLine($"top_offset lock {FormatFloat(LockButtonOffsetX)} {FormatFloat(LockButtonOffsetY)}");
        output.AppendLine($"top_design textbar {FormatFloat(TextBarWidth)} {FormatFloat(TextBarHeight)} {FormatFloat(TextBarFontScale)}");
        output.AppendLine($"top_design mode {FormatFloat(ModeButtonWidth)} {FormatFloat(ModeButtonHeight)} {FormatFloat(ModeButtonFontScale)}");
        output.AppendLine($"top_design lock {FormatFloat(LockButtonWidth)} {FormatFloat(LockButtonHeight)} {FormatFloat(LockButtonFontScale)}");
        WriteControlDesign(output, "size", SizeControlDesign);
        WriteControlDesign(output, "opacity", OpacityControlDesign);
        WriteControlDesign(output, "tilt", TiltControlDesign);
        if (!string.IsNullOrWhiteSpace(ControlArrowImagePath) || !string.IsNullOrWhiteSpace(ControlArrowFileName))
            output.Append("control_arrow OCUKeyboardControlArrow.png ")
                .Append(FormatFloat(NormalizeRotation(ControlArrowRotation))).Append(' ')
                .Append(ControlArrowBreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
                .Append(Math.Clamp(ControlArrowBreatheMinPercent, 0, 100)).Append(' ')
                .Append(FormatFloat(Math.Clamp(ControlArrowBreathePeriodSeconds, 0.5f, 10f))).Append(' ')
                .AppendLine(FormatFloat(NormalizeRotation(ControlArrowBreathePhaseDegrees)));
        output.Append("control_arrow_effect ")
            .Append(ControlArrowGlowEnabled.ToString().ToLowerInvariant()).Append(' ')
            .Append(FormatColor(ControlArrowGlowColor)).Append(' ')
            .Append(Math.Clamp(ControlArrowGlowStrength, 0, 100)).Append(' ')
            .Append(Math.Clamp(ControlArrowGlowRadius, 1, 48)).Append(' ')
            .Append(ControlArrowBreatheEnabled.ToString().ToLowerInvariant()).Append(' ')
            .Append(Math.Clamp(ControlArrowBreatheMinPercent, 0, 100)).Append(' ')
            .Append(FormatFloat(Math.Clamp(ControlArrowBreathePeriodSeconds, 0.5f, 10f))).Append(' ')
            .AppendLine(FormatFloat(NormalizeRotation(ControlArrowBreathePhaseDegrees)));
        output.AppendLine();

        foreach (KeyboardKey key in Keys)
        {
            output.Append("key ")
                .Append(EncodeCharacter(key.Character)).Append(' ')
                .Append(EncodeCharacter(key.ShiftCharacter)).Append(' ')
                .Append(FormatFloat(key.X)).Append(' ')
                .Append(FormatFloat(key.Y)).Append(' ')
                .Append(FormatFloat(key.Width)).Append(' ')
                .AppendLine(FormatFloat(key.Height));
            output.Append(".label ").AppendLine(Quote(key.Label));
            if (!string.Equals(key.ShiftLabel, key.Label, StringComparison.Ordinal))
                output.Append(".label_shift ").AppendLine(Quote(key.ShiftLabel));
            if (Math.Abs(key.LabelOffsetX) > 0.0001f || Math.Abs(key.LabelOffsetY) > 0.0001f)
                output.Append(".label_offset ").Append(FormatFloat(key.LabelOffsetX)).Append(' ').AppendLine(FormatFloat(key.LabelOffsetY));
            if (Math.Abs(key.LabelScale - 1f) > 0.0001f)
                output.Append(".label_scale ").AppendLine(FormatFloat(key.LabelScale));
            if (key.SpansToRight)
                output.AppendLine(".spans_to_right true");
            output.AppendLine();
        }

        return output.ToString();
    }

    private static void WriteControlDesign(StringBuilder output, string name, KeyboardControlDesign design)
    {
        output.Append("control_box ").Append(name).Append(' ')
            .Append(FormatFloat(Math.Max(24, design.Width))).Append(' ')
            .AppendLine(FormatFloat(Math.Max(40, design.Height)));
        output.Append("control_part ").Append(name).Append(" up ")
            .Append(FormatFloat(design.UpOffsetX)).Append(' ').Append(FormatFloat(design.UpOffsetY)).Append(' ')
            .Append(FormatFloat(Math.Max(4, design.UpWidth))).Append(' ').AppendLine(FormatFloat(Math.Max(4, design.UpHeight)));
        output.Append("control_part ").Append(name).Append(" down ")
            .Append(FormatFloat(design.DownOffsetX)).Append(' ').Append(FormatFloat(design.DownOffsetY)).Append(' ')
            .Append(FormatFloat(Math.Max(4, design.DownWidth))).Append(' ').AppendLine(FormatFloat(Math.Max(4, design.DownHeight)));
        output.Append("control_text ").Append(name).Append(" label ")
            .Append(FormatFloat(design.LabelOffsetX)).Append(' ').Append(FormatFloat(design.LabelOffsetY)).Append(' ')
            .AppendLine(FormatFloat(Math.Clamp(design.LabelScale, 0.2f, 3f)));
        output.Append("control_text ").Append(name).Append(" value ")
            .Append(FormatFloat(design.ValueOffsetX)).Append(' ').Append(FormatFloat(design.ValueOffsetY)).Append(' ')
            .AppendLine(FormatFloat(Math.Clamp(design.ValueScale, 0.2f, 3f)));
    }

    public static char DecodeCharacter(string token)
    {
        if (string.IsNullOrEmpty(token))
            throw new FormatException("Empty key character.");
        if (token[0] != '\\')
            return token[0];
        if (token.Length < 2)
            throw new FormatException("Incomplete key escape sequence.");
        return token[1] switch
        {
            't' => '\t',
            '\\' => '\\',
            'n' => '\n',
            'b' => '\b',
            's' => ' ',
            'z' => '\x01',
            'c' => '\x02',
            'q' => '\x03',
            'U' => '\x04',
            'D' => '\x05',
            'L' => '\x06',
            'R' => '\x07',
            'e' => '\x0E',
            'm' => '\x0F',
            'v' => '\x1C',
            'E' => '\x1D',
            'C' => '\x1E',
            'P' => '\x1F',
            'o' => '"',
            'f' => DecodeFunctionKey(token),
            _ => throw new FormatException($"Unknown key escape '{token}'.")
        };
    }

    public static string EncodeCharacter(char value) => value switch
    {
        '\t' => "\\t",
        '\\' => "\\\\",
        '\n' => "\\n",
        '\b' => "\\b",
        ' ' => "\\s",
        '\x01' => "\\z",
        '\x02' => "\\c",
        '\x03' => "\\q",
        '\x04' => "\\U",
        '\x05' => "\\D",
        '\x06' => "\\L",
        '\x07' => "\\R",
        '\x0E' => "\\e",
        '\x0F' => "\\m",
        '\x1C' => "\\v",
        '\x1D' => "\\E",
        '\x1E' => "\\C",
        '\x1F' => "\\P",
        '"' => "\\o",
        >= '\x10' and <= '\x1B' => $"\\f{value - '\x10' + 1}",
        _ => value.ToString()
    };

    private static char DecodeFunctionKey(string token)
    {
        if (!int.TryParse(token.AsSpan(2), NumberStyles.None, CultureInfo.InvariantCulture, out int number) || number is < 1 or > 12)
            throw new FormatException($"Invalid function key escape '{token}'.");
        return (char)('\x10' + number - 1);
    }

    private static float ParseFloat(string value) => float.Parse(value, NumberStyles.Float, CultureInfo.InvariantCulture);
    private static string FormatFloat(float value) => value.ToString("0.###", CultureInfo.InvariantCulture);

    private static string Quote(string value)
    {
        string escaped = value.Replace("\\", "\\\\").Replace("\"", "\\\"");
        return $"\"{escaped}\"";
    }

    private static void Require(IReadOnlyCollection<string> tokens, int count, string line)
    {
        if (tokens.Count < count)
            throw new FormatException($"Incomplete layout line: {line.Trim()}");
    }

    private static bool TryParseVisualDirective(KeyboardDocument document, string op, IReadOnlyList<string> tokens, string line)
    {
        switch (op.ToLowerInvariant())
        {
            case "style":
                Require(tokens, 2, line);
                document.CustomStyleEnabled = tokens[1].Equals("custom", StringComparison.OrdinalIgnoreCase);
                document.CustomStyleInitialized = document.CustomStyleEnabled;
                return true;
            case "base_theme":
                Require(tokens, 2, line);
                document.BaseTheme = SanitizeDesignName(tokens[1], "parchment");
                return true;
            case "font":
                Require(tokens, 2, line);
                document.FontName = SanitizeDesignName(tokens[1], "parchment");
                return true;
            case "key_plates":
                Require(tokens, 2, line);
                document.KeyPlatesEnabled = ParseBool(tokens[1]);
                return true;
            case "top_button_plates":
                Require(tokens, 2, line);
                document.TopButtonPlatesEnabled = ParseBool(tokens[1]);
                return true;
            case "input_bar_plate":
                Require(tokens, 2, line);
                document.InputBarPlateEnabled = ParseBool(tokens[1]);
                return true;
            case "parchment_ribbon":
                Require(tokens, 2, line);
                document.ParchmentRibbonEnabled = ParseBool(tokens[1]);
                return true;
            case "font_color":
                Require(tokens, 2, line);
                document.FontColor = ParseColor(tokens[1]);
                return true;
            case "font_outline_color":
                Require(tokens, 2, line);
                document.FontOutlineColor = ParseColor(tokens[1]);
                return true;
            case "font_glow_color":
                Require(tokens, 2, line);
                document.FontGlowColor = ParseColor(tokens[1]);
                return true;
            case "font_glow_enabled":
                Require(tokens, 2, line);
                document.FontGlowEnabled = ParseBool(tokens[1]);
                return true;
            case "font_glow_strength":
                Require(tokens, 2, line);
                document.FontGlowStrength = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 0, 100);
                return true;
            case "font_glow_radius":
                Require(tokens, 2, line);
                document.FontGlowRadius = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 1, 8);
                return true;
            case "font_breathe":
                Require(tokens, 2, line);
                document.FontBreatheEnabled = ParseBool(tokens[1]);
                document.FontBreatheMinPercent = tokens.Count > 2 ? Math.Clamp(int.Parse(tokens[2], CultureInfo.InvariantCulture), 0, 100) : 35;
                document.FontBreathePeriodSeconds = tokens.Count > 3 ? Math.Clamp(ParseFloat(tokens[3]), 0.5f, 10f) : 2f;
                document.FontBreathePhaseDegrees = tokens.Count > 4 ? NormalizeRotation(ParseFloat(tokens[4])) : 0;
                return true;
            case "key_color":
                Require(tokens, 2, line);
                document.KeyColor = ParseColor(tokens[1]);
                return true;
            case "plate_fill_color":
                Require(tokens, 2, line);
                document.PlateFillColor = ParseColor(tokens[1]);
                return true;
            case "plate_outline_width":
                Require(tokens, 2, line);
                document.PlateOutlineWidth = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 0, 8);
                return true;
            case "glow_color":
                Require(tokens, 2, line);
                document.GlowColor = ParseColor(tokens[1]);
                return true;
            case "hover_color":
                Require(tokens, 2, line);
                document.HoverColor = ParseColor(tokens[1]);
                return true;
            case "glow_enabled":
                Require(tokens, 2, line);
                document.GlowEnabled = ParseBool(tokens[1]);
                return true;
            case "glow_strength":
                Require(tokens, 2, line);
                document.GlowStrength = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 0, 100);
                return true;
            case "glow_radius":
                Require(tokens, 2, line);
                document.GlowRadius = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 1, 8);
                return true;
            case "hover_enabled":
                Require(tokens, 2, line);
                document.HoverEnabled = ParseBool(tokens[1]);
                return true;
            case "hover_strength":
                Require(tokens, 2, line);
                document.HoverStrength = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 0, 100);
                return true;
            case "label_outline":
                Require(tokens, 2, line);
                document.OutlineEnabled = ParseBool(tokens[1]);
                return true;
            case "key_roundness":
                Require(tokens, 2, line);
                document.KeyRoundness = Math.Clamp(int.Parse(tokens[1], CultureInfo.InvariantCulture), 0, 24);
                return true;
            case "key_breathe":
                Require(tokens, 2, line);
                document.KeyBreatheEnabled = ParseBool(tokens[1]);
                document.KeyBreatheMinPercent = tokens.Count > 2 ? Math.Clamp(int.Parse(tokens[2], CultureInfo.InvariantCulture), 0, 100) : 35;
                document.KeyBreathePeriodSeconds = tokens.Count > 3 ? Math.Clamp(ParseFloat(tokens[3]), 0.5f, 10f) : 2f;
                document.KeyBreathePhaseDegrees = tokens.Count > 4 ? NormalizeRotation(ParseFloat(tokens[4])) : 0;
                return true;
            case "background":
                Require(tokens, 2, line);
                document.BackgroundFileName = tokens[1];
                return true;
            case "background_rect":
                Require(tokens, 5, line);
                document.BackgroundX = ParseFloat(tokens[1]);
                document.BackgroundY = ParseFloat(tokens[2]);
                document.BackgroundWidth = Math.Max(1, ParseFloat(tokens[3]));
                document.BackgroundHeight = Math.Max(1, ParseFloat(tokens[4]));
                document.BackgroundOpacity = tokens.Count > 5 ? Math.Clamp(int.Parse(tokens[5], CultureInfo.InvariantCulture), 0, 100) : 100;
                document.BackgroundEdgeFade = tokens.Count > 6 ? Math.Clamp(int.Parse(tokens[6], CultureInfo.InvariantCulture), 0, 300) : 0;
                document.BackgroundRotation = tokens.Count > 7 ? NormalizeRotation(ParseFloat(tokens[7])) : 0;
                document.BackgroundBreatheEnabled = tokens.Count > 8 && ParseBool(tokens[8]);
                document.BackgroundBreatheMinPercent = tokens.Count > 9 ? Math.Clamp(int.Parse(tokens[9], CultureInfo.InvariantCulture), 0, 100) : 35;
                document.BackgroundBreathePeriodSeconds = tokens.Count > 10 ? Math.Clamp(ParseFloat(tokens[10]), 0.5f, 10f) : 2f;
                document.BackgroundBreathePhaseDegrees = tokens.Count > 11 ? NormalizeRotation(ParseFloat(tokens[11])) : 0;
                document.BackgroundRoundness = tokens.Count > 12 ? Math.Clamp(int.Parse(tokens[12], CultureInfo.InvariantCulture), 0, 100) : 0;
                return true;
            case "sprite":
                Require(tokens, 6, line);
                document.Sprites.Add(new KeyboardSprite
                {
                    FileName = tokens[1],
                    X = ParseFloat(tokens[2]),
                    Y = ParseFloat(tokens[3]),
                    Width = Math.Max(1, ParseFloat(tokens[4])),
                    Height = Math.Max(1, ParseFloat(tokens[5])),
                    Opacity = tokens.Count > 6 ? Math.Clamp(int.Parse(tokens[6], CultureInfo.InvariantCulture), 0, 100) : 100,
                    EdgeFade = tokens.Count > 7 ? Math.Clamp(int.Parse(tokens[7], CultureInfo.InvariantCulture), 0, 300) : 0
                });
                if (tokens.Count > 8)
                    document.Sprites[^1].Rotation = NormalizeRotation(ParseFloat(tokens[8]));
                if (tokens.Count > 9)
                    document.Sprites[^1].BreatheEnabled = ParseBool(tokens[9]);
                if (tokens.Count > 10)
                    document.Sprites[^1].BreatheMinPercent = Math.Clamp(int.Parse(tokens[10], CultureInfo.InvariantCulture), 0, 100);
                if (tokens.Count > 11)
                    document.Sprites[^1].BreathePeriodSeconds = Math.Clamp(ParseFloat(tokens[11]), 0.5f, 10f);
                if (tokens.Count > 12)
                    document.Sprites[^1].BreathePhaseDegrees = NormalizeRotation(ParseFloat(tokens[12]));
                if (tokens.Count > 13)
                    document.Sprites[^1].GlowEnabled = ParseBool(tokens[13]);
                else if (document.Sprites[^1].BreatheEnabled)
                    document.Sprites[^1].GlowEnabled = true;
                if (tokens.Count > 14)
                    document.Sprites[^1].GlowColor = ParseColor(tokens[14]);
                if (tokens.Count > 15)
                    document.Sprites[^1].GlowStrength = Math.Clamp(int.Parse(tokens[15], CultureInfo.InvariantCulture), 0, 100);
                if (tokens.Count > 16)
                    document.Sprites[^1].GlowRadius = Math.Clamp(int.Parse(tokens[16], CultureInfo.InvariantCulture), 1, 48);
                return true;
            case "control_offset":
                Require(tokens, 4, line);
                float controlX = ParseFloat(tokens[2]);
                float controlY = ParseFloat(tokens[3]);
                switch (tokens[1].ToLowerInvariant())
                {
                    case "size": document.SizeControlOffsetX = controlX; document.SizeControlOffsetY = controlY; break;
                    case "opacity": document.OpacityControlOffsetX = controlX; document.OpacityControlOffsetY = controlY; break;
                    case "tilt": document.TiltControlOffsetX = controlX; document.TiltControlOffsetY = controlY; break;
                    default: throw new FormatException($"Unknown runtime control '{tokens[1]}'.");
                }
                return true;
            case "top_offset":
                Require(tokens, 4, line);
                float topX = ParseFloat(tokens[2]);
                float topY = ParseFloat(tokens[3]);
                switch (tokens[1].ToLowerInvariant())
                {
                    case "textbar": document.TextBarOffsetX = topX; document.TextBarOffsetY = topY; break;
                    case "mode": document.ModeButtonOffsetX = topX; document.ModeButtonOffsetY = topY; break;
                    case "lock": document.LockButtonOffsetX = topX; document.LockButtonOffsetY = topY; break;
                    default: throw new FormatException($"Unknown top-bar element '{tokens[1]}'.");
                }
                return true;
            case "top_design":
                Require(tokens, 5, line);
                float topWidth = Math.Max(0, ParseFloat(tokens[2]));
                float topHeight = Math.Max(0, ParseFloat(tokens[3]));
                float topFontScale = Math.Clamp(ParseFloat(tokens[4]), 0, 3f);
                switch (tokens[1].ToLowerInvariant())
                {
                    case "textbar":
                        document.TextBarWidth = topWidth; document.TextBarHeight = topHeight;
                        document.TextBarFontScale = topFontScale; break;
                    case "mode":
                        document.ModeButtonWidth = topWidth; document.ModeButtonHeight = topHeight;
                        document.ModeButtonFontScale = topFontScale; break;
                    case "lock":
                        document.LockButtonWidth = topWidth; document.LockButtonHeight = topHeight;
                        document.LockButtonFontScale = topFontScale; break;
                    default: throw new FormatException($"Unknown top-bar design target '{tokens[1]}'.");
                }
                return true;
            case "control_box":
                Require(tokens, 4, line);
                KeyboardControlDesign boxDesign = document.GetControlDesign(tokens[1]);
                boxDesign.Width = Math.Max(24, ParseFloat(tokens[2]));
                boxDesign.Height = Math.Max(40, ParseFloat(tokens[3]));
                return true;
            case "control_part":
                Require(tokens, 7, line);
                KeyboardControlDesign partDesign = document.GetControlDesign(tokens[1]);
                bool up = tokens[2].Equals("up", StringComparison.OrdinalIgnoreCase);
                if (!up && !tokens[2].Equals("down", StringComparison.OrdinalIgnoreCase))
                    throw new FormatException($"Unknown control part '{tokens[2]}'.");
                if (up)
                {
                    partDesign.UpOffsetX = ParseFloat(tokens[3]);
                    partDesign.UpOffsetY = ParseFloat(tokens[4]);
                    partDesign.UpWidth = Math.Max(4, ParseFloat(tokens[5]));
                    partDesign.UpHeight = Math.Max(4, ParseFloat(tokens[6]));
                }
                else
                {
                    partDesign.DownOffsetX = ParseFloat(tokens[3]);
                    partDesign.DownOffsetY = ParseFloat(tokens[4]);
                    partDesign.DownWidth = Math.Max(4, ParseFloat(tokens[5]));
                    partDesign.DownHeight = Math.Max(4, ParseFloat(tokens[6]));
                }
                return true;
            case "control_text":
                Require(tokens, 6, line);
                KeyboardControlDesign textDesign = document.GetControlDesign(tokens[1]);
                bool label = tokens[2].Equals("label", StringComparison.OrdinalIgnoreCase);
                if (!label && !tokens[2].Equals("value", StringComparison.OrdinalIgnoreCase))
                    throw new FormatException($"Unknown control text '{tokens[2]}'.");
                if (label)
                {
                    textDesign.LabelOffsetX = ParseFloat(tokens[3]);
                    textDesign.LabelOffsetY = ParseFloat(tokens[4]);
                    textDesign.LabelScale = Math.Clamp(ParseFloat(tokens[5]), 0.2f, 3f);
                }
                else
                {
                    textDesign.ValueOffsetX = ParseFloat(tokens[3]);
                    textDesign.ValueOffsetY = ParseFloat(tokens[4]);
                    textDesign.ValueScale = Math.Clamp(ParseFloat(tokens[5]), 0.2f, 3f);
                }
                return true;
            case "control_arrow":
                Require(tokens, 2, line);
                document.ControlArrowFileName = tokens[1];
                document.ControlArrowRotation = tokens.Count > 2 ? NormalizeRotation(ParseFloat(tokens[2])) : 0;
                document.ControlArrowBreatheEnabled = tokens.Count > 3 && ParseBool(tokens[3]);
                document.ControlArrowGlowEnabled = document.ControlArrowBreatheEnabled;
                document.ControlArrowBreatheMinPercent = tokens.Count > 4 ? Math.Clamp(int.Parse(tokens[4], CultureInfo.InvariantCulture), 0, 100) : 35;
                document.ControlArrowBreathePeriodSeconds = tokens.Count > 5 ? Math.Clamp(ParseFloat(tokens[5]), 0.5f, 10f) : 2f;
                document.ControlArrowBreathePhaseDegrees = tokens.Count > 6 ? NormalizeRotation(ParseFloat(tokens[6])) : 0;
                return true;
            case "control_arrow_effect":
                Require(tokens, 9, line);
                document.ControlArrowGlowEnabled = ParseBool(tokens[1]);
                document.ControlArrowGlowColor = ParseColor(tokens[2]);
                document.ControlArrowGlowStrength = Math.Clamp(int.Parse(tokens[3], CultureInfo.InvariantCulture), 0, 100);
                document.ControlArrowGlowRadius = Math.Clamp(int.Parse(tokens[4], CultureInfo.InvariantCulture), 1, 48);
                document.ControlArrowBreatheEnabled = ParseBool(tokens[5]);
                document.ControlArrowBreatheMinPercent = Math.Clamp(int.Parse(tokens[6], CultureInfo.InvariantCulture), 0, 100);
                document.ControlArrowBreathePeriodSeconds = Math.Clamp(ParseFloat(tokens[7]), 0.5f, 10f);
                document.ControlArrowBreathePhaseDegrees = NormalizeRotation(ParseFloat(tokens[8]));
                return true;
            case "overlay":
                Require(tokens, 2, line);
                document.OverlayFileName = tokens[1];
                return true;
            case "overlay_rect":
                Require(tokens, 5, line);
                document.OverlayX = ParseFloat(tokens[1]);
                document.OverlayY = ParseFloat(tokens[2]);
                document.OverlayWidth = Math.Max(1, ParseFloat(tokens[3]));
                document.OverlayHeight = Math.Max(1, ParseFloat(tokens[4]));
                document.OverlayOpacity = tokens.Count > 5
                    ? Math.Clamp(int.Parse(tokens[5], CultureInfo.InvariantCulture), 0, 100)
                    : 100;
                return true;
            default:
                return false;
        }
    }

    private static bool ParseBool(string value)
        => value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1" || value.Equals("yes", StringComparison.OrdinalIgnoreCase);

    private static string SanitizeDesignName(string? value, string fallback)
    {
        string candidate = (value ?? "").Trim().ToLowerInvariant();
        return candidate.Length > 0 && candidate.All(character => char.IsAsciiLetterOrDigit(character) || character == '_')
            ? candidate
            : fallback;
    }

    private static Color ParseColor(string value)
    {
        string hex = value.Trim().TrimStart('#');
        if (hex.Length is not (6 or 8) || !uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint packed))
            throw new FormatException($"Invalid RGBA color '{value}'. Use #RRGGBB or #RRGGBBAA.");
        if (hex.Length == 6)
            return Color.FromArgb(255, (int)(packed >> 16) & 255, (int)(packed >> 8) & 255, (int)packed & 255);
        return Color.FromArgb((int)packed & 255, (int)(packed >> 24) & 255, (int)(packed >> 16) & 255, (int)(packed >> 8) & 255);
    }

    private static string FormatColor(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}{color.A:X2}";

    private static bool TryParseOcuCommentMetadata(KeyboardDocument document, string rawLine)
    {
        string line = rawLine.Trim();
        const string fillPrefix = "# ocu_input_fill_color ";
        const string enabledPrefix = "# ocu_input_outline_enabled ";
        const string colorPrefix = "# ocu_input_outline_color ";
        const string widthPrefix = "# ocu_input_outline_width ";
        const string titleOffsetPrefix = "# ocu_input_title_offset ";
        const string textOffsetPrefix = "# ocu_input_text_offset ";
        const string backgroundPrefix = "# ocu_console_input_background ";
        const string stateArtworkPrefix = "# ocu_top_state_art ";
        const string stateTextPrefix = "# ocu_top_state_text ";
        const string stateArtworkDesignPrefix = "# ocu_top_state_art_design ";
        const string stateTextOffsetPrefix = "# ocu_top_state_text_offset ";
        if (line.StartsWith(fillPrefix, StringComparison.OrdinalIgnoreCase))
        {
            document.InputFillColor = ParseColor(line[fillPrefix.Length..].Trim());
            document.InputFillOverrideEnabled = true;
            return true;
        }
        if (line.StartsWith(enabledPrefix, StringComparison.OrdinalIgnoreCase))
        {
            document.InputOutlineVisible = ParseBool(line[enabledPrefix.Length..].Trim());
            return true;
        }
        if (line.StartsWith(colorPrefix, StringComparison.OrdinalIgnoreCase))
        {
            document.InputOutlineColor = ParseColor(line[colorPrefix.Length..].Trim());
            document.InputOutlineOverrideEnabled = true;
            return true;
        }
        if (line.StartsWith(titleOffsetPrefix, StringComparison.OrdinalIgnoreCase))
        {
            ParseOffset(line[titleOffsetPrefix.Length..], out float x, out float y);
            document.InputTitleOffsetX = x;
            document.InputTitleOffsetY = y;
            return true;
        }
        if (line.StartsWith(textOffsetPrefix, StringComparison.OrdinalIgnoreCase))
        {
            ParseOffset(line[textOffsetPrefix.Length..], out float x, out float y);
            document.InputTextOffsetX = x;
            document.InputTextOffsetY = y;
            return true;
        }
        if (line.StartsWith(widthPrefix, StringComparison.OrdinalIgnoreCase))
        {
            document.InputOutlineWidth = Math.Clamp(
                int.Parse(line[widthPrefix.Length..].Trim(), CultureInfo.InvariantCulture), 0, 8);
            document.InputOutlineOverrideEnabled = true;
            return true;
        }
        if (line.StartsWith(backgroundPrefix, StringComparison.OrdinalIgnoreCase))
        {
            string fileName = line[backgroundPrefix.Length..].Trim();
            if (fileName.Length == 0 || Path.GetFileName(fileName) != fileName
                || fileName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0
                || fileName.Contains("..", StringComparison.Ordinal))
                throw new FormatException("Console INPUT background must be a filename beside the keyboard layout.");
            document.ConsoleInputBackgroundFileName = fileName;
            return true;
        }
        if (line.StartsWith(stateArtworkPrefix, StringComparison.OrdinalIgnoreCase))
        {
            string[] parts = line[stateArtworkPrefix.Length..]
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 2)
                throw new FormatException("Top-state artwork metadata requires a slot and portable PNG filename.");
            string fileName = ValidatePortableFileName(parts[1], "Top-state artwork");
            switch (parts[0].ToLowerInvariant())
            {
                case "mode_vr": document.ModeVrArtworkFileName = fileName; break;
                case "mode_pc": document.ModePcArtworkFileName = fileName; break;
                case "lock_world": document.LockWorldArtworkFileName = fileName; break;
                case "lock_head": document.LockHeadArtworkFileName = fileName; break;
                default: throw new FormatException($"Unknown top-state artwork slot '{parts[0]}'.");
            }
            return true;
        }
        if (line.StartsWith(stateTextPrefix, StringComparison.OrdinalIgnoreCase))
        {
            string[] parts = line[stateTextPrefix.Length..]
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 2)
                throw new FormatException("Top-state text metadata requires 'mode' or 'lock' and a boolean.");
            bool enabled = ParseBool(parts[1]);
            if (parts[0].Equals("mode", StringComparison.OrdinalIgnoreCase))
                document.ModeTextOverArtwork = enabled;
            else if (parts[0].Equals("lock", StringComparison.OrdinalIgnoreCase))
                document.LockTextOverArtwork = enabled;
            else
                throw new FormatException($"Unknown top-state text slot '{parts[0]}'.");
            return true;
        }
        if (line.StartsWith(stateArtworkDesignPrefix, StringComparison.OrdinalIgnoreCase))
        {
            string[] parts = line[stateArtworkDesignPrefix.Length..]
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 5)
                throw new FormatException("Top-state artwork design requires 'mode' or 'lock', x, y, width and height.");
            float x = ParseFloat(parts[1]);
            float y = ParseFloat(parts[2]);
            float width = Math.Max(0, ParseFloat(parts[3]));
            float height = Math.Max(0, ParseFloat(parts[4]));
            if (parts[0].Equals("mode", StringComparison.OrdinalIgnoreCase))
            {
                document.ModeArtworkOffsetX = x; document.ModeArtworkOffsetY = y;
                document.ModeArtworkWidth = width; document.ModeArtworkHeight = height;
            }
            else if (parts[0].Equals("lock", StringComparison.OrdinalIgnoreCase))
            {
                document.LockArtworkOffsetX = x; document.LockArtworkOffsetY = y;
                document.LockArtworkWidth = width; document.LockArtworkHeight = height;
            }
            else
                throw new FormatException($"Unknown top-state artwork design slot '{parts[0]}'.");
            return true;
        }
        if (line.StartsWith(stateTextOffsetPrefix, StringComparison.OrdinalIgnoreCase))
        {
            string[] parts = line[stateTextOffsetPrefix.Length..]
                .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 3)
                throw new FormatException("Top-state text offset requires 'mode' or 'lock', x and y.");
            float x = ParseFloat(parts[1]);
            float y = ParseFloat(parts[2]);
            if (parts[0].Equals("mode", StringComparison.OrdinalIgnoreCase))
            {
                document.ModeTextOffsetX = x; document.ModeTextOffsetY = y;
            }
            else if (parts[0].Equals("lock", StringComparison.OrdinalIgnoreCase))
            {
                document.LockTextOffsetX = x; document.LockTextOffsetY = y;
            }
            else
                throw new FormatException($"Unknown top-state text offset slot '{parts[0]}'.");
            return true;
        }
        return false;
    }

    private static void ResolvePortableArtwork(string directory, string? fileName, Action<string> apply)
    {
        if (string.IsNullOrWhiteSpace(fileName))
            return;
        string candidate = Path.Combine(directory, fileName);
        if (File.Exists(candidate))
            apply(candidate);
    }

    private static void AppendStateArtworkMetadata(StringBuilder output, string slot,
        string? imagePath, string? fileName, string portableName)
    {
        if (!string.IsNullOrWhiteSpace(imagePath) || !string.IsNullOrWhiteSpace(fileName))
            output.AppendLine($"# ocu_top_state_art {slot} {portableName}");
    }

    private static string ValidatePortableFileName(string fileName, string description)
    {
        if (fileName.Length == 0 || Path.GetFileName(fileName) != fileName
            || fileName.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0
            || fileName.Contains("..", StringComparison.Ordinal))
            throw new FormatException($"{description} must be a filename beside the keyboard layout.");
        return fileName;
    }

    private static void ParseOffset(string value, out float x, out float y)
    {
        string[] parts = value.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != 2
            || !float.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out x)
            || !float.TryParse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture, out y))
            throw new FormatException($"Invalid INPUT offset '{value}'. Expected two pixel values.");
        x = Math.Clamp(x, -2048f, 2048f);
        y = Math.Clamp(y, -240f, 240f);
    }

    public static string SpriteFileName(int index) => $"OCUKeyboardSprite{index + 1:00}.png";
    private static float NormalizeRotation(float value)
    {
        value %= 360f;
        return value < 0 ? value + 360f : value;
    }

    private static List<string> Tokenize(string line)
    {
        var tokens = new List<string>();
        var current = new StringBuilder();
        bool quoted = false;
        bool escaped = false;

        for (int index = 0; index < line.Length; index++)
        {
            char character = line[index];
            if (escaped)
            {
                current.Append(character);
                escaped = false;
                continue;
            }
            if (quoted && character == '\\' && index + 1 < line.Length && line[index + 1] is '\\' or '"')
            {
                escaped = true;
                continue;
            }
            if (character == '"' && (quoted || current.Length == 0))
            {
                quoted = !quoted;
                continue;
            }
            if (!quoted && char.IsWhiteSpace(character))
            {
                if (current.Length > 0)
                {
                    tokens.Add(current.ToString());
                    current.Clear();
                }
                continue;
            }
            current.Append(character);
        }

        if (quoted)
            throw new FormatException("Unterminated quote in keyboard layout.");
        if (current.Length > 0 || line.Contains("\"\"", StringComparison.Ordinal))
            tokens.Add(current.ToString());
        return tokens;
    }
}
