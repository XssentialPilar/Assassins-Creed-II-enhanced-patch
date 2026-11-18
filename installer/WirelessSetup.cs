using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;

namespace Ac2Setup;

internal static class WirelessSetup
{
    private sealed class Package(string name, string url, string hash)
    {
        internal string Name { get; } = name;
        internal string Url { get; } = url;
        internal string Hash { get; } = hash;
    }
    private static readonly Package Bluetooth = new("BthPS3 3.2.1",
        "https://github.com/nefarius/BthPS3/releases/download/setup-v3.2.1/Nefarius_BthPS3_Drivers_x64_arm64_v3.2.1.msi",
        "704dbfc3e7fe961b255ed53aadc21c730cc5c250686eb29fbb6c82c34bfcc1be");
    private static readonly Package Controller = new("DsHidMini 3.28.0",
        "https://github.com/nefarius/DsHidMini/releases/download/setup-v3.28.0/Nefarius_DsHidMini_Drivers_x64_arm64_v3.28.0.msi",
        "3ec1dd2044ac87a1dde0f333569f292827f49c1382b6f620723b3c46153782ad");
    private static readonly Package Desktop = new("Microsoft Windows Desktop Runtime 10.0.15",
        "https://builds.dotnet.microsoft.com/dotnet/WindowsDesktop/10.0.15/windowsdesktop-runtime-10.0.15-win-x64.exe",
        "f838960962172f1a769c49fa600e6f60f56f8d08a28e3cb878890ffc2e3a7a2e1b65697588e23efa767b8b8e8523ec01f55e875120507dde16d9a0869102dab3");

    private static readonly HttpClient Client = new() { Timeout = TimeSpan.FromMinutes(5) };

    internal static async Task<bool> Install(IProgress<string> progress)
    {
        if (Environment.OSVersion.Version < new Version(10, 0, 17763) ||
            !string.Equals(Environment.GetEnvironmentVariable("PROCESSOR_ARCHITECTURE"), "AMD64", StringComparison.OrdinalIgnoreCase))
            throw new IOException(Languages.Text("DriverPlatform"));
        string cache = CreateCache();
        try
        {
            var packages = new List<Package>();
            string desktop = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                "dotnet", "shared", "Microsoft.WindowsDesktop.App");
            if (!Directory.Exists(desktop) || !Directory.EnumerateDirectories(desktop, "10.0.*")
                    .Any(path => File.Exists(Path.Combine(path, "PresentationFramework.dll"))))
                packages.Add(Desktop);
            packages.Add(Bluetooth);
            packages.Add(Controller);

            var downloaded = new List<(Package Package, string Path)>();
            foreach (var package in packages)
            {
                progress.Report(Languages.Format("Download", package.Name));
                downloaded.Add((package, await Download(package, cache, CancellationToken.None)));
            }
            bool restart = false;
            foreach (var (package, path) in downloaded)
            {
                progress.Report(Languages.Format("DriverWindow", package.Name));
                var start = new ProcessStartInfo { UseShellExecute = true, Verb = "runas" };
                if (package == Desktop)
                {
                    start.FileName = path;
                    start.Arguments = "/install /norestart";
                }
                else
                {
                    start.FileName = Path.Combine(Environment.SystemDirectory, "msiexec.exe");
                    start.Arguments = $"/i \"{path}\" /norestart";
                }
                using var installer = Process.Start(start) ?? throw new IOException($"Could not open {package.Name}.");
                await Task.Run(installer.WaitForExit);
                if (installer.ExitCode is 3010 or 1641) restart = true;
                else if (installer.ExitCode != 0)
                    throw new IOException($"{package.Name} exited with code {installer.ExitCode}.");
            }
            return restart;
        }
        catch (Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            throw new IOException(Languages.Text("DriverCancelled"), error);
        }
        catch (Exception error)
        {
            throw new IOException(Languages.Text("DriverFailure") + " " + error.Message, error);
        }
        finally { RemoveCache(cache); }
    }

    internal static async Task VerifyDownloads(CancellationToken cancel)
    {
        string cache = CreateCache();
        try
        {
            foreach (var package in new[] { Bluetooth, Controller, Desktop }) await Download(package, cache, cancel);
        }
        finally { RemoveCache(cache); }
    }

    private static string CreateCache() =>
        Directory.CreateDirectory(Path.Combine(Path.GetTempPath(), $"ac2-setup-{Guid.NewGuid():N}")).FullName;

    private static void RemoveCache(string cache)
    {
        foreach (string file in Directory.EnumerateFiles(cache)) File.Delete(file);
        Directory.Delete(cache);
    }

    private static async Task<string> Download(Package package, string cache, CancellationToken cancel)
    {
        string name = new Uri(package.Url).Segments.Last();
        string path = Path.Combine(cache, name);
        using (var response = await Client.GetAsync(package.Url, HttpCompletionOption.ResponseHeadersRead, cancel))
        {
            response.EnsureSuccessStatusCode();
            using var input = await response.Content.ReadAsStreamAsync();
            using var output = File.Create(path);
            await input.CopyToAsync(output, 81920, cancel);
        }
        using var downloaded = File.OpenRead(path);
        using HashAlgorithm algorithm = package == Desktop ? SHA512.Create() : SHA256.Create();
        byte[] hash = algorithm.ComputeHash(downloaded);
        if (!string.Equals(PatchFiles.Hex(hash), package.Hash, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"The checksum for {package.Name} did not match its official release.");
        return path;
    }
}
