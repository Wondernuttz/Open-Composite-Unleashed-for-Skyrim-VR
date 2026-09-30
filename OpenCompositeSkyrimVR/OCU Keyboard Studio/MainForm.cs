using System.Globalization;

namespace OCUKeyboardStudio;

internal sealed record KeyboardDesignChoice(string Name, string Path, bool BuiltIn)
{
    public override string ToString() => BuiltIn ? $"{Name} (Built-in)" : Name;
}

internal enum ConsoleInputPart
{
    None,
    Heading,
    TypedText
}

internal enum TopStateArtworkSlot
{
    ModeVr,
    ModePc,
    LockWorld,
    LockHead
}

internal readonly record struct EffectiveKeyboardStyle(
    Color Font,
    Color FontOutline,
    Color FontGlow,
    Color PlateFill,
    Color PlateOutline,
    Color PlateGlow,
    Color Hover);

internal sealed class MainForm : Form
{
    private static readonly Color Surface = StudioTheme.Window;
    private static readonly Color SurfaceRaised = StudioTheme.Surface;
    private static readonly Color Edge = StudioTheme.Border;
    private static readonly Color Accent = StudioTheme.KeyGlow;
    private static readonly Color AccentBright = StudioTheme.KeyGlowBright;
    private static readonly Color TextPrimary = StudioTheme.TextPrimary;
    private static readonly Color TextMuted = StudioTheme.TextMuted;

    private readonly KeyboardCanvas _canvas = new() { Dock = DockStyle.Fill };
    private readonly ModernTabControl _inspectorTabs = new();
    private readonly ComboBox _themeCombo = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox _fontCombo = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox _designCombo = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox _stateCombo = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ModernCheckBox _pressedCheck = new() { Text = "Pressed", AutoSize = true };
    private readonly ModernCheckBox _snapCheck = new() { Text = "Snap", AutoSize = true, Checked = true };
    private readonly Label _statusLabel = new() { AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
    private readonly Label _historyLabel = new() { AutoSize = false, Dock = DockStyle.Right, Width = 190, TextAlign = ContentAlignment.MiddleRight };
    private readonly Label _selectedLabel = new() { AutoSize = true };
    private readonly TextBox _baseLabel = new();
    private readonly TextBox _shiftLabel = new();
    private readonly ModernPillButton _assignedKeyButton = new() { Text = "Select a key first", AutoSize = false, Height = 34 };
    private readonly NumericUpDown _x = NumberBox(-100, 100, 2, 0.05m);
    private readonly NumericUpDown _y = NumberBox(-100, 100, 2, 0.05m);
    private readonly NumericUpDown _width = NumberBox(0.25m, 30, 2, 0.05m);
    private readonly NumericUpDown _height = NumberBox(0.25m, 20, 2, 0.05m);
    private readonly NumericUpDown _offsetX = NumberBox(-250, 250, 1, 1);
    private readonly NumericUpDown _offsetY = NumberBox(-250, 250, 1, 1);
    private readonly NumericUpDown _scale = NumberBox(0.25m, 3, 2, 0.05m);
    private readonly NumericUpDown _layoutWidth = NumberBox(5, 30, 0, 1);
    private readonly Label _topElementLabel = new() { AutoSize = true };
    private readonly NumericUpDown _topElementX = NumberBox(-1024, 1024, 0, 1);
    private readonly NumericUpDown _topElementY = NumberBox(-560, 560, 0, 1);
    private readonly NumericUpDown _topElementWidth = NumberBox(8, 2048, 0, 1);
    private readonly NumericUpDown _topElementHeight = NumberBox(8, 1120, 0, 1);
    private readonly NumericUpDown _topElementFontScale = NumberBox(0.2m, 3, 2, 0.05m);
    private readonly ComboBox _modeStatePreview = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox _lockStatePreview = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly Label _modeVrArtworkName = new() { AutoSize = true, MaximumSize = new Size(300, 0) };
    private readonly Label _modePcArtworkName = new() { AutoSize = true, MaximumSize = new Size(300, 0) };
    private readonly Label _lockWorldArtworkName = new() { AutoSize = true, MaximumSize = new Size(300, 0) };
    private readonly Label _lockHeadArtworkName = new() { AutoSize = true, MaximumSize = new Size(300, 0) };
    private readonly ModernCheckBox _modeTextOverArtwork = new() { Text = "Show PC/VR text over state images", AutoSize = true };
    private readonly ModernCheckBox _lockTextOverArtwork = new() { Text = "Show LOCK text over state images", AutoSize = true };
    private readonly ModernCheckBox _keyPlatesEnabled = new() { Text = "Show key plates", AutoSize = true };
    private readonly ModernCheckBox _topButtonPlatesEnabled = new() { Text = "Show PC/VR Mode + Lock plates", AutoSize = true };
    private readonly ModernCheckBox _inputBarPlateEnabled = new() { Text = "Show input bar plate", AutoSize = true };
    private readonly ModernCheckBox _parchmentRibbonEnabled = new() { Text = "Show Parchment spacebar ribbon", AutoSize = true };
    private readonly PictureBox _consoleInputPreview = new()
    {
        Dock = DockStyle.Fill,
        Height = 72,
        MinimumSize = new Size(320, 72),
        BackColor = Color.FromArgb(9, 11, 15),
        SizeMode = PictureBoxSizeMode.Zoom
    };
    private readonly Label _consoleInputBackgroundName = new() { AutoSize = true, MaximumSize = new Size(360, 0) };
    private readonly ModernCheckBox _glowEnabled = new() { Text = "Plate outline glow", AutoSize = true };
    private readonly ModernCheckBox _hoverEnabled = new() { Text = "Hover effect", AutoSize = true };
    private readonly ModernCheckBox _outlineEnabled = new() { Text = "Font outline", AutoSize = true };
    private readonly ColorEntryControl _fontColor = new();
    private readonly ColorEntryControl _fontOutlineColor = new();
    private readonly ColorEntryControl _fontGlowColor = new();
    private readonly ColorEntryControl _keyColor = new();
    private readonly ColorEntryControl _plateFillColor = new();
    private readonly ColorEntryControl _inputFillColor = new();
    private readonly ColorEntryControl _inputOutlineColor = new();
    private readonly ColorEntryControl _glowColor = new();
    private readonly ColorEntryControl _hoverColor = new();
    private readonly NumericUpDown _glowStrength = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _glowRadius = NumberBox(1, 8, 0, 1);
    private readonly NumericUpDown _hoverStrength = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _keyRoundness = NumberBox(0, 24, 0, 1);
    private readonly NumericUpDown _plateOutlineWidth = NumberBox(0, 8, 0, 1);
    private readonly NumericUpDown _inputOutlineWidth = NumberBox(0, 8, 0, 1);
    private readonly ModernCheckBox _inputOutlineVisible = new() { Text = "Show outline", AutoSize = true, Checked = true };
    private readonly NumericUpDown _inputTitleX = NumberBox(-2048, 2048, 0, 1);
    private readonly NumericUpDown _inputTitleY = NumberBox(-240, 240, 0, 1);
    private readonly NumericUpDown _inputTextX = NumberBox(-2048, 2048, 0, 1);
    private readonly NumericUpDown _inputTextY = NumberBox(-240, 240, 0, 1);
    private readonly ModernCheckBox _keyBreathe = new() { Text = "Breathing plate glow", AutoSize = true };
    private readonly NumericUpDown _keyBreatheMin = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _keyBreathePeriod = NumberBox(0.5m, 10, 2, 0.25m);
    private readonly NumericUpDown _keyBreathePhase = NumberBox(0, 360, 0, 15);
    private readonly ModernCheckBox _fontGlowEnabled = new() { Text = "Font glow", AutoSize = true };
    private readonly NumericUpDown _fontGlowStrength = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _fontGlowRadius = NumberBox(1, 8, 0, 1);
    private readonly ModernCheckBox _fontBreathe = new() { Text = "Breathing font glow", AutoSize = true };
    private readonly NumericUpDown _fontBreatheMin = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _fontBreathePeriod = NumberBox(0.5m, 10, 2, 0.25m);
    private readonly NumericUpDown _fontBreathePhase = NumberBox(0, 360, 0, 15);
    private readonly Label _backgroundName = new() { AutoSize = true, MaximumSize = new Size(280, 0) };
    private ConsoleInputPart _selectedConsoleInputPart;
    private bool _draggingConsoleInputPart;
    private PointF _consoleInputDragStart;
    private float _consoleInputStartX;
    private float _consoleInputStartY;
    private KeyboardDocument? _pendingConsoleInputUndo;
    private readonly Label _overlayName = new() { AutoSize = true, MaximumSize = new Size(280, 0) };
    private readonly ListBox _spriteList = new() { Height = 105, Dock = DockStyle.Fill, IntegralHeight = false };
    private readonly NumericUpDown _backgroundX = NumberBox(-2048, 2048, 0, 1);
    private readonly NumericUpDown _backgroundY = NumberBox(-1120, 1120, 0, 1);
    private readonly NumericUpDown _backgroundWidth = NumberBox(1, 4096, 0, 1);
    private readonly NumericUpDown _backgroundHeight = NumberBox(1, 2240, 0, 1);
    private readonly NumericUpDown _backgroundOpacity = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _backgroundFade = NumberBox(0, 300, 0, 2);
    private readonly NumericUpDown _backgroundRotation = NumberBox(-360, 360, 0, 5);
    private readonly NumericUpDown _backgroundRoundness = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _overlayX = NumberBox(-1024, 2048, 0, 1);
    private readonly NumericUpDown _overlayY = NumberBox(-560, 1120, 0, 1);
    private readonly NumericUpDown _overlayWidth = NumberBox(1, 2048, 0, 1);
    private readonly NumericUpDown _overlayHeight = NumberBox(1, 1120, 0, 1);
    private readonly NumericUpDown _overlayOpacity = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _spriteFade = NumberBox(0, 300, 0, 2);
    private readonly NumericUpDown _spriteRotation = NumberBox(-360, 360, 0, 5);
    private readonly ModernCheckBox _spriteGlow = new() { Text = "Procedural glow", AutoSize = true };
    private readonly ColorEntryControl _spriteGlowColor = new();
    private readonly NumericUpDown _spriteGlowStrength = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _spriteGlowRadius = NumberBox(1, 48, 0, 1);
    private readonly ModernCheckBox _spriteBreathe = new() { Text = "Breathing glow", AutoSize = true };
    private readonly NumericUpDown _spriteBreatheMin = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _spriteBreathePeriod = NumberBox(0.5m, 10, 2, 0.25m);
    private readonly NumericUpDown _spriteBreathePhase = NumberBox(0, 360, 0, 15);
    private readonly ComboBox _controlCombo = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly Label _controlPartLabel = new() { AutoSize = true };
    private readonly NumericUpDown _controlX = NumberBox(-1024, 1024, 0, 1);
    private readonly NumericUpDown _controlY = NumberBox(-560, 560, 0, 1);
    private readonly NumericUpDown _controlWidth = NumberBox(24, 600, 1, 1);
    private readonly NumericUpDown _controlHeight = NumberBox(40, 560, 1, 1);
    private readonly NumericUpDown _controlPartX = NumberBox(-500, 500, 1, 1);
    private readonly NumericUpDown _controlPartY = NumberBox(-500, 500, 1, 1);
    private readonly NumericUpDown _controlPartWidth = NumberBox(4, 400, 1, 1);
    private readonly NumericUpDown _controlPartHeight = NumberBox(4, 400, 1, 1);
    private readonly NumericUpDown _controlPartScale = NumberBox(0.2m, 3, 2, 0.05m);
    private readonly Label _controlArrowName = new() { AutoSize = true, MaximumSize = new Size(280, 0) };
    private readonly NumericUpDown _controlArrowRotation = NumberBox(-360, 360, 0, 5);
    private readonly ModernCheckBox _controlArrowGlow = new() { Text = "Procedural arrow glow", AutoSize = true };
    private readonly ColorEntryControl _controlArrowGlowColor = new();
    private readonly NumericUpDown _controlArrowGlowStrength = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _controlArrowGlowRadius = NumberBox(1, 48, 0, 1);
    private readonly ModernCheckBox _controlArrowBreathe = new() { Text = "Breathing arrow glow", AutoSize = true };
    private readonly NumericUpDown _controlArrowBreatheMin = NumberBox(0, 100, 0, 5);
    private readonly NumericUpDown _controlArrowBreathePeriod = NumberBox(0.5m, 10, 2, 0.25m);
    private readonly NumericUpDown _controlArrowBreathePhase = NumberBox(0, 360, 0, 15);
    private readonly Stack<KeyboardDocument> _undo = [];
    private readonly Stack<KeyboardDocument> _redo = [];
    private readonly List<FontChoice> _fonts = [];
    private readonly List<KeyboardDesignChoice> _designs = [];
    private readonly ToolTip _toolTip = new();
    private Button _undoButton = null!;
    private Button _redoButton = null!;
    private Button _saveButton = null!;
    private Button _installButton = null!;

    private KeyboardDocument _document = new();
    private KeyboardRenderer? _renderer;
    private KeyboardDocument? _pendingCanvasUndo;
    private bool _updatingEditor;
    private bool _updatingDesignLibrary;
    private string _assetsDirectory = "";
    private string? _hostOcuRoot;
    private string? _currentPackagePath;
    private bool _capturingAssignedKey;

    private static string DesignLibraryDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "OpenCompositeUnleashed", "KeyboardStudio", "Designs");
    private static string DesignRegistryPath => Path.Combine(DesignLibraryDirectory, "design-library.txt");

    public MainForm(string? requestedOcuRoot = null, string? keyboardFile = null)
    {
        _hostOcuRoot = ResolveSelectedOcuRoot(requestedOcuRoot)
            ?? FindOcuRootFromStudioDirectory(AppContext.BaseDirectory);
        Text = "OCU Keyboard Studio";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(1180, 720);
        Size = new Size(1540, 900);
        BackColor = Surface;
        ForeColor = TextPrimary;
        Font = new Font("Segoe UI", 9.5f);
        AutoScaleMode = AutoScaleMode.Dpi;
        StudioTheme.EnableDarkWindowChrome(this);

        Controls.Add(BuildMainLayout());
        Controls.Add(BuildToolbar());
        Controls.Add(BuildStatusBar());

        DiscoverAssets();
        WireEvents();
        _toolTip.SetToolTip(_canvas,
            "When text is selected, the mouse wheel resizes that font anywhere on the canvas. Select a plate, artwork, background, or empty canvas to wheel-zoom the keyboard. Middle-drag pans while zoomed.");
        LoadDefaultLayout();
        if (!string.IsNullOrWhiteSpace(keyboardFile))
            LoadKeyboardFile(keyboardFile);
        UpdateTitle();
    }

