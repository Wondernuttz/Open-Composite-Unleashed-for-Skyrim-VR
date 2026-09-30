using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace OCUKeyboardStudio;

internal sealed record KeyboardPackageImportResult(
    string LayoutPath,
    string DisplayName,
    int ArtworkCount,
    bool HasCustomFont);

// A .ocukb is a ZIP container with a deliberately small, flat allow-listed
// payload. OCU's native runtime still consumes loose .kb/PNG/SFN files; Studio
// imports this portable authoring package into its managed design library.
internal static class KeyboardPackage
{
    public const string Extension = ".ocukb";
    private const string ManifestName = "manifest.json";
    private const string LayoutName = "keyboard.kb";
    private const int FormatVersion = 1;
    private const int MaxEntries = 128;
    private const long MaxEntryBytes = 64L * 1024 * 1024;
    private const long MaxPackageBytes = 256L * 1024 * 1024;

    public static void Export(string packagePath, KeyboardDocument document, string displayName)
    {
        ValidateExportAssets(document);
        string fullPath = Path.GetFullPath(packagePath);
        if (!Path.GetExtension(fullPath).Equals(Extension, StringComparison.OrdinalIgnoreCase))
            fullPath += Extension;

        using var file = new FileStream(fullPath, FileMode.Create, FileAccess.Write, FileShare.None);
        using var archive = new ZipArchive(file, ZipArchiveMode.Create);

        var manifest = new
        {
            formatVersion = FormatVersion,
            name = string.IsNullOrWhiteSpace(displayName) ? "OCU Keyboard" : displayName.Trim(),
            layout = LayoutName,
            createdWith = "OCU Keyboard Studio",
            containsArtwork = HasArtwork(document),
            containsCustomFont = HasCustomFont(document)
        };
        WriteTextEntry(archive, ManifestName,
            JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true }));
        WriteTextEntry(archive, LayoutName, document.Serialize());

        if (HasBackground(document))
            Mo2ModExporter.AddArtwork(archive, document.BackgroundImagePath!, "OCUKeyboardBackground.png");
        if (HasConsoleInputBackground(document))
            Mo2ModExporter.AddArtwork(archive, document.ConsoleInputBackgroundImagePath!,
                KeyboardDocument.ConsoleInputBackgroundPortableName);
        AddStateArtwork(archive, document.ModeVrArtworkImagePath, KeyboardDocument.ModeVrArtworkPortableName);
        AddStateArtwork(archive, document.ModePcArtworkImagePath, KeyboardDocument.ModePcArtworkPortableName);
        AddStateArtwork(archive, document.LockWorldArtworkImagePath, KeyboardDocument.LockWorldArtworkPortableName);
        AddStateArtwork(archive, document.LockHeadArtworkImagePath, KeyboardDocument.LockHeadArtworkPortableName);
        for (int index = 0; index < document.Sprites.Count; index++)
            Mo2ModExporter.AddArtwork(archive, document.Sprites[index].SourcePath!,
                KeyboardDocument.SpriteFileName(index));
        if (HasControlArrow(document))
            Mo2ModExporter.AddArtwork(archive, document.ControlArrowImagePath!, "OCUKeyboardControlArrow.png");
        if (HasCustomFont(document))
        {
            Mo2ModExporter.AddFile(archive, document.CustomFontMetadataPath!, "OCUKeyboardFont.sfn");
            Mo2ModExporter.AddFile(archive, document.CustomFontTexturePath!, "OCUKeyboardFont.png");
        }
    }

    public static KeyboardPackageImportResult Import(string packagePath, string designLibraryDirectory)
    {
        string fullPackagePath = Path.GetFullPath(packagePath);
        if (!File.Exists(fullPackagePath))
            throw new FileNotFoundException("OCU keyboard package was not found.", fullPackagePath);
        if (!Path.GetExtension(fullPackagePath).Equals(Extension, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"OCU keyboard packages must use the {Extension} extension.");

        Directory.CreateDirectory(designLibraryDirectory);
        using var archive = ZipFile.OpenRead(fullPackagePath);
        ZipArchiveEntry[] entries = archive.Entries
            .Where(entry => !string.IsNullOrEmpty(entry.Name))
            .ToArray();
        if (entries.Length == 0 || entries.Length > MaxEntries)
            throw new InvalidDataException($"The package contains {entries.Length} files; the supported range is 1-{MaxEntries}.");

        var byName = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        long totalBytes = 0;
        foreach (ZipArchiveEntry entry in entries)
        {
            string normalized = entry.FullName.Replace('\\', '/');
            if (normalized.Contains('/') || normalized is "." or ".." || !IsAllowedEntry(normalized))
                throw new InvalidDataException($"Unsupported or unsafe package entry: {entry.FullName}");
            if (!byName.TryAdd(normalized, entry))
                throw new InvalidDataException($"Duplicate package entry: {entry.FullName}");
            if (entry.Length < 0 || entry.Length > MaxEntryBytes)
                throw new InvalidDataException($"Package entry is too large: {entry.FullName}");
            totalBytes = checked(totalBytes + entry.Length);
            if (totalBytes > MaxPackageBytes)
                throw new InvalidDataException("The uncompressed keyboard package is larger than 256 MB.");
        }

        if (!byName.TryGetValue(ManifestName, out ZipArchiveEntry? manifestEntry)
            || !byName.TryGetValue(LayoutName, out ZipArchiveEntry? layoutEntry))
            throw new InvalidDataException($"The package must contain {ManifestName} and {LayoutName}.");

        (string displayName, int version) = ReadManifest(manifestEntry);
        if (version != FormatVersion)
            throw new InvalidDataException($"Unsupported OCU keyboard package version {version}; this Studio supports version {FormatVersion}.");

        string safeName = SafeFileStem(displayName);
        // The source path, rather than the package contents, is the identity of
        // a saved project. Re-saving the same .ocukb refreshes one library entry
        // instead of manufacturing a new design on every edit.
        string hash = ComputePathHash(fullPackagePath)[..10];
        string finalDirectory = Path.Combine(Path.GetFullPath(designLibraryDirectory), $"{safeName}-{hash}");
        string finalLayout = Path.Combine(finalDirectory, $"{safeName}.kb");

        string stagingDirectory = Path.Combine(Path.GetFullPath(designLibraryDirectory), $".import-{Guid.NewGuid():N}");
        string? replacedDirectory = null;
        Directory.CreateDirectory(stagingDirectory);
        try
        {
            foreach ((string entryName, ZipArchiveEntry entry) in byName)
            {
                string outputName = entryName.Equals(LayoutName, StringComparison.OrdinalIgnoreCase)
                    ? $"{safeName}.kb"
                    : entryName;
                string outputPath = ResolveChildPath(stagingDirectory, outputName);
                using Stream input = entry.Open();
                using var output = new FileStream(outputPath, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                CopyWithLimit(input, output, MaxEntryBytes, entryName);
            }

            string stagedLayout = Path.Combine(stagingDirectory, $"{safeName}.kb");
            KeyboardDocument imported = KeyboardDocument.Load(stagedLayout);
            ValidateImportedAssets(imported);

            if (Directory.Exists(finalDirectory))
            {
                replacedDirectory = Path.Combine(Path.GetFullPath(designLibraryDirectory), $".replace-{Guid.NewGuid():N}");
                Directory.Move(finalDirectory, replacedDirectory);
            }
            Directory.Move(stagingDirectory, finalDirectory);
            if (replacedDirectory is not null)
            {
                try { Directory.Delete(replacedDirectory, recursive: true); }
                catch { /* The valid replacement is already active; stale backup cleanup is non-fatal. */ }
                replacedDirectory = null;
            }
            finalLayout = Path.Combine(finalDirectory, $"{safeName}.kb");
            imported = KeyboardDocument.Load(finalLayout);
            return DescribeImport(imported, finalLayout, displayName);
        }
        catch
        {
            if (Directory.Exists(stagingDirectory))
                Directory.Delete(stagingDirectory, recursive: true);
            if (replacedDirectory is not null
                && Directory.Exists(replacedDirectory)
                && !Directory.Exists(finalDirectory))
                Directory.Move(replacedDirectory, finalDirectory);
            throw;
        }
    }

    private static KeyboardPackageImportResult DescribeImport(
        KeyboardDocument document, string layoutPath, string displayName)
    {
        int artworkCount = (HasBackground(document) ? 1 : 0)
            + (HasConsoleInputBackground(document) ? 1 : 0)
            + StateArtworkCount(document)
            + document.Sprites.Count
            + (HasControlArrow(document) ? 1 : 0);
        return new KeyboardPackageImportResult(
            layoutPath, displayName, artworkCount, HasCustomFont(document));
    }

    private static void ValidateExportAssets(KeyboardDocument document)
    {
        if (HasBackground(document))
            RequireFile(document.BackgroundImagePath, "background image");
        if (HasConsoleInputBackground(document))
            RequireFile(document.ConsoleInputBackgroundImagePath, "console INPUT panel image");
        ValidateStateArtwork(document.ModeVrArtworkImagePath, document.ModeVrArtworkFileName, "VR Mode artwork");
        ValidateStateArtwork(document.ModePcArtworkImagePath, document.ModePcArtworkFileName, "PC Mode artwork");
        ValidateStateArtwork(document.LockWorldArtworkImagePath, document.LockWorldArtworkFileName, "world/unlocked artwork");
        ValidateStateArtwork(document.LockHeadArtworkImagePath, document.LockHeadArtworkFileName, "head-locked artwork");
        for (int index = 0; index < document.Sprites.Count; index++)
            RequireFile(document.Sprites[index].SourcePath, $"sprite {index + 1}");
        if (HasControlArrow(document))
            RequireFile(document.ControlArrowImagePath, "side-control arrow");
        if (document.FontName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase)
            || !string.IsNullOrWhiteSpace(document.CustomFontMetadataPath)
            || !string.IsNullOrWhiteSpace(document.CustomFontTexturePath))
        {
            RequireFile(document.CustomFontMetadataPath, "custom font metadata");
            RequireFile(document.CustomFontTexturePath, "custom font texture");
        }
    }

    private static void ValidateImportedAssets(KeyboardDocument document)
    {
        if (!string.IsNullOrWhiteSpace(document.BackgroundFileName))
            RequireFile(document.BackgroundImagePath, "packaged background image");
        if (!string.IsNullOrWhiteSpace(document.ConsoleInputBackgroundFileName))
            RequireFile(document.ConsoleInputBackgroundImagePath, "packaged console INPUT panel image");
        ValidateImportedStateArtwork(document.ModeVrArtworkFileName, document.ModeVrArtworkImagePath, "packaged VR Mode artwork");
        ValidateImportedStateArtwork(document.ModePcArtworkFileName, document.ModePcArtworkImagePath, "packaged PC Mode artwork");
        ValidateImportedStateArtwork(document.LockWorldArtworkFileName, document.LockWorldArtworkImagePath, "packaged world/unlocked artwork");
        ValidateImportedStateArtwork(document.LockHeadArtworkFileName, document.LockHeadArtworkImagePath, "packaged head-locked artwork");
        for (int index = 0; index < document.Sprites.Count; index++)
            RequireFile(document.Sprites[index].SourcePath, $"packaged sprite {index + 1}");
        if (!string.IsNullOrWhiteSpace(document.ControlArrowFileName))
            RequireFile(document.ControlArrowImagePath, "packaged side-control arrow");
        if (document.FontName.StartsWith("custom_", StringComparison.OrdinalIgnoreCase))
        {
            RequireFile(document.CustomFontMetadataPath, "packaged custom font metadata");
            RequireFile(document.CustomFontTexturePath, "packaged custom font texture");
        }
    }

    private static void RequireFile(string? path, string description)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            throw new FileNotFoundException($"The keyboard's {description} is missing. Re-select it before exporting the package.", path);
    }

    private static bool HasBackground(KeyboardDocument document)
        => !string.IsNullOrWhiteSpace(document.BackgroundImagePath)
            || !string.IsNullOrWhiteSpace(document.BackgroundFileName);

    private static bool HasControlArrow(KeyboardDocument document)
        => !string.IsNullOrWhiteSpace(document.ControlArrowImagePath)
            || !string.IsNullOrWhiteSpace(document.ControlArrowFileName);

    private static bool HasConsoleInputBackground(KeyboardDocument document)
        => !string.IsNullOrWhiteSpace(document.ConsoleInputBackgroundImagePath)
            || !string.IsNullOrWhiteSpace(document.ConsoleInputBackgroundFileName);

    private static int StateArtworkCount(KeyboardDocument document)
        => (HasStateArtwork(document.ModeVrArtworkImagePath, document.ModeVrArtworkFileName) ? 1 : 0)
            + (HasStateArtwork(document.ModePcArtworkImagePath, document.ModePcArtworkFileName) ? 1 : 0)
            + (HasStateArtwork(document.LockWorldArtworkImagePath, document.LockWorldArtworkFileName) ? 1 : 0)
            + (HasStateArtwork(document.LockHeadArtworkImagePath, document.LockHeadArtworkFileName) ? 1 : 0);

    private static bool HasStateArtwork(string? imagePath, string? fileName)
        => !string.IsNullOrWhiteSpace(imagePath) || !string.IsNullOrWhiteSpace(fileName);

    private static void AddStateArtwork(ZipArchive archive, string? sourcePath, string portableName)
        => Mo2ModExporter.AddArtwork(archive, sourcePath, portableName);

    private static void ValidateStateArtwork(string? imagePath, string? fileName, string description)
    {
        if (HasStateArtwork(imagePath, fileName))
            RequireFile(imagePath, description);
    }

    private static void ValidateImportedStateArtwork(string? fileName, string? imagePath, string description)
    {
        if (!string.IsNullOrWhiteSpace(fileName))
            RequireFile(imagePath, description);
    }

    private static bool HasArtwork(KeyboardDocument document)
        => HasBackground(document) || HasConsoleInputBackground(document)
            || StateArtworkCount(document) > 0 || document.Sprites.Count > 0 || HasControlArrow(document);

    private static bool HasCustomFont(KeyboardDocument document)
        => !string.IsNullOrWhiteSpace(document.CustomFontMetadataPath)
            && !string.IsNullOrWhiteSpace(document.CustomFontTexturePath)
            && File.Exists(document.CustomFontMetadataPath)
            && File.Exists(document.CustomFontTexturePath);

    private static void WriteTextEntry(ZipArchive archive, string entryName, string contents)
    {
        ZipArchiveEntry entry = archive.CreateEntry(entryName, CompressionLevel.Optimal);
        using var writer = new StreamWriter(entry.Open(), new UTF8Encoding(false));
        writer.Write(contents);
    }

    private static (string name, int version) ReadManifest(ZipArchiveEntry entry)
    {
        using Stream stream = entry.Open();
        using var buffer = new MemoryStream();
        CopyWithLimit(stream, buffer, 64 * 1024, ManifestName);
        using JsonDocument json = JsonDocument.Parse(buffer.ToArray());
        JsonElement root = json.RootElement;
        int version = root.TryGetProperty("formatVersion", out JsonElement versionNode)
            && versionNode.TryGetInt32(out int parsedVersion)
            ? parsedVersion
            : 0;
        string name = root.TryGetProperty("name", out JsonElement nameNode)
            ? nameNode.GetString() ?? "OCU Keyboard"
            : "OCU Keyboard";
        return (name, version);
    }

    private static bool IsAllowedEntry(string name)
    {
        if (name.Equals(ManifestName, StringComparison.OrdinalIgnoreCase)
            || name.Equals(LayoutName, StringComparison.OrdinalIgnoreCase)
            || name.Equals("OCUKeyboardBackground.png", StringComparison.OrdinalIgnoreCase)
            || name.Equals("OCUKeyboardControlArrow.png", StringComparison.OrdinalIgnoreCase)
            || name.Equals("OCUKeyboardFont.sfn", StringComparison.OrdinalIgnoreCase)
            || name.Equals("OCUKeyboardFont.png", StringComparison.OrdinalIgnoreCase))
            return true;
        if (!name.StartsWith("OCUKeyboardSprite", StringComparison.OrdinalIgnoreCase)
            || !name.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
            return false;
        string number = name["OCUKeyboardSprite".Length..^4];
        return number.Length is >= 2 and <= 4 && number.All(char.IsAsciiDigit);
    }

    private static string SafeFileStem(string value)
    {
        string stem = string.Concat((value ?? "").Select(character =>
            char.IsLetterOrDigit(character) || character is '-' or '_' ? character : '-')).Trim('-');
        if (string.IsNullOrWhiteSpace(stem))
            stem = "OCU-Keyboard";
        return stem.Length <= 80 ? stem : stem[..80].TrimEnd('-');
    }

    private static string ComputePathHash(string path)
    {
        byte[] pathBytes = Encoding.UTF8.GetBytes(Path.GetFullPath(path).ToUpperInvariant());
        return Convert.ToHexString(SHA256.HashData(pathBytes));
    }

    private static string ResolveChildPath(string root, string fileName)
    {
        string fullRoot = Path.GetFullPath(root)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;
        string fullPath = Path.GetFullPath(Path.Combine(fullRoot, fileName));
        if (!fullPath.StartsWith(fullRoot, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Package entry escapes its import directory: {fileName}");
        return fullPath;
    }

    private static void CopyWithLimit(Stream input, Stream output, long limit, string entryName)
    {
        byte[] buffer = new byte[81920];
        long total = 0;
        int read;
        while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
        {
            total += read;
            if (total > limit)
                throw new InvalidDataException($"Package entry expanded beyond its allowed size: {entryName}");
            output.Write(buffer, 0, read);
        }
    }
}
