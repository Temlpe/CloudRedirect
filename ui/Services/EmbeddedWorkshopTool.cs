using System;
using System.IO;
using System.Reflection;

namespace CloudRedirect.Services;

// Extracts the embedded workshop_sync_tool.exe + steam_api64.dll into a
// stable per-machine folder (%APPDATA%\CloudRedirect\tools). Unlike
// EmbeddedCloud760 (which the UI spawns directly from a hash-keyed temp dir),
// the workshop tool is spawned by the injected DLL, so its path is persisted
// in config.json (workshop_tool_path) and must stay valid across sessions.
// The files are refreshed when the embedded bytes change (upgrades).
internal static class EmbeddedWorkshopTool
{
    private const string ToolResourceName = "workshop_sync_tool.exe";
    private const string DllResourceName = "steam_api64.dll";
    private const string HashFileName = "workshop_sync_tool.sha256";
    private static string? _cachedToolPath;

    // Returns the path to workshop_sync_tool.exe, or null if not embedded.
    public static string? EnsureExtracted()
    {
        if (_cachedToolPath != null && File.Exists(_cachedToolPath))
            return _cachedToolPath;

        var assembly = Assembly.GetExecutingAssembly();
        using var toolStream = assembly.GetManifestResourceStream(ToolResourceName);
        using var dllStream = assembly.GetManifestResourceStream(DllResourceName);
        if (toolStream == null || dllStream == null)
            return null;

        string toolsDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "CloudRedirect", "tools");
        Directory.CreateDirectory(toolsDir);

        string exePath = Path.Combine(toolsDir, "workshop_sync_tool.exe");
        string dllPath = Path.Combine(toolsDir, "steam_api64.dll");
        string hashFile = Path.Combine(toolsDir, HashFileName);

        string toolHash = ComputeResourceHash(toolStream);
        string existingHash = File.Exists(hashFile) ? File.ReadAllText(hashFile).Trim() : "";

        if (!File.Exists(exePath) || !File.Exists(dllPath) || existingHash != toolHash)
        {
            toolStream.Position = 0;
            using var ms = new MemoryStream(checked((int)toolStream.Length));
            toolStream.CopyTo(ms);
            FileUtils.AtomicWriteAllBytes(exePath, ms.ToArray());

            dllStream.Position = 0;
            using var ms2 = new MemoryStream(checked((int)dllStream.Length));
            dllStream.CopyTo(ms2);
            FileUtils.AtomicWriteAllBytes(dllPath, ms2.ToArray());

            FileUtils.AtomicWriteAllText(hashFile, toolHash);
        }

        _cachedToolPath = exePath;
        return exePath;
    }

    private static string ComputeResourceHash(Stream stream)
    {
        stream.Position = 0;
        using var sha = System.Security.Cryptography.SHA256.Create();
        var hash = sha.ComputeHash(stream);
        return Convert.ToHexString(hash);
    }
}