    private Control BuildToolbar()
    {
        EnsureHistoryButtons();
        _saveButton = ActionButton("Save", (_, _) => SaveLayout(false));
        _installButton = ActionButton("Install for Next Launch", (_, _) => InstallToOcu());
        _toolTip.SetToolTip(_installButton,
            "Writes this keyboard into the OCU installation that opened Studio. Restart Skyrim VR to load it.");
        Button relosControllersButton = ActionButton(
            "Relos Custom Controllers",
            (_, _) => OpenExternalUrl("https://www.nexusmods.com/skyrimspecialedition/mods/187713"));
        _toolTip.SetToolTip(
            relosControllersButton,
            "Opens Relos Customs for SteamVR controller textures. This does not change OCU bindings, calibration dots, or laser alignment.");
        var toolbar = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = 98,
            Padding = new Padding(12, 7, 8, 5),
            BackColor = SurfaceRaised,
            ColumnCount = 1,
            RowCount = 2
        };
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        toolbar.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        toolbar.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));

        var actions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            WrapContents = false,
            Margin = Padding.Empty
        };
        actions.Controls.AddRange([
            ActionButton("Open / Import", (_, _) => OpenLayout()),
            _saveButton,
            ActionButton("Save As", (_, _) => SaveLayout(true)),
            ActionButton("Export PNG", (_, _) => ExportPng()),
            ActionButton("Export MO2 Mod", (_, _) => ExportMo2Mod()),
            _installButton,
            relosControllersButton
        ]);

        var design = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            WrapContents = false,
            Margin = Padding.Empty
        };
        Label baseThemeCaption = Caption("Base Theme");
        const string baseThemeHelp = "The built-in visual foundation: background/colors, plate treatment, Parchment ribbon eligibility, and the authored positions for PC/VR, Lock, Size, Opacity, and Tilt. A custom background and custom colors override most of its visible appearance.";
        _toolTip.SetToolTip(baseThemeCaption, baseThemeHelp);
        _toolTip.SetToolTip(_themeCombo, baseThemeHelp);
        Button addTtfButton = ActionButton("Add TTF", (_, _) => ImportFont(), compact: true);
        Button findFontsButton = ActionButton("Find Fonts", (_, _) => OpenExternalUrl("https://www.fontspace.com"), compact: true);
        _toolTip.SetToolTip(findFontsButton,
            "Opens FontSpace in your browser. Check the individual font license before including a font in a shared .ocukb or mod package.");
        design.Controls.AddRange([
            Caption("Keyboard Design"), _designCombo,
            Spacer(12),
            baseThemeCaption, _themeCombo,
            Caption("Font"), _fontCombo, addTtfButton, findFontsButton,
            Caption("State"), _stateCombo,
            _pressedCheck,
            _snapCheck
        ]);

        _designCombo.Width = 245;
        _themeCombo.Width = 145;
        _fontCombo.Width = 185;
        _stateCombo.Width = 90;
        StyleCombo(_designCombo);
        StyleCombo(_themeCombo);
        StyleCombo(_fontCombo);
        StyleCombo(_stateCombo);
        StyleCheck(_pressedCheck);
        StyleCheck(_snapCheck);
        toolbar.Controls.Add(actions, 0, 0);
        toolbar.Controls.Add(design, 0, 1);
        return toolbar;
    }

    private Control BuildMainLayout()
    {
        EnsureHistoryButtons();
        var split = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Size = new Size(1510, 800),
            FixedPanel = FixedPanel.Panel2,
            SplitterWidth = 6,
            BackColor = Edge,
            Panel1MinSize = 700,
            Panel2MinSize = 440,
            SplitterDistance = 1045
        };
        split.Panel1.BackColor = Color.FromArgb(9, 11, 15);
        split.Panel1.Padding = new Padding(12);
        var canvasLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Margin = Padding.Empty,
            Padding = Padding.Empty,
            ColumnCount = 1,
            RowCount = 2
        };
        canvasLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        canvasLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        canvasLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        var historyButtons = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Margin = Padding.Empty,
            Padding = new Padding(0, 5, 4, 3),
            BackColor = Color.FromArgb(9, 11, 15),
            ColumnCount = 3,
            RowCount = 1
        };
        historyButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        historyButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 82));
        historyButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 82));
        _undoButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _redoButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        historyButtons.Controls.Add(_undoButton, 1, 0);
        historyButtons.Controls.Add(_redoButton, 2, 0);
        var previewHost = new Panel { Dock = DockStyle.Fill, BackColor = Color.FromArgb(9, 11, 15) };
        _consoleInputPreview.Visible = false;
        previewHost.Controls.Add(_canvas);
        previewHost.Controls.Add(_consoleInputPreview);
        canvasLayout.Controls.Add(previewHost, 0, 0);
        canvasLayout.Controls.Add(historyButtons, 0, 1);
        split.Panel1.Controls.Add(canvasLayout);
        split.Panel2.BackColor = Surface;
        split.Panel2.Padding = new Padding(12);
        split.Panel2.Controls.Add(BuildInspector());
        return split;
    }

    private void EnsureHistoryButtons()
    {
        _undoButton ??= ActionButton("Undo", (_, _) => Undo(), compact: true);
        _redoButton ??= ActionButton("Redo", (_, _) => Redo(), compact: true);
    }

    private Control BuildInspector()
    {
        _inspectorTabs.Dock = DockStyle.Fill;
        TabPage keyPage = InspectorPage("Key");
        TabPage appearancePage = InspectorPage("Appearance");
        TabPage inputPage = InspectorPage("Input");
        TabPage artworkPage = InspectorPage("Artwork");
        TabPage controlsPage = InspectorPage("Controls");
        keyPage.Controls.Add(BuildKeyInspector());
        appearancePage.Controls.Add(BuildAppearanceInspector());
        inputPage.Controls.Add(BuildInputInspector());
        artworkPage.Controls.Add(BuildArtworkInspector());
        controlsPage.Controls.Add(BuildControlsInspector());
        _inspectorTabs.TabPages.AddRange([keyPage, appearancePage, inputPage, artworkPage, controlsPage]);
        return _inspectorTabs;
    }

    private Control BuildKeyInspector()
    {
        var inspector = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            ColumnCount = 2,
            RowCount = 1,
            Padding = new Padding(2),
            BackColor = Surface
        };
        inspector.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        inspector.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        AddSection(inspector, "DIRECT EDITING");
        AddHint(inspector, "Click directly on a letter or symbol to move its content. Click the empty part of its plate to move the whole key. Drag plate handles to resize; mouse wheel resizes selected content. The text bar, PC/VR Mode, and Lock are also directly draggable.");

        AddSection(inspector, "SELECTED KEY");
        AddWide(inspector, _selectedLabel);
        AddRow(inspector, "Assigned key", _assignedKeyButton);
        AddHint(inspector, "Click Assigned key, then press the real key on your physical keyboard. Studio translates its normal and Shift values using your Windows keyboard layout.");
        AddRow(inspector, "Key text", _baseLabel);
        AddRow(inspector, "Shift text", _shiftLabel);
        AddRow(inspector, "Key X", _x);
        AddRow(inspector, "Key Y", _y);
        AddRow(inspector, "Key width", _width);
        AddRow(inspector, "Key height", _height);
        AddRow(inspector, "Font X px", _offsetX);
        AddRow(inspector, "Font Y px", _offsetY);
        AddRow(inspector, "Font size / scale", _scale);

        var keyActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        keyActions.Controls.Add(ActionButton("Duplicate", (_, _) => DuplicateKey(), compact: true));
        keyActions.Controls.Add(ActionButton("Delete", (_, _) => DeleteKey(), compact: true));
        AddWide(inspector, keyActions);

        AddSection(inspector, "SELECTED TOP-BAR ELEMENT");
        AddWide(inspector, _topElementLabel);
        AddRow(inspector, "Offset X", _topElementX);
        AddRow(inspector, "Offset Y", _topElementY);
        AddRow(inspector, "Width px", _topElementWidth);
        AddRow(inspector, "Height px", _topElementHeight);
        AddRow(inspector, "Font size / scale", _topElementFontScale);
        AddHint(inspector, "PC/VR and Lock are layered controls: select and move the interaction box, state image/sprite, or text independently. Only the interaction box changes the in-game laser hit area. Scroll resizes selected text, state images, and ordinary sprites.");

        AddSection(inspector, "PC / VR + LOCK STATE IMAGES");
        StyleCombo(_modeStatePreview);
        StyleCombo(_lockStatePreview);
        StyleCheck(_modeTextOverArtwork);
        StyleCheck(_lockTextOverArtwork);
        AddRow(inspector, "Preview mode", _modeStatePreview);
        AddRow(inspector, "VR Mode image", StateArtworkActions(TopStateArtworkSlot.ModeVr, _modeVrArtworkName));
        AddRow(inspector, "PC Mode image", StateArtworkActions(TopStateArtworkSlot.ModePc, _modePcArtworkName));
        AddWide(inspector, _modeTextOverArtwork);
        AddRow(inspector, "Edit Mode layer", TopLayerActions(KeyboardTopElement.Mode));
        AddRow(inspector, "Preview lock", _lockStatePreview);
        AddRow(inspector, "World image", StateArtworkActions(TopStateArtworkSlot.LockWorld, _lockWorldArtworkName));
        AddRow(inspector, "Head-lock image", StateArtworkActions(TopStateArtworkSlot.LockHead, _lockHeadArtworkName));
        AddWide(inspector, _lockTextOverArtwork);
        AddRow(inspector, "Edit Lock layer", TopLayerActions(KeyboardTopElement.Lock));
        AddHint(inspector, "Each PNG is a state-aware sprite that switches with the real in-game state. Edit selects the matching preview and its picture immediately. Drag to move; use handles or the wheel to resize. Picture and text remain visual-only and create no extra OpenXR overlay.");

        AddSection(inspector, "GRID COLUMN COUNT");
        AddRow(inspector, "Columns across", _layoutWidth);
        AddHint(inspector, "This controls density, not pixel width: 14 columns makes larger keys than 15 because the same 784 px area is divided into fewer cells.");

        return inspector;
    }

    private Control BuildAppearanceInspector()
    {
        var inspector = InspectorTable();
        AddSection(inspector, "INTERACTION / SELECTION");
        StyleCheck(_keyPlatesEnabled);
        StyleCheck(_topButtonPlatesEnabled);
        StyleCheck(_inputBarPlateEnabled);
        StyleCheck(_parchmentRibbonEnabled);
        AddWide(inspector, _keyPlatesEnabled);
        AddWide(inspector, _topButtonPlatesEnabled);
        AddWide(inspector, _inputBarPlateEnabled);
        AddWide(inspector, _parchmentRibbonEnabled);
        AddHint(inspector, "Visible plates can be selected anywhere inside their shape. Hidden plates are click-through in Studio; select their visible text or artwork instead. Hiding a plate changes the look, not the key assigned to that location in game.");

        AddSection(inspector, "CUSTOM COLORS");
        AddHint(inspector, "The palette always shows the colors currently visible from the selected Base Theme. Editing any color or effect automatically creates an override; no enable checkbox is required.");
        AddWide(inspector, ActionButton("Reset colors and effects to Base Theme", (_, _) => ResetStyleToBaseTheme(), compact: true));
        AddHint(inspector, "Click a swatch for the hue/saturation wheel, or type/paste #RRGGBB or #RRGGBBAA in the box. Reset removes the overrides and restores the selected theme palette.");
        AddRow(inspector, "Font", _fontColor);
        AddRow(inspector, "Font outline", _fontOutlineColor);
        AddRow(inspector, "Font glow", _fontGlowColor);
        AddRow(inspector, "Plate fill", _plateFillColor);
        AddRow(inspector, "Plate outline", _keyColor);
        AddRow(inspector, "Outline width", _plateOutlineWidth);
        AddRow(inspector, "Plate glow", _glowColor);
        AddRow(inspector, "Hover", _hoverColor);

        AddSection(inspector, "EFFECTS");
        StyleCheck(_glowEnabled);
        StyleCheck(_hoverEnabled);
        StyleCheck(_outlineEnabled);
        StyleCheck(_fontGlowEnabled);
        StyleCheck(_fontBreathe);
        AddWide(inspector, _glowEnabled);
        AddRow(inspector, "Plate glow strength", _glowStrength);
        AddRow(inspector, "Plate glow radius", _glowRadius);
        StyleCheck(_keyBreathe);
        AddWide(inspector, _keyBreathe);
        AddRow(inspector, "Plate minimum %", _keyBreatheMin);
        AddRow(inspector, "Plate period seconds", _keyBreathePeriod);
        AddRow(inspector, "Plate phase degrees", _keyBreathePhase);
        AddWide(inspector, _fontGlowEnabled);
        AddRow(inspector, "Font glow strength", _fontGlowStrength);
        AddRow(inspector, "Font glow radius", _fontGlowRadius);
        AddWide(inspector, _fontBreathe);
        AddRow(inspector, "Font minimum %", _fontBreatheMin);
        AddRow(inspector, "Font period seconds", _fontBreathePeriod);
        AddRow(inspector, "Font phase degrees", _fontBreathePhase);
        AddWide(inspector, _hoverEnabled);
        AddRow(inspector, "Hover strength", _hoverStrength);
        AddWide(inspector, _outlineEnabled);
        AddRow(inspector, "Key roundness", _keyRoundness);
        AddHint(inspector, "Font and plate glows have independent colors and breathing cycles. Their base font and outline stay readable while the halo breathes.");
        return inspector;
    }

    private Control BuildInputInspector()
    {
        var inspector = InspectorTable();
        AddSection(inspector, "CONSOLE INPUT PANEL");
        AddHint(inspector, "Selecting this tab replaces the center keyboard preview with the exact 1024x120 floating INPUT panel used by Skyrim's console. Click and drag the INPUT heading or typed-text row directly in the preview to place them independently.");

        AddSection(inspector, "INPUT PANEL IMAGE");
        var imageActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        imageActions.Controls.Add(ActionButton("Choose image", (_, _) => ChooseConsoleInputBackground(), compact: true));
        imageActions.Controls.Add(ActionButton("Clear", (_, _) => ClearConsoleInputBackground(), compact: true));
        AddWide(inspector, imageActions);
        _consoleInputBackgroundName.ForeColor = TextMuted;
        AddWide(inspector, _consoleInputBackgroundName);
        AddHint(inspector, "The image is scaled once to the panel's exact 1024x120 texture and composited into its existing swapchain. Transparent PNG is recommended; this does not add an OpenXR overlay.");

        AddSection(inspector, "INPUT APPEARANCE");
        AddRow(inspector, "Inside color", _inputFillColor);
        AddWide(inspector, ActionButton("Use keyboard theme fill", (_, _) => ResetInputFill(), compact: true));
        StyleCheck(_inputOutlineVisible);
        AddWide(inspector, _inputOutlineVisible);
        AddRow(inspector, "Outline", _inputOutlineColor);
        AddRow(inspector, "Outline width", _inputOutlineWidth);
        AddWide(inspector, ActionButton("Use default INPUT border", (_, _) => ResetInputOutline(), compact: true));
        AddHint(inspector, "Inside color and outline belong only to the floating console INPUT panel. The font and its effects still follow the keyboard design.");

        AddSection(inspector, "TEXT POSITION");
        AddRow(inspector, "INPUT heading X", _inputTitleX);
        AddRow(inspector, "INPUT heading Y", _inputTitleY);
        AddRow(inspector, "Typed text X", _inputTextX);
        AddRow(inspector, "Typed text Y", _inputTextY);
        AddWide(inspector, ActionButton("Center/reset text positions", (_, _) => ResetInputTextPositions(), compact: true));
        AddHint(inspector, "These are pixel offsets from OCU's exact runtime positions. Dragging either text region updates the same values.");
        return inspector;
    }

    private Control BuildArtworkInspector()
    {
        var inspector = InspectorTable();
        AddSection(inspector, "BACKGROUND IMAGE");
        var backgroundActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        backgroundActions.Controls.Add(ActionButton("Choose image", (_, _) => ChooseBackground(), compact: true));
        backgroundActions.Controls.Add(ActionButton("Clear", (_, _) => ClearBackground(), compact: true));
        AddWide(inspector, backgroundActions);
        _backgroundName.ForeColor = TextMuted;
        AddWide(inspector, _backgroundName);
        AddRow(inspector, "X", _backgroundX);
        AddRow(inspector, "Y", _backgroundY);
        AddRow(inspector, "Width", _backgroundWidth);
        AddRow(inspector, "Height", _backgroundHeight);
        AddRow(inspector, "Opacity", _backgroundOpacity);
        AddRow(inspector, "Edge fade px", _backgroundFade);
        AddRow(inspector, "Rotation deg", _backgroundRotation);
        AddRow(inspector, "Corner roundness %", _backgroundRoundness);
        AddHint(inspector, "Click the background in the preview, then drag it, use its edge/corner handles, or turn it into a pill with Corner roundness. JPEG is background-only and converted to PNG on save/export.");

        AddSection(inspector, "SPRITES / RIBBONS");
        var overlayActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        overlayActions.Controls.Add(ActionButton("Add PNG", (_, _) => ChooseOverlay(), compact: true));
        overlayActions.Controls.Add(ActionButton("Add Parchment Ribbon", (_, _) => AddParchmentRibbon(), compact: true));
        overlayActions.Controls.Add(ActionButton("Remove", (_, _) => ClearOverlay(), compact: true));
        AddWide(inspector, overlayActions);
        _spriteList.BackColor = StudioTheme.Input;
        _spriteList.ForeColor = TextPrimary;
        _spriteList.BorderStyle = BorderStyle.FixedSingle;
        AddWide(inspector, _spriteList);
        var orderActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        orderActions.Controls.Add(ActionButton("Send back", (_, _) => MoveSpriteLayer(-1), compact: true));
        orderActions.Controls.Add(ActionButton("Bring front", (_, _) => MoveSpriteLayer(1), compact: true));
        AddWide(inspector, orderActions);
        _overlayName.ForeColor = TextMuted;
        AddWide(inspector, _overlayName);

        AddRow(inspector, "Sprite X", _overlayX);
        AddRow(inspector, "Sprite Y", _overlayY);
        AddRow(inspector, "Sprite width", _overlayWidth);
        AddRow(inspector, "Sprite height", _overlayHeight);
        AddRow(inspector, "Opacity", _overlayOpacity);
        AddRow(inspector, "Edge fade px", _spriteFade);
        AddRow(inspector, "Rotation deg", _spriteRotation);
        StyleCheck(_spriteGlow);
        AddWide(inspector, _spriteGlow);
        AddRow(inspector, "Glow color", _spriteGlowColor);
        AddRow(inspector, "Glow strength", _spriteGlowStrength);
        AddRow(inspector, "Glow radius px", _spriteGlowRadius);
        StyleCheck(_spriteBreathe);
        AddWide(inspector, _spriteBreathe);
        AddRow(inspector, "Breathe min %", _spriteBreatheMin);
        AddRow(inspector, "Period seconds", _spriteBreathePeriod);
        AddRow(inspector, "Phase degrees", _spriteBreathePhase);
        AddHint(inspector, "Drop, paste, or right-click transparent PNGs. Drag the image itself; drag corners or edges to resize; use the circle above it to rotate. Glow has its own color wheel and can breathe without fading the PNG.");
        return inspector;
    }

    private Control BuildControlsInspector()
    {
        var inspector = InspectorTable();
        AddSection(inspector, "IN-GAME SIDE CONTROLS");
        _controlCombo.Items.AddRange(Enum.GetNames<KeyboardRuntimeControl>());
        _controlCombo.SelectedIndex = 0;
        StyleCombo(_controlCombo);
        AddRow(inspector, "Control", _controlCombo);
        AddRow(inspector, "Selected part", _controlPartLabel);
        AddRow(inspector, "Offset X", _controlX);
        AddRow(inspector, "Offset Y", _controlY);
        AddRow(inspector, "Box width", _controlWidth);
        AddRow(inspector, "Box height", _controlHeight);
        AddSection(inspector, "SELECTED CHILD");
        AddRow(inspector, "Child X", _controlPartX);
        AddRow(inspector, "Child Y", _controlPartY);
        AddRow(inspector, "Child width", _controlPartWidth);
        AddRow(inspector, "Child height", _controlPartHeight);
        AddRow(inspector, "Text scale", _controlPartScale);
        AddSection(inspector, "ARROW SYMBOL");
        var arrowActions = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = true };
        arrowActions.Controls.Add(ActionButton("Choose PNG", (_, _) => ChooseControlArrow(), compact: true));
        arrowActions.Controls.Add(ActionButton("Use triangle", (_, _) => ClearControlArrow(), compact: true));
        AddWide(inspector, arrowActions);
        _controlArrowName.ForeColor = TextMuted;
        AddWide(inspector, _controlArrowName);
        AddRow(inspector, "Rotation deg", _controlArrowRotation);
        StyleCheck(_controlArrowGlow);
        AddWide(inspector, _controlArrowGlow);
        AddRow(inspector, "Glow color", _controlArrowGlowColor);
        AddRow(inspector, "Glow strength", _controlArrowGlowStrength);
        AddRow(inspector, "Glow radius px", _controlArrowGlowRadius);
        StyleCheck(_controlArrowBreathe);
        AddWide(inspector, _controlArrowBreathe);
        AddRow(inspector, "Breathe min %", _controlArrowBreatheMin);
        AddRow(inspector, "Period seconds", _controlArrowBreathePeriod);
        AddRow(inspector, "Phase degrees", _controlArrowBreathePhase);
        AddHint(inspector, "Click empty space inside a settings control to select its outer box; drag its handles to widen or resize the group. Click directly on an arrow, label glyph, or value glyph to select and move that child alone. Arrow children have their own resize handles. Procedural glow and breathing work with both the built-in triangles and replacement PNGs. Down reuses Up rotated 180 degrees.");
        return inspector;
    }

    private Control BuildStatusBar()
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Bottom,
            Height = 32,
            BackColor = SurfaceRaised,
            Padding = new Padding(12, 0, 12, 0),
            ColumnCount = 2,
            RowCount = 1
        };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        _statusLabel.ForeColor = TextMuted;
        _historyLabel.ForeColor = AccentBright;
        _historyLabel.Dock = DockStyle.Fill;
        panel.Controls.Add(_statusLabel, 0, 0);
        panel.Controls.Add(_historyLabel, 1, 0);
        return panel;
    }

    private void DiscoverAssets()
    {
        string besideExe = Path.Combine(AppContext.BaseDirectory, "Assets");
        string sourceAssets = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "assets"));
        _assetsDirectory = Directory.Exists(besideExe) ? besideExe : sourceAssets;
        if (!Directory.Exists(_assetsDirectory))
            throw new DirectoryNotFoundException("OCU Keyboard Studio could not find its Assets folder.");

        foreach (string metadata in Directory.EnumerateFiles(_assetsDirectory, "*.sfn").OrderBy(Path.GetFileName))
        {
            string stem = Path.GetFileNameWithoutExtension(metadata);
            string texture = Path.Combine(_assetsDirectory, stem + "-texture.png");
            if (!File.Exists(texture))
                continue;
            string display = stem.EndsWith("-30", StringComparison.OrdinalIgnoreCase) ? stem[..^3] : stem;
            string configName = display.ToLowerInvariant() switch
            {
                "parchmentmf" => "parchment",
                "medievalsharp" => "medieval",
                "ocu-nordic" => "ocu_nordic",
                "ocu-unease" => "ocu_unease",
                _ => display.ToLowerInvariant().Replace('-', '_')
            };
            FontImportManifest? manifest = FontImporter.ReadManifest(_assetsDirectory, configName);
            string? sourceFont = manifest is null ? null : Path.Combine(_assetsDirectory, manifest.SourceFile);
            if (sourceFont is not null && !File.Exists(sourceFont)) sourceFont = null;
            _fonts.Add(new FontChoice(manifest?.DisplayName ?? display, metadata, texture, configName, sourceFont));
        }
        if (_fonts.Count == 0)
            throw new InvalidDataException("No matching .sfn and -texture.png font pairs were found.");

        _fontCombo.Items.AddRange(_fonts.Cast<object>().ToArray());
        _themeCombo.Items.AddRange(KeyboardTheme.BuiltIns.Cast<object>().ToArray());
        _stateCombo.Items.AddRange(Enum.GetNames<KeyboardPreviewState>());
        _modeStatePreview.Items.AddRange(["VR Mode", "PC Mode"]);
        _lockStatePreview.Items.AddRange(["World / Unlocked", "Head-Locked"]);
        int parchmentTheme = KeyboardTheme.BuiltIns.ToList().FindIndex(theme => ThemeConfigName(theme) == "parchment");
        _themeCombo.SelectedIndex = parchmentTheme >= 0 ? parchmentTheme : 0;
        int parchmentFont = _fonts.FindIndex(font => font.ConfigName == "parchment");
        _fontCombo.SelectedIndex = parchmentFont >= 0 ? parchmentFont : 0;
        _stateCombo.SelectedIndex = 0;
        _modeStatePreview.SelectedIndex = 0;
        _lockStatePreview.SelectedIndex = 0;

        SudoFont font = LoadSelectedFont();
        _renderer = new KeyboardRenderer(_assetsDirectory, font)
        {
            Theme = (KeyboardTheme)_themeCombo.SelectedItem!
        };
        _canvas.Renderer = _renderer;
    }

    private void LoadDefaultLayout()
    {
        _currentPackagePath = null;
        string path = Path.Combine(_assetsDirectory, "en_gb.kb");
        if (!File.Exists(path))
            throw new FileNotFoundException("The bundled en_gb.kb layout is missing.", path);
        SetDocument(KeyboardDocument.Load(path), clearHistory: true);
        RefreshDesignChoices(path);
        SetStatus(_hostOcuRoot is null
            ? "Loaded the real OCU en_gb.kb layout. Drag a key label to start editing."
            : $"Loaded Parchment. Save applies directly to this OCU: {_hostOcuRoot}");
    }

    private void WireEvents()
    {
        _inspectorTabs.SelectedIndexChanged += (_, _) => UpdatePreviewMode();
        _canvas.SelectionChanged += (_, _) =>
        {
            if (!_canvas.HasMultipleSelection)
                _inspectorTabs.SelectedIndex = _canvas.SelectionKind switch
                {
                    CanvasSelectionKind.Sprite or CanvasSelectionKind.Background => 3,
                    CanvasSelectionKind.Control or CanvasSelectionKind.ControlUpArrow or CanvasSelectionKind.ControlLabel
                        or CanvasSelectionKind.ControlValue or CanvasSelectionKind.ControlDownArrow => 4,
                    CanvasSelectionKind.KeyContent or CanvasSelectionKind.KeyPlate => 0,
                    CanvasSelectionKind.TopTextBar or CanvasSelectionKind.TopMode or CanvasSelectionKind.TopModeArtwork
                        or CanvasSelectionKind.TopModeText or CanvasSelectionKind.TopLock
                        or CanvasSelectionKind.TopLockArtwork or CanvasSelectionKind.TopLockText => 0,
                    _ => _inspectorTabs.SelectedIndex
                };
            if (_canvas.HasMultipleSelection)
            {
                SetStatus($"{_canvas.SelectionCount} visible items selected. Drag any outlined item to move the group; Ctrl/Shift-drag adds more.");
            }
            PopulateInspector();
            PopulateArtworkInspector();
            PopulateControlsInspector();
        };
        _canvas.EditStarted += (_, _) => _pendingCanvasUndo ??= _document.Clone();
        _canvas.ViewZoomChanged += (_, _) => SetStatus(
            $"Canvas zoom: {_canvas.ViewZoomPercent}%. Scroll over selected text or artwork to resize it; scroll elsewhere to zoom. Middle-drag pans.");
        _canvas.DocumentChanged += (_, _) =>
        {
            PopulateInspector();
            PopulateAppearanceInspector();
            PopulateInputInspector();
            PopulateArtworkInspector();
            PopulateControlsInspector();
            UpdateTitle();
        };
        _canvas.EditCompleted += (_, _) =>
        {
            if (_pendingCanvasUndo is null)
                return;
            if (!_pendingCanvasUndo.IsEquivalentForHistory(_document))
            {
                PushUndo(_pendingCanvasUndo);
                _redo.Clear();
            }
            else
            {
                // Pointer jitter can enter the drag path without changing any
                // authored value. Keep the prior dirty state and Redo chain.
                _document.IsDirty = _pendingCanvasUndo.IsDirty;
            }
            _pendingCanvasUndo = null;
            UpdateHistoryControls();
        };
        _canvas.ChooseControlArrowRequested += (_, _) => ChooseControlArrow();
        _canvas.UseBuiltInControlArrowRequested += (_, _) => ClearControlArrow();
        _canvas.KeyboardFileDropped += path =>
        {
            if (ConfirmDiscard("Opening the dropped keyboard"))
                LoadKeyboardFile(path);
        };

        _designCombo.SelectedIndexChanged += (_, _) =>
        {
            if (_updatingDesignLibrary || _designCombo.SelectedItem is not KeyboardDesignChoice choice)
                return;
            string? currentPath = _document.SourcePath;
            if (!string.IsNullOrWhiteSpace(currentPath)
                && Path.GetFullPath(currentPath).Equals(Path.GetFullPath(choice.Path), StringComparison.OrdinalIgnoreCase))
                return;
            if (!ConfirmDiscard("Switching keyboard designs"))
            {
                RefreshDesignChoices(currentPath);
                return;
            }
            try
            {
                _currentPackagePath = null;
                SetDocument(KeyboardDocument.Load(choice.Path), clearHistory: true);
                RefreshDesignChoices(choice.Path);
                SetStatus(_hostOcuRoot is null
                    ? $"Loaded keyboard design: {choice.Name}. Install for Next Launch when you want to activate it."
                    : $"Loaded keyboard design: {choice.Name}. Save applies it directly to {_hostOcuRoot}.");
            }
            catch (Exception exception)
            {
                RefreshDesignChoices(currentPath);
                ShowError("Keyboard design load failed", exception);
            }
        };

        _themeCombo.SelectedIndexChanged += (_, _) =>
        {
            if (_renderer is null || _themeCombo.SelectedItem is not KeyboardTheme theme)
                return;
            _renderer.Theme = theme;
            if (!_updatingEditor)
            {
                string configName = ThemeConfigName(theme);
                if (_document.BaseTheme != configName)
                {
                    PushUndo();
                    _document.BaseTheme = configName;
                    _document.ApplyThemeControlLayout(theme);
                    _document.CustomStyleEnabled = false;
                    _document.CustomStyleInitialized = false;
                    MarkChanged();
                    SetStatus($"Base Theme applied: {theme.Name}. Its runtime control positions and Appearance palette now match the selected theme; Undo restores the prior layout and overrides.");
                }
            }
            _canvas.RefreshPreview();
            PopulateAppearanceInspector();
            PopulateInputInspector();
            PopulateControlsInspector();
        };
        _fontCombo.SelectedIndexChanged += (_, _) =>
        {
            if (_renderer is null)
                return;
            try
            {
                _renderer.Font = LoadSelectedFont();
                if (!_updatingEditor && _fontCombo.SelectedItem is FontChoice choice)
                {
                    string? metadata = choice.IsCustom ? choice.MetadataPath : null;
                    string? texture = choice.IsCustom ? choice.TexturePath : null;
                    if (_document.FontName != choice.ConfigName
                        || _document.CustomFontMetadataPath != metadata
                        || _document.CustomFontTexturePath != texture)
                    {
                        PushUndo();
                        _document.FontName = choice.ConfigName;
                        _document.CustomFontMetadataPath = metadata;
                        _document.CustomFontTexturePath = texture;
                        MarkChanged();
                    }
                    SetStatus($"Design font: {_fontCombo.SelectedItem}. Shared exports carry this exact selection.");
                }
                _canvas.RefreshPreview();
                RefreshConsoleInputPreview();
            }
            catch (Exception exception)
            {
                ShowError("Font load failed", exception);
            }
        };
        _stateCombo.SelectedIndexChanged += (_, _) =>
        {
            if (_renderer is null)
                return;
            _renderer.State = (KeyboardPreviewState)_stateCombo.SelectedIndex;
            _canvas.RefreshPreview();
            PopulateInspector();
        };
        _modeStatePreview.SelectedIndexChanged += (_, _) =>
        {
            if (_renderer is null)
                return;
            _renderer.PreviewPcMode = _modeStatePreview.SelectedIndex == 1;
            _canvas.RefreshPreview();
        };
        _lockStatePreview.SelectedIndexChanged += (_, _) =>
        {
            if (_renderer is null)
                return;
            _renderer.PreviewHeadLocked = _lockStatePreview.SelectedIndex == 1;
            _canvas.RefreshPreview();
        };
        _modeTextOverArtwork.CheckedChanged += (_, _) => ApplyDocumentChange(
            document => document.ModeTextOverArtwork = _modeTextOverArtwork.Checked);
        _lockTextOverArtwork.CheckedChanged += (_, _) => ApplyDocumentChange(
            document => document.LockTextOverArtwork = _lockTextOverArtwork.Checked);
        _pressedCheck.CheckedChanged += (_, _) =>
        {
            if (_renderer is not null)
                _renderer.Pressed = _pressedCheck.Checked;
            _canvas.RefreshPreview();
        };
        _snapCheck.CheckedChanged += (_, _) => _canvas.SnapToTenth = _snapCheck.Checked;
        _keyPlatesEnabled.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.KeyPlatesEnabled = _keyPlatesEnabled.Checked);
        _topButtonPlatesEnabled.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.TopButtonPlatesEnabled = _topButtonPlatesEnabled.Checked);
        _inputBarPlateEnabled.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.InputBarPlateEnabled = _inputBarPlateEnabled.Checked);
        _parchmentRibbonEnabled.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.ParchmentRibbonEnabled = _parchmentRibbonEnabled.Checked);
        _glowEnabled.CheckedChanged += (_, _) => ApplyStyleChange(document => document.GlowEnabled = _glowEnabled.Checked);
        _fontGlowEnabled.CheckedChanged += (_, _) => ApplyStyleChange(document => document.FontGlowEnabled = _fontGlowEnabled.Checked);
        _hoverEnabled.CheckedChanged += (_, _) => ApplyStyleChange(document => document.HoverEnabled = _hoverEnabled.Checked);
        _outlineEnabled.CheckedChanged += (_, _) => ApplyStyleChange(document => document.OutlineEnabled = _outlineEnabled.Checked);
        _glowStrength.ValueChanged += (_, _) => ApplyStyleChange(document => document.GlowStrength = (int)_glowStrength.Value);
        _glowRadius.ValueChanged += (_, _) => ApplyStyleChange(document => document.GlowRadius = (int)_glowRadius.Value);
        _fontGlowStrength.ValueChanged += (_, _) => ApplyStyleChange(document => document.FontGlowStrength = (int)_fontGlowStrength.Value);
        _fontGlowRadius.ValueChanged += (_, _) => ApplyStyleChange(document => document.FontGlowRadius = (int)_fontGlowRadius.Value);
        _hoverStrength.ValueChanged += (_, _) => ApplyStyleChange(document => document.HoverStrength = (int)_hoverStrength.Value);
        _keyRoundness.ValueChanged += (_, _) => ApplyStyleChange(document => document.KeyRoundness = (int)_keyRoundness.Value);
        _plateOutlineWidth.ValueChanged += (_, _) => ApplyStyleChange(document => document.PlateOutlineWidth = (int)_plateOutlineWidth.Value);
        _inputOutlineVisible.CheckedChanged += (_, _) => ApplyInputPanelChange(
            document => document.InputOutlineVisible = _inputOutlineVisible.Checked);
        _inputOutlineWidth.ValueChanged += (_, _) => ApplyInputOutlineChange(document => document.InputOutlineWidth = (int)_inputOutlineWidth.Value);
        _inputTitleX.ValueChanged += (_, _) => ApplyInputPanelChange(document => document.InputTitleOffsetX = (float)_inputTitleX.Value);
        _inputTitleY.ValueChanged += (_, _) => ApplyInputPanelChange(document => document.InputTitleOffsetY = (float)_inputTitleY.Value);
        _inputTextX.ValueChanged += (_, _) => ApplyInputPanelChange(document => document.InputTextOffsetX = (float)_inputTextX.Value);
        _inputTextY.ValueChanged += (_, _) => ApplyInputPanelChange(document => document.InputTextOffsetY = (float)_inputTextY.Value);
        _keyBreathe.CheckedChanged += (_, _) => ApplyStyleChange(document => document.KeyBreatheEnabled = _keyBreathe.Checked);
        _keyBreatheMin.ValueChanged += (_, _) => ApplyStyleChange(document => document.KeyBreatheMinPercent = (int)_keyBreatheMin.Value);
        _keyBreathePeriod.ValueChanged += (_, _) => ApplyStyleChange(document => document.KeyBreathePeriodSeconds = (float)_keyBreathePeriod.Value);
        _keyBreathePhase.ValueChanged += (_, _) => ApplyStyleChange(document => document.KeyBreathePhaseDegrees = (float)_keyBreathePhase.Value);
        _fontBreathe.CheckedChanged += (_, _) => ApplyStyleChange(document => document.FontBreatheEnabled = _fontBreathe.Checked);
        _fontBreatheMin.ValueChanged += (_, _) => ApplyStyleChange(document => document.FontBreatheMinPercent = (int)_fontBreatheMin.Value);
        _fontBreathePeriod.ValueChanged += (_, _) => ApplyStyleChange(document => document.FontBreathePeriodSeconds = (float)_fontBreathePeriod.Value);
        _fontBreathePhase.ValueChanged += (_, _) => ApplyStyleChange(document => document.FontBreathePhaseDegrees = (float)_fontBreathePhase.Value);
        WireVisualColor(_fontColor, "Font color", () => EffectiveStyle().Font, color => _document.FontColor = color);
        WireVisualColor(_fontOutlineColor, "Font outline color and transparency", () => EffectiveStyle().FontOutline, color => _document.FontOutlineColor = color);
        WireVisualColor(_fontGlowColor, "Font glow color and transparency", () => EffectiveStyle().FontGlow, color => _document.FontGlowColor = color);
        WireVisualColor(_plateFillColor, "Plate fill color and transparency", () => EffectiveStyle().PlateFill, color => _document.PlateFillColor = color);
        WireVisualColor(_keyColor, "Plate outline color and transparency", () => EffectiveStyle().PlateOutline, color => _document.KeyColor = color);
        WireInputFillColor();
        WireInputOutlineColor();
        WireVisualColor(_glowColor, "Glow color", () => EffectiveStyle().PlateGlow, color => _document.GlowColor = color);
        WireVisualColor(_hoverColor, "Hover color", () => EffectiveStyle().Hover, color => _document.HoverColor = color);

        _consoleInputPreview.MouseDown += ConsoleInputPreviewMouseDown;
        _consoleInputPreview.MouseMove += ConsoleInputPreviewMouseMove;
        _consoleInputPreview.MouseUp += ConsoleInputPreviewMouseUp;
        _consoleInputPreview.MouseLeave += (_, _) =>
        {
            if (!_draggingConsoleInputPart)
                _consoleInputPreview.Cursor = Cursors.Default;
        };

        _backgroundX.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundX = (float)_backgroundX.Value);
        _backgroundY.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundY = (float)_backgroundY.Value);
        _backgroundWidth.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundWidth = (float)_backgroundWidth.Value);
        _backgroundHeight.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundHeight = (float)_backgroundHeight.Value);
        _backgroundOpacity.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundOpacity = (int)_backgroundOpacity.Value);
        _backgroundFade.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundEdgeFade = (int)_backgroundFade.Value);
        _backgroundRotation.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundRotation = (float)_backgroundRotation.Value);
        _backgroundRoundness.ValueChanged += (_, _) => ApplyDocumentChange(document => document.BackgroundRoundness = (int)_backgroundRoundness.Value);
        _overlayX.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.X = (float)_overlayX.Value);
        _overlayY.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.Y = (float)_overlayY.Value);
        _overlayWidth.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.Width = (float)_overlayWidth.Value);
        _overlayHeight.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.Height = (float)_overlayHeight.Value);
        _overlayOpacity.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.Opacity = (int)_overlayOpacity.Value);
        _spriteFade.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.EdgeFade = (int)_spriteFade.Value);
        _spriteRotation.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.Rotation = (float)_spriteRotation.Value);
        _spriteGlow.CheckedChanged += (_, _) => ApplySpriteChange(sprite => sprite.GlowEnabled = _spriteGlow.Checked);
        _spriteGlowColor.SwatchClicked += (_, _) => ChooseSpriteGlowColor();
        _spriteGlowColor.ColorCommitted += ApplySpriteGlowColor;
        _spriteGlowStrength.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.GlowStrength = (int)_spriteGlowStrength.Value);
        _spriteGlowRadius.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.GlowRadius = (int)_spriteGlowRadius.Value);
        _spriteBreathe.CheckedChanged += (_, _) => ApplySpriteChange(sprite => sprite.BreatheEnabled = _spriteBreathe.Checked);
        _spriteBreatheMin.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.BreatheMinPercent = (int)_spriteBreatheMin.Value);
        _spriteBreathePeriod.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.BreathePeriodSeconds = (float)_spriteBreathePeriod.Value);
        _spriteBreathePhase.ValueChanged += (_, _) => ApplySpriteChange(sprite => sprite.BreathePhaseDegrees = (float)_spriteBreathePhase.Value);
        _spriteList.SelectedIndexChanged += (_, _) =>
        {
            if (_updatingEditor)
                return;
            _canvas.SelectSprite(_spriteList.SelectedItem as KeyboardSprite);
            PopulateArtworkInspector();
        };
        _controlCombo.SelectedIndexChanged += (_, _) =>
        {
            if (_updatingEditor || _controlCombo.SelectedIndex < 0)
                return;
            _canvas.SelectControl((KeyboardRuntimeControl)_controlCombo.SelectedIndex);
            PopulateControlsInspector();
            _canvas.RefreshPreview();
        };
        _controlX.ValueChanged += (_, _) => ApplyControlChange((float)_controlX.Value, (float)_controlY.Value);
        _controlY.ValueChanged += (_, _) => ApplyControlChange((float)_controlX.Value, (float)_controlY.Value);
        _controlWidth.ValueChanged += (_, _) => ApplyControlDesignChange(design => design.Width = (float)_controlWidth.Value);
        _controlHeight.ValueChanged += (_, _) => ApplyControlDesignChange(design => design.Height = (float)_controlHeight.Value);
        _controlPartX.ValueChanged += (_, _) => ApplySelectedControlPartChange((design, part) =>
        {
            if (part == KeyboardControlPart.UpArrow) design.UpOffsetX = (float)_controlPartX.Value;
            else if (part == KeyboardControlPart.DownArrow) design.DownOffsetX = (float)_controlPartX.Value;
            else if (part == KeyboardControlPart.Label) design.LabelOffsetX = (float)_controlPartX.Value;
            else if (part == KeyboardControlPart.Value) design.ValueOffsetX = (float)_controlPartX.Value;
        });
        _controlPartY.ValueChanged += (_, _) => ApplySelectedControlPartChange((design, part) =>
        {
            if (part == KeyboardControlPart.UpArrow) design.UpOffsetY = (float)_controlPartY.Value;
            else if (part == KeyboardControlPart.DownArrow) design.DownOffsetY = (float)_controlPartY.Value;
            else if (part == KeyboardControlPart.Label) design.LabelOffsetY = (float)_controlPartY.Value;
            else if (part == KeyboardControlPart.Value) design.ValueOffsetY = (float)_controlPartY.Value;
        });
        _controlPartWidth.ValueChanged += (_, _) => ApplySelectedControlPartChange((design, part) =>
        {
            if (part == KeyboardControlPart.UpArrow) design.UpWidth = (float)_controlPartWidth.Value;
            else if (part == KeyboardControlPart.DownArrow) design.DownWidth = (float)_controlPartWidth.Value;
        });
        _controlPartHeight.ValueChanged += (_, _) => ApplySelectedControlPartChange((design, part) =>
        {
            if (part == KeyboardControlPart.UpArrow) design.UpHeight = (float)_controlPartHeight.Value;
            else if (part == KeyboardControlPart.DownArrow) design.DownHeight = (float)_controlPartHeight.Value;
        });
        _controlPartScale.ValueChanged += (_, _) => ApplySelectedControlPartChange((design, part) =>
        {
            if (part == KeyboardControlPart.Label) design.LabelScale = (float)_controlPartScale.Value;
            else if (part == KeyboardControlPart.Value) design.ValueScale = (float)_controlPartScale.Value;
        });
        _controlArrowRotation.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowRotation = (float)_controlArrowRotation.Value);
        _controlArrowGlow.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowGlowEnabled = _controlArrowGlow.Checked);
        _controlArrowGlowColor.SwatchClicked += (_, _) => ChooseControlArrowGlowColor();
        _controlArrowGlowColor.ColorCommitted += ApplyControlArrowGlowColor;
        _controlArrowGlowStrength.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowGlowStrength = (int)_controlArrowGlowStrength.Value);
        _controlArrowGlowRadius.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowGlowRadius = (int)_controlArrowGlowRadius.Value);
        _controlArrowBreathe.CheckedChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowBreatheEnabled = _controlArrowBreathe.Checked);
        _controlArrowBreatheMin.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowBreatheMinPercent = (int)_controlArrowBreatheMin.Value);
        _controlArrowBreathePeriod.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowBreathePeriodSeconds = (float)_controlArrowBreathePeriod.Value);
        _controlArrowBreathePhase.ValueChanged += (_, _) => ApplyDocumentChange(document => document.ControlArrowBreathePhaseDegrees = (float)_controlArrowBreathePhase.Value);

        _baseLabel.TextChanged += (_, _) => ApplyInspectorChange(key => key.Label = _baseLabel.Text);
        _shiftLabel.TextChanged += (_, _) => ApplyInspectorChange(key => key.ShiftLabel = _shiftLabel.Text);
        _baseLabel.Leave += (_, _) => AuditSelectedCustomFont(showMessage: true);
        _shiftLabel.Leave += (_, _) => AuditSelectedCustomFont(showMessage: true);
        _assignedKeyButton.Click += (_, _) => BeginAssignedKeyCapture();
        _assignedKeyButton.LostFocus += (_, _) =>
        {
            if (!_capturingAssignedKey)
                return;
            _capturingAssignedKey = false;
            PopulateInspector();
            SetStatus("Physical-key capture cancelled.");
        };
        _x.ValueChanged += (_, _) => ApplyInspectorChange(key => key.X = (float)_x.Value);
        _y.ValueChanged += (_, _) => ApplyInspectorChange(key => key.Y = (float)_y.Value);
        _width.ValueChanged += (_, _) => ApplyInspectorChange(key =>
        {
            key.Width = (float)_width.Value;
            key.SpansToRight = false;
        });
        _height.ValueChanged += (_, _) => ApplyInspectorChange(key => key.Height = (float)_height.Value);
        _offsetX.ValueChanged += (_, _) => ApplyInspectorChange(key => key.LabelOffsetX = (float)_offsetX.Value);
        _offsetY.ValueChanged += (_, _) => ApplyInspectorChange(key => key.LabelOffsetY = (float)_offsetY.Value);
        _scale.ValueChanged += (_, _) => ApplyInspectorChange(key => key.LabelScale = (float)_scale.Value);
        _topElementX.ValueChanged += (_, _) => ApplyTopElementChange((float)_topElementX.Value, (float)_topElementY.Value);
        _topElementY.ValueChanged += (_, _) => ApplyTopElementChange((float)_topElementX.Value, (float)_topElementY.Value);
        _topElementWidth.ValueChanged += (_, _) => ApplyTopElementDesignChange();
        _topElementHeight.ValueChanged += (_, _) => ApplyTopElementDesignChange();
        _topElementFontScale.ValueChanged += (_, _) => ApplyTopElementDesignChange();
        _layoutWidth.ValueChanged += (_, _) =>
        {
            if (_updatingEditor)
                return;
            PushUndo();
            _document.Width = (int)_layoutWidth.Value;
            MarkChanged();
        };

        FormClosing += OnFormClosing;
        KeyPreview = true;
        KeyDown += OnShortcutKeyDown;
    }

    private void ApplyInspectorChange(Action<KeyboardKey> apply)
    {
        if (_updatingEditor || _canvas.SelectedKey is not KeyboardKey key)
            return;
        PushUndo();
        apply(key);
        MarkChanged();
    }

    private void ApplyDocumentChange(Action<KeyboardDocument> apply)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        apply(_document);
        MarkChanged();
        PopulateAppearanceInspector();
        PopulateInputInspector();
        PopulateArtworkInspector();
        PopulateControlsInspector();
    }

    private KeyboardTheme ActiveTheme()
        => _themeCombo.SelectedItem as KeyboardTheme
            ?? _renderer?.Theme
            ?? KeyboardTheme.BuiltIns[0];

    private EffectiveKeyboardStyle EffectiveStyle()
    {
        if (_document.CustomStyleEnabled)
            return new(_document.FontColor, _document.FontOutlineColor, _document.FontGlowColor,
                _document.PlateFillColor, _document.KeyColor, _document.GlowColor, _document.HoverColor);

        KeyboardTheme theme = ActiveTheme();
        return new(theme.Ink, Color.FromArgb(220, 8, 11, 15), theme.Bright,
            theme.KeyIdle, theme.Accent, theme.Bright,
            Color.FromArgb(255, theme.KeyHot.R, theme.KeyHot.G, theme.KeyHot.B));
    }

    private void EnsureStyleOverride()
        => _document.EnableCustomStyleFromTheme(ActiveTheme());

    private void ApplyStyleChange(Action<KeyboardDocument> apply)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        EnsureStyleOverride();
        apply(_document);
        MarkChanged();
        PopulateAppearanceInspector();
        PopulateInputInspector();
    }

    private void ResetStyleToBaseTheme()
    {
        if (_updatingEditor)
            return;
        if (!_document.CustomStyleEnabled && !_document.CustomStyleInitialized)
        {
            PopulateAppearanceInspector();
            SetStatus($"Appearance is already using the {ActiveTheme().Name} Base Theme palette.");
            return;
        }

        PushUndo();
        _document.CustomStyleEnabled = false;
        _document.CustomStyleInitialized = false;
        MarkChanged();
        PopulateAppearanceInspector();
        PopulateInputInspector();
        SetStatus($"Colors and effects reset to the {ActiveTheme().Name} Base Theme. Undo restores the overrides.");
    }

    private Color EffectiveInputOutlineColor()
        => _document.InputOutlineOverrideEnabled
            ? _document.InputOutlineColor
            : _document.CustomStyleEnabled ? _document.KeyColor : KeyboardRenderer.ConsoleBorderForTheme(ActiveTheme());

    private int EffectiveInputOutlineWidth()
        => _document.InputOutlineOverrideEnabled
            ? _document.InputOutlineWidth
            : _document.CustomStyleEnabled ? _document.PlateOutlineWidth : 2;

    private Color EffectiveInputFillColor()
        => _document.InputFillOverrideEnabled
            ? _document.InputFillColor : KeyboardRenderer.ConsoleBackgroundForTheme(ActiveTheme());

    private void ApplyInputPanelChange(Action<KeyboardDocument> apply)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        apply(_document);
        MarkChanged();
        PopulateInputInspector();
    }

    private void EnsureInputFillOverride()
    {
        if (_document.InputFillOverrideEnabled)
            return;
        _document.InputFillColor = KeyboardRenderer.ConsoleBackgroundForTheme(ActiveTheme());
        _document.InputFillOverrideEnabled = true;
    }

    private void WireInputFillColor()
    {
        _inputFillColor.SwatchClicked += (_, _) =>
        {
            using var dialog = new ColorWheelDialog("INPUT inside color and transparency", EffectiveInputFillColor());
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return;
            PushUndo();
            EnsureInputFillOverride();
            _document.InputFillColor = dialog.SelectedColor;
            MarkChanged();
            PopulateInputInspector();
        };
        _inputFillColor.ColorCommitted += color =>
        {
            if (_updatingEditor)
                return;
            PushUndo();
            EnsureInputFillOverride();
            _document.InputFillColor = color;
            MarkChanged();
            PopulateInputInspector();
        };
    }

    private void ResetInputFill()
    {
        if (_updatingEditor)
            return;
        if (!_document.InputFillOverrideEnabled)
        {
            SetStatus("Console INPUT is already using its keyboard theme fill.");
            return;
        }
        PushUndo();
        _document.InputFillOverrideEnabled = false;
        MarkChanged();
        PopulateInputInspector();
        SetStatus("Console INPUT inside color reset to the keyboard theme fill.");
    }

    private void EnsureInputOutlineOverride()
    {
        if (_document.InputOutlineOverrideEnabled)
            return;
        _document.InputOutlineColor = _document.CustomStyleEnabled
            ? _document.KeyColor : KeyboardRenderer.ConsoleBorderForTheme(ActiveTheme());
        _document.InputOutlineWidth = _document.CustomStyleEnabled
            ? _document.PlateOutlineWidth : 2;
        _document.InputOutlineOverrideEnabled = true;
    }

    private void ApplyInputOutlineChange(Action<KeyboardDocument> apply)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        EnsureInputOutlineOverride();
        apply(_document);
        MarkChanged();
        PopulateInputInspector();
    }

    private void WireInputOutlineColor()
    {
        _inputOutlineColor.SwatchClicked += (_, _) =>
        {
            using var dialog = new ColorWheelDialog("Input outline color and transparency", EffectiveInputOutlineColor());
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return;
            PushUndo();
            EnsureInputOutlineOverride();
            _document.InputOutlineColor = dialog.SelectedColor;
            MarkChanged();
            PopulateInputInspector();
        };
        _inputOutlineColor.ColorCommitted += color => ApplyInputOutlineChange(
            document => document.InputOutlineColor = color);
    }

    private void ResetInputOutline()
    {
        if (_updatingEditor)
            return;
        if (!_document.InputOutlineOverrideEnabled)
        {
            SetStatus("Console INPUT is already using its default border.");
            return;
        }
        PushUndo();
        _document.InputOutlineOverrideEnabled = false;
        MarkChanged();
        PopulateInputInspector();
        SetStatus("Console INPUT border reset to its theme/custom-style default.");
    }

    private void ResetInputTextPositions()
    {
        if (_updatingEditor)
            return;
        if (Math.Abs(_document.InputTitleOffsetX) < 0.001f
            && Math.Abs(_document.InputTitleOffsetY) < 0.001f
            && Math.Abs(_document.InputTextOffsetX) < 0.001f
            && Math.Abs(_document.InputTextOffsetY) < 0.001f)
            return;
        PushUndo();
        _document.InputTitleOffsetX = 0;
        _document.InputTitleOffsetY = 0;
        _document.InputTextOffsetX = 0;
        _document.InputTextOffsetY = 0;
        MarkChanged();
        PopulateInputInspector();
        SetStatus("Console INPUT heading and typed text returned to their runtime defaults.");
    }

    private void ApplyTopElementChange(float x, float y)
    {
        if (_updatingEditor || _canvas.SelectedTopElement is not KeyboardTopElement element)
            return;
        PushUndo();
        switch (_canvas.SelectionKind)
        {
            case CanvasSelectionKind.TopModeArtwork: _document.ModeArtworkOffsetX = x; _document.ModeArtworkOffsetY = y; break;
            case CanvasSelectionKind.TopLockArtwork: _document.LockArtworkOffsetX = x; _document.LockArtworkOffsetY = y; break;
            case CanvasSelectionKind.TopModeText: _document.ModeTextOffsetX = x; _document.ModeTextOffsetY = y; break;
            case CanvasSelectionKind.TopLockText: _document.LockTextOffsetX = x; _document.LockTextOffsetY = y; break;
            default:
                switch (element)
                {
                    case KeyboardTopElement.TextBar: _document.TextBarOffsetX = x; _document.TextBarOffsetY = y; break;
                    case KeyboardTopElement.Mode:
                    case KeyboardTopElement.Lock:
                        _canvas.SetTopInteractionOffsetsPreservingVisuals(element, x, y); break;
                }
                break;
        }
        MarkChanged();
        PopulateInspector();
    }

    private void ApplyTopElementDesignChange()
    {
        if (_updatingEditor || _canvas.SelectedTopElement is not KeyboardTopElement element)
            return;
        PushUndo();
        float width = (float)_topElementWidth.Value;
        float height = (float)_topElementHeight.Value;
        float scale = (float)_topElementFontScale.Value;
        switch (_canvas.SelectionKind)
        {
            case CanvasSelectionKind.TopModeArtwork:
                _document.ModeArtworkWidth = width; _document.ModeArtworkHeight = height; break;
            case CanvasSelectionKind.TopLockArtwork:
                _document.LockArtworkWidth = width; _document.LockArtworkHeight = height; break;
            case CanvasSelectionKind.TopModeText:
                _document.ModeButtonFontScale = scale; break;
            case CanvasSelectionKind.TopLockText:
                _document.LockButtonFontScale = scale; break;
            default:
                switch (element)
                {
                    case KeyboardTopElement.TextBar:
                        _document.TextBarWidth = width; _document.TextBarHeight = height;
                        _document.TextBarFontScale = scale; break;
                    case KeyboardTopElement.Mode:
                    case KeyboardTopElement.Lock:
                        _canvas.SetTopInteractionSizePreservingVisuals(element, width, height); break;
                }
                break;
        }
        MarkChanged();
        PopulateInspector();
    }

    private void ApplySpriteChange(Action<KeyboardSprite> apply)
    {
        if (_updatingEditor || _canvas.SelectedSprite is not KeyboardSprite sprite)
            return;
        PushUndo();
        apply(sprite);
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void ApplyControlChange(float x, float y)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        switch (_canvas.SelectedControl)
        {
            case KeyboardRuntimeControl.Size: _document.SizeControlOffsetX = x; _document.SizeControlOffsetY = y; break;
            case KeyboardRuntimeControl.Opacity: _document.OpacityControlOffsetX = x; _document.OpacityControlOffsetY = y; break;
            case KeyboardRuntimeControl.Tilt: _document.TiltControlOffsetX = x; _document.TiltControlOffsetY = y; break;
        }
        MarkChanged();
        PopulateControlsInspector();
    }

    private void ApplyControlDesignChange(Action<KeyboardControlDesign> apply)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        apply(_document.GetControlDesign(_canvas.SelectedControl));
        MarkChanged();
        PopulateControlsInspector();
    }

    private void ApplySelectedControlPartChange(Action<KeyboardControlDesign, KeyboardControlPart> apply)
    {
        if (_updatingEditor || _canvas.SelectedControlPart == KeyboardControlPart.Group)
            return;
        PushUndo();
        apply(_document.GetControlDesign(_canvas.SelectedControl), _canvas.SelectedControlPart);
        MarkChanged();
        PopulateControlsInspector();
    }

    private void ChooseVisualColor(string title, Color initial, Action<Color> apply)
    {
        using var dialog = new ColorWheelDialog(title, initial);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        PushUndo();
        EnsureStyleOverride();
        apply(dialog.SelectedColor);
        MarkChanged();
        PopulateAppearanceInspector();
    }

    private void WireVisualColor(ColorEntryControl entry, string title,
        Func<Color> current, Action<Color> apply)
    {
        entry.SwatchClicked += (_, _) => ChooseVisualColor(title, current(), apply);
        entry.ColorCommitted += color =>
        {
            if (_updatingEditor)
                return;
            PushUndo();
            EnsureStyleOverride();
            apply(color);
            MarkChanged();
            PopulateAppearanceInspector();
        };
    }

    private void ChooseSpriteGlowColor()
    {
        if (_canvas.SelectedSprite is not KeyboardSprite sprite)
            return;
        using var dialog = new ColorWheelDialog("Sprite / ribbon glow color", sprite.GlowColor);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        PushUndo();
        sprite.GlowEnabled = true;
        sprite.GlowColor = dialog.SelectedColor;
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void ApplySpriteGlowColor(Color color)
    {
        if (_updatingEditor || _canvas.SelectedSprite is not KeyboardSprite sprite)
            return;
        PushUndo();
        sprite.GlowEnabled = true;
        sprite.GlowColor = color;
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void ChooseControlArrowGlowColor()
    {
        using var dialog = new ColorWheelDialog("Control triangle glow color", _document.ControlArrowGlowColor);
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        PushUndo();
        _document.ControlArrowGlowEnabled = true;
        _document.ControlArrowGlowColor = dialog.SelectedColor;
        MarkChanged();
        PopulateControlsInspector();
    }

    private void ApplyControlArrowGlowColor(Color color)
    {
        if (_updatingEditor)
            return;
        PushUndo();
        _document.ControlArrowGlowEnabled = true;
        _document.ControlArrowGlowColor = color;
        MarkChanged();
        PopulateControlsInspector();
    }

    private void BeginAssignedKeyCapture()
    {
        if (_canvas.SelectedKey is not KeyboardKey)
        {
            SetStatus("Select a keyboard key before assigning a physical key.");
            return;
        }
        _capturingAssignedKey = true;
        _assignedKeyButton.Text = "Press a physical key...";
        _assignedKeyButton.Focus();
        SetStatus("Press the physical key to assign. Normal and Shift output will be translated automatically.");
    }

    private void CaptureAssignedKey(Keys keyCode)
    {
        if (_canvas.SelectedKey is not KeyboardKey key)
        {
            _capturingAssignedKey = false;
            PopulateInspector();
            return;
        }

        if (!PhysicalKeyboardTranslator.TryTranslate(keyCode, out PhysicalKeyAssignment assignment))
        {
            System.Media.SystemSounds.Beep.Play();
            _assignedKeyButton.Text = "Unsupported - press another key";
            SetStatus($"{keyCode} is not an OCU keyboard output. Press a letter, number, symbol, arrow, function key, or supported control key.");
            return;
        }

        _capturingAssignedKey = false;
        if (key.Character == assignment.Normal && key.ShiftCharacter == assignment.Shifted)
        {
            PopulateInspector();
            SetStatus($"{DescribeAssignment(assignment.Normal, assignment.Shifted)} was already assigned.");
            return;
        }

        PushUndo();
        key.Character = assignment.Normal;
        key.ShiftCharacter = assignment.Shifted;
        key.Label = DefaultKeyText(assignment.Normal);
        key.ShiftLabel = DefaultKeyText(assignment.Shifted);
        MarkChanged();
        PopulateInspector();
        AuditSelectedCustomFont(showMessage: true);
        SetStatus($"Assigned {DescribeAssignment(assignment.Normal, assignment.Shifted)} using the active Windows keyboard layout.");
    }

    private static string DescribeAssignment(char normal, char shifted)
        => normal == shifted
            ? DisplayKeyName(normal)
            : $"{DisplayKeyName(normal)} / Shift + {DisplayKeyName(shifted)}";

    private static string DefaultKeyText(char key) => key switch
    {
        '\t' => "tab",
        '\n' => "Enter",
        '\b' => "back",
        ' ' => "",
        '\x01' => "shift",
        '\x02' => "caps",
        '\x03' => "Done",
        '\x04' or '\x05' or '\x06' or '\x07' => "",
        '\x0E' => "ESC",
        '\x1D' => "End",
        '\x1E' => "Ctrl",
        '\x1F' => "PrtSc",
        >= '\x10' and <= '\x1B' => $"F{key - '\x10' + 1}",
        _ => key.ToString()
    };

    private void MarkChanged()
    {
        _document.IsDirty = true;
        _redo.Clear();
        UpdateHistoryControls();
        _canvas.RefreshPreview();
        UpdateTitle();
    }

    private void PushUndo() => PushUndo(_document.Clone());

    private void PushUndo(KeyboardDocument snapshot)
    {
        _undo.Push(snapshot);
        if (_undo.Count > 150)
        {
            KeyboardDocument[] recent = _undo.Take(150).Reverse().ToArray();
            _undo.Clear();
            foreach (KeyboardDocument item in recent)
                _undo.Push(item);
        }
        UpdateHistoryControls();
    }

    private void Undo()
    {
        KeyboardDocument current = _document.Clone();
        KeyboardDocument? target = PopDifferentDocument(_undo, current);
        if (target is null)
        {
            UpdateHistoryControls();
            return;
        }
        _redo.Push(current);
        SetDocument(target, clearHistory: false, preserveCanvasSelection: true);
        SetStatus("Undid the last edit.");
    }

    private void Redo()
    {
        KeyboardDocument current = _document.Clone();
        KeyboardDocument? target = PopDifferentDocument(_redo, current);
        if (target is null)
        {
            UpdateHistoryControls();
            return;
        }
        _undo.Push(current);
        SetDocument(target, clearHistory: false, preserveCanvasSelection: true);
        SetStatus("Redid the edit.");
    }

    private static KeyboardDocument? PopDifferentDocument(Stack<KeyboardDocument> history, KeyboardDocument current)
    {
        while (history.Count > 0)
        {
            KeyboardDocument candidate = history.Pop();
            if (!candidate.IsEquivalentForHistory(current))
                return candidate;
        }
        return null;
    }

    private void UpdateHistoryControls()
    {
        // Keep the toolbar text bright even when a stack is empty. WinForms'
        // disabled-button renderer ignores ForeColor and turns these nearly
        // black against the dark toolbar; clicking an empty stack is harmless.
        if (_undoButton is not null)
            _undoButton.Enabled = true;
        if (_redoButton is not null)
            _redoButton.Enabled = true;
        _historyLabel.Text = $"History: {_undo.Count} undo / {_redo.Count} redo";
    }

    private void DuplicateKey()
    {
        if (_canvas.SelectedKey is not KeyboardKey selected)
            return;
        PushUndo();
        KeyboardKey copy = selected.Clone();
        copy.Id = _document.Keys.Count == 0 ? 0 : _document.Keys.Max(key => key.Id) + 1;
        copy.X += 0.25f;
        copy.Y += 0.25f;
        _document.Keys.Add(copy);
        _canvas.SelectedKeyId = copy.Id;
        MarkChanged();
    }

    private void DeleteKey()
    {
        if (_canvas.SelectedKey is not KeyboardKey selected)
            return;
        PushUndo();
        int index = _document.Keys.IndexOf(selected);
        _document.Keys.Remove(selected);
        _canvas.SelectedKeyId = _document.Keys.Count == 0 ? -1 : _document.Keys[Math.Clamp(index, 0, _document.Keys.Count - 1)].Id;
        MarkChanged();
    }

    private void SetDocument(KeyboardDocument document, bool clearHistory, bool preserveCanvasSelection = false)
    {
        _document = document;
        if (clearHistory)
        {
            _undo.Clear();
            _redo.Clear();
        }
        SelectDocumentThemeAndFont();
        _canvas.ReplaceDocument(_document, preserveCanvasSelection);
        PopulateInspector();
        PopulateAppearanceInspector();
        PopulateInputInspector();
        PopulateArtworkInspector();
        PopulateControlsInspector();
        _canvas.RefreshPreview();
        _canvas.Update();
        UpdateTitle();
        UpdateHistoryControls();
    }

    private void SelectDocumentThemeAndFont()
    {
        _updatingEditor = true;
        try
        {
            int themeIndex = KeyboardTheme.BuiltIns.ToList().FindIndex(theme => ThemeConfigName(theme) == _document.BaseTheme);
            _themeCombo.SelectedIndex = themeIndex >= 0 ? themeIndex
                : KeyboardTheme.BuiltIns.ToList().FindIndex(theme => ThemeConfigName(theme) == "parchment");
            int fontIndex = _fonts.FindIndex(font => font.ConfigName.Equals(_document.FontName, StringComparison.OrdinalIgnoreCase));
            if (fontIndex < 0
                && _document.FontName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase)
                && File.Exists(_document.CustomFontMetadataPath)
                && File.Exists(_document.CustomFontTexturePath))
            {
                string display = _document.FontName["custom_".Length..].Replace('_', ' ');
                _fonts.Add(new FontChoice(display, _document.CustomFontMetadataPath!,
                    _document.CustomFontTexturePath!, _document.FontName, null));
                _fontCombo.Items.Add(_fonts[^1]);
                fontIndex = _fonts.Count - 1;
            }
            _fontCombo.SelectedIndex = fontIndex >= 0 ? fontIndex : _fonts.FindIndex(font => font.ConfigName == "parchment");

            if (_renderer is not null && _themeCombo.SelectedItem is KeyboardTheme theme)
                _renderer.Theme = theme;
            if (_renderer is not null)
                _renderer.Font = LoadSelectedFont();
        }
        finally
        {
            _updatingEditor = false;
        }
        RefreshConsoleInputPreview();
    }

    private void RefreshConsoleInputPreview()
    {
        if (_renderer is null || _consoleInputPreview.IsDisposed)
            return;
        Image? previous = _consoleInputPreview.Image;
        Bitmap preview = _renderer.RenderConsolePreview(_document);
        Rectangle selection = _selectedConsoleInputPart switch
        {
            ConsoleInputPart.Heading => _renderer.ConsoleTitleBounds(_document),
            ConsoleInputPart.TypedText => _renderer.ConsoleTextBounds(_document),
            _ => Rectangle.Empty
        };
        if (!selection.IsEmpty)
        {
            selection.Inflate(7, 5);
            using Graphics graphics = Graphics.FromImage(preview);
            using var pen = new Pen(AccentBright, 2)
            {
                DashStyle = System.Drawing.Drawing2D.DashStyle.Dash
            };
            graphics.DrawRectangle(pen, selection);
        }
        _consoleInputPreview.Image = preview;
        previous?.Dispose();
    }

    private bool TryConsolePreviewPoint(Point clientPoint, out PointF texturePoint)
    {
        const float textureWidth = 1024f;
        const float textureHeight = 120f;
        float scale = Math.Min(_consoleInputPreview.ClientSize.Width / textureWidth,
            _consoleInputPreview.ClientSize.Height / textureHeight);
        if (scale <= 0)
        {
            texturePoint = default;
            return false;
        }
        float displayedWidth = textureWidth * scale;
        float displayedHeight = textureHeight * scale;
        float left = (_consoleInputPreview.ClientSize.Width - displayedWidth) * 0.5f;
        float top = (_consoleInputPreview.ClientSize.Height - displayedHeight) * 0.5f;
        if (clientPoint.X < left || clientPoint.Y < top
            || clientPoint.X >= left + displayedWidth || clientPoint.Y >= top + displayedHeight)
        {
            texturePoint = default;
            return false;
        }
        texturePoint = new PointF((clientPoint.X - left) / scale, (clientPoint.Y - top) / scale);
        return true;
    }

    private void ConsoleInputPreviewMouseDown(object? sender, MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Left || _renderer is null
            || !TryConsolePreviewPoint(e.Location, out PointF point))
            return;

        Rectangle title = _renderer.ConsoleTitleBounds(_document);
        Rectangle typed = _renderer.ConsoleTextBounds(_document);
        title.Inflate(12, 10);
        typed.Inflate(12, 10);
        _selectedConsoleInputPart = title.Contains(Point.Round(point))
            ? ConsoleInputPart.Heading
            : typed.Contains(Point.Round(point)) ? ConsoleInputPart.TypedText
            : point.Y < 60 ? ConsoleInputPart.Heading : ConsoleInputPart.TypedText;
        _draggingConsoleInputPart = true;
        _consoleInputDragStart = point;
        _pendingConsoleInputUndo = _document.Clone();
        if (_selectedConsoleInputPart == ConsoleInputPart.Heading)
        {
            _consoleInputStartX = _document.InputTitleOffsetX;
            _consoleInputStartY = _document.InputTitleOffsetY;
        }
        else
        {
            _consoleInputStartX = _document.InputTextOffsetX;
            _consoleInputStartY = _document.InputTextOffsetY;
        }
        _consoleInputPreview.Capture = true;
        _consoleInputPreview.Cursor = Cursors.SizeAll;
        RefreshConsoleInputPreview();
        SetStatus(_selectedConsoleInputPart == ConsoleInputPart.Heading
            ? "Dragging the INPUT heading."
            : "Dragging the console's typed-text row.");
    }

    private void ConsoleInputPreviewMouseMove(object? sender, MouseEventArgs e)
    {
        if (!TryConsolePreviewPoint(e.Location, out PointF point))
        {
            if (!_draggingConsoleInputPart)
                _consoleInputPreview.Cursor = Cursors.Default;
            return;
        }
        _consoleInputPreview.Cursor = Cursors.SizeAll;
        if (!_draggingConsoleInputPart || (e.Button & MouseButtons.Left) == 0)
            return;

        float x = Math.Clamp(_consoleInputStartX + point.X - _consoleInputDragStart.X, -2048f, 2048f);
        float y = Math.Clamp(_consoleInputStartY + point.Y - _consoleInputDragStart.Y, -240f, 240f);
        if (_selectedConsoleInputPart == ConsoleInputPart.Heading)
        {
            _document.InputTitleOffsetX = x;
            _document.InputTitleOffsetY = y;
        }
        else
        {
            _document.InputTextOffsetX = x;
            _document.InputTextOffsetY = y;
        }
        _document.IsDirty = true;
        _redo.Clear();
        UpdateHistoryControls();
        UpdateTitle();
        PopulateInputInspector();
    }

    private void ConsoleInputPreviewMouseUp(object? sender, MouseEventArgs e)
    {
        if (!_draggingConsoleInputPart || e.Button != MouseButtons.Left)
            return;
        _draggingConsoleInputPart = false;
        _consoleInputPreview.Capture = false;
        if (_pendingConsoleInputUndo is not null)
        {
            if (!_pendingConsoleInputUndo.IsEquivalentForHistory(_document))
                PushUndo(_pendingConsoleInputUndo);
            else
                _document.IsDirty = _pendingConsoleInputUndo.IsDirty;
        }
        _pendingConsoleInputUndo = null;
        UpdateHistoryControls();
        UpdateTitle();
        PopulateInputInspector();
        SetStatus(_selectedConsoleInputPart == ConsoleInputPart.Heading
            ? "INPUT heading position saved in the keyboard design."
            : "Typed-text position saved in the keyboard design.");
    }

    private void UpdatePreviewMode()
    {
        bool showInput = _inspectorTabs.SelectedIndex == 2;
        _consoleInputPreview.Visible = showInput;
        _canvas.Visible = !showInput;
        if (showInput)
        {
            RefreshConsoleInputPreview();
            _consoleInputPreview.BringToFront();
            SetStatus("Console INPUT preview: exact 1024x120 runtime panel. Choose another tab to return to the keyboard canvas.");
        }
        else
        {
            _canvas.BringToFront();
        }
    }

    private void PopulateInspector()
    {
        _updatingEditor = true;
        try
        {
            _layoutWidth.Value = ClampDecimal(_document.Width, _layoutWidth);
            PopulateTopElementInspectorFields();
            bool enabled = !_canvas.HasMultipleSelection
                && _canvas.SelectionKind is CanvasSelectionKind.KeyContent or CanvasSelectionKind.KeyPlate
                && _canvas.SelectedKey is KeyboardKey;
            foreach (Control control in new Control[] { _assignedKeyButton, _baseLabel, _shiftLabel, _x, _y, _width, _height, _offsetX, _offsetY, _scale })
                control.Enabled = enabled;

            if (!enabled || _canvas.SelectedKey is not KeyboardKey key)
            {
                _selectedLabel.Text = _canvas.HasMultipleSelection
                    ? $"{_canvas.SelectionCount} visible items selected"
                    : "No key selected";
                _assignedKeyButton.Text = _canvas.HasMultipleSelection ? "Group selection" : "Select a key first";
                return;
            }
            _selectedLabel.Text = $"Key #{key.Id} — {DisplayKeyName(key.Character)}";
            if (!_capturingAssignedKey)
                _assignedKeyButton.Text = DescribeAssignment(key.Character, key.ShiftCharacter);
            _baseLabel.Text = key.Label;
            _shiftLabel.Text = key.ShiftLabel;
            _x.Value = ClampDecimal((decimal)key.X, _x);
            _y.Value = ClampDecimal((decimal)key.Y, _y);
            float displayedWidth = key.SpansToRight && _renderer is not null
                ? _renderer.KeyRectangle(_document, key).Width / Math.Max(1, _renderer.KeySize(_document))
                : key.Width;
            _width.Value = ClampDecimal((decimal)displayedWidth, _width);
            _height.Value = ClampDecimal((decimal)key.Height, _height);
            _offsetX.Value = ClampDecimal((decimal)key.LabelOffsetX, _offsetX);
            _offsetY.Value = ClampDecimal((decimal)key.LabelOffsetY, _offsetY);
            _scale.Value = ClampDecimal((decimal)key.LabelScale, _scale);
        }
        finally
        {
            _updatingEditor = false;
        }
    }

    private void PopulateTopElementInspectorFields()
    {
        KeyboardTopElement? topElement = _canvas.HasMultipleSelection ? null : _canvas.SelectedTopElement;
        CanvasSelectionKind selectionKind = _canvas.SelectionKind;
        _topElementLabel.Text = selectionKind switch
        {
            CanvasSelectionKind.TopTextBar => "Text bar",
            CanvasSelectionKind.TopMode => "PC / VR Mode — interaction box",
            CanvasSelectionKind.TopModeArtwork => "PC / VR Mode — state image",
            CanvasSelectionKind.TopModeText => "PC / VR Mode — text",
            CanvasSelectionKind.TopLock => "Lock / Unlock — interaction box",
            CanvasSelectionKind.TopLockArtwork => "Lock / Unlock — state image",
            CanvasSelectionKind.TopLockText => "Lock / Unlock — text",
            _ => "Click a top-bar element"
        };
        (float topX, float topY) = selectionKind switch
        {
            CanvasSelectionKind.TopTextBar => (_document.TextBarOffsetX, _document.TextBarOffsetY),
            CanvasSelectionKind.TopMode => (_document.ModeButtonOffsetX, _document.ModeButtonOffsetY),
            CanvasSelectionKind.TopModeArtwork => (_document.ModeArtworkOffsetX, _document.ModeArtworkOffsetY),
            CanvasSelectionKind.TopModeText => (_document.ModeTextOffsetX, _document.ModeTextOffsetY),
            CanvasSelectionKind.TopLock => (_document.LockButtonOffsetX, _document.LockButtonOffsetY),
            CanvasSelectionKind.TopLockArtwork => (_document.LockArtworkOffsetX, _document.LockArtworkOffsetY),
            CanvasSelectionKind.TopLockText => (_document.LockTextOffsetX, _document.LockTextOffsetY),
            _ => (0, 0)
        };
        _topElementX.Value = ClampDecimal((decimal)topX, _topElementX);
        _topElementY.Value = ClampDecimal((decimal)topY, _topElementY);
        bool artworkSelected = selectionKind is CanvasSelectionKind.TopModeArtwork or CanvasSelectionKind.TopLockArtwork;
        bool textSelected = selectionKind is CanvasSelectionKind.TopModeText or CanvasSelectionKind.TopLockText;
        RectangleF artworkRectangle = artworkSelected && topElement is KeyboardTopElement artworkElement && _renderer is not null
            ? _renderer.TopStateArtworkRectangle(_document, artworkElement) : RectangleF.Empty;
        float topWidth = artworkSelected ? artworkRectangle.Width
            : topElement is KeyboardTopElement selected && _renderer is not null
                ? _renderer.TopElementWidth(_document, selected) : 8;
        float topHeight = artworkSelected ? artworkRectangle.Height
            : topElement is KeyboardTopElement selectedHeight && _renderer is not null
                ? _renderer.TopElementHeight(_document, selectedHeight) : 8;
        float topFontScale = topElement is KeyboardTopElement selectedScale && _renderer is not null
            ? _renderer.TopElementFontScale(_document, selectedScale) : 1;
        _topElementWidth.Value = ClampDecimal((decimal)topWidth, _topElementWidth);
        _topElementHeight.Value = ClampDecimal((decimal)topHeight, _topElementHeight);
        _topElementFontScale.Value = ClampDecimal((decimal)topFontScale, _topElementFontScale);
        _topElementX.Enabled = topElement is not null;
        _topElementY.Enabled = topElement is not null;
        _topElementWidth.Enabled = topElement is not null && !textSelected;
        _topElementHeight.Enabled = topElement is not null && !textSelected;
        _topElementFontScale.Enabled = selectionKind == CanvasSelectionKind.TopTextBar || textSelected;
        _modeStatePreview.SelectedIndex = _renderer?.PreviewPcMode == true ? 1 : 0;
        _lockStatePreview.SelectedIndex = _renderer?.PreviewHeadLocked == true ? 1 : 0;
        _modeTextOverArtwork.Checked = _document.ModeTextOverArtwork;
        _lockTextOverArtwork.Checked = _document.LockTextOverArtwork;
        PopulateStateArtworkName(_modeVrArtworkName, _document.ModeVrArtworkImagePath, "Text fallback");
        PopulateStateArtworkName(_modePcArtworkName, _document.ModePcArtworkImagePath, "Text fallback");
        PopulateStateArtworkName(_lockWorldArtworkName, _document.LockWorldArtworkImagePath, "Text fallback");
        PopulateStateArtworkName(_lockHeadArtworkName, _document.LockHeadArtworkImagePath, "Text fallback");
    }

    private void PopulateAppearanceInspector()
    {
        _updatingEditor = true;
        try
        {
            KeyboardTheme theme = ActiveTheme();
            EffectiveKeyboardStyle style = EffectiveStyle();
            bool custom = _document.CustomStyleEnabled;
            bool plateGlowEnabled = custom ? _document.GlowEnabled : theme.Modern;
            bool fontGlowEnabled = custom && _document.FontGlowEnabled;
            bool hoverEnabled = custom ? _document.HoverEnabled : true;
            bool outlineEnabled = custom ? _document.OutlineEnabled : theme.Outline;
            bool keyBreatheEnabled = custom && _document.KeyBreatheEnabled;
            bool fontBreatheEnabled = custom && _document.FontBreatheEnabled;

            _keyPlatesEnabled.Checked = _document.KeyPlatesEnabled;
            _topButtonPlatesEnabled.Checked = _document.TopButtonPlatesEnabled;
            _inputBarPlateEnabled.Checked = _document.InputBarPlateEnabled;
            _parchmentRibbonEnabled.Checked = _document.ParchmentRibbonEnabled;
            _parchmentRibbonEnabled.Enabled = _document.BaseTheme.Equals("parchment", StringComparison.OrdinalIgnoreCase);
            _glowEnabled.Checked = plateGlowEnabled;
            _fontGlowEnabled.Checked = fontGlowEnabled;
            _hoverEnabled.Checked = hoverEnabled;
            _outlineEnabled.Checked = outlineEnabled;
            _glowStrength.Value = ClampDecimal(custom ? _document.GlowStrength : theme.Modern ? 35 : 0, _glowStrength);
            _glowRadius.Value = ClampDecimal(custom ? _document.GlowRadius : 4, _glowRadius);
            _fontGlowStrength.Value = ClampDecimal(_document.FontGlowStrength, _fontGlowStrength);
            _fontGlowRadius.Value = ClampDecimal(_document.FontGlowRadius, _fontGlowRadius);
            _hoverStrength.Value = ClampDecimal(custom
                ? _document.HoverStrength
                : Math.Clamp((int)Math.Round(theme.KeyHot.A / 2.0), 0, 100), _hoverStrength);
            _keyRoundness.Value = ClampDecimal(custom ? _document.KeyRoundness : theme.Modern ? 14 : 2, _keyRoundness);
            _plateOutlineWidth.Value = ClampDecimal(custom ? _document.PlateOutlineWidth : theme.Modern ? 2 : 1, _plateOutlineWidth);
            _keyBreathe.Checked = keyBreatheEnabled;
            _keyBreatheMin.Value = ClampDecimal(_document.KeyBreatheMinPercent, _keyBreatheMin);
            _keyBreathePeriod.Value = ClampDecimal((decimal)_document.KeyBreathePeriodSeconds, _keyBreathePeriod);
            _keyBreathePhase.Value = ClampDecimal((decimal)_document.KeyBreathePhaseDegrees, _keyBreathePhase);
            _fontBreathe.Checked = fontBreatheEnabled;
            _fontBreatheMin.Value = ClampDecimal(_document.FontBreatheMinPercent, _fontBreatheMin);
            _fontBreathePeriod.Value = ClampDecimal((decimal)_document.FontBreathePeriodSeconds, _fontBreathePeriod);
            _fontBreathePhase.Value = ClampDecimal((decimal)_document.FontBreathePhaseDegrees, _fontBreathePhase);
            SetSwatch(_fontColor, style.Font);
            SetSwatch(_fontOutlineColor, style.FontOutline);
            SetSwatch(_fontGlowColor, style.FontGlow);
            SetSwatch(_keyColor, style.PlateOutline);
            SetSwatch(_plateFillColor, style.PlateFill);
            SetSwatch(_glowColor, style.PlateGlow);
            SetSwatch(_hoverColor, style.Hover);

            foreach (Control control in new Control[]
            {
                _fontColor, _plateFillColor, _keyColor,
                _glowEnabled, _fontGlowEnabled, _hoverEnabled, _outlineEnabled,
                _keyRoundness, _plateOutlineWidth
            })
                control.Enabled = true;
            _fontOutlineColor.Enabled = outlineEnabled;
            foreach (Control control in new Control[] { _glowColor, _glowStrength, _glowRadius, _keyBreathe })
                control.Enabled = plateGlowEnabled;
            foreach (Control control in new Control[] { _keyBreatheMin, _keyBreathePeriod, _keyBreathePhase })
                control.Enabled = plateGlowEnabled && keyBreatheEnabled;
            foreach (Control control in new Control[] { _fontGlowColor, _fontGlowStrength, _fontGlowRadius, _fontBreathe })
                control.Enabled = fontGlowEnabled;
            foreach (Control control in new Control[] { _fontBreatheMin, _fontBreathePeriod, _fontBreathePhase })
                control.Enabled = fontGlowEnabled && fontBreatheEnabled;
            _hoverColor.Enabled = hoverEnabled;
            _hoverStrength.Enabled = hoverEnabled;
        }
        finally
        {
            _updatingEditor = false;
        }
        RefreshConsoleInputPreview();
    }

    private void PopulateInputInspector()
    {
        _updatingEditor = true;
        try
        {
            _consoleInputBackgroundName.Text = string.IsNullOrWhiteSpace(_document.ConsoleInputBackgroundImagePath)
                ? "Theme fill (no custom image)"
                : Path.GetFileName(_document.ConsoleInputBackgroundImagePath);
            _consoleInputBackgroundName.ForeColor = TextMuted;
            SetSwatch(_inputFillColor, EffectiveInputFillColor());
            _inputOutlineVisible.Checked = _document.InputOutlineVisible;
            _inputOutlineWidth.Value = ClampDecimal(EffectiveInputOutlineWidth(), _inputOutlineWidth);
            SetSwatch(_inputOutlineColor, EffectiveInputOutlineColor());
            _inputTitleX.Value = ClampDecimal((decimal)_document.InputTitleOffsetX, _inputTitleX);
            _inputTitleY.Value = ClampDecimal((decimal)_document.InputTitleOffsetY, _inputTitleY);
            _inputTextX.Value = ClampDecimal((decimal)_document.InputTextOffsetX, _inputTextX);
            _inputTextY.Value = ClampDecimal((decimal)_document.InputTextOffsetY, _inputTextY);
            _inputFillColor.Enabled = true;
            _inputOutlineColor.Enabled = true;
            _inputOutlineWidth.Enabled = _document.InputOutlineVisible;
            _inputOutlineColor.Enabled = _document.InputOutlineVisible;
        }
        finally
        {
            _updatingEditor = false;
        }
        RefreshConsoleInputPreview();
    }

    private void PopulateArtworkInspector()
    {
        _updatingEditor = true;
        try
        {
            _backgroundName.Text = string.IsNullOrWhiteSpace(_document.BackgroundImagePath)
                ? "Theme background"
                : Path.GetFileName(_document.BackgroundImagePath);
            _backgroundX.Value = ClampDecimal((decimal)_document.BackgroundX, _backgroundX);
            _backgroundY.Value = ClampDecimal((decimal)_document.BackgroundY, _backgroundY);
            _backgroundWidth.Value = ClampDecimal((decimal)_document.BackgroundWidth, _backgroundWidth);
            _backgroundHeight.Value = ClampDecimal((decimal)_document.BackgroundHeight, _backgroundHeight);
            _backgroundOpacity.Value = ClampDecimal(_document.BackgroundOpacity, _backgroundOpacity);
            _backgroundFade.Value = ClampDecimal(_document.BackgroundEdgeFade, _backgroundFade);
            _backgroundRotation.Value = ClampDecimal((decimal)_document.BackgroundRotation, _backgroundRotation);
            _backgroundRoundness.Value = ClampDecimal(_document.BackgroundRoundness, _backgroundRoundness);
            bool hasBackground = !string.IsNullOrWhiteSpace(_document.BackgroundImagePath);
            foreach (Control control in new Control[]
            {
                _backgroundX, _backgroundY, _backgroundWidth, _backgroundHeight,
                _backgroundOpacity, _backgroundFade, _backgroundRotation, _backgroundRoundness
            })
                control.Enabled = hasBackground;
            Guid? selectedId = _canvas.HasMultipleSelection ? null : _canvas.SelectedSpriteId;
            _spriteList.BeginUpdate();
            _spriteList.Items.Clear();
            _spriteList.Items.AddRange(_document.Sprites.Cast<object>().ToArray());
            int selectedIndex = selectedId is Guid id ? _document.Sprites.FindIndex(sprite => sprite.Id == id) : -1;
            if (!_canvas.HasMultipleSelection && selectedIndex < 0 && _document.Sprites.Count > 0)
                selectedIndex = 0;
            _spriteList.SelectedIndex = selectedIndex;
            _spriteList.EndUpdate();

            KeyboardSprite? sprite = selectedIndex >= 0 ? _document.Sprites[selectedIndex] : null;
            _overlayName.Text = sprite is null ? "No sprite selected" : sprite.ToString();
            _overlayX.Value = ClampDecimal((decimal)(sprite?.X ?? 0), _overlayX);
            _overlayY.Value = ClampDecimal((decimal)(sprite?.Y ?? 0), _overlayY);
            _overlayWidth.Value = ClampDecimal((decimal)(sprite?.Width ?? 1), _overlayWidth);
            _overlayHeight.Value = ClampDecimal((decimal)(sprite?.Height ?? 1), _overlayHeight);
            _overlayOpacity.Value = ClampDecimal(sprite?.Opacity ?? 100, _overlayOpacity);
            _spriteFade.Value = ClampDecimal(sprite?.EdgeFade ?? 0, _spriteFade);
            _spriteRotation.Value = ClampDecimal((decimal)(sprite?.Rotation ?? 0), _spriteRotation);
            _spriteGlow.Checked = sprite?.GlowEnabled ?? false;
            SetSwatch(_spriteGlowColor, sprite?.GlowColor ?? Color.FromArgb(255, 132, 242, 158));
            _spriteGlowStrength.Value = ClampDecimal(sprite?.GlowStrength ?? 55, _spriteGlowStrength);
            _spriteGlowRadius.Value = ClampDecimal(sprite?.GlowRadius ?? 12, _spriteGlowRadius);
            _spriteBreathe.Checked = sprite?.BreatheEnabled ?? false;
            _spriteBreatheMin.Value = ClampDecimal(sprite?.BreatheMinPercent ?? 35, _spriteBreatheMin);
            _spriteBreathePeriod.Value = ClampDecimal((decimal)(sprite?.BreathePeriodSeconds ?? 2f), _spriteBreathePeriod);
            _spriteBreathePhase.Value = ClampDecimal((decimal)(sprite?.BreathePhaseDegrees ?? 0f), _spriteBreathePhase);
            bool hasSprite = sprite is not null;
            foreach (Control control in new Control[]
            {
                _overlayX, _overlayY,
                _overlayWidth, _overlayHeight, _overlayOpacity, _spriteFade, _spriteRotation,
                _spriteGlow, _spriteGlowColor, _spriteGlowStrength, _spriteGlowRadius, _spriteBreathe
            })
                control.Enabled = hasSprite;
            foreach (Control control in new Control[] { _spriteGlowColor, _spriteGlowStrength, _spriteGlowRadius, _spriteBreathe })
                control.Enabled = hasSprite && sprite!.GlowEnabled;
            foreach (Control control in new Control[] { _spriteBreatheMin, _spriteBreathePeriod, _spriteBreathePhase })
                control.Enabled = hasSprite && sprite!.GlowEnabled && sprite.BreatheEnabled;
        }
        finally
        {
            _updatingEditor = false;
        }
    }

    private void PopulateControlsInspector()
    {
        _updatingEditor = true;
        try
        {
            _controlCombo.SelectedIndex = (int)_canvas.SelectedControl;
            (float x, float y) = _canvas.SelectedControl switch
            {
                KeyboardRuntimeControl.Size => (_document.SizeControlOffsetX, _document.SizeControlOffsetY),
                KeyboardRuntimeControl.Opacity => (_document.OpacityControlOffsetX, _document.OpacityControlOffsetY),
                _ => (_document.TiltControlOffsetX, _document.TiltControlOffsetY)
            };
            _controlX.Value = ClampDecimal((decimal)x, _controlX);
            _controlY.Value = ClampDecimal((decimal)y, _controlY);
            KeyboardControlDesign design = _document.GetControlDesign(_canvas.SelectedControl);
            _controlWidth.Value = ClampDecimal((decimal)design.Width, _controlWidth);
            _controlHeight.Value = ClampDecimal((decimal)design.Height, _controlHeight);
            KeyboardControlPart part = _canvas.SelectedControlPart;
            _controlPartLabel.Text = part switch
            {
                KeyboardControlPart.Group => "Outer settings box",
                KeyboardControlPart.UpArrow => "Up arrow",
                KeyboardControlPart.DownArrow => "Down arrow",
                KeyboardControlPart.Label => "Label font",
                _ => "Value font"
            };
            (float partX, float partY, float partWidth, float partHeight, float partScale) = part switch
            {
                KeyboardControlPart.UpArrow => (design.UpOffsetX, design.UpOffsetY, design.UpWidth, design.UpHeight, 1f),
                KeyboardControlPart.DownArrow => (design.DownOffsetX, design.DownOffsetY, design.DownWidth, design.DownHeight, 1f),
                KeyboardControlPart.Label => (design.LabelOffsetX, design.LabelOffsetY, 4f, 4f, design.LabelScale),
                KeyboardControlPart.Value => (design.ValueOffsetX, design.ValueOffsetY, 4f, 4f, design.ValueScale),
                _ => (0f, 0f, 4f, 4f, 1f)
            };
            _controlPartX.Value = ClampDecimal((decimal)partX, _controlPartX);
            _controlPartY.Value = ClampDecimal((decimal)partY, _controlPartY);
            _controlPartWidth.Value = ClampDecimal((decimal)partWidth, _controlPartWidth);
            _controlPartHeight.Value = ClampDecimal((decimal)partHeight, _controlPartHeight);
            _controlPartScale.Value = ClampDecimal((decimal)partScale, _controlPartScale);
            bool arrowPart = part is KeyboardControlPart.UpArrow or KeyboardControlPart.DownArrow;
            bool textPart = part is KeyboardControlPart.Label or KeyboardControlPart.Value;
            _controlPartX.Enabled = arrowPart || textPart;
            _controlPartY.Enabled = arrowPart || textPart;
            _controlPartWidth.Enabled = arrowPart;
            _controlPartHeight.Enabled = arrowPart;
            _controlPartScale.Enabled = textPart;
            _controlArrowName.Text = string.IsNullOrWhiteSpace(_document.ControlArrowImagePath)
                ? "Built-in triangle"
                : Path.GetFileName(_document.ControlArrowImagePath);
            _controlArrowRotation.Value = ClampDecimal((decimal)_document.ControlArrowRotation, _controlArrowRotation);
            _controlArrowGlow.Checked = _document.ControlArrowGlowEnabled;
            SetSwatch(_controlArrowGlowColor, _document.ControlArrowGlowColor);
            _controlArrowGlowStrength.Value = ClampDecimal(_document.ControlArrowGlowStrength, _controlArrowGlowStrength);
            _controlArrowGlowRadius.Value = ClampDecimal(_document.ControlArrowGlowRadius, _controlArrowGlowRadius);
            _controlArrowBreathe.Checked = _document.ControlArrowBreatheEnabled;
            _controlArrowBreatheMin.Value = ClampDecimal(_document.ControlArrowBreatheMinPercent, _controlArrowBreatheMin);
            _controlArrowBreathePeriod.Value = ClampDecimal((decimal)_document.ControlArrowBreathePeriodSeconds, _controlArrowBreathePeriod);
            _controlArrowBreathePhase.Value = ClampDecimal((decimal)_document.ControlArrowBreathePhaseDegrees, _controlArrowBreathePhase);
            _controlArrowGlowColor.Enabled = _document.ControlArrowGlowEnabled;
            _controlArrowGlowStrength.Enabled = _document.ControlArrowGlowEnabled;
            _controlArrowGlowRadius.Enabled = _document.ControlArrowGlowEnabled;
            _controlArrowBreathe.Enabled = _document.ControlArrowGlowEnabled;
            foreach (Control control in new Control[] { _controlArrowBreatheMin, _controlArrowBreathePeriod, _controlArrowBreathePhase })
                control.Enabled = _document.ControlArrowGlowEnabled && _document.ControlArrowBreatheEnabled;
        }
        finally
        {
            _updatingEditor = false;
        }
    }

    private void OpenLayout()
    {
        if (!ConfirmDiscard("Opening another keyboard"))
            return;
        using var dialog = new OpenFileDialog
        {
            Title = "Open or import an OCU keyboard",
            Filter = "OCU keyboard package or layout (*.ocukb;*.kb)|*.ocukb;*.kb|OCU keyboard package (*.ocukb)|*.ocukb|Legacy keyboard layout (*.kb)|*.kb",
            InitialDirectory = Path.GetDirectoryName(_currentPackagePath ?? _document.SourcePath) ?? _assetsDirectory
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        LoadKeyboardFile(dialog.FileName);
    }

    private void LoadKeyboardFile(string path)
    {
        try
        {
            string fullPath = Path.GetFullPath(path);
            if (Path.GetExtension(fullPath).Equals(KeyboardPackage.Extension, StringComparison.OrdinalIgnoreCase))
            {
                KeyboardPackageImportResult imported = KeyboardPackage.Import(fullPath, DesignLibraryDirectory);
                _currentPackagePath = fullPath;
                SetDocument(KeyboardDocument.Load(imported.LayoutPath), clearHistory: true);
                string registeredPath = RegisterDesign(imported.LayoutPath);
                RefreshDesignChoices(registeredPath);
                SetStatus($"Imported {imported.DisplayName}: {imported.ArtworkCount} artwork file(s)" +
                    (imported.HasCustomFont ? " plus its custom font." : "."));
                MessageBox.Show(this,
                    $"{imported.DisplayName} imported successfully.\n\n" +
                    $"Artwork files: {imported.ArtworkCount}\n" +
                    $"Custom font: {(imported.HasCustomFont ? "included" : "not used")}\n\n" +
                    "The design is now in Keyboard Design. Use Install for Next Launch to activate it.",
                    "OCU keyboard imported", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (!Path.GetExtension(fullPath).Equals(".kb", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Choose an .ocukb package or a legacy .kb layout.");
            _currentPackagePath = null;
            SetDocument(KeyboardDocument.Load(fullPath), clearHistory: true);
            string legacyRegisteredPath = RegisterDesign(fullPath);
            RefreshDesignChoices(legacyRegisteredPath);
            SetStatus($"Loaded legacy .kb layout: {fullPath}. Keep its PNG files beside it.");
        }
        catch (Exception exception)
        {
            ShowError("Keyboard import failed", exception);
        }
    }

    private bool SaveLayout(bool saveAs)
    {
        if (!saveAs && !_document.IsDirty)
        {
            SetStatus("No unsaved changes. Use Save As to create another copy.");
            return true;
        }

        string? packagePath = saveAs ? null : _currentPackagePath;
        if (packagePath is null)
        {
            using var dialog = new SaveFileDialog
            {
                Title = "Save portable OCU keyboard project",
                Filter = "OCU keyboard package (*.ocukb)|*.ocukb",
                DefaultExt = "ocukb",
                AddExtension = true,
                FileName = $"{SuggestedPackageName()}.ocukb",
                InitialDirectory = Path.GetDirectoryName(_currentPackagePath)
                    ?? Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
            };
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return false;
            packagePath = dialog.FileName;
        }

        try
        {
            packagePath = Path.GetFullPath(packagePath);
            string displayName = Path.GetFileNameWithoutExtension(packagePath);
            KeyboardPackage.Export(packagePath, _document, displayName);
            KeyboardPackageImportResult imported = KeyboardPackage.Import(packagePath, DesignLibraryDirectory);
            _currentPackagePath = packagePath;
            SetDocument(KeyboardDocument.Load(imported.LayoutPath), clearHistory: false);
            string registeredPath = RegisterDesign(imported.LayoutPath);
            RefreshDesignChoices(registeredPath);
            string? installedTarget = _hostOcuRoot is null ? null : InstallDocumentToRoot(_hostOcuRoot);
            UpdateTitle();
            SetStatus(installedTarget is null
                ? $"Saved portable keyboard project: {packagePath}"
                : $"Saved {packagePath} and applied it to this OCU. Restart Skyrim VR to load it.");
            return true;
        }
        catch (Exception exception)
        {
            ShowError("Layout save failed", exception);
            return false;
        }
    }

    private string SuggestedPackageName()
    {
        string? sourceName = Path.GetFileNameWithoutExtension(_currentPackagePath ?? _document.SourcePath);
        return string.IsNullOrWhiteSpace(sourceName) || sourceName.Equals("en_gb", StringComparison.OrdinalIgnoreCase)
            ? "My OCU Keyboard"
            : sourceName;
    }

    private void ExportPng()
    {
        using var dialog = new SaveFileDialog
        {
            Title = "Export exact 1024x560 preview",
            Filter = "PNG image (*.png)|*.png",
            FileName = $"{Path.GetFileNameWithoutExtension(_document.SourcePath) ?? "OCU-Keyboard"}-preview.png",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        try
        {
            using Bitmap preview = _canvas.RenderExact();
            preview.Save(dialog.FileName, System.Drawing.Imaging.ImageFormat.Png);
            SetStatus($"Exported exact 1024x560 preview to {dialog.FileName}");
        }
        catch (Exception exception)
        {
            ShowError("PNG export failed", exception);
        }
    }

    private Control StateArtworkActions(TopStateArtworkSlot slot, Label nameLabel)
    {
        nameLabel.ForeColor = TextMuted;
        nameLabel.Margin = new Padding(4, 7, 3, 0);
        var actions = new FlowLayoutPanel
        {
            AutoSize = true,
            Dock = DockStyle.Fill,
            WrapContents = true,
            Margin = Padding.Empty
        };
        actions.Controls.Add(ActionButton("Choose PNG", (_, _) => ChooseStateArtwork(slot), compact: true));
        actions.Controls.Add(ActionButton("Edit", (_, _) => EditStateArtwork(slot), compact: true));
        actions.Controls.Add(ActionButton("Use text", (_, _) => ClearStateArtwork(slot), compact: true));
        actions.Controls.Add(nameLabel);
        return actions;
    }

    private void ChooseStateArtwork(TopStateArtworkSlot slot)
    {
        using var dialog = PngArtworkDialog($"Choose PNG artwork for {DescribeStateArtworkSlot(slot)}");
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        PushUndo();
        SetStateArtwork(slot, dialog.FileName, StateArtworkPortableName(slot));
        MarkChanged();
        PopulateInspector();
        SelectStateArtwork(slot);
        SetStatus($"{DescribeStateArtworkSlot(slot)} state sprite selected for editing. Drag it or scroll to resize; it travels with .ocukb and MO2 exports.");
    }

    private void ClearStateArtwork(TopStateArtworkSlot slot)
    {
        (string? path, string? fileName) = GetStateArtwork(slot);
        if (string.IsNullOrWhiteSpace(path) && string.IsNullOrWhiteSpace(fileName))
            return;
        PushUndo();
        SetStateArtwork(slot, null, null);
        MarkChanged();
        PopulateInspector();
        PreviewStateArtworkSlot(slot);
        _canvas.SelectTopText(TopElementForStateArtworkSlot(slot));
        SetStatus($"{DescribeStateArtworkSlot(slot)} returned to the ordinary text fallback.");
    }

    private Control TopLayerActions(KeyboardTopElement element)
    {
        var actions = new FlowLayoutPanel
        {
            AutoSize = true,
            Dock = DockStyle.Fill,
            WrapContents = true,
            Margin = Padding.Empty
        };
        actions.Controls.Add(ActionButton("Hit box", (_, _) =>
        {
            _canvas.SelectTopInteractionBox(element);
            SetStatus($"{DescribeTopElement(element)} interaction box selected. Moving it changes the in-game laser target.");
        }, compact: true));
        actions.Controls.Add(ActionButton("State image", (_, _) =>
        {
            if (_canvas.SelectTopStateArtwork(element))
                SetStatus($"{DescribeTopElement(element)} state image selected. Drag, use its handles, or scroll to resize it without moving the hit box.");
            else
                SetStatus($"The previewed {DescribeTopElement(element)} state has no PNG. Choose one first.");
        }, compact: true));
        actions.Controls.Add(ActionButton("Text", (_, _) =>
        {
            if (_canvas.SelectTopText(element))
                SetStatus($"{DescribeTopElement(element)} text selected. Drag to move it; scroll to resize the font.");
            else
                SetStatus($"{DescribeTopElement(element)} text is hidden by its state image. Enable the text-over-image option or use text fallback.");
        }, compact: true));
        actions.Controls.Add(ActionButton("Fit image to box", (_, _) => FitTopLayers(element, imageToBox: true), compact: true));
        actions.Controls.Add(ActionButton("Fit box to image", (_, _) => FitTopLayers(element, imageToBox: false), compact: true));
        return actions;
    }

    private void FitTopLayers(KeyboardTopElement element, bool imageToBox)
    {
        if (!_canvas.HasTopStateArtwork(element))
        {
            SetStatus($"{DescribeTopElement(element)} has no state image to fit.");
            return;
        }
        PushUndo();
        bool changed = imageToBox
            ? _canvas.FitTopArtworkToInteractionBox(element)
            : _canvas.FitTopInteractionBoxToArtwork(element);
        if (!changed)
            return;
        MarkChanged();
        PopulateInspector();
        if (imageToBox)
        {
            _canvas.SelectTopStateArtwork(element);
            SetStatus($"{DescribeTopElement(element)} image fitted to its hit box. Resize the hit box again to separate them without stretching the image.");
        }
        else
        {
            _canvas.SelectTopInteractionBox(element);
            SetStatus($"{DescribeTopElement(element)} hit box fitted to its image. Resize either layer independently from here.");
        }
    }

    private void EditStateArtwork(TopStateArtworkSlot slot)
    {
        PreviewStateArtworkSlot(slot);
        KeyboardTopElement element = TopElementForStateArtworkSlot(slot);
        if (_canvas.SelectTopStateArtwork(element))
            SetStatus($"{DescribeStateArtworkSlot(slot)} state sprite selected. Drag, use its handles, or scroll to resize.");
        else
            SetStatus($"{DescribeStateArtworkSlot(slot)} has no PNG yet. Choose one first.");
    }

    private void SelectStateArtwork(TopStateArtworkSlot slot)
    {
        PreviewStateArtworkSlot(slot);
        _canvas.SelectTopStateArtwork(TopElementForStateArtworkSlot(slot));
    }

    private void PreviewStateArtworkSlot(TopStateArtworkSlot slot)
    {
        if (slot is TopStateArtworkSlot.ModeVr or TopStateArtworkSlot.ModePc)
            _modeStatePreview.SelectedIndex = slot == TopStateArtworkSlot.ModePc ? 1 : 0;
        else
            _lockStatePreview.SelectedIndex = slot == TopStateArtworkSlot.LockHead ? 1 : 0;
    }

    private static KeyboardTopElement TopElementForStateArtworkSlot(TopStateArtworkSlot slot)
        => slot is TopStateArtworkSlot.ModeVr or TopStateArtworkSlot.ModePc
            ? KeyboardTopElement.Mode
            : KeyboardTopElement.Lock;

    private static string DescribeTopElement(KeyboardTopElement element)
        => element == KeyboardTopElement.Mode ? "PC / VR Mode" : "Lock / Unlock";

    private (string? path, string? fileName) GetStateArtwork(TopStateArtworkSlot slot) => slot switch
    {
        TopStateArtworkSlot.ModeVr => (_document.ModeVrArtworkImagePath, _document.ModeVrArtworkFileName),
        TopStateArtworkSlot.ModePc => (_document.ModePcArtworkImagePath, _document.ModePcArtworkFileName),
        TopStateArtworkSlot.LockWorld => (_document.LockWorldArtworkImagePath, _document.LockWorldArtworkFileName),
        _ => (_document.LockHeadArtworkImagePath, _document.LockHeadArtworkFileName)
    };

    private void SetStateArtwork(TopStateArtworkSlot slot, string? path, string? fileName)
    {
        switch (slot)
        {
            case TopStateArtworkSlot.ModeVr:
                _document.ModeVrArtworkImagePath = path;
                _document.ModeVrArtworkFileName = fileName;
                break;
            case TopStateArtworkSlot.ModePc:
                _document.ModePcArtworkImagePath = path;
                _document.ModePcArtworkFileName = fileName;
                break;
            case TopStateArtworkSlot.LockWorld:
                _document.LockWorldArtworkImagePath = path;
                _document.LockWorldArtworkFileName = fileName;
                break;
            case TopStateArtworkSlot.LockHead:
                _document.LockHeadArtworkImagePath = path;
                _document.LockHeadArtworkFileName = fileName;
                break;
        }
    }

    private static string StateArtworkPortableName(TopStateArtworkSlot slot) => slot switch
    {
        TopStateArtworkSlot.ModeVr => KeyboardDocument.ModeVrArtworkPortableName,
        TopStateArtworkSlot.ModePc => KeyboardDocument.ModePcArtworkPortableName,
        TopStateArtworkSlot.LockWorld => KeyboardDocument.LockWorldArtworkPortableName,
        _ => KeyboardDocument.LockHeadArtworkPortableName
    };

    private static string DescribeStateArtworkSlot(TopStateArtworkSlot slot) => slot switch
    {
        TopStateArtworkSlot.ModeVr => "VR Mode",
        TopStateArtworkSlot.ModePc => "PC Mode",
        TopStateArtworkSlot.LockWorld => "world/unlocked",
        _ => "head-locked"
    };

    private static void PopulateStateArtworkName(Label label, string? imagePath, string fallback)
        => label.Text = string.IsNullOrWhiteSpace(imagePath) ? fallback : Path.GetFileName(imagePath);

    private void ChooseBackground()
    {
        using var dialog = BackgroundArtworkDialog("Choose a custom keyboard background");
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        string extension = Path.GetExtension(dialog.FileName);
        if (extension.Equals(".jpg", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".jpeg", StringComparison.OrdinalIgnoreCase))
        {
            DialogResult warning = MessageBox.Show(this,
                "JPEG backgrounds have no transparency and may show compression artifacts around detailed edges. PNG is strongly recommended.\n\nKeyboard Studio will convert this JPEG to PNG when you Save, Install, or export a shareable mod. Continue?",
                "JPEG background warning", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
            if (warning != DialogResult.OK)
                return;
        }
        PushUndo();
        _document.BackgroundImagePath = dialog.FileName;
        _document.BackgroundFileName = "OCUKeyboardBackground.png";
        _document.BackgroundX = 0;
        _document.BackgroundY = 0;
        _document.BackgroundWidth = KeyboardRenderer.TextureWidth;
        _document.BackgroundHeight = KeyboardRenderer.TextureHeight;
        _document.BackgroundOpacity = 100;
        _document.BackgroundEdgeFade = 0;
        _document.BackgroundRotation = 0;
        _document.BackgroundRoundness = 0;
        _document.BackgroundBreatheEnabled = false;
        _canvas.SelectBackground();
        MarkChanged();
        PopulateArtworkInspector();
        SetStatus("Custom background selected. It will be copied into Save, Install, and MO2 exports.");
    }

    private void ClearBackground()
    {
        if (string.IsNullOrWhiteSpace(_document.BackgroundImagePath) && string.IsNullOrWhiteSpace(_document.BackgroundFileName))
            return;
        PushUndo();
        _document.BackgroundImagePath = null;
        _document.BackgroundFileName = null;
        _document.BackgroundBreatheEnabled = false;
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void ChooseConsoleInputBackground()
    {
        using var dialog = BackgroundArtworkDialog("Choose artwork for the console INPUT panel");
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        string extension = Path.GetExtension(dialog.FileName);
        if (extension.Equals(".jpg", StringComparison.OrdinalIgnoreCase)
            || extension.Equals(".jpeg", StringComparison.OrdinalIgnoreCase))
        {
            DialogResult warning = MessageBox.Show(this,
                "JPEG has no transparency and may show compression artifacts. PNG is strongly recommended for INPUT panel artwork.\n\nKeyboard Studio will convert this JPEG to PNG when you Save, Install, or export a mod. Continue?",
                "JPEG artwork warning", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
            if (warning != DialogResult.OK)
                return;
        }
        PushUndo();
        _document.ConsoleInputBackgroundImagePath = dialog.FileName;
        _document.ConsoleInputBackgroundFileName = KeyboardDocument.ConsoleInputBackgroundPortableName;
        MarkChanged();
        PopulateInputInspector();
        _inspectorTabs.SelectedIndex = 2;
        SetStatus("Console INPUT artwork selected. It is scaled to 1024x120 and uses the panel's existing swapchain.");
    }

    private void ClearConsoleInputBackground()
    {
        if (string.IsNullOrWhiteSpace(_document.ConsoleInputBackgroundImagePath)
            && string.IsNullOrWhiteSpace(_document.ConsoleInputBackgroundFileName))
            return;
        PushUndo();
        _document.ConsoleInputBackgroundImagePath = null;
        _document.ConsoleInputBackgroundFileName = null;
        MarkChanged();
        PopulateInputInspector();
        SetStatus("Console INPUT artwork cleared; the panel is using its theme fill.");
    }

    private void ChooseOverlay()
    {
        using var dialog = PngArtworkDialog("Add a transparent sprite, ribbon, border, or decoration PNG");
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        AddSprite(dialog.FileName);
    }

    private void AddParchmentRibbon()
    {
        string path = Path.Combine(_assetsDirectory, "spacebar.png");
        if (!File.Exists(path))
        {
            MessageBox.Show(this, "The bundled Parchment ribbon asset is missing.", "Asset missing", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        AddSprite(path);
    }

    private void AddSprite(string path)
    {
        PushUndo();
        var sprite = new KeyboardSprite { SourcePath = path };
        using (var image = new Bitmap(path))
        {
            float width = Math.Min(520, image.Width);
            float height = Math.Max(1, width * image.Height / Math.Max(1f, image.Width));
            if (height > 280)
            {
                height = 280;
                width = Math.Max(1, height * image.Width / Math.Max(1f, image.Height));
            }
            sprite.Width = width;
            sprite.Height = height;
            sprite.X = (KeyboardRenderer.TextureWidth - width) / 2f;
            sprite.Y = 62 + _document.Sprites.Count * 12;
        }
        _document.Sprites.Add(sprite);
        _canvas.SelectSprite(sprite);
        MarkChanged();
        PopulateArtworkInspector();
        SetStatus("Sprite added. Drag it in the preview, resize it, fade its edges, or change its layer order.");
    }

    private void ClearOverlay()
    {
        if (_canvas.SelectedSprite is not KeyboardSprite sprite)
            return;
        PushUndo();
        int index = _document.Sprites.IndexOf(sprite);
        _document.Sprites.Remove(sprite);
        KeyboardSprite? next = _document.Sprites.Count == 0 ? null : _document.Sprites[Math.Clamp(index, 0, _document.Sprites.Count - 1)];
        _canvas.SelectSprite(next);
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void MoveSpriteLayer(int direction)
    {
        if (_canvas.SelectedSprite is not KeyboardSprite sprite)
            return;
        int index = _document.Sprites.IndexOf(sprite);
        int target = Math.Clamp(index + direction, 0, _document.Sprites.Count - 1);
        if (target == index)
            return;
        PushUndo();
        _document.Sprites.RemoveAt(index);
        _document.Sprites.Insert(target, sprite);
        MarkChanged();
        PopulateArtworkInspector();
    }

    private void ChooseControlArrow()
    {
        using var dialog = PngArtworkDialog("Choose a transparent PNG for the side-control Up arrow");
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        PushUndo();
        _document.ControlArrowImagePath = dialog.FileName;
        _document.ControlArrowFileName = "OCUKeyboardControlArrow.png";
        MarkChanged();
        PopulateControlsInspector();
        SetStatus("Custom side-control arrow selected. OCU rotates the same PNG 180 degrees for Down.");
    }

    private void ClearControlArrow()
    {
        if (string.IsNullOrWhiteSpace(_document.ControlArrowImagePath)
            && string.IsNullOrWhiteSpace(_document.ControlArrowFileName))
            return;
        PushUndo();
        _document.ControlArrowImagePath = null;
        _document.ControlArrowFileName = null;
        _document.ControlArrowRotation = 0;
        MarkChanged();
        PopulateControlsInspector();
    }

    private void ExportMo2Mod()
    {
        string? sourceName = Path.GetFileNameWithoutExtension(_document.SourcePath);
        if (string.IsNullOrWhiteSpace(sourceName) || sourceName.Equals("en_gb", StringComparison.OrdinalIgnoreCase))
            sourceName = "My OCU Keyboard";
        using var dialog = new SaveFileDialog
        {
            Title = "Export installable MO2 keyboard mod",
            Filter = "MO2 mod archive (*.zip)|*.zip",
            FileName = $"{sourceName}.zip",
            InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

        try
        {
            Mo2ModExporter.Export(dialog.FileName, _document);

            SetStatus($"Exported installable MO2 keyboard mod: {dialog.FileName}");
            MessageBox.Show(this,
                "Shareable keyboard mod exported. Send this ZIP to another user, or install it as a normal MO2 mod after Open Composite Unleashed. It carries the layout, exact theme/font selection, colors, and artwork without replacing opencomposite.ini.",
                "Shareable keyboard exported", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception exception)
        {
            ShowError("MO2 mod export failed", exception);
        }
    }

    private void InstallToOcu()
    {
        string? root = _hostOcuRoot;
        if (root is null)
        {
            using var dialog = new FolderBrowserDialog
            {
                Description = "Choose the Skyrim VR game root, an OCU mod folder containing root, or the OCU mod's root folder",
                UseDescriptionForTitle = true,
                ShowNewFolderButton = false
            };
            if (dialog.ShowDialog(this) != DialogResult.OK)
                return;
            root = ResolveSelectedOcuRoot(dialog.SelectedPath);
            if (root is null)
            {
                MessageBox.Show(this, "That folder does not contain openvr_api.dll, either directly or inside a root subfolder. Choose the Skyrim VR game root or the active OCU MO2 mod.",
                    "Wrong folder", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            _hostOcuRoot = root;
        }

        try
        {
            string serialized = _document.Serialize();
            string expectedTarget = Path.Combine(root, "OCUKeyboard.kb");
            bool changed = !File.Exists(expectedTarget)
                || !string.Equals(File.ReadAllText(expectedTarget), serialized, StringComparison.Ordinal);
            string target = InstallDocumentToRoot(root);
            UpdateTitle();
            SetStatus($"Installed {target}. Restart Skyrim VR to load it.");
            MessageBox.Show(this,
                $"Keyboard installed successfully.\n\n" +
                $"Layout:\n{target}\n\n" +
                $"OCU installation:\n{root}\n\n" +
                (changed
                    ? "The installed layout was updated.\n\n"
                    : "That exact layout was already installed, so no file-content change was needed.\n\n") +
                "This is not a live reload. Restart Skyrim VR to see it in game.",
                "Keyboard installed for next launch", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception exception)
        {
            ShowError("OCU install failed", exception);
        }
    }

    private string InstallDocumentToRoot(string root)
    {
        string target = Path.Combine(root, "OCUKeyboard.kb");
        string serialized = _document.Serialize();
        File.WriteAllText(target, serialized, new System.Text.UTF8Encoding(false));
        CopyArtworkBesideLayout(root);
        SetIniValue(Path.Combine(root, "opencomposite.ini"), "keyboard", "layout", "auto");
        SetIniValue(Path.Combine(root, "opencomposite.ini"), "keyboard", "design", CurrentInstalledDesignId());
        if (!File.Exists(target)
            || !string.Equals(File.ReadAllText(target), serialized, StringComparison.Ordinal))
            throw new IOException($"OCU Keyboard Studio could not verify the installed layout at {target}.");
        VerifyInstalledArtwork(target);
        return target;
    }

    private static string CurrentInstalledDesignId()
    {
        // The installed loose layout is authoritative. Reusing the source
        // package/stock filename here allowed an edited "Dwemer" to resolve
        // back to bundled Dwemer when Configurator refreshed its design list.
        return "installed";
    }

    private void VerifyInstalledArtwork(string installedLayoutPath)
    {
        KeyboardDocument installed = KeyboardDocument.Load(installedLayoutPath);
        static bool Authored(string? imagePath, string? fileName)
            => !string.IsNullOrWhiteSpace(imagePath) || !string.IsNullOrWhiteSpace(fileName);
        static void RequireResolved(bool expected, string? resolvedPath, string description)
        {
            if (expected && (string.IsNullOrWhiteSpace(resolvedPath) || !File.Exists(resolvedPath)))
                throw new IOException($"OCU Keyboard Studio installed the layout but could not verify its {description} beside OCUKeyboard.kb.");
        }

        RequireResolved(Authored(_document.BackgroundImagePath, _document.BackgroundFileName),
            installed.BackgroundImagePath, "keyboard background");
        RequireResolved(Authored(_document.ConsoleInputBackgroundImagePath, _document.ConsoleInputBackgroundFileName),
            installed.ConsoleInputBackgroundImagePath, "console INPUT artwork");
        RequireResolved(Authored(_document.ModeVrArtworkImagePath, _document.ModeVrArtworkFileName),
            installed.ModeVrArtworkImagePath, "VR Mode state artwork");
        RequireResolved(Authored(_document.ModePcArtworkImagePath, _document.ModePcArtworkFileName),
            installed.ModePcArtworkImagePath, "PC Mode state artwork");
        RequireResolved(Authored(_document.LockWorldArtworkImagePath, _document.LockWorldArtworkFileName),
            installed.LockWorldArtworkImagePath, "world-lock state artwork");
        RequireResolved(Authored(_document.LockHeadArtworkImagePath, _document.LockHeadArtworkFileName),
            installed.LockHeadArtworkImagePath, "head-lock state artwork");
        RequireResolved(Authored(_document.ControlArrowImagePath, _document.ControlArrowFileName),
            installed.ControlArrowImagePath, "side-control arrow artwork");

        if (_document.Sprites.Count != installed.Sprites.Count
            || installed.Sprites.Any(sprite => string.IsNullOrWhiteSpace(sprite.SourcePath) || !File.Exists(sprite.SourcePath)))
            throw new IOException("OCU Keyboard Studio installed the layout but could not verify all of its sprite artwork beside OCUKeyboard.kb.");

        if (_document.FontName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase)
            && (string.IsNullOrWhiteSpace(installed.CustomFontMetadataPath)
                || string.IsNullOrWhiteSpace(installed.CustomFontTexturePath)
                || !File.Exists(installed.CustomFontMetadataPath)
                || !File.Exists(installed.CustomFontTexturePath)))
            throw new IOException("OCU Keyboard Studio installed the layout but could not verify its custom font files beside OCUKeyboard.kb.");
    }

    internal static string? FindOcuRootFromStudioDirectory(string studioDirectory)
    {
        DirectoryInfo? directory;
        try { directory = new DirectoryInfo(Path.GetFullPath(studioDirectory)); }
        catch { return null; }

        for (int depth = 0; directory is not null && depth < 5; depth++, directory = directory.Parent)
        {
            string? direct = ResolveSelectedOcuRoot(directory.FullName);
            if (direct is not null)
                return direct;
        }
        return null;
    }

    internal static string? ResolveSelectedOcuRoot(string? selectedFolder)
    {
        if (string.IsNullOrWhiteSpace(selectedFolder))
            return null;
        string direct = Path.Combine(selectedFolder, "openvr_api.dll");
        if (File.Exists(direct))
            return Path.GetFullPath(selectedFolder);
        string nested = Path.Combine(selectedFolder, "root", "openvr_api.dll");
        return File.Exists(nested) ? Path.GetFullPath(Path.Combine(selectedFolder, "root")) : null;
    }

    private void RefreshDesignChoices(string? preferredPath = null)
    {
        _updatingDesignLibrary = true;
        try
        {
            string? preferred = NormalizeExistingPath(preferredPath ?? _document.SourcePath);
            var choices = new List<KeyboardDesignChoice>();
            IEnumerable<string> builtInLayouts = Directory.EnumerateFiles(_assetsDirectory, "*.kb");
            string stockDesignDirectory = Path.Combine(_assetsDirectory, "Stock Designs");
            if (Directory.Exists(stockDesignDirectory))
                builtInLayouts = builtInLayouts.Concat(
                    Directory.EnumerateFiles(stockDesignDirectory, "*.kb", SearchOption.AllDirectories));
            foreach (string path in builtInLayouts.OrderBy(Path.GetFileName))
            {
                string stem = Path.GetFileNameWithoutExtension(path);
                string name = stem.Equals("en_gb", StringComparison.OrdinalIgnoreCase)
                    ? "Parchment" : FriendlyDesignName(stem);
                choices.Add(new KeyboardDesignChoice(name, Path.GetFullPath(path), BuiltIn: true));
            }
            if (preferred is not null)
            {
                KeyboardDesignChoice? matchingBuiltIn = choices.FirstOrDefault(choice =>
                    choice.BuiltIn && KeyboardLayoutFilesEquivalent(choice.Path, preferred));
                if (matchingBuiltIn is not null)
                    preferred = matchingBuiltIn.Path;
            }

            var registered = new List<string>();
            try
            {
                if (File.Exists(DesignRegistryPath))
                    registered.AddRange(File.ReadAllLines(DesignRegistryPath));
                if (Directory.Exists(DesignLibraryDirectory))
                    registered.AddRange(Directory.EnumerateFiles(DesignLibraryDirectory, "*.kb", SearchOption.AllDirectories));
            }
            catch
            {
                // A damaged optional library must never stop Studio opening.
            }

            foreach (string path in registered
                .Select(NormalizeExistingPath)
                .Where(path => path is not null)
                .Cast<string>()
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(Path.GetFileNameWithoutExtension))
            {
                if (choices.Any(choice => choice.Path.Equals(path, StringComparison.OrdinalIgnoreCase)))
                    continue;
                KeyboardDesignChoice? equivalent = choices.FirstOrDefault(choice =>
                    KeyboardLayoutFilesEquivalent(choice.Path, path));
                if (equivalent is not null)
                {
                    if (preferred is not null && path.Equals(preferred, StringComparison.OrdinalIgnoreCase))
                        preferred = equivalent.Path;
                    continue;
                }
                string stem = Path.GetFileNameWithoutExtension(path);
                string name = UniqueCustomDesignName(choices, FriendlyDesignName(stem));
                choices.Add(new KeyboardDesignChoice(name, path, BuiltIn: false));
            }

            if (preferred is not null && choices.All(choice => !choice.Path.Equals(preferred, StringComparison.OrdinalIgnoreCase)))
                choices.Add(new KeyboardDesignChoice(
                    UniqueCustomDesignName(choices, FriendlyDesignName(Path.GetFileNameWithoutExtension(preferred))),
                    preferred, BuiltIn: false));

            _designs.Clear();
            _designs.AddRange(choices);
            _designCombo.Items.Clear();
            _designCombo.Items.AddRange(_designs.Cast<object>().ToArray());
            int selected = preferred is null ? -1 : _designs.FindIndex(choice => choice.Path.Equals(preferred, StringComparison.OrdinalIgnoreCase));
            _designCombo.SelectedIndex = selected >= 0 ? selected : (_designs.Count > 0 ? 0 : -1);
        }
        finally
        {
            _updatingDesignLibrary = false;
        }
    }

    private string RegisterDesign(string path)
    {
        string fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(DesignLibraryDirectory);
        string assetsRoot = Path.GetFullPath(_assetsDirectory).TrimEnd(Path.DirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        if (fullPath.StartsWith(assetsRoot, StringComparison.OrdinalIgnoreCase))
            return fullPath;

        string libraryRoot = Path.GetFullPath(DesignLibraryDirectory).TrimEnd(Path.DirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        string registeredPath = fullPath;
        if (!fullPath.StartsWith(libraryRoot, StringComparison.OrdinalIgnoreCase))
        {
            string stem = Path.GetFileNameWithoutExtension(fullPath);
            string safeStem = string.Concat(stem.Select(character =>
                char.IsLetterOrDigit(character) || character is '-' or '_' ? character : '-')).Trim('-');
            if (string.IsNullOrWhiteSpace(safeStem))
                safeStem = "Keyboard";
            byte[] pathBytes = System.Text.Encoding.UTF8.GetBytes(fullPath.ToUpperInvariant());
            string suffix = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(pathBytes))[..8];
            string projectDirectory = Path.Combine(DesignLibraryDirectory, $"{safeStem}-{suffix}");
            Directory.CreateDirectory(projectDirectory);
            registeredPath = Path.Combine(projectDirectory, $"{safeStem}.kb");
            File.WriteAllText(registeredPath, _document.Serialize(), new System.Text.UTF8Encoding(false));
            CopyArtworkBesideLayout(projectDirectory);
        }

        var paths = File.Exists(DesignRegistryPath)
            ? File.ReadAllLines(DesignRegistryPath).Where(line => !string.IsNullOrWhiteSpace(line)).ToList()
            : [];
        if (!paths.Any(existing => existing.Equals(registeredPath, StringComparison.OrdinalIgnoreCase)))
        {
            paths.Add(registeredPath);
            File.WriteAllLines(DesignRegistryPath, paths.Distinct(StringComparer.OrdinalIgnoreCase));
        }
        return registeredPath;
    }

    private static string? NormalizeExistingPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;
        try
        {
            string fullPath = Path.GetFullPath(path);
            return File.Exists(fullPath) ? fullPath : null;
        }
        catch
        {
            return null;
        }
    }

    private static bool KeyboardLayoutFilesEquivalent(string left, string right)
    {
        try
        {
            return File.ReadAllText(left).Equals(File.ReadAllText(right), StringComparison.Ordinal);
        }
        catch
        {
            return false;
        }
    }

    private static string FriendlyDesignName(string stem)
    {
        string value = System.Text.RegularExpressions.Regex.Replace(
            stem.Replace('_', ' ').Replace('-', ' '), "(?<=[a-z0-9])(?=[A-Z])", " ").Trim();
        return string.IsNullOrWhiteSpace(value) ? "Unnamed Keyboard" : value;
    }

    private static string UniqueCustomDesignName(
        IReadOnlyCollection<KeyboardDesignChoice> choices, string baseName)
    {
        if (!choices.Any(choice => choice.Name.Equals(baseName, StringComparison.OrdinalIgnoreCase)))
            return baseName;
        string candidate = $"{baseName} (Custom)";
        int suffix = 2;
        while (choices.Any(choice => choice.Name.Equals(candidate, StringComparison.OrdinalIgnoreCase)))
            candidate = $"{baseName} (Custom {suffix++})";
        return candidate;
    }

    private static void SetIniValue(string path, string section, string key, string value)
    {
        List<string> lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];
        int sectionStart = lines.FindIndex(line => line.Trim().Equals($"[{section}]", StringComparison.OrdinalIgnoreCase));
        if (sectionStart < 0)
        {
            if (lines.Count > 0 && !string.IsNullOrWhiteSpace(lines[^1]))
                lines.Add("");
            lines.Add($"[{section}]");
            lines.Add($"{key}={value}");
        }
        else
        {
            int sectionEnd = lines.FindIndex(sectionStart + 1, line => line.TrimStart().StartsWith('['));
            if (sectionEnd < 0)
                sectionEnd = lines.Count;
            int keyLine = -1;
            for (int index = sectionStart + 1; index < sectionEnd; index++)
            {
                string trimmed = lines[index].TrimStart();
                int equals = trimmed.IndexOf('=');
                if (equals > 0 && trimmed[..equals].Trim().Equals(key, StringComparison.OrdinalIgnoreCase))
                {
                    keyLine = index;
                    break;
                }
            }
            if (keyLine >= 0)
                lines[keyLine] = $"{key}={value}";
            else
                lines.Insert(sectionEnd, $"{key}={value}");
        }
        File.WriteAllLines(path, lines);
    }

    private void CopyArtworkBesideLayout(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory))
            return;
        CopyArtwork(_document.BackgroundImagePath, Path.Combine(directory, "OCUKeyboardBackground.png"));
        CopyArtwork(_document.ConsoleInputBackgroundImagePath,
            Path.Combine(directory, KeyboardDocument.ConsoleInputBackgroundPortableName));
        CopyArtwork(_document.ModeVrArtworkImagePath,
            Path.Combine(directory, KeyboardDocument.ModeVrArtworkPortableName));
        CopyArtwork(_document.ModePcArtworkImagePath,
            Path.Combine(directory, KeyboardDocument.ModePcArtworkPortableName));
        CopyArtwork(_document.LockWorldArtworkImagePath,
            Path.Combine(directory, KeyboardDocument.LockWorldArtworkPortableName));
        CopyArtwork(_document.LockHeadArtworkImagePath,
            Path.Combine(directory, KeyboardDocument.LockHeadArtworkPortableName));
        for (int index = 0; index < _document.Sprites.Count; index++)
            CopyArtwork(_document.Sprites[index].SourcePath,
                Path.Combine(directory, KeyboardDocument.SpriteFileName(index)));
        CopyArtwork(_document.ControlArrowImagePath, Path.Combine(directory, "OCUKeyboardControlArrow.png"));
        CopyFile(_document.CustomFontMetadataPath, Path.Combine(directory, "OCUKeyboardFont.sfn"));
        CopyFile(_document.CustomFontTexturePath, Path.Combine(directory, "OCUKeyboardFont.png"));
    }

    private static void CopyFile(string? source, string destination)
    {
        if (string.IsNullOrWhiteSpace(source) || !File.Exists(source))
            return;
        if (Path.GetFullPath(source).Equals(Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase))
            return;
        File.Copy(source, destination, overwrite: true);
    }

    private static void CopyArtwork(string? source, string destination)
    {
        if (string.IsNullOrWhiteSpace(source) || !File.Exists(source))
            return;
        if (Path.GetFullPath(source).Equals(Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase))
            return;
        string extension = Path.GetExtension(source);
        if (extension.Equals(".png", StringComparison.OrdinalIgnoreCase))
        {
            File.Copy(source, destination, overwrite: true);
            return;
        }
        using var image = new Bitmap(source);
        using var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None);
        image.Save(output, System.Drawing.Imaging.ImageFormat.Png);
    }

    private static OpenFileDialog PngArtworkDialog(string title) => new()
    {
        Title = title,
        Filter = "PNG artwork (*.png)|*.png"
    };

    private static OpenFileDialog BackgroundArtworkDialog(string title) => new()
    {
        Title = title,
        Filter = "Recommended PNG background (*.png)|*.png|JPEG background (*.jpg;*.jpeg)|*.jpg;*.jpeg"
    };

    private bool ConfirmDiscard(string action = "Continuing")
    {
        if (!_document.IsDirty)
            return true;
        DialogResult result = MessageBox.Show(this,
            $"This keyboard has unsaved changes. {action} will discard them.\n\nSave before continuing?",
            "Unsaved keyboard changes", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Warning);
        return result switch
        {
            DialogResult.Yes => SaveLayout(false),
            DialogResult.No => true,
            _ => false
        };
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs e)
    {
        if (!ConfirmDiscard("Closing Keyboard Studio"))
            e.Cancel = true;
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (_capturingAssignedKey)
        {
            Keys keyCode = keyData & Keys.KeyCode;
            if (keyCode != Keys.None)
                CaptureAssignedKey(keyCode);
            return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    private void OnShortcutKeyDown(object? sender, KeyEventArgs e)
    {
        if (_capturingAssignedKey)
        {
            CaptureAssignedKey(e.KeyCode);
            e.SuppressKeyPress = true;
            e.Handled = true;
            return;
        }
        bool editingTextOrNumber = FocusedEditor() is not null;
        if (e.Control && e.KeyCode == Keys.S) { SaveLayout(e.Shift); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.O) { OpenLayout(); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.Z && !editingTextOrNumber) { Undo(); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.Y && !editingTextOrNumber) { Redo(); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.X && !editingTextOrNumber) { _canvas.CutSelection(); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.C && !editingTextOrNumber) { _canvas.CopySelection(); e.SuppressKeyPress = true; }
        else if (e.Control && e.KeyCode == Keys.V && !editingTextOrNumber) { _canvas.PasteClipboard(); e.SuppressKeyPress = true; }
        else if (e.KeyCode == Keys.Delete && !editingTextOrNumber)
        {
            if (!_canvas.DeleteSelection()) DeleteKey();
            e.SuppressKeyPress = true;
        }
    }

    private Control? FocusedEditor()
    {
        return FindFocusedEditor(this);

        static Control? FindFocusedEditor(Control root)
        {
            if (!root.ContainsFocus)
                return null;
            if (root is TextBoxBase or UpDownBase)
                return root;
            foreach (Control child in root.Controls)
            {
                Control? editor = FindFocusedEditor(child);
                if (editor is not null)
                    return editor;
            }
            return null;
        }
    }

    private SudoFont LoadSelectedFont()
    {
        if (_fontCombo.SelectedItem is not FontChoice choice)
            choice = _fonts[0];
        return new SudoFont(choice.MetadataPath, choice.TexturePath);
    }

    private void ImportFont()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Add a TrueType or OpenType font",
            Filter = "Font files (*.ttf;*.otf)|*.ttf;*.otf|TrueType font (*.ttf)|*.ttf|OpenType font (*.otf)|*.otf"
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        try
        {
            FontChoice fallbackChoice = _fonts.First(font => font.ConfigName == "ocu_nordic");
            var fallback = new SudoFont(fallbackChoice.MetadataPath, fallbackChoice.TexturePath);
            FontImportResult result = FontImporter.Import(dialog.FileName, _document, _assetsDirectory, fallback);
            var choice = new FontChoice(result.DisplayName, result.MetadataPath, result.TexturePath,
                result.ConfigName, result.SourceFontPath);
            int index = _fonts.FindIndex(font => font.ConfigName == result.ConfigName);
            if (index >= 0)
            {
                _fonts[index] = choice;
                _fontCombo.Items[index] = choice;
            }
            else
            {
                _fonts.Add(choice);
                _fontCombo.Items.Add(choice);
                index = _fonts.Count - 1;
            }

            PushUndo();
            _document.FontName = result.ConfigName;
            _document.CustomFontMetadataPath = result.MetadataPath;
            _document.CustomFontTexturePath = result.TexturePath;
            _updatingEditor = true;
            try { _fontCombo.SelectedIndex = index; }
            finally { _updatingEditor = false; }
            _renderer!.Font = new SudoFont(result.MetadataPath, result.TexturePath);
            MarkChanged();
            _canvas.RefreshPreview();
            ReportFontCoverage(result, imported: true);
        }
        catch (Exception exception)
        {
            ShowError("Font import failed", exception);
        }
    }

    private void AuditSelectedCustomFont(bool showMessage)
    {
        if (_updatingEditor || _fontCombo.SelectedItem is not FontChoice choice)
            return;
        try
        {
            var current = new SudoFont(choice.MetadataPath, choice.TexturePath);
            char[] absent = _document.RequiredFontCharacters().Where(character => !current.ContainsGlyph(character)).ToArray();
            if (absent.Length == 0)
                return;
            if (!choice.IsCustom)
            {
                if (showMessage)
                    MessageBox.Show(this,
                        $"{choice.Name} cannot draw the new key text: {DescribeCharacters(absent)}\n\nChoose another font or import a TTF/OTF that supports it.",
                        "Unsupported key text", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (string.IsNullOrWhiteSpace(choice.SourceFontPath) || !File.Exists(choice.SourceFontPath))
            {
                if (showMessage)
                    MessageBox.Show(this,
                        $"The selected custom font does not contain the new key text: {DescribeCharacters(absent)}\n\nRe-import its TTF/OTF so Studio can rebuild the font and use OCU Nordic where needed.",
                        "Unsupported key text", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            FontChoice fallbackChoice = _fonts.First(font => font.ConfigName == "ocu_nordic");
            var fallback = new SudoFont(fallbackChoice.MetadataPath, fallbackChoice.TexturePath);
            FontImportResult result = FontImporter.Import(choice.SourceFontPath, _document, _assetsDirectory,
                fallback, choice.ConfigName);
            var refreshed = choice with
            {
                Name = result.DisplayName,
                MetadataPath = result.MetadataPath,
                TexturePath = result.TexturePath,
                SourceFontPath = result.SourceFontPath
            };
            int index = _fonts.IndexOf(choice);
            _fonts[index] = refreshed;
            _fontCombo.Items[index] = refreshed;
            _fontCombo.SelectedIndex = index;
            _document.CustomFontMetadataPath = result.MetadataPath;
            _document.CustomFontTexturePath = result.TexturePath;
            _renderer!.Font = new SudoFont(result.MetadataPath, result.TexturePath);
            _canvas.RefreshPreview();
            if (showMessage)
                ReportFontCoverage(result, imported: false);
        }
        catch (Exception exception)
        {
            ShowError("Font coverage check failed", exception);
        }
    }

    private void ReportFontCoverage(FontImportResult result, bool imported)
    {
        if (result.UnsupportedCharacters.Count > 0)
        {
            MessageBox.Show(this,
                $"These characters are used by existing keys but are not supported by either {result.DisplayName} or OCU Nordic:\n\n{DescribeCharacters(result.UnsupportedCharacters)}\n\nThose key characters cannot be drawn.",
                "Unsupported key text", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            SetStatus($"{result.DisplayName}: unsupported existing-key characters were reported.");
            return;
        }
        if (result.FallbackCharacters.Count > 0)
        {
            MessageBox.Show(this,
                $"{result.DisplayName} does not contain these characters used by existing keys:\n\n{DescribeCharacters(result.FallbackCharacters)}\n\nOCU Nordic will draw only those missing characters.",
                "OCU Nordic fallback", MessageBoxButtons.OK, MessageBoxIcon.Information);
            SetStatus($"{result.DisplayName} converted; {result.FallbackCharacters.Count} existing-key glyphs use OCU Nordic.");
            return;
        }
        SetStatus(imported
            ? $"{result.DisplayName} converted from TTF/OTF for the {_document.Keys.Count} existing keys."
            : $"{result.DisplayName} rebuilt for the newly edited key text.");
    }

    private static string DescribeCharacters(IEnumerable<char> characters)
        => string.Join("  ", characters.Distinct().Select(character => character == ' '
            ? "[space]" : $"{character} (U+{(int)character:X4})"));

    private void UpdateTitle()
    {
        string name = Path.GetFileName(_currentPackagePath ?? _document.SourcePath) ?? "Untitled";
        if (_saveButton is not null)
        {
            _saveButton.Enabled = _document.IsDirty;
            _toolTip.SetToolTip(_saveButton, _document.IsDirty
                ? "Save changes to this keyboard project."
                : "Nothing changed. Use Save As to create another copy.");
        }
        Text = $"{(_document.IsDirty ? "● " : "")}OCU Keyboard Studio — {name}";
    }

    private void SetStatus(string text) => _statusLabel.Text = text;

    private void ShowError(string title, Exception exception)
        => MessageBox.Show(this, exception.Message, title, MessageBoxButtons.OK, MessageBoxIcon.Error);

    private static string DisplayKeyName(char key) => key switch
    {
        '\t' => "Tab",
        '\n' => "Enter / Done",
        '\b' => "Backspace",
        ' ' => "Space",
        '\x01' => "Shift",
        '\x02' => "Caps Lock",
        '\x03' => "Done",
        '\x04' => "Arrow Up",
        '\x05' => "Arrow Down",
        '\x06' => "Arrow Left",
        '\x07' => "Arrow Right",
        '\x0E' => "Escape",
        '\x1D' => "End",
        '\x1E' => "Ctrl",
        '\x1F' => "Print Screen",
        >= '\x10' and <= '\x1B' => $"F{key - '\x10' + 1}",
        _ => key.ToString()
    };

    private static string ThemeConfigName(KeyboardTheme theme) => theme.Name switch
    {
        "Modern Green" => "modern_green",
        "Modern White" => "modern_white",
        "Modern Blue" => "modern_blue",
        "Modern Amber" => "modern_amber",
        "Modern Purple" => "modern_purple",
        "SkyUI Dark" => "skyui",
        _ => theme.Name.ToLowerInvariant()
    };

    private static NumericUpDown NumberBox(decimal minimum, decimal maximum, int decimals, decimal increment)
        => new()
        {
            Minimum = minimum,
            Maximum = maximum,
            DecimalPlaces = decimals,
            Increment = increment,
            Width = 150,
            BorderStyle = BorderStyle.FixedSingle,
            BackColor = StudioTheme.Input,
            ForeColor = TextPrimary
        };

    private static decimal ClampDecimal(decimal value, NumericUpDown control) => Math.Clamp(value, control.Minimum, control.Maximum);

    private static Button ActionButton(string text, EventHandler action, bool compact = false)
    {
        var button = new ModernPillButton
        {
            Text = text,
            AutoSize = true,
            Height = compact ? 30 : 34,
            ForeColor = TextPrimary,
            Padding = compact ? new Padding(7, 1, 7, 1) : new Padding(10, 2, 10, 2),
            Margin = new Padding(3, 1, 3, 1),
            Cursor = Cursors.Hand,
            Destructive = text.Contains("Delete", StringComparison.OrdinalIgnoreCase)
                || text.Equals("Remove", StringComparison.OrdinalIgnoreCase)
                || text.Equals("Clear", StringComparison.OrdinalIgnoreCase)
        };
        button.Click += action;
        return button;
    }

    private void OpenExternalUrl(string url)
    {
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url)
            {
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                this,
                $"Could not open the link.\n\n{url}\n\n{ex.Message}",
                "OCU Keyboard Studio",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
        }
    }

    private static void SetSwatch(ColorEntryControl entry, Color color) => entry.Value = color;

    private static TabPage InspectorPage(string title) => new(title)
    {
        BackColor = Surface,
        ForeColor = TextPrimary,
        Padding = new Padding(8),
        AutoScroll = true
    };

    private static TableLayoutPanel InspectorTable()
    {
        var table = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            ColumnCount = 2,
            RowCount = 1,
            Padding = new Padding(2),
            BackColor = Surface
        };
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        return table;
    }

    private static Control Spacer(int width) => new Panel { Width = width, Height = 1, Margin = Padding.Empty };

    private static Label Caption(string text) => new()
    {
        Text = text,
        AutoSize = true,
        ForeColor = TextMuted,
        Margin = new Padding(9, 9, 4, 0)
    };

    private static void StyleCombo(ComboBox combo)
    {
        combo.BackColor = StudioTheme.Input;
        combo.ForeColor = TextPrimary;
        combo.FlatStyle = FlatStyle.Flat;
        combo.Height = 32;
        combo.Margin = new Padding(0, 4, 4, 0);
    }

    private static void StyleCheck(ButtonBase check)
    {
        check.ForeColor = TextPrimary;
        check.BackColor = Color.Transparent;
        check.Margin = new Padding(8, 8, 3, 0);
    }

    private static void AddSection(TableLayoutPanel table, string text)
    {
        var label = new Label
        {
            Text = text,
            AutoSize = true,
            ForeColor = AccentBright,
            Font = new Font("Segoe UI Semibold", 9.5f),
            Margin = new Padding(0, 18, 0, 7)
        };
        AddWide(table, label);
    }

    private static void AddHint(TableLayoutPanel table, string text)
    {
        var label = new Label
        {
            Text = text,
            AutoSize = true,
            MaximumSize = new Size(390, 0),
            ForeColor = TextMuted,
            Margin = new Padding(0, 7, 0, 3)
        };
        AddWide(table, label);
    }

    private static void AddRow(TableLayoutPanel table, string caption, Control control)
    {
        int row = table.RowCount++;
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var label = new Label { Text = caption, AutoSize = true, ForeColor = TextMuted, Anchor = AnchorStyles.Left, Margin = new Padding(0, 8, 5, 4) };
        control.Dock = DockStyle.Fill;
        control.Margin = new Padding(0, 3, 0, 3);
        if (control is TextBox or NumericUpDown or ComboBox or ListBox)
            control.BackColor = StudioTheme.Input;
        control.ForeColor = TextPrimary;
        table.Controls.Add(label, 0, row);
        table.Controls.Add(control, 1, row);
    }

    private static void AddWide(TableLayoutPanel table, Control control)
    {
        int row = table.RowCount++;
        table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        control.Margin = new Padding(0, 3, 0, 3);
        table.Controls.Add(control, 0, row);
        table.SetColumnSpan(control, 2);
    }

    private sealed record FontChoice(string Name, string MetadataPath, string TexturePath, string ConfigName, string? SourceFontPath)
    {
        public bool IsCustom => ConfigName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase);
        public override string ToString() => Name;
    }
}
