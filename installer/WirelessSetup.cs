using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Ac2Setup;

internal static class WirelessSetup
{
    private sealed record Package(string Name, string Url, string Hash);
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
        if (RuntimeInformation.OSArchitecture != Architecture.X64 || !OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17763))
            throw new IOException("The patch was installed. This DualShock 3 setup supports Windows 10 1809+ and 11 on x64.");
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
                progress.Report($"Downloading and verifying {package.Name}…");
                downloaded.Add((package, await Download(package, cache, CancellationToken.None)));
            }
            bool restart = false;
            foreach (var (package, path) in downloaded)
            {
                progress.Report($"Follow the {package.Name} installation window.");
                var start = new ProcessStartInfo { UseShellExecute = true, Verb = "runas" };
                if (package == Desktop)
                {
                    start.FileName = path;
                    start.ArgumentList.Add("/install");
                }
                else
                {
                    start.FileName = Path.Combine(Environment.SystemDirectory, "msiexec.exe");
                    start.ArgumentList.Add("/i");
                    start.ArgumentList.Add(path);
                }
                start.ArgumentList.Add("/norestart");
                using var installer = Process.Start(start) ?? throw new IOException($"Could not open {package.Name}.");
                await installer.WaitForExitAsync();
                if (installer.ExitCode is 3010 or 1641) restart = true;
                else if (installer.ExitCode != 0)
                    throw new IOException($"{package.Name} exited with code {installer.ExitCode}.");
            }
            return restart;
        }
        catch (Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            throw new IOException("The patch was installed. The optional DualShock 3 driver setup was canceled.", error);
        }
        catch (Exception error)
        {
            throw new IOException($"The patch was installed. DualShock 3 setup did not finish. {error.Message}", error);
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

    private static string CreateCache() => Directory.CreateTempSubdirectory("ac2-setup-").FullName;

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
            await using var input = await response.Content.ReadAsStreamAsync(cancel);
            await using var output = File.Create(path);
            await input.CopyToAsync(output, cancel);
        }
        await using var downloaded = File.OpenRead(path);
        byte[] hash = package == Desktop ? await SHA512.HashDataAsync(downloaded, cancel)
            : await SHA256.HashDataAsync(downloaded, cancel);
        if (!string.Equals(Convert.ToHexString(hash), package.Hash, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"The checksum for {package.Name} did not match its official release.");
        return path;
    }
}
