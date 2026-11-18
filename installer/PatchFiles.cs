using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Ac2Setup;

internal static class PatchFiles
{
    private const string GameExe = "AssassinsCreedIIGame.exe";
    private const string GameHash = "A4BA454B0E2A9190A42DB631574B694CC0DBDCEC750106C1B0364B70A6E4D17E";
    private const string BackupFolder = ".ac2-enhanced-patch";
    private const string PromptArtworkName = "input-prompts.png";
    private static readonly string[] PatchNames = ["SDL3.dll", "d3d9.dll"];
    private static readonly Lazy<PackageManifest> Manifest = new(ReadManifest);

    internal static string Version => Manifest.Value.Version;
    internal static bool EnhancedPrompts => ReadPromptMode(PromptSettingsPath());

    internal static string FindGame()
    {
        using var registry = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
        string? steam = registry?.GetValue("SteamPath") as string;
        if (steam == null || steam.Length == 0) return "";
        var libraries = new List<string> { steam };
        string config = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        try
        {
            if (File.Exists(config))
                foreach (Match match in Regex.Matches(File.ReadAllText(config), """"path"\s+"([^"]+)""""))
                    libraries.Add(match.Groups[1].Value.Replace(@"\\", @"\"));
        }
        catch (IOException) {}
        foreach (string library in libraries)
        {
            string folder = Path.Combine(library, "steamapps", "common", "Assassin's Creed 2");
            if (File.Exists(Path.Combine(folder, GameExe))) return Path.GetFullPath(folder);
        }
        return "";
    }

    internal static string Install(string folder)
    {
        folder = Validate(folder);
        var payload = ReadPayload();
        string state = Path.Combine(folder, BackupFolder);
        Directory.CreateDirectory(state);
        using var gate = File.Open(Path.Combine(state, "setup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (PatchNames.All(name => Matches(Path.Combine(folder, name), payload[name]))) return "";

        string snapshot = Path.Combine(state, $"{DateTime.UtcNow:yyyyMMdd-HHmmss.fffffff}-{Guid.NewGuid():N}");
        Directory.CreateDirectory(snapshot);
        Snapshot(folder, snapshot);
        foreach (string name in PatchNames)
            File.WriteAllText(Path.Combine(snapshot, name + ".installed"), Hash(payload[name]));
        EnsureGameClosed(folder);
        try
        {
            foreach (string name in PatchNames)
            {
                Replace(Path.Combine(folder, name), payload[name]);
                if (!Matches(Path.Combine(folder, name), payload[name]))
                    throw new IOException($"The installed {name} could not be verified.");
            }
            File.WriteAllText(Path.Combine(snapshot, "complete"), "AC2 Enhanced Patch\n");
        }
        catch
        {
            RestoreSnapshot(folder, snapshot);
            throw;
        }
        return snapshot;
    }

    internal static void Restore(string folder)
    {
        folder = Validate(folder);
        string state = Path.Combine(folder, BackupFolder);
        if (!Directory.Exists(state)) throw new IOException(Languages.Text("MissingBackup"));
        using var gate = File.Open(Path.Combine(state, "setup.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        string? snapshot = Directory.EnumerateDirectories(state).OrderByDescending(path => path, StringComparer.Ordinal)
            .FirstOrDefault(path => File.Exists(Path.Combine(path, "complete")));
        if (snapshot == null) throw new IOException(Languages.Text("MissingBackup"));
        foreach (string name in PatchNames)
        {
            string installed = Path.Combine(folder, name);
            if (!File.Exists(installed) || !string.Equals(Hash(installed),
                    File.ReadAllText(Path.Combine(snapshot, name + ".installed")), StringComparison.Ordinal))
                throw new IOException("The patch files changed after installation. The backup is still in the game folder.");
        }
        string undo = Path.Combine(state, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(undo);
        Snapshot(folder, undo);
        EnsureGameClosed(folder);
        try
        {
            RestoreSnapshot(folder, snapshot);
            File.Move(Path.Combine(snapshot, "complete"), Path.Combine(snapshot, "restored"));
        }
        catch
        {
            RestoreSnapshot(folder, undo);
            throw;
        }
    }

    internal static void VerifyRoundTrip(string folder)
    {
        Validate(folder);
        if (PatchNames.Any(name => File.Exists(Path.Combine(folder, name))))
            throw new IOException("The test folder must start without patch DLLs.");
        byte[] previous = Encoding.ASCII.GetBytes("Existing test file");
        File.WriteAllBytes(Path.Combine(folder, "d3d9.dll"), previous);
        Install(folder);
        if (!string.IsNullOrEmpty(Install(folder))) throw new IOException("Repeated install was not a no-op.");
        Restore(folder);
        if (File.Exists(Path.Combine(folder, "SDL3.dll")) || !Matches(Path.Combine(folder, "d3d9.dll"), previous))
            throw new IOException("Restore did not reproduce the original files.");

        string settings = Path.Combine(folder, "setup-test-settings.ini");
        File.WriteAllText(settings, "[Controllers]\r\nDualSenseBluetoothHaptics=0\r\n");
        if (!ReadPromptMode(settings)) throw new IOException("Assigned keys should be the default.");
        WritePromptMode(settings, enhanced: false);
        if (ReadPromptMode(settings)) throw new IOException("Original icons were not saved.");
        WritePromptMode(settings, enhanced: true);
        if (!ReadPromptMode(settings) || GetPrivateProfileIntW("Controllers", "DualSenseBluetoothHaptics", 1, settings) != 0)
            throw new IOException("The prompt preference did not preserve the other settings.");
    }

    internal static byte[] PromptPreview()
    {
        using var zip = OpenPackage();
        return ReadVerified(zip, PromptArtworkName);
    }

    internal static void VerifyEmbeddedPackage(string file)
    {
        using var embedded = OpenPayloadStream();
        using var expected = File.OpenRead(file);
        if (Hash(embedded) != Hash(expected))
            throw new IOException("The installer embeds a different patch archive.");
        _ = ReadPayload();
        _ = PromptPreview();
    }

    internal static void SetEnhancedPrompts(bool enhanced) => WritePromptMode(PromptSettingsPath(), enhanced);

    private static string PromptSettingsPath() => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Ubisoft", "Assassin's Creed 2", "AC2EnhancedPatch.ini");

    private static bool ReadPromptMode(string path) => GetPrivateProfileIntW("Interface", "AssignedKeys", 1, path) != 0;

    private static void WritePromptMode(string path, bool enhanced)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        if (!WritePrivateProfileStringW("Interface", "AssignedKeys", enhanced ? "1" : "0", path))
            throw new IOException(Languages.Text("SettingsFailure"));
    }

    [DllImport("kernel32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern uint GetPrivateProfileIntW(string section, string key, int fallback, string path);

    [DllImport("kernel32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);

    private static string Validate(string folder)
    {
        if (string.IsNullOrWhiteSpace(folder)) throw new IOException(Languages.Text("EmptyFolder"));
        folder = Path.GetFullPath(folder.Trim());
        string exe = Path.Combine(folder, GameExe);
        if (!File.Exists(exe) || Hash(exe) != GameHash)
            throw new IOException(Languages.Text("InvalidGame"));
        EnsureGameClosed(folder);
        return folder;
    }

    private static void EnsureGameClosed(string folder)
    {
        string executable = Path.Combine(folder, GameExe);
        Process[] active = Process.GetProcessesByName("AssassinsCreedIIGame");
        bool locked = false;
        foreach (Process process in active)
        {
            using (process)
            {
                try { locked |= process.MainModule?.FileName is not { } path || path.Equals(executable, StringComparison.OrdinalIgnoreCase); }
                catch (InvalidOperationException) {}
                catch (System.ComponentModel.Win32Exception) { locked = true; }
            }
        }
        if (locked) throw new IOException(Languages.Text("RunningGame"));
    }

    private static Dictionary<string, byte[]> ReadPayload()
    {
        using var zip = OpenPackage();
        return PatchNames.ToDictionary(name => name, name => ReadVerified(zip, name));
    }

    private static Stream OpenPayloadStream() => PatchPayload.Open();

    private static ZipArchive OpenPackage() => new(OpenPayloadStream(), ZipArchiveMode.Read);

    private static PackageManifest ReadManifest()
    {
        using var zip = OpenPackage();
        using var input = OpenEntry(zip, "patch-manifest.json");
        var serializer = new DataContractJsonSerializer(typeof(PackageManifest),
            new DataContractJsonSerializerSettings { UseSimpleDictionaryFormat = true });
        return (PackageManifest)(serializer.ReadObject(input) ?? throw new IOException("Invalid patch manifest."));
    }

    private static Stream OpenEntry(ZipArchive zip, string name) =>
        (zip.GetEntry(name) ?? throw new IOException($"The installer is missing {name}.")).Open();

    private static byte[] ReadVerified(ZipArchive zip, string name)
    {
        using var input = OpenEntry(zip, name);
        using var contents = new MemoryStream();
        input.CopyTo(contents);
        byte[] data = contents.ToArray();
        string actual = Hash(data);
        if (!Manifest.Value.Hashes.TryGetValue(name, out string? expected) ||
            !string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"The checksum for {name} does not match this installer.");
        return data;
    }

    [DataContract]
    private sealed class PackageManifest
    {
        [DataMember(Name = "version", IsRequired = true)]
        public string Version { get; set; } = "";

        [DataMember(Name = "sha256", IsRequired = true)]
        public Dictionary<string, string> Hashes { get; set; } = new();
    }

    private static string Hash(string file)
    {
        using var stream = File.OpenRead(file);
        return Hash(stream);
    }

    private static string Hash(Stream stream)
    {
        using var sha = SHA256.Create();
        return Hex(sha.ComputeHash(stream));
    }

    private static string Hash(byte[] data)
    {
        using var sha = SHA256.Create();
        return Hex(sha.ComputeHash(data));
    }

    internal static string Hex(byte[] bytes) => BitConverter.ToString(bytes).Replace("-", "");

    private static bool Matches(string file, byte[] data) =>
        File.Exists(file) && Hash(file) == Hash(data);

    private static void Snapshot(string folder, string snapshot)
    {
        foreach (string name in PatchNames)
        {
            string file = Path.Combine(folder, name);
            if (File.Exists(file)) File.Copy(file, Path.Combine(snapshot, name));
        }
    }

    private static void RestoreSnapshot(string folder, string snapshot)
    {
        foreach (string name in PatchNames)
        {
            string before = Path.Combine(snapshot, name);
            string file = Path.Combine(folder, name);
            if (File.Exists(before)) Replace(file, File.ReadAllBytes(before));
            else File.Delete(file);
        }
    }

    private static void Replace(string file, byte[] data)
    {
        string pending = file + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllBytes(pending, data);
            if (File.Exists(file))
                File.Replace(pending, file, null);
            else
                File.Move(pending, file);
        }
        finally { File.Delete(pending); }
    }
}
