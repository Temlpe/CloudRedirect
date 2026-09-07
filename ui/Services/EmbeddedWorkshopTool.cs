using System;
using System.IO;

namespace CloudRedirect.Services;

// Returns the path where workshop_sync_tool.exe is expected to live.
// The actual extraction (from embedded resources inside cloud_redirect.dll)
// and SHA256 hash generation are now handled by the DLL itself at runtime,
// so the GUI no longer needs to extract files or write the hash file.
internal static class EmbeddedWorkshopTool
{
    private static string? _cachedToolPath;

    public static string? EnsureExtracted()
    {
        if (_cachedToolPath != null && File.Exists(_cachedToolPath))
            return _cachedToolPath;

        string toolsDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "CloudRedirect", "tools");
        string exePath = Path.Combine(toolsDir, "workshop_sync_tool.exe");

        if (File.Exists(exePath))
        {
            _cachedToolPath = exePath;
            return exePath;
        }

        return null;
    }
}
