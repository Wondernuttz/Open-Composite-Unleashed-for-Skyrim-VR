using System.IO.Compression;
using System.Text;

namespace OCUKeyboardStudio;

internal static class Mo2ModExporter
{
    public static void Export(string archivePath, KeyboardDocument document)
    {
        using var file = new FileStream(archivePath, FileMode.Create, FileAccess.Write, FileShare.None);
        using var archive = new ZipArchive(file, ZipArchiveMode.Create);

        ZipArchiveEntry layoutEntry = archive.CreateEntry("root/OCUKeyboard.kb", CompressionLevel.Optimal);
        using (var writer = new StreamWriter(layoutEntry.Open(), new UTF8Encoding(false)))
            writer.Write(document.Serialize());

        AddArtwork(archive, document.BackgroundImagePath, "root/OCUKeyboardBackground.png");
        AddArtwork(archive, document.ConsoleInputBackgroundImagePath,
            $"root/{KeyboardDocument.ConsoleInputBackgroundPortableName}");
        AddArtwork(archive, document.ModeVrArtworkImagePath,
            $"root/{KeyboardDocument.ModeVrArtworkPortableName}");
        AddArtwork(archive, document.ModePcArtworkImagePath,
            $"root/{KeyboardDocument.ModePcArtworkPortableName}");
        AddArtwork(archive, document.LockWorldArtworkImagePath,
            $"root/{KeyboardDocument.LockWorldArtworkPortableName}");
        AddArtwork(archive, document.LockHeadArtworkImagePath,
            $"root/{KeyboardDocument.LockHeadArtworkPortableName}");
        for (int index = 0; index < document.Sprites.Count; index++)
            AddArtwork(archive, document.Sprites[index].SourcePath,
                $"root/{KeyboardDocument.SpriteFileName(index)}");
        AddArtwork(archive, document.ControlArrowImagePath, "root/OCUKeyboardControlArrow.png");
        AddFile(archive, document.CustomFontMetadataPath, "root/OCUKeyboardFont.sfn");
        AddFile(archive, document.CustomFontTexturePath, "root/OCUKeyboardFont.png");

        ZipArchiveEntry readmeEntry = archive.CreateEntry("OCU Keyboard README.txt", CompressionLevel.Optimal);
        using var readme = new StreamWriter(readmeEntry.Open(), new UTF8Encoding(false));
        readme.WriteLine("OCU Custom Keyboard");
        readme.WriteLine("===================");
        readme.WriteLine();
        readme.WriteLine("Install this archive as its own mod in Mod Organizer 2 and enable it after Open Composite Unleashed.");
        readme.WriteLine("OCU automatically loads root\\OCUKeyboard.kb. This design carries its own theme, font, colors, geometry, side controls, console INPUT artwork, semantic PC/VR and lock-state artwork, and layered PNG artwork without replacing opencomposite.ini.");
        readme.WriteLine("Disable this mod to return to OCU's embedded keyboard. Restart Skyrim VR after enabling or disabling it.");
        readme.WriteLine($"Design theme: {document.BaseTheme}; font: {document.FontName}.");
        readme.WriteLine();
        readme.WriteLine($"Authored with OCU Keyboard Studio on {DateTime.Now:yyyy-MM-dd}.");
    }

    internal static void AddArtwork(ZipArchive archive, string? sourcePath, string entryName)
    {
        if (string.IsNullOrWhiteSpace(sourcePath) || !File.Exists(sourcePath))
            return;
        ZipArchiveEntry entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
        using Stream output = entry.Open();
        if (Path.GetExtension(sourcePath).Equals(".png", StringComparison.OrdinalIgnoreCase))
        {
            using Stream input = File.OpenRead(sourcePath);
            input.CopyTo(output);
            return;
        }
        using var image = new Bitmap(sourcePath);
        using var converted = new MemoryStream();
        image.Save(converted, System.Drawing.Imaging.ImageFormat.Png);
        converted.Position = 0;
        converted.CopyTo(output);
    }

    internal static void AddFile(ZipArchive archive, string? sourcePath, string entryName)
    {
        if (string.IsNullOrWhiteSpace(sourcePath) || !File.Exists(sourcePath))
            return;
        ZipArchiveEntry entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
        using Stream output = entry.Open();
        using Stream input = File.OpenRead(sourcePath);
        input.CopyTo(output);
    }
}
