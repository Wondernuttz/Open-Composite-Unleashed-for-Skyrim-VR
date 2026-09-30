namespace OCUKeyboardStudio;

internal static class StudioSelfTest
{
    public static int Run(string outputDirectory)
    {
        try
        {
            string assets = Path.Combine(AppContext.BaseDirectory, "Assets");
            string layoutPath = Path.Combine(assets, "en_gb.kb");
            var document = KeyboardDocument.Load(layoutPath);
            if (document.Keys.Count < 60)
                throw new InvalidDataException($"Expected the full OCU layout, got only {document.Keys.Count} keys.");
            if (document.InputOutlineOverrideEnabled)
                throw new InvalidDataException("A legacy keyboard unexpectedly enabled the optional input-outline override.");
            KeyboardKey numberThree = document.Keys.Single(key => key.Character == '3');
            if (numberThree.ShiftCharacter != '#')
                throw new InvalidDataException("The bundled number row does not map Shift+3 to #.");
            if (!PhysicalKeyboardTranslator.TryTranslate(Keys.PrintScreen, out PhysicalKeyAssignment printScreen)
                || printScreen.Normal != '\x1F' || printScreen.Shifted != '\x1F')
                throw new InvalidDataException("Physical-key capture did not translate Print Screen to OCU's supported output.");

            KeyboardKey f10 = document.Keys.Single(key => key.Label == "F10");
            f10.LabelOffsetY = -1;
            f10.LabelScale = 0.95f;
            KeyboardKey upArrow = document.Keys.Single(key => key.Character == '\x04');
            upArrow.LabelOffsetX = 9;
            upArrow.LabelOffsetY = -6;
            upArrow.LabelScale = 0.5f;
            document.BaseTheme = "modern_green";
            document.FontName = "ocu_nordic";
            document.CustomStyleEnabled = true;
            document.KeyPlatesEnabled = false;
            document.TopButtonPlatesEnabled = false;
            document.InputBarPlateEnabled = false;
            document.ParchmentRibbonEnabled = false;
            document.FontColor = Color.FromArgb(255, 225, 245, 235);
            document.FontOutlineColor = Color.FromArgb(210, 31, 18, 52);
            document.FontGlowColor = Color.FromArgb(230, 70, 140, 255);
            document.FontGlowEnabled = true;
            document.FontGlowStrength = 68;
            document.FontGlowRadius = 5;
            document.FontBreatheEnabled = true;
            document.FontBreatheMinPercent = 25;
            document.FontBreathePeriodSeconds = 3.5f;
            document.FontBreathePhaseDegrees = 60;
            document.KeyColor = Color.FromArgb(190, 62, 190, 143);
            document.PlateFillColor = Color.FromArgb(90, 12, 18, 24);
            document.PlateOutlineWidth = 3;
            document.InputFillOverrideEnabled = true;
            document.InputFillColor = Color.FromArgb(210, 24, 31, 42);
            document.InputOutlineOverrideEnabled = true;
            document.InputOutlineVisible = false;
            document.InputOutlineColor = Color.FromArgb(255, 17, 99, 203);
            document.InputOutlineWidth = 5;
            document.InputTitleOffsetX = 17;
            document.InputTitleOffsetY = -3;
            document.InputTextOffsetX = 23;
            document.InputTextOffsetY = 4;
            document.GlowRadius = 6;
            document.KeyRoundness = 24;
            document.KeyBreatheEnabled = true;
            document.KeyBreatheMinPercent = 30;
            document.KeyBreathePeriodSeconds = 2.5f;
            document.KeyBreathePhaseDegrees = 15;
            document.BackgroundImagePath = Path.Combine(assets, "skyui-bg.png");
            document.BackgroundFileName = "OCUKeyboardBackground.png";
            document.BackgroundX = 12;
            document.BackgroundY = 8;
            document.BackgroundWidth = 1000;
            document.BackgroundHeight = 544;
            document.BackgroundOpacity = 88;
            document.BackgroundEdgeFade = 12;
            document.BackgroundRotation = 1.5f;
            document.BackgroundRoundness = 36;
            document.Sprites.Add(new KeyboardSprite
            {
                SourcePath = Path.Combine(assets, "spacebar.png"),
                FileName = "OCUKeyboardSprite01.png",
                X = 80,
                Y = 24,
                Width = 360,
                Height = 70,
                Opacity = 72,
                EdgeFade = 5,
                Rotation = -4,
                GlowEnabled = true,
                GlowColor = Color.FromArgb(230, 90, 210, 255),
                GlowStrength = 70,
                GlowRadius = 16,
                BreatheEnabled = true,
                BreatheMinPercent = 20,
                BreathePeriodSeconds = 2,
                BreathePhaseDegrees = 0
            });
            document.Sprites.Add(new KeyboardSprite
            {
                SourcePath = Path.Combine(assets, "dwemer-spacebar.png"),
                FileName = "OCUKeyboardSprite02.png",
                X = 590,
                Y = 454,
                Width = 300,
                Height = 55,
                Opacity = 64,
                EdgeFade = 3,
                Rotation = 7
            });
            document.SizeControlOffsetX = 8;
            document.SizeControlOffsetY = -6;
            document.OpacityControlOffsetX = -5;
            document.TiltControlOffsetY = 9;
            document.TextBarOffsetX = 14;
            document.TextBarOffsetY = -7;
            document.TextBarWidth = 700;
            document.TextBarHeight = 50;
            document.TextBarFontScale = 0.8f;
            document.ModeButtonOffsetX = 19;
            document.ModeButtonOffsetY = 8;
            document.ModeButtonWidth = 240;
            document.ModeButtonHeight = 40;
            document.ModeButtonFontScale = 0.9f;
            document.ModeArtworkOffsetX = 7;
            document.ModeArtworkOffsetY = -4;
            document.ModeArtworkWidth = 218;
            document.ModeArtworkHeight = 34;
            document.ModeTextOffsetX = -9;
            document.ModeTextOffsetY = 3;
            document.LockButtonOffsetX = -11;
            document.LockButtonOffsetY = 6;
            document.LockButtonWidth = 140;
            document.LockButtonHeight = 36;
            document.LockButtonFontScale = 1.1f;
            document.LockArtworkOffsetX = -5;
            document.LockArtworkOffsetY = 2;
            document.LockArtworkWidth = 126;
            document.LockArtworkHeight = 30;
            document.LockTextOffsetX = 6;
            document.LockTextOffsetY = -2;
            document.SizeControlDesign.Width = 128;
            document.SizeControlDesign.Height = 116;
            document.SizeControlDesign.UpOffsetX = 11;
            document.SizeControlDesign.UpOffsetY = -3;
            document.SizeControlDesign.UpWidth = 31;
            document.SizeControlDesign.UpHeight = 23;
            document.SizeControlDesign.DownOffsetX = -7;
            document.SizeControlDesign.DownOffsetY = 5;
            document.SizeControlDesign.DownWidth = 29;
            document.SizeControlDesign.DownHeight = 19;
            document.SizeControlDesign.LabelOffsetX = 4;
            document.SizeControlDesign.LabelOffsetY = -2;
            document.SizeControlDesign.LabelScale = 0.72f;
            document.SizeControlDesign.ValueOffsetX = -3;
            document.SizeControlDesign.ValueOffsetY = 2;
            document.SizeControlDesign.ValueScale = 0.66f;
            document.ControlArrowImagePath = Path.Combine(assets, "spacebar.png");
            document.ControlArrowFileName = "OCUKeyboardControlArrow.png";
            document.ControlArrowRotation = 12;
            document.ControlArrowGlowEnabled = true;
            document.ControlArrowGlowColor = Color.FromArgb(230, 70, 210, 255);
            document.ControlArrowGlowStrength = 63;
            document.ControlArrowGlowRadius = 9;
            document.ControlArrowBreatheEnabled = true;
            document.ControlArrowBreatheMinPercent = 40;
            document.ControlArrowBreathePeriodSeconds = 3;
            document.ControlArrowBreathePhaseDegrees = 45;

            Directory.CreateDirectory(outputDirectory);
            string consoleInputArtwork = Path.Combine(outputDirectory, "self-test-console-input.png");
            using (var consoleImage = new Bitmap(1024, 120))
            using (Graphics graphics = Graphics.FromImage(consoleImage))
            using (var brush = new SolidBrush(Color.FromArgb(255, 210, 35, 180)))
            {
                graphics.Clear(Color.Transparent);
                graphics.FillRectangle(brush, 80, 10, 160, 32);
                consoleImage.Save(consoleInputArtwork, System.Drawing.Imaging.ImageFormat.Png);
            }
            document.ConsoleInputBackgroundImagePath = consoleInputArtwork;
            document.ConsoleInputBackgroundFileName = KeyboardDocument.ConsoleInputBackgroundPortableName;
            document.ModeVrArtworkImagePath = Path.Combine(assets, "spacebar.png");
            document.ModeVrArtworkFileName = KeyboardDocument.ModeVrArtworkPortableName;
            document.ModePcArtworkImagePath = Path.Combine(assets, "dwemer-spacebar.png");
            document.ModePcArtworkFileName = KeyboardDocument.ModePcArtworkPortableName;
            document.LockWorldArtworkImagePath = consoleInputArtwork;
            document.LockWorldArtworkFileName = KeyboardDocument.LockWorldArtworkPortableName;
            document.LockHeadArtworkImagePath = Path.Combine(assets, "skyui-bg.png");
            document.LockHeadArtworkFileName = KeyboardDocument.LockHeadArtworkPortableName;
            document.ModeTextOverArtwork = true;
            document.LockTextOverArtwork = true;
            string hostFixture = Path.Combine(outputDirectory, "ocu-host-fixture");
            string hostedStudio = Path.Combine(hostFixture, "OCU Keyboard Studio");
            string hostedRoot = Path.Combine(hostFixture, "root");
            Directory.CreateDirectory(hostedStudio);
            Directory.CreateDirectory(hostedRoot);
            File.WriteAllBytes(Path.Combine(hostedRoot, "openvr_api.dll"), [0x4f, 0x43, 0x55]);
            string? detectedRoot = MainForm.FindOcuRootFromStudioDirectory(hostedStudio);
            if (!string.Equals(Path.GetFullPath(hostedRoot), detectedRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Studio did not auto-detect its containing OCU mod root.");
            string? explicitRoot = MainForm.ResolveSelectedOcuRoot(hostFixture);
            if (!string.Equals(Path.GetFullPath(hostedRoot), explicitRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Studio did not resolve the OCU root explicitly supplied by Configurator.");
            string? launchRoot = Program.GetOptionValue(
                ["--unrelated", "value", "--ocu-root", hostFixture], "--ocu-root");
            if (!string.Equals(hostFixture, launchRoot, StringComparison.Ordinal))
                throw new InvalidDataException("Studio did not parse Configurator's explicit OCU root handoff.");
            string roundTrip = Path.Combine(outputDirectory, "keyboard-studio-roundtrip.kb");
            document.Save(roundTrip);
            File.Copy(consoleInputArtwork,
                Path.Combine(outputDirectory, KeyboardDocument.ConsoleInputBackgroundPortableName), overwrite: true);
            File.Copy(document.ModeVrArtworkImagePath,
                Path.Combine(outputDirectory, KeyboardDocument.ModeVrArtworkPortableName), overwrite: true);
            File.Copy(document.ModePcArtworkImagePath,
                Path.Combine(outputDirectory, KeyboardDocument.ModePcArtworkPortableName), overwrite: true);
            File.Copy(document.LockWorldArtworkImagePath,
                Path.Combine(outputDirectory, KeyboardDocument.LockWorldArtworkPortableName), overwrite: true);
            File.Copy(document.LockHeadArtworkImagePath,
                Path.Combine(outputDirectory, KeyboardDocument.LockHeadArtworkPortableName), overwrite: true);
            string serializedRoundTrip = File.ReadAllText(roundTrip);
            if (!serializedRoundTrip.Contains("# ocu_input_fill_color #181F2AD2", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_input_outline_enabled false", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_input_outline_color #1163CBFF", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_input_outline_width 5", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_input_title_offset 17 -3", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_input_text_offset 23 4", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains(
                    $"# ocu_console_input_background {KeyboardDocument.ConsoleInputBackgroundPortableName}",
                    StringComparison.Ordinal)
                || !serializedRoundTrip.Contains(
                    $"# ocu_top_state_art mode_vr {KeyboardDocument.ModeVrArtworkPortableName}",
                    StringComparison.Ordinal)
                || !serializedRoundTrip.Contains(
                    $"# ocu_top_state_art mode_pc {KeyboardDocument.ModePcArtworkPortableName}",
                    StringComparison.Ordinal)
                || !serializedRoundTrip.Contains(
                    $"# ocu_top_state_art lock_world {KeyboardDocument.LockWorldArtworkPortableName}",
                    StringComparison.Ordinal)
                || !serializedRoundTrip.Contains(
                    $"# ocu_top_state_art lock_head {KeyboardDocument.LockHeadArtworkPortableName}",
                    StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_top_state_text mode true", StringComparison.Ordinal)
                || !serializedRoundTrip.Contains("# ocu_top_state_text lock true", StringComparison.Ordinal))
                throw new InvalidDataException("Input-outline metadata is not stored as backward-compatible layout comments.");
            KeyboardDocument loaded = KeyboardDocument.Load(roundTrip);
            KeyboardDocument historyClone = loaded.Clone();
            if (!loaded.IsEquivalentForHistory(historyClone))
                throw new InvalidDataException("An unchanged history snapshot was not recognized as equivalent.");
            historyClone.Keys[0].X += 3f;
            if (loaded.IsEquivalentForHistory(historyClone))
                throw new InvalidDataException("A visible key edit was incorrectly treated as a no-op history snapshot.");
            historyClone = loaded.Clone();
            historyClone.BackgroundImagePath += ".different";
            if (loaded.IsEquivalentForHistory(historyClone))
                throw new InvalidDataException("A different authoring artwork path was incorrectly treated as a no-op history snapshot.");
            KeyboardKey loadedF10 = loaded.Keys.Single(key => key.Label == "F10");
            if (Math.Abs(loadedF10.LabelOffsetY + 1f) > 0.001f || Math.Abs(loadedF10.LabelScale - 0.95f) > 0.001f)
                throw new InvalidDataException("Layout label placement did not survive the save/load round trip.");
            KeyboardKey loadedUpArrow = loaded.Keys.Single(key => key.Character == '\x04');
            if (Math.Abs(loadedUpArrow.LabelOffsetX - 9f) > 0.001f
                || Math.Abs(loadedUpArrow.LabelOffsetY + 6f) > 0.001f
                || Math.Abs(loadedUpArrow.LabelScale - 0.5f) > 0.001f)
                throw new InvalidDataException("Arrow-key placement did not survive the save/load round trip.");
            if (loaded.BaseTheme != "modern_green" || loaded.FontName != "ocu_nordic"
                || !loaded.CustomStyleEnabled || loaded.KeyPlatesEnabled
                || loaded.TopButtonPlatesEnabled || loaded.InputBarPlateEnabled
                || loaded.ParchmentRibbonEnabled
                || loaded.KeyRoundness != 24
                || loaded.PlateOutlineWidth != 3 || loaded.PlateFillColor.A != 90
                || !loaded.InputFillOverrideEnabled || loaded.InputFillColor.ToArgb() != document.InputFillColor.ToArgb()
                || !loaded.InputOutlineOverrideEnabled || loaded.InputOutlineVisible || loaded.InputOutlineWidth != 5
                || loaded.InputOutlineColor.R != 17 || loaded.InputOutlineColor.G != 99
                || loaded.InputOutlineColor.B != 203 || loaded.InputOutlineColor.A != 255
                || Math.Abs(loaded.InputTitleOffsetX - 17) > 0.001f
                || Math.Abs(loaded.InputTitleOffsetY + 3) > 0.001f
                || Math.Abs(loaded.InputTextOffsetX - 23) > 0.001f
                || Math.Abs(loaded.InputTextOffsetY - 4) > 0.001f
                || string.IsNullOrWhiteSpace(loaded.ConsoleInputBackgroundImagePath)
                || !File.Exists(loaded.ConsoleInputBackgroundImagePath)
                || string.IsNullOrWhiteSpace(loaded.ModeVrArtworkImagePath) || !File.Exists(loaded.ModeVrArtworkImagePath)
                || string.IsNullOrWhiteSpace(loaded.ModePcArtworkImagePath) || !File.Exists(loaded.ModePcArtworkImagePath)
                || string.IsNullOrWhiteSpace(loaded.LockWorldArtworkImagePath) || !File.Exists(loaded.LockWorldArtworkImagePath)
                || string.IsNullOrWhiteSpace(loaded.LockHeadArtworkImagePath) || !File.Exists(loaded.LockHeadArtworkImagePath)
                || !loaded.ModeTextOverArtwork || !loaded.LockTextOverArtwork
                || loaded.FontOutlineColor.R != 31 || loaded.FontOutlineColor.A != 210
                || loaded.FontGlowColor.B != 255 || !loaded.FontGlowEnabled
                || loaded.FontGlowStrength != 68 || loaded.FontGlowRadius != 5
                || !loaded.FontBreatheEnabled || loaded.FontBreatheMinPercent != 25
                || Math.Abs(loaded.FontBreathePeriodSeconds - 3.5f) > 0.001f
                || Math.Abs(loaded.FontBreathePhaseDegrees - 60f) > 0.001f
                || !loaded.KeyBreatheEnabled || loaded.KeyBreatheMinPercent != 30
                || loaded.GlowRadius != 6
                || loaded.BackgroundFileName != "OCUKeyboardBackground.png"
                || Math.Abs(loaded.BackgroundRotation - 1.5f) > 0.001f
                || loaded.BackgroundRoundness != 36
                || loaded.Sprites.Count != 2 || loaded.Sprites[0].Opacity != 72
                || !loaded.Sprites[0].GlowEnabled || loaded.Sprites[0].GlowColor.B != 255
                || loaded.Sprites[0].GlowStrength != 70 || loaded.Sprites[0].GlowRadius != 16
                || !loaded.Sprites[0].BreatheEnabled || loaded.Sprites[0].BreatheMinPercent != 20
                || Math.Abs(loaded.Sprites[1].Rotation - 7f) > 0.001f
                || loaded.SizeControlOffsetX != 8 || loaded.TiltControlOffsetY != 9
                || loaded.TextBarOffsetX != 14 || loaded.TextBarOffsetY != -7
                || loaded.TextBarWidth != 700 || loaded.TextBarHeight != 50
                || Math.Abs(loaded.TextBarFontScale - 0.8f) > 0.001f
                || loaded.ModeButtonOffsetX != 19 || loaded.ModeButtonOffsetY != 8
                || loaded.ModeButtonWidth != 240 || loaded.ModeButtonHeight != 40
                || Math.Abs(loaded.ModeButtonFontScale - 0.9f) > 0.001f
                || loaded.ModeArtworkOffsetX != 7 || loaded.ModeArtworkOffsetY != -4
                || loaded.ModeArtworkWidth != 218 || loaded.ModeArtworkHeight != 34
                || loaded.ModeTextOffsetX != -9 || loaded.ModeTextOffsetY != 3
                || loaded.LockButtonOffsetX != -11 || loaded.LockButtonOffsetY != 6
                || loaded.LockButtonWidth != 140 || loaded.LockButtonHeight != 36
                || Math.Abs(loaded.LockButtonFontScale - 1.1f) > 0.001f
                || loaded.LockArtworkOffsetX != -5 || loaded.LockArtworkOffsetY != 2
                || loaded.LockArtworkWidth != 126 || loaded.LockArtworkHeight != 30
                || loaded.LockTextOffsetX != 6 || loaded.LockTextOffsetY != -2
                || loaded.SizeControlDesign.Width != 128 || loaded.SizeControlDesign.Height != 116
                || loaded.SizeControlDesign.UpOffsetX != 11 || loaded.SizeControlDesign.UpOffsetY != -3
                || loaded.SizeControlDesign.UpWidth != 31 || loaded.SizeControlDesign.UpHeight != 23
                || loaded.SizeControlDesign.DownOffsetX != -7 || loaded.SizeControlDesign.DownOffsetY != 5
                || loaded.SizeControlDesign.DownWidth != 29 || loaded.SizeControlDesign.DownHeight != 19
                || loaded.SizeControlDesign.LabelOffsetX != 4 || loaded.SizeControlDesign.LabelOffsetY != -2
                || Math.Abs(loaded.SizeControlDesign.LabelScale - 0.72f) > 0.001f
                || loaded.SizeControlDesign.ValueOffsetX != -3 || loaded.SizeControlDesign.ValueOffsetY != 2
                || Math.Abs(loaded.SizeControlDesign.ValueScale - 0.66f) > 0.001f
                || loaded.ControlArrowFileName != "OCUKeyboardControlArrow.png"
                || !loaded.ControlArrowGlowEnabled || loaded.ControlArrowGlowColor.B != 255
                || loaded.ControlArrowGlowColor.A != 230 || loaded.ControlArrowGlowStrength != 63
                || loaded.ControlArrowGlowRadius != 9
                || !loaded.ControlArrowBreatheEnabled || loaded.ControlArrowBreatheMinPercent != 40
                || Math.Abs(loaded.ControlArrowRotation - 12f) > 0.001f)
                throw new InvalidDataException("Keyboard appearance settings did not survive the save/load round trip.");
            KeyboardDocument negativePositionDocument = loaded.Clone();
            negativePositionDocument.Keys[0].X = -2.25f;
            negativePositionDocument.Keys[0].Y = -1.5f;
            KeyboardDocument negativePositionLoaded = KeyboardDocument.Parse(negativePositionDocument.Serialize());
            if (Math.Abs(negativePositionLoaded.Keys[0].X + 2.25f) > 0.001f
                || Math.Abs(negativePositionLoaded.Keys[0].Y + 1.5f) > 0.001f)
                throw new InvalidDataException("Negative key coordinates did not survive layout serialization.");
            // The serialized paths are portable names beside the layout. Point
            // this isolated test document back to its source art before render/export.
            loaded.BackgroundImagePath = Path.Combine(assets, "skyui-bg.png");
            loaded.Sprites[0].SourcePath = Path.Combine(assets, "spacebar.png");
            loaded.Sprites[1].SourcePath = Path.Combine(assets, "dwemer-spacebar.png");
            loaded.ControlArrowImagePath = Path.Combine(assets, "spacebar.png");

            string metadata = Directory.EnumerateFiles(assets, "OCU-Nordic-30.sfn").Single();
            string texture = Path.Combine(assets, "OCU-Nordic-30-texture.png");
            var renderer = new KeyboardRenderer(assets, new SudoFont(metadata, texture))
            {
                Theme = KeyboardTheme.BuiltIns.Single(theme => theme.Name == "Modern Green"),
                SelectedKeyId = -1
            };
            if (renderer.Font.Width("A A") <= renderer.Font.Width("AA"))
                throw new InvalidDataException("A missing SFN space glyph still collapses word spacing to zero pixels.");

            renderer.AnimationTimeSeconds = 0;
            renderer.PreviewPcMode = false;
            renderer.PreviewHeadLocked = false;
            using Bitmap vrWorldState = renderer.Render(loaded);
            renderer.PreviewPcMode = true;
            using Bitmap pcWorldState = renderer.Render(loaded);
            renderer.PreviewHeadLocked = true;
            using Bitmap pcHeadState = renderer.Render(loaded);
            using var vrWorldBytes = new MemoryStream();
            using var pcWorldBytes = new MemoryStream();
            using var pcHeadBytes = new MemoryStream();
            vrWorldState.Save(vrWorldBytes, System.Drawing.Imaging.ImageFormat.Png);
            pcWorldState.Save(pcWorldBytes, System.Drawing.Imaging.ImageFormat.Png);
            pcHeadState.Save(pcHeadBytes, System.Drawing.Imaging.ImageFormat.Png);
            if (vrWorldBytes.ToArray().SequenceEqual(pcWorldBytes.ToArray())
                || pcWorldBytes.ToArray().SequenceEqual(pcHeadBytes.ToArray()))
                throw new InvalidDataException("Semantic PC/VR or lock-state artwork did not change the Studio preview.");
            renderer.PreviewPcMode = false;
            renderer.PreviewHeadLocked = false;

            using (Bitmap consolePreview = renderer.RenderConsolePreview(loaded))
            {
                if (consolePreview.Width != 1024 || consolePreview.Height != 120)
                    throw new InvalidDataException("Console input preview does not match the runtime panel dimensions.");
                Color consoleBackground = consolePreview.GetPixel(20, 110);
                if (consoleBackground.ToArgb() != loaded.InputFillColor.ToArgb())
                    throw new InvalidDataException("Console input preview ignored its independent inside color.");
                Color inputBorder = consolePreview.GetPixel(0, consolePreview.Height / 2);
                if (inputBorder.ToArgb() == loaded.InputOutlineColor.ToArgb())
                    throw new InvalidDataException("Console input preview drew an outline after it was switched off.");
                Color artworkPixel = consolePreview.GetPixel(100, 20);
                if (artworkPixel.R < 150 || artworkPixel.B < 120)
                    throw new InvalidDataException("Console input preview did not composite the custom INPUT panel artwork.");
            }
            KeyboardDocument outlinedConsole = loaded.Clone();
            outlinedConsole.InputOutlineVisible = true;
            using (Bitmap outlinedConsolePreview = renderer.RenderConsolePreview(outlinedConsole))
            {
                Color inputBorder = outlinedConsolePreview.GetPixel(0, outlinedConsolePreview.Height / 2);
                if (inputBorder.ToArgb() != loaded.InputOutlineColor.ToArgb())
                    throw new InvalidDataException("Console input preview ignored the enabled input-outline color.");
            }
            KeyboardDocument alternateConsoleInk = loaded.Clone();
            alternateConsoleInk.FontColor = Color.FromArgb(255, 255, 30, 40);
            using Bitmap originalConsoleInk = renderer.RenderConsolePreview(loaded);
            using Bitmap changedConsoleInk = renderer.RenderConsolePreview(alternateConsoleInk);
            bool consoleInkChanged = false;
            for (int y = 0; y < originalConsoleInk.Height && !consoleInkChanged; y++)
                for (int x = 0; x < originalConsoleInk.Width; x++)
                    if (originalConsoleInk.GetPixel(x, y).ToArgb() != changedConsoleInk.GetPixel(x, y).ToArgb())
                    {
                        consoleInkChanged = true;
                        break;
                    }
            if (!consoleInkChanged)
                throw new InvalidDataException("Console input preview ignored the custom keyboard font color.");

            var plateTest = new KeyboardDocument
            {
                Width = 15,
                BaseTheme = "modern_green",
                FontName = "ocu_nordic",
                CustomStyleEnabled = true,
                KeyPlatesEnabled = true,
                TopButtonPlatesEnabled = false,
                InputBarPlateEnabled = false,
                FontColor = Color.Transparent,
                OutlineEnabled = false,
                GlowEnabled = false,
                PlateOutlineWidth = 3,
                KeyColor = Color.FromArgb(170, 240, 20, 30),
                PlateFillColor = Color.FromArgb(90, 15, 50, 180),
                KeyRoundness = 12
            };
            var plateTestKey = new KeyboardKey { Id = 8001, Character = 'x', X = 4, Y = 2, Label = "", ShiftLabel = "" };
            plateTest.Keys.Add(plateTestKey);
            Rectangle plateTestRectangle = Rectangle.Round(renderer.KeyRectangle(plateTest, plateTestKey));
            Point plateCenter = new(plateTestRectangle.Left + plateTestRectangle.Width / 2,
                plateTestRectangle.Top + plateTestRectangle.Height / 2);
            Point plateEdge = new(plateTestRectangle.Left + 1,
                plateTestRectangle.Top + plateTestRectangle.Height / 2);
            using Bitmap redOutline = renderer.Render(plateTest);
            plateTest.KeyColor = Color.FromArgb(170, 20, 240, 30);
            using Bitmap greenOutline = renderer.Render(plateTest);
            if (redOutline.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                    != greenOutline.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                || redOutline.GetPixel(plateEdge.X, plateEdge.Y).ToArgb()
                    == greenOutline.GetPixel(plateEdge.X, plateEdge.Y).ToArgb())
                throw new InvalidDataException("Plate outline color still bleeds through the translucent fill instead of staying on the border ring.");

            Color stableOutlinePixel = greenOutline.GetPixel(plateEdge.X, plateEdge.Y);
            plateTest.PlateFillColor = Color.FromArgb(90, 220, 160, 20);
            using Bitmap amberFill = renderer.Render(plateTest);
            if (amberFill.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                    == greenOutline.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                || amberFill.GetPixel(plateEdge.X, plateEdge.Y).ToArgb() != stableOutlinePixel.ToArgb())
                throw new InvalidDataException("Plate fill color is not isolated from the outline ring.");

            plateTest.PlateOutlineWidth = 0;
            plateTest.PlateFillColor = Color.Transparent;
            plateTest.GlowColor = Color.FromArgb(255, 255, 20, 80);
            plateTest.GlowStrength = 100;
            plateTest.GlowRadius = 4;
            using Bitmap noPlateGlow = renderer.Render(plateTest);
            plateTest.GlowEnabled = true;
            using Bitmap outsidePlateGlow = renderer.Render(plateTest);
            Point glowPoint = new(plateTestRectangle.Left - 2,
                plateTestRectangle.Top + plateTestRectangle.Height / 2);
            if (noPlateGlow.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                    != outsidePlateGlow.GetPixel(plateCenter.X, plateCenter.Y).ToArgb()
                || noPlateGlow.GetPixel(glowPoint.X, glowPoint.Y).ToArgb()
                    == outsidePlateGlow.GetPixel(glowPoint.X, glowPoint.Y).ToArgb())
                throw new InvalidDataException("Plate glow is not isolated to the outside edge.");

            KeyboardDocument ribbonDocument = KeyboardDocument.Load(layoutPath);
            KeyboardKey ribbonKey = ribbonDocument.Keys.Single(key => key.Character == ' ');
            renderer.Theme = KeyboardTheme.BuiltIns.Single(theme => theme.Name == "Parchment");
            RectangleF ribbonBounds = renderer.KeyContentRectangle(ribbonDocument, ribbonKey);
            bool foundRibbonPixel = false;
            for (float y = ribbonBounds.Top; y < ribbonBounds.Bottom && !foundRibbonPixel; y += 1f)
                for (float x = ribbonBounds.Left; x < ribbonBounds.Right; x += 1f)
                    if (renderer.IsPointOnKeyContent(ribbonDocument, ribbonKey, new PointF(x, y))) { foundRibbonPixel = true; break; }
            if (!foundRibbonPixel)
                throw new InvalidDataException("The Parchment ribbon cannot be selected as key content.");
            ribbonDocument.ParchmentRibbonEnabled = false;
            if (renderer.IsPointOnKeyContent(ribbonDocument, ribbonKey,
                    new PointF(ribbonBounds.Left + ribbonBounds.Width / 2f, ribbonBounds.Top + ribbonBounds.Height / 2f)))
                throw new InvalidDataException("The removed Parchment ribbon retained a content hit target.");
            renderer.Theme = KeyboardTheme.BuiltIns.Single(theme => theme.Name == "Modern Green");

            KeyboardKey letterKey = loaded.Keys.First(key => key.Label.Length == 1 && char.IsAsciiLetter(key.Label[0]));
            RectangleF letterPlate = renderer.KeyRectangle(loaded, letterKey);
            RectangleF letterInk = renderer.KeyContentRectangle(loaded, letterKey);
            if (letterInk.Width >= letterPlate.Width * 0.75f || letterInk.Height >= letterPlate.Height * 0.85f)
                throw new InvalidDataException("Single-letter glyph selection still occupies most of its key plate.");
            if (renderer.IsPointOnKeyContent(loaded, letterKey, new PointF(letterPlate.Left + 2, letterPlate.Top + 2)))
                throw new InvalidDataException("Empty key-plate space incorrectly hit the glyph content.");
            bool foundPaintedGlyphPixel = false;
            for (float y = letterInk.Top; y < letterInk.Bottom && !foundPaintedGlyphPixel; y += 1f)
                for (float x = letterInk.Left; x < letterInk.Right; x += 1f)
                    if (renderer.IsPointOnKeyContent(loaded, letterKey, new PointF(x, y))) { foundPaintedGlyphPixel = true; break; }
            if (!foundPaintedGlyphPixel)
                throw new InvalidDataException("Painted glyph pixels were not selectable.");
            RectangleF arrowPlate = renderer.KeyRectangle(loaded, loadedUpArrow);
            RectangleF arrowContent = renderer.KeyContentRectangle(loaded, loadedUpArrow);
            if (Math.Abs(arrowContent.Width - 10f) > 0.001f || Math.Abs(arrowContent.Height - 8f) > 0.001f
                || Math.Abs((arrowContent.Left + arrowContent.Width / 2f) - (arrowPlate.Left + arrowPlate.Width / 2f + 9f)) > 0.001f
                || Math.Abs((arrowContent.Top + arrowContent.Height / 2f) - (arrowPlate.Top + arrowPlate.Height / 2f - 6f)) > 0.001f)
                throw new InvalidDataException("Arrow-key content did not apply its authored move and scale transform.");
            RectangleF textBar = renderer.TopElementRectangle(loaded, KeyboardTopElement.TextBar);
            RectangleF modeButton = renderer.TopElementRectangle(loaded, KeyboardTopElement.Mode);
            RectangleF lockButton = renderer.TopElementRectangle(loaded, KeyboardTopElement.Lock);
            if (Math.Abs(textBar.Left - (KeyboardRenderer.MarginHorizontal + 14)) > 0.001f
                || Math.Abs(textBar.Top - (KeyboardRenderer.GrabBarHeight + KeyboardRenderer.MarginTop - 7)) > 0.001f
                || Math.Abs(textBar.Width - 700) > 0.001f || Math.Abs(textBar.Height - 50) > 0.001f
                || Math.Abs(modeButton.Left - (KeyboardRenderer.MarginHorizontal + 19)) > 0.001f
                || Math.Abs(modeButton.Top - (KeyboardRenderer.GrabBarHeight + KeyboardRenderer.MarginTop - 36 + 8)) > 0.001f
                || Math.Abs(modeButton.Width - 240) > 0.001f || Math.Abs(modeButton.Height - 40) > 0.001f
                || Math.Abs(lockButton.Left - (KeyboardRenderer.TextureWidth - KeyboardRenderer.MarginHorizontal - 120 - 11)) > 0.001f
                || Math.Abs(lockButton.Top - (KeyboardRenderer.GrabBarHeight + KeyboardRenderer.MarginTop - 36 + 6)) > 0.001f
                || Math.Abs(lockButton.Width - 140) > 0.001f || Math.Abs(lockButton.Height - 36) > 0.001f
                || Math.Abs(renderer.TopElementFontScale(loaded, KeyboardTopElement.TextBar) - 0.8f) > 0.001f
                || Math.Abs(renderer.TopElementFontScale(loaded, KeyboardTopElement.Mode) - 0.9f) > 0.001f
                || Math.Abs(renderer.TopElementFontScale(loaded, KeyboardTopElement.Lock) - 1.1f) > 0.001f)
                throw new InvalidDataException("Movable/resizable top-bar geometry did not survive round trip.");
            RectangleF modeArtwork = renderer.TopStateArtworkRectangle(loaded, KeyboardTopElement.Mode);
            RectangleF lockArtwork = renderer.TopStateArtworkRectangle(loaded, KeyboardTopElement.Lock);
            if (Math.Abs(modeArtwork.Left - (modeButton.Left + 7)) > 0.001f
                || Math.Abs(modeArtwork.Top - (modeButton.Top - 4)) > 0.001f
                || Math.Abs(modeArtwork.Width - 218) > 0.001f || Math.Abs(modeArtwork.Height - 34) > 0.001f
                || Math.Abs(lockArtwork.Left - (lockButton.Left - 5)) > 0.001f
                || Math.Abs(lockArtwork.Top - (lockButton.Top + 2)) > 0.001f
                || Math.Abs(lockArtwork.Width - 126) > 0.001f || Math.Abs(lockArtwork.Height - 30) > 0.001f)
                throw new InvalidDataException("Top-state artwork did not keep its independent transform.");
            using (var layerSelectionCanvas = new KeyboardCanvas { Renderer = renderer, Document = loaded })
            {
                layerSelectionCanvas.SelectTopInteractionBox(KeyboardTopElement.Mode);
                if (layerSelectionCanvas.SelectionKind != CanvasSelectionKind.TopMode)
                    throw new InvalidDataException("Mode interaction-box layer could not be selected explicitly.");
                if (!layerSelectionCanvas.SelectTopStateArtwork(KeyboardTopElement.Mode)
                    || layerSelectionCanvas.SelectionKind != CanvasSelectionKind.TopModeArtwork)
                    throw new InvalidDataException("Mode state-image layer could not take selection authority.");
                if (!layerSelectionCanvas.SelectTopText(KeyboardTopElement.Mode)
                    || layerSelectionCanvas.SelectionKind != CanvasSelectionKind.TopModeText)
                    throw new InvalidDataException("Mode text layer could not take selection authority.");
                layerSelectionCanvas.SelectTopInteractionBox(KeyboardTopElement.Lock);
                if (!layerSelectionCanvas.SelectTopStateArtwork(KeyboardTopElement.Lock)
                    || layerSelectionCanvas.SelectionKind != CanvasSelectionKind.TopLockArtwork)
                    throw new InvalidDataException("Lock state-image layer could not take selection authority.");
                if (!layerSelectionCanvas.SelectTopText(KeyboardTopElement.Lock)
                    || layerSelectionCanvas.SelectionKind != CanvasSelectionKind.TopLockText)
                    throw new InvalidDataException("Lock text layer could not take selection authority.");
            }
            KeyboardDocument independentTopLayers = loaded.Clone();
            using (var geometryCanvas = new KeyboardCanvas { Renderer = renderer, Document = independentTopLayers })
            {
                RectangleF originalButton = renderer.TopElementRectangle(independentTopLayers, KeyboardTopElement.Mode);
                RectangleF originalArtwork = renderer.TopStateArtworkRectangle(independentTopLayers, KeyboardTopElement.Mode);
                RectangleF originalText = renderer.TopElementContentRectangle(independentTopLayers, KeyboardTopElement.Mode);
                geometryCanvas.SetTopInteractionSizePreservingVisuals(KeyboardTopElement.Mode,
                    originalButton.Width + 80, originalButton.Height + 20);
                RectangleF separatedArtwork = renderer.TopStateArtworkRectangle(independentTopLayers, KeyboardTopElement.Mode);
                RectangleF separatedText = renderer.TopElementContentRectangle(independentTopLayers, KeyboardTopElement.Mode);
                if (Math.Abs(separatedArtwork.Left - originalArtwork.Left) > 0.01f
                    || Math.Abs(separatedArtwork.Top - originalArtwork.Top) > 0.01f
                    || Math.Abs(separatedArtwork.Width - originalArtwork.Width) > 0.01f
                    || Math.Abs(separatedArtwork.Height - originalArtwork.Height) > 0.01f
                    || Math.Abs((separatedText.Left + separatedText.Width / 2f)
                        - (originalText.Left + originalText.Width / 2f)) > 0.01f
                    || Math.Abs((separatedText.Top + separatedText.Height / 2f)
                        - (originalText.Top + originalText.Height / 2f)) > 0.01f)
                    throw new InvalidDataException("Resizing a top hit box stretched or moved its independent artwork/text.");
                if (!geometryCanvas.FitTopArtworkToInteractionBox(KeyboardTopElement.Mode))
                    throw new InvalidDataException("State image could not be fitted to its hit box.");
                RectangleF fittedButton = renderer.TopElementRectangle(independentTopLayers, KeyboardTopElement.Mode);
                RectangleF fittedArtwork = renderer.TopStateArtworkRectangle(independentTopLayers, KeyboardTopElement.Mode);
                if (Math.Abs(fittedArtwork.Left - fittedButton.Left) > 0.01f
                    || Math.Abs(fittedArtwork.Top - fittedButton.Top) > 0.01f
                    || Math.Abs(fittedArtwork.Width - fittedButton.Width) > 0.01f
                    || Math.Abs(fittedArtwork.Height - fittedButton.Height) > 0.01f)
                    throw new InvalidDataException("Fit image to box did not align both rectangles.");
                geometryCanvas.SetTopInteractionSizePreservingVisuals(KeyboardTopElement.Mode,
                    fittedButton.Width + 40, fittedButton.Height + 10);
                RectangleF detachedAfterFit = renderer.TopStateArtworkRectangle(independentTopLayers, KeyboardTopElement.Mode);
                if (Math.Abs(detachedAfterFit.Width - fittedArtwork.Width) > 0.01f
                    || Math.Abs(detachedAfterFit.Height - fittedArtwork.Height) > 0.01f)
                    throw new InvalidDataException("A fitted state image remained implicitly stretched with the hit box.");
                if (!geometryCanvas.FitTopInteractionBoxToArtwork(KeyboardTopElement.Mode))
                    throw new InvalidDataException("Hit box could not be fitted back to its state image.");
                RectangleF finalButton = renderer.TopElementRectangle(independentTopLayers, KeyboardTopElement.Mode);
                RectangleF finalArtwork = renderer.TopStateArtworkRectangle(independentTopLayers, KeyboardTopElement.Mode);
                if (Math.Abs(finalArtwork.Left - finalButton.Left) > 0.01f
                    || Math.Abs(finalArtwork.Top - finalButton.Top) > 0.01f
                    || Math.Abs(finalArtwork.Width - finalButton.Width) > 0.01f
                    || Math.Abs(finalArtwork.Height - finalButton.Height) > 0.01f)
                    throw new InvalidDataException("Fit box to image did not align both rectangles.");
            }
            KeyboardDocument centeredTopText = loaded.Clone();
            centeredTopText.ModeTextOffsetX = centeredTopText.ModeTextOffsetY = 0;
            centeredTopText.LockTextOffsetX = centeredTopText.LockTextOffsetY = 0;
            RectangleF centeredModeText = renderer.TopElementContentRectangle(centeredTopText, KeyboardTopElement.Mode);
            RectangleF movedModeText = renderer.TopElementContentRectangle(loaded, KeyboardTopElement.Mode);
            RectangleF centeredLockText = renderer.TopElementContentRectangle(centeredTopText, KeyboardTopElement.Lock);
            RectangleF movedLockText = renderer.TopElementContentRectangle(loaded, KeyboardTopElement.Lock);
            if (Math.Abs((movedModeText.Left - centeredModeText.Left) + 9) > 0.01f
                || Math.Abs((movedModeText.Top - centeredModeText.Top) - 3) > 0.01f
                || Math.Abs((movedLockText.Left - centeredLockText.Left) - 6) > 0.01f
                || Math.Abs((movedLockText.Top - centeredLockText.Top) + 2) > 0.01f)
                throw new InvalidDataException("Top-state text did not keep its independent offset.");
            KeyboardDocument visibleTopPlates = loaded.Clone();
            visibleTopPlates.TopButtonPlatesEnabled = true;
            visibleTopPlates.InputBarPlateEnabled = true;
            renderer.AnimationTimeSeconds = 0;
            using Bitmap hiddenTopPlateFrame = renderer.Render(loaded);
            using Bitmap visibleTopPlateFrame = renderer.Render(visibleTopPlates);
            using var hiddenTopPlateBytes = new MemoryStream();
            using var visibleTopPlateBytes = new MemoryStream();
            hiddenTopPlateFrame.Save(hiddenTopPlateBytes, System.Drawing.Imaging.ImageFormat.Png);
            visibleTopPlateFrame.Save(visibleTopPlateBytes, System.Drawing.Imaging.ImageFormat.Png);
            if (hiddenTopPlateBytes.ToArray().SequenceEqual(visibleTopPlateBytes.ToArray()))
                throw new InvalidDataException("Independent top-button/input-bar plate visibility did not change rendering.");
            RectangleF sizeGroup = renderer.RuntimeControlRectangle(loaded, KeyboardRuntimeControl.Size);
            RectangleF sizeUp = renderer.RuntimeControlPartRectangle(loaded, KeyboardRuntimeControl.Size, KeyboardControlPart.UpArrow);
            RectangleF sizeDown = renderer.RuntimeControlPartRectangle(loaded, KeyboardRuntimeControl.Size, KeyboardControlPart.DownArrow);
            if (Math.Abs(sizeGroup.Width - 128) > 0.001f || Math.Abs(sizeGroup.Height - 116) > 0.001f
                || Math.Abs(sizeUp.Width - 31) > 0.001f || Math.Abs(sizeUp.Height - 23) > 0.001f
                || Math.Abs(sizeDown.Width - 29) > 0.001f || Math.Abs(sizeDown.Height - 19) > 0.001f
                || Math.Abs(sizeUp.Left - (sizeGroup.Left + (128 - 31) / 2f + 11)) > 0.001f
                || Math.Abs(sizeDown.Bottom - (sizeGroup.Bottom + 5)) > 0.001f)
                throw new InvalidDataException("Runtime control child geometry does not match the authored nested boxes.");
            RectangleF sizeLabel = renderer.RuntimeControlPartRectangle(loaded, KeyboardRuntimeControl.Size, KeyboardControlPart.Label);
            bool foundControlGlyphPixel = false;
            bool foundControlGlyphGap = false;
            for (float y = sizeLabel.Top; y < sizeLabel.Bottom; y += 0.5f)
            {
                for (float x = sizeLabel.Left; x < sizeLabel.Right; x += 0.5f)
                {
                    bool hit = renderer.IsPointOnRuntimeControlText(loaded, KeyboardRuntimeControl.Size, true, new PointF(x, y));
                    foundControlGlyphPixel |= hit;
                    foundControlGlyphGap |= !hit;
                }
            }
            if (!foundControlGlyphPixel || !foundControlGlyphGap)
                throw new InvalidDataException("Runtime control font selection is not restricted to painted glyph pixels.");
            renderer.AnimationTimeSeconds = 0;
            using Bitmap preview = renderer.Render(loaded);
            if (preview.Width != 1024 || preview.Height != 560)
                throw new InvalidDataException("Preview dimensions are not OCU's 1024x560 texture dimensions.");
            if (preview.GetPixel((int)loaded.BackgroundX, (int)loaded.BackgroundY).A != 0)
                throw new InvalidDataException("Rounded background corner did not produce a transparent mask.");
            byte pillEdgeAlpha = preview.GetPixel((int)loaded.BackgroundX + 31, (int)loaded.BackgroundY + 31).A;
            byte pillInnerAlpha = preview.GetPixel((int)loaded.BackgroundX + 45, (int)loaded.BackgroundY + 45).A;
            if (pillEdgeAlpha >= pillInnerAlpha)
                throw new InvalidDataException("Background feathering does not begin at the rounded pill boundary.");
            preview.Save(Path.Combine(outputDirectory, "keyboard-studio-preview.png"), System.Drawing.Imaging.ImageFormat.Png);
            renderer.AnimationTimeSeconds = 1;
            using Bitmap animatedPreview = renderer.Render(loaded);
            using var frameA = new MemoryStream();
            using var frameB = new MemoryStream();
            preview.Save(frameA, System.Drawing.Imaging.ImageFormat.Png);
            animatedPreview.Save(frameB, System.Drawing.Imaging.ImageFormat.Png);
            if (frameA.ToArray().SequenceEqual(frameB.ToArray()))
                throw new InvalidDataException("Breathing animation did not change the rendered keyboard frame.");
            animatedPreview.Save(Path.Combine(outputDirectory, "keyboard-studio-preview-breathing.png"), System.Drawing.Imaging.ImageFormat.Png);

            string highDpiBackground = Path.Combine(outputDirectory, "self-test-300dpi-background.png");
            using (var highDpiSource = new Bitmap(320, 180, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
            {
                highDpiSource.SetResolution(300, 300);
                using Graphics graphics = Graphics.FromImage(highDpiSource);
                graphics.Clear(Color.FromArgb(255, 37, 83, 149));
                highDpiSource.Save(highDpiBackground, System.Drawing.Imaging.ImageFormat.Png);
            }
            using (var savedHighDpiSource = new Bitmap(highDpiBackground))
            {
                if (savedHighDpiSource.HorizontalResolution < 250)
                    throw new InvalidDataException("The high-DPI artwork regression fixture lost its DPI metadata.");
            }
            var highDpiDocument = new KeyboardDocument
            {
                BackgroundImagePath = highDpiBackground,
                BackgroundFileName = "OCUKeyboardBackground.png",
                BackgroundX = 0,
                BackgroundY = 0,
                BackgroundWidth = KeyboardRenderer.TextureWidth,
                BackgroundHeight = KeyboardRenderer.TextureHeight,
                BackgroundOpacity = 100,
                BackgroundEdgeFade = 0,
                BackgroundRotation = 0,
                BackgroundRoundness = 0
            };
            using (Bitmap highDpiPreview = renderer.Render(highDpiDocument))
            {
                Color farPixel = highDpiPreview.GetPixel(800, 400);
                if (farPixel.A != 255 || farPixel.R != 37 || farPixel.G != 83 || farPixel.B != 149)
                    throw new InvalidDataException("High-DPI background artwork was shrunk into the upper-left corner.");
            }

            string windowsFonts = Environment.GetFolderPath(Environment.SpecialFolder.Fonts);
            string systemTtf = new[] { "segoeui.ttf", "arial.ttf", "tahoma.ttf" }
                .Select(name => Path.Combine(windowsFonts, name)).FirstOrDefault(File.Exists)
                ?? throw new FileNotFoundException("A Windows TTF was not available for the import self-test.");
            var fontDocument = new KeyboardDocument();
            fontDocument.Keys.Add(new KeyboardKey { Id = 1, Label = "AB", ShiftLabel = "C" });
            fontDocument.Keys.Add(new KeyboardKey { Id = 2, Label = "1", ShiftLabel = "" });
            var nordicFallback = new SudoFont(metadata, texture);
            FontImportResult imported = FontImporter.Import(systemTtf, fontDocument, outputDirectory, nordicFallback);
            var importedFont = new SudoFont(imported.MetadataPath, imported.TexturePath);
            char[] expectedImportedCharacters = fontDocument.RequiredFontCharacters().ToArray();
            if (!importedFont.Characters.Order().SequenceEqual(expectedImportedCharacters.Order()))
                throw new InvalidDataException("TTF conversion audited characters outside the existing keyboard keys.");
            if (importedFont.ContainsGlyph('Z'))
                throw new InvalidDataException("TTF conversion included a hypothetical key that is not in the keyboard.");
            if (!importedFont.TryGetGlyphRaster('A', out FontGlyphRaster importedA)
                || importedA.Alpha.Length == 0 || !importedA.Alpha.Any(alpha => alpha > 0))
                throw new InvalidDataException("TTF conversion produced a blank raster for an existing key glyph.");
            fontDocument.Keys.Add(new KeyboardKey { Id = 3, Label = "Z", ShiftLabel = "" });
            FontImportResult rebuilt = FontImporter.Import(imported.SourceFontPath, fontDocument, outputDirectory,
                nordicFallback, imported.ConfigName);
            var rebuiltFont = new SudoFont(rebuilt.MetadataPath, rebuilt.TexturePath);
            if (!rebuiltFont.ContainsGlyph('Z'))
                throw new InvalidDataException("Adding key text did not rebuild the converted font with the new glyph.");
            fontDocument.FontName = rebuilt.ConfigName;
            fontDocument.CustomFontMetadataPath = rebuilt.MetadataPath;
            fontDocument.CustomFontTexturePath = rebuilt.TexturePath;
            string customFontArchive = Path.Combine(outputDirectory, "keyboard-studio-custom-font-mod.zip");
            Mo2ModExporter.Export(customFontArchive, fontDocument);
            using (var archive = System.IO.Compression.ZipFile.OpenRead(customFontArchive))
            {
                if (archive.GetEntry("root/OCUKeyboardFont.sfn") is null
                    || archive.GetEntry("root/OCUKeyboardFont.png") is null)
                    throw new InvalidDataException("Custom font export did not carry its generated SFN and texture atlas.");
            }

            KeyboardDocument keyOnlyBreathing = loaded.Clone();
            keyOnlyBreathing.KeyPlatesEnabled = true;
            keyOnlyBreathing.FontBreatheEnabled = false;
            keyOnlyBreathing.BackgroundBreatheEnabled = false;
            keyOnlyBreathing.ControlArrowBreatheEnabled = false;
            foreach (KeyboardSprite sprite in keyOnlyBreathing.Sprites)
                sprite.BreatheEnabled = false;
            renderer.AnimationTimeSeconds = 0;
            using Bitmap keyFrameA = renderer.Render(keyOnlyBreathing);
            renderer.AnimationTimeSeconds = 1.25;
            using Bitmap keyFrameB = renderer.Render(keyOnlyBreathing);
            using var keyBytesA = new MemoryStream();
            using var keyBytesB = new MemoryStream();
            keyFrameA.Save(keyBytesA, System.Drawing.Imaging.ImageFormat.Png);
            keyFrameB.Save(keyBytesB, System.Drawing.Imaging.ImageFormat.Png);
            if (keyBytesA.ToArray().SequenceEqual(keyBytesB.ToArray()))
                throw new InvalidDataException("Breathing key glow did not change the rendered keyboard frame.");

            KeyboardDocument fontOnlyBreathing = loaded.Clone();
            fontOnlyBreathing.KeyBreatheEnabled = false;
            fontOnlyBreathing.BackgroundBreatheEnabled = false;
            fontOnlyBreathing.ControlArrowBreatheEnabled = false;
            foreach (KeyboardSprite sprite in fontOnlyBreathing.Sprites)
                sprite.BreatheEnabled = false;
            renderer.AnimationTimeSeconds = 0;
            using Bitmap fontFrameA = renderer.Render(fontOnlyBreathing);
            renderer.AnimationTimeSeconds = 1.75;
            using Bitmap fontFrameB = renderer.Render(fontOnlyBreathing);
            using var fontBytesA = new MemoryStream();
            using var fontBytesB = new MemoryStream();
            fontFrameA.Save(fontBytesA, System.Drawing.Imaging.ImageFormat.Png);
            fontFrameB.Save(fontBytesB, System.Drawing.Imaging.ImageFormat.Png);
            if (fontBytesA.ToArray().SequenceEqual(fontBytesB.ToArray()))
                throw new InvalidDataException("Breathing font glow did not change the rendered keyboard frame.");

            var animationWatch = System.Diagnostics.Stopwatch.StartNew();
            for (int frame = 0; frame < 20; frame++)
            {
                renderer.AnimationTimeSeconds = frame / 20.0;
                using Bitmap animationFrame = renderer.Render(loaded);
            }
            animationWatch.Stop();
            double averageAnimationFrameMs = animationWatch.Elapsed.TotalMilliseconds / 20.0;

            string jpegBackground = Path.Combine(outputDirectory, "self-test-background.jpg");
            using (var sourceBackground = new Bitmap(Path.Combine(assets, "skyui-bg.png")))
                sourceBackground.Save(jpegBackground, System.Drawing.Imaging.ImageFormat.Jpeg);
            loaded.BackgroundImagePath = jpegBackground;

            string modArchive = Path.Combine(outputDirectory, "keyboard-studio-mo2-mod.zip");
            Mo2ModExporter.Export(modArchive, loaded);
            using (var archive = System.IO.Compression.ZipFile.OpenRead(modArchive))
            {
                if (archive.GetEntry("root/OCUKeyboard.kb") is null
                    || archive.GetEntry("root/OCUKeyboardBackground.png") is null
                    || archive.GetEntry($"root/{KeyboardDocument.ConsoleInputBackgroundPortableName}") is null
                    || archive.GetEntry($"root/{KeyboardDocument.ModeVrArtworkPortableName}") is null
                    || archive.GetEntry($"root/{KeyboardDocument.ModePcArtworkPortableName}") is null
                    || archive.GetEntry($"root/{KeyboardDocument.LockWorldArtworkPortableName}") is null
                    || archive.GetEntry($"root/{KeyboardDocument.LockHeadArtworkPortableName}") is null
                    || archive.GetEntry("root/OCUKeyboardSprite01.png") is null
                    || archive.GetEntry("root/OCUKeyboardSprite02.png") is null
                    || archive.GetEntry("root/OCUKeyboardControlArrow.png") is null
                    || archive.GetEntry("OCU Keyboard README.txt") is null)
                    throw new InvalidDataException("MO2 archive is missing its layout, artwork, or README entry.");
                using Stream background = archive.GetEntry("root/OCUKeyboardBackground.png")!.Open();
                byte[] signature = new byte[8];
                if (background.Read(signature, 0, signature.Length) != signature.Length
                    || !signature.SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }))
                    throw new InvalidDataException("JPEG background was not transcoded to a real PNG in the MO2 archive.");
            }

            string portablePackage = Path.Combine(outputDirectory, "keyboard-studio-portable.ocukb");
            KeyboardPackage.Export(portablePackage, loaded, "Portable Test Keyboard");
            using (var archive = System.IO.Compression.ZipFile.OpenRead(portablePackage))
            {
                if (archive.GetEntry("manifest.json") is null
                    || archive.GetEntry("keyboard.kb") is null
                    || archive.GetEntry("OCUKeyboardBackground.png") is null
                    || archive.GetEntry(KeyboardDocument.ConsoleInputBackgroundPortableName) is null
                    || archive.GetEntry(KeyboardDocument.ModeVrArtworkPortableName) is null
                    || archive.GetEntry(KeyboardDocument.ModePcArtworkPortableName) is null
                    || archive.GetEntry(KeyboardDocument.LockWorldArtworkPortableName) is null
                    || archive.GetEntry(KeyboardDocument.LockHeadArtworkPortableName) is null
                    || archive.GetEntry("OCUKeyboardSprite01.png") is null
                    || archive.GetEntry("OCUKeyboardSprite02.png") is null
                    || archive.GetEntry("OCUKeyboardControlArrow.png") is null)
                    throw new InvalidDataException("Portable .ocukb is missing its layout or artwork.");
            }
            string portableLibrary = Path.Combine(outputDirectory, "portable-design-library");
            KeyboardPackageImportResult importedPackage = KeyboardPackage.Import(portablePackage, portableLibrary);
            KeyboardDocument importedDocument = KeyboardDocument.Load(importedPackage.LayoutPath);
            if (importedPackage.DisplayName != "Portable Test Keyboard"
                || importedPackage.ArtworkCount != 9
                || importedDocument.Sprites.Count != 2
                || !importedDocument.InputFillOverrideEnabled
                || importedDocument.InputFillColor.ToArgb() != loaded.InputFillColor.ToArgb()
                || !importedDocument.InputOutlineOverrideEnabled
                || importedDocument.InputOutlineVisible
                || importedDocument.InputOutlineColor.ToArgb() != loaded.InputOutlineColor.ToArgb()
                || importedDocument.InputOutlineWidth != loaded.InputOutlineWidth
                || Math.Abs(importedDocument.InputTitleOffsetX - loaded.InputTitleOffsetX) > 0.001f
                || Math.Abs(importedDocument.InputTitleOffsetY - loaded.InputTitleOffsetY) > 0.001f
                || Math.Abs(importedDocument.InputTextOffsetX - loaded.InputTextOffsetX) > 0.001f
                || Math.Abs(importedDocument.InputTextOffsetY - loaded.InputTextOffsetY) > 0.001f
                || string.IsNullOrWhiteSpace(importedDocument.ConsoleInputBackgroundImagePath)
                || !File.Exists(importedDocument.ConsoleInputBackgroundImagePath)
                || string.IsNullOrWhiteSpace(importedDocument.ModeVrArtworkImagePath)
                || !File.Exists(importedDocument.ModeVrArtworkImagePath)
                || string.IsNullOrWhiteSpace(importedDocument.ModePcArtworkImagePath)
                || !File.Exists(importedDocument.ModePcArtworkImagePath)
                || string.IsNullOrWhiteSpace(importedDocument.LockWorldArtworkImagePath)
                || !File.Exists(importedDocument.LockWorldArtworkImagePath)
                || string.IsNullOrWhiteSpace(importedDocument.LockHeadArtworkImagePath)
                || !File.Exists(importedDocument.LockHeadArtworkImagePath)
                || !importedDocument.ModeTextOverArtwork || !importedDocument.LockTextOverArtwork
                || importedDocument.ModeArtworkOffsetX != loaded.ModeArtworkOffsetX
                || importedDocument.ModeArtworkOffsetY != loaded.ModeArtworkOffsetY
                || importedDocument.ModeArtworkWidth != loaded.ModeArtworkWidth
                || importedDocument.ModeArtworkHeight != loaded.ModeArtworkHeight
                || importedDocument.ModeTextOffsetX != loaded.ModeTextOffsetX
                || importedDocument.ModeTextOffsetY != loaded.ModeTextOffsetY
                || importedDocument.LockArtworkOffsetX != loaded.LockArtworkOffsetX
                || importedDocument.LockArtworkOffsetY != loaded.LockArtworkOffsetY
                || importedDocument.LockArtworkWidth != loaded.LockArtworkWidth
                || importedDocument.LockArtworkHeight != loaded.LockArtworkHeight
                || importedDocument.LockTextOffsetX != loaded.LockTextOffsetX
                || importedDocument.LockTextOffsetY != loaded.LockTextOffsetY
                || string.IsNullOrWhiteSpace(importedDocument.BackgroundImagePath)
                || !File.Exists(importedDocument.BackgroundImagePath)
                || importedDocument.Sprites.Any(sprite => string.IsNullOrWhiteSpace(sprite.SourcePath) || !File.Exists(sprite.SourcePath))
                || string.IsNullOrWhiteSpace(importedDocument.ControlArrowImagePath)
                || !File.Exists(importedDocument.ControlArrowImagePath))
                throw new InvalidDataException("Portable .ocukb did not round-trip its layout and artwork.");

            string firstImportedLayout = importedPackage.LayoutPath;
            KeyboardDocument updatedPackageDocument = importedDocument.Clone();
            updatedPackageDocument.FontGlowStrength = importedDocument.FontGlowStrength == 100
                ? 99 : importedDocument.FontGlowStrength + 1;
            KeyboardPackage.Export(portablePackage, updatedPackageDocument, "Portable Test Keyboard");
            KeyboardPackageImportResult refreshedPackage = KeyboardPackage.Import(portablePackage, portableLibrary);
            KeyboardDocument refreshedDocument = KeyboardDocument.Load(refreshedPackage.LayoutPath);
            if (!refreshedPackage.LayoutPath.Equals(firstImportedLayout, StringComparison.OrdinalIgnoreCase)
                || refreshedDocument.FontGlowStrength != updatedPackageDocument.FontGlowStrength)
                throw new InvalidDataException("Re-saving one .ocukb did not refresh its existing managed design.");

            string unsafePackage = Path.Combine(outputDirectory, "keyboard-studio-unsafe.ocukb");
            using (var file = new FileStream(unsafePackage, FileMode.Create, FileAccess.Write, FileShare.None))
            using (var archive = new System.IO.Compression.ZipArchive(file, System.IO.Compression.ZipArchiveMode.Create))
            {
                void WriteUnsafeEntry(string name, string contents)
                {
                    var entry = archive.CreateEntry(name);
                    using var writer = new StreamWriter(entry.Open(), new System.Text.UTF8Encoding(false));
                    writer.Write(contents);
                }
                WriteUnsafeEntry("manifest.json", "{\"formatVersion\":1,\"name\":\"Unsafe\",\"layout\":\"keyboard.kb\"}");
                WriteUnsafeEntry("keyboard.kb", loaded.Serialize());
                WriteUnsafeEntry("../escaped.png", "not an image");
            }
            bool traversalRejected = false;
            try
            {
                KeyboardPackage.Import(unsafePackage, Path.Combine(outputDirectory, "unsafe-import-library"));
            }
            catch (InvalidDataException)
            {
                traversalRejected = true;
            }
            if (!traversalRejected || File.Exists(Path.Combine(outputDirectory, "escaped.png")))
                throw new InvalidDataException("Portable package traversal protection failed.");

            KeyboardTheme dwemerTheme = KeyboardTheme.BuiltIns.Single(theme => theme.Name == "Dwemer");
            var stockStyle = new KeyboardDocument();
            stockStyle.EnableCustomStyleFromTheme(dwemerTheme);
            if (!stockStyle.CustomStyleEnabled || !stockStyle.CustomStyleInitialized
                || stockStyle.FontColor != dwemerTheme.Ink
                || stockStyle.KeyColor != dwemerTheme.Accent
                || stockStyle.PlateFillColor != dwemerTheme.KeyIdle
                || stockStyle.GlowColor != dwemerTheme.Bright
                || stockStyle.HoverColor.R != dwemerTheme.KeyHot.R
                || stockStyle.HoverColor.G != dwemerTheme.KeyHot.G
                || stockStyle.HoverColor.B != dwemerTheme.KeyHot.B
                || stockStyle.KeyRoundness != 2
                || stockStyle.PlateOutlineWidth != 1)
                throw new InvalidDataException("Enabling custom colors did not seed the visible Dwemer theme values.");
            Color rememberedDwemerFont = Color.Fuchsia;
            stockStyle.FontColor = rememberedDwemerFont;
            stockStyle.CustomStyleEnabled = false;
            stockStyle.EnableCustomStyleFromTheme(dwemerTheme);
            if (stockStyle.FontColor != rememberedDwemerFont)
                throw new InvalidDataException("Re-enabling custom colors discarded the user's prior swatches.");

            string stockDwemerLayout = Path.Combine(assets, "Stock Designs", "Dwemer", "Dwemer.kb");
            if (!File.Exists(stockDwemerLayout))
                throw new FileNotFoundException("The stock Dwemer layout is missing.", stockDwemerLayout);
            KeyboardDocument stockDwemerDocument = KeyboardDocument.Load(stockDwemerLayout);
            if (!stockDwemerDocument.BaseTheme.Equals("dwemer", StringComparison.OrdinalIgnoreCase)
                || !stockDwemerDocument.CustomStyleEnabled
                || stockDwemerDocument.Keys.Count < 50)
                throw new InvalidDataException("The bundled Dwemer design is not the authored stock layout.");
            RectangleF dwemerSize = renderer.RuntimeControlRectangle(stockDwemerDocument, KeyboardRuntimeControl.Size);
            RectangleF dwemerOpacity = renderer.RuntimeControlRectangle(stockDwemerDocument, KeyboardRuntimeControl.Opacity);
            RectangleF dwemerTilt = renderer.RuntimeControlRectangle(stockDwemerDocument, KeyboardRuntimeControl.Tilt);
            if (dwemerSize != new RectangleF(38, 252, 92, 98)
                || dwemerOpacity != new RectangleF(923.071f, 87.894f, 89.929f, 101.106f)
                || dwemerTilt != new RectangleF(922, 325, 92, 98))
                throw new InvalidDataException("The bundled Dwemer controls do not match their authored runtime slots.");
            KeyboardControlDesign dwemerSizeDesign = stockDwemerDocument.GetControlDesign(KeyboardRuntimeControl.Size);
            KeyboardControlDesign dwemerOpacityDesign = stockDwemerDocument.GetControlDesign(KeyboardRuntimeControl.Opacity);
            KeyboardControlDesign dwemerTiltDesign = stockDwemerDocument.GetControlDesign(KeyboardRuntimeControl.Tilt);
            if (dwemerSizeDesign.LabelOffsetY != 0 || dwemerSizeDesign.ValueOffsetX != 0 || dwemerSizeDesign.ValueOffsetY != 4.142f
                || dwemerOpacityDesign.UpOffsetX != 0 || dwemerOpacityDesign.UpOffsetY != -15.531f
                || dwemerOpacityDesign.DownOffsetX != 0 || dwemerOpacityDesign.DownOffsetY != 0
                || dwemerOpacityDesign.LabelOffsetX != 1.035f || dwemerOpacityDesign.LabelOffsetY != -4.142f
                || dwemerOpacityDesign.ValueOffsetX != 1.035f || dwemerOpacityDesign.ValueOffsetY != 4.142f
                || dwemerTiltDesign.ValueOffsetX != 0 || dwemerTiltDesign.ValueOffsetY != 7.248f)
                throw new InvalidDataException("The bundled Dwemer arrows and text do not match their authored internal spacing.");
            var themeSwitchedDocument = new KeyboardDocument();
            themeSwitchedDocument.ApplyThemeControlLayout(dwemerTheme);
            if (renderer.RuntimeControlRectangle(themeSwitchedDocument, KeyboardRuntimeControl.Size) != dwemerSize
                || renderer.RuntimeControlRectangle(themeSwitchedDocument, KeyboardRuntimeControl.Opacity) != dwemerOpacity
                || renderer.RuntimeControlRectangle(themeSwitchedDocument, KeyboardRuntimeControl.Tilt) != dwemerTilt
                || themeSwitchedDocument.SizeControlDesign.ValueOffsetY != dwemerSizeDesign.ValueOffsetY
                || themeSwitchedDocument.OpacityControlDesign.UpOffsetY != dwemerOpacityDesign.UpOffsetY
                || themeSwitchedDocument.OpacityControlDesign.LabelOffsetX != dwemerOpacityDesign.LabelOffsetX
                || themeSwitchedDocument.OpacityControlDesign.LabelOffsetY != dwemerOpacityDesign.LabelOffsetY
                || themeSwitchedDocument.OpacityControlDesign.ValueOffsetX != dwemerOpacityDesign.ValueOffsetX
                || themeSwitchedDocument.OpacityControlDesign.ValueOffsetY != dwemerOpacityDesign.ValueOffsetY
                || themeSwitchedDocument.TiltControlDesign.ValueOffsetY != dwemerTiltDesign.ValueOffsetY)
                throw new InvalidDataException("Switching to the Dwemer Base Theme did not apply the stock control geometry.");
            string dwemerBaseThemePath = Path.Combine(assets, "dwemer-bg.png");
            renderer.Theme = dwemerTheme;
            using (Bitmap dwemerBaseTheme = new(dwemerBaseThemePath))
            using (Bitmap authoredDwemerBackground = renderer.RenderBackgroundExact(stockDwemerDocument))
            {
                if (dwemerBaseTheme.Width != KeyboardRenderer.TextureWidth
                    || dwemerBaseTheme.Height != KeyboardRenderer.TextureHeight
                    || authoredDwemerBackground.Width != KeyboardRenderer.TextureWidth
                    || authoredDwemerBackground.Height != KeyboardRenderer.TextureHeight)
                    throw new InvalidDataException("The Dwemer Base Theme or stock preview is not the runtime 1024x560 size.");
            }

            string stockPugLayout = Path.Combine(assets, "Stock Designs", "PugDragonKeyboard", "PugDragonKeyboard.kb");
            KeyboardDocument stockPugDocument = KeyboardDocument.Load(stockPugLayout);
            if (string.IsNullOrWhiteSpace(stockPugDocument.BackgroundImagePath)
                || !File.Exists(stockPugDocument.BackgroundImagePath))
                throw new InvalidDataException("The bundled Pug Dragon design is missing its packaged background.");

            RectangleF normalizedMarquee = KeyboardCanvas.RectangleFromPoints(new PointF(90, 80), new PointF(10, 20));
            if (normalizedMarquee != new RectangleF(10, 20, 80, 60)
                || !KeyboardCanvas.MarqueeSelects(normalizedMarquee, new RectangleF(85, 75, 20, 20), requireContainment: false)
                || KeyboardCanvas.MarqueeSelects(normalizedMarquee, new RectangleF(85, 75, 20, 20), requireContainment: true)
                || !KeyboardCanvas.MarqueeSelects(normalizedMarquee, new RectangleF(20, 30, 10, 10), requireContainment: true))
                throw new InvalidDataException("Marquee selection intersection/containment behavior failed.");

            File.WriteAllText(Path.Combine(outputDirectory, "keyboard-studio-self-test.txt"),
                $"PASS\nKeys={loaded.Keys.Count}\nTheme=modern_green\nFont=ocu_nordic\nPreview=1024x560\nCustomStyle=True\nCustomStyleSeed=VisibleBaseTheme+RestorePriorValues\nStockDwemer=LatestAuthoredDesign\nStockPugDragon=BundledArtworkDesign\nDwemerBaseTheme=AuthoredStockBackground1024x560\nCanvasZoom=WheelZoomUnlessTextSelected+MiddlePan\nKeyPlates=False\nPlateLayers=Fill+OutlineRing+OutsideGlowIndependent\nParchmentRibbon=Selectable+Movable+Resizable+Removable\nOcuTarget=ContainingModRootAutoDetected\nTopPlates=ModeLock+InputBarIndependent\nTopStateArtwork=ModeVR+ModePC+LockWorld+LockHead+TextFallback+IndependentGeometry+BidirectionalFit\nArtwork=MovableBackground+2Sprites+ControlArrow\nControls=NestedSelectableBoxes\nControlFontHit=PaintedGlyphPixelsOnly\nControlArrowGlow=BuiltInOrPng+Breathing\nArrowKeys=IndependentMove+Resize\nTopBar=TextBar+Mode+LockMove+PlateResize+FontScale\nKeyPosition=NegativeGridCoordinatesSupported\nGridControl=ColumnCountDensity\nHistory=NoOpFiltered+VisualChangesDetected\nMarqueeSelection=VisibleItems+Additive+GroupMove\nDragPreview=Coalesced16ms+InspectorOnRelease\nBackgroundFeather=RoundedPillBoundary\nHighDpiArtwork=PixelSized\nTextEffects=OutlineColor+FontGlow+IndependentBreathing\nSpaceAdvance=MissingSFNGlyphFallback\nTTFImport=ExistingKeysOnly+NewKeyRebuild\nCustomFontExport=SFN+PNG\nBreathing=Keys+Font+Sprite+Arrow\nAnimationPreviewAverageMs={averageAnimationFrameMs:F2}\nSampling=PremultipliedBilinear\nJpegBackground=TranscodedToPng\nMO2Archive=root/OCUKeyboard.kb\nPortablePackage=FormatV1+.ocukb+ArtworkRoundTrip+StableResave+TraversalGuard\n");
            return 0;
        }
        catch (Exception exception)
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllText(Path.Combine(outputDirectory, "keyboard-studio-self-test.txt"), exception.ToString());
            return 1;
        }
    }
}
