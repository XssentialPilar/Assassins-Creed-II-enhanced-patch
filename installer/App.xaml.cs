using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Ac2Setup;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Length != 0)
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = RunVerification(e.Args);
            return;
        }
        try { new SetupWindow().Show(); }
        catch (Exception error)
        {
            MessageBox.Show(Languages.Text("Failure") + Environment.NewLine + error.Message,
                "Assassin’s Creed II Enhanced Patch", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    private async Task RunVerification(string[] args)
    {
        string command = args[0];
        string path = args.Length == 2 ? args[1] : "";
        try
        {
            switch (command)
            {
                case "--verify-package": PatchFiles.VerifyEmbeddedPackage(path); break;
                case "--prompt-preview": File.WriteAllBytes(path, PatchFiles.PromptPreview()); break;
                case "--preview": RenderSetup(path, Languages.Select("en")); break;
                case "--verify-languages": VerifyLanguages(path); break;
                case "--verify": PatchFiles.VerifyRoundTrip(path); break;
                case "--verify-drivers": await WirelessSetup.VerifyDownloads(CancellationToken.None); break;
                default: Shutdown(2); return;
            }
            Shutdown();
        }
        catch (Exception error)
        {
            if (Directory.Exists(path))
                File.WriteAllText(Path.Combine(path, "setup-verification-error.txt"), error.ToString());
            Shutdown(1);
        }
    }

    private static void VerifyLanguages(string folder)
    {
        if (Languages.All.Count != 25 || Languages.All.Select(language => language.Tag).Distinct().Count() != 25)
            throw new IOException("Expected 25 distinct languages.");
        Directory.CreateDirectory(folder);
        foreach (var language in Languages.All)
        {
            if (Languages.Select(language.Tag) != language) throw new IOException(language.Tag);
            Languages.Current = language;
            foreach (string key in new[] { "Download", "DriverWindow" })
                if (!Languages.Format(key, "TEST").Contains("TEST")) throw new IOException(language.Tag + " " + key);
            RenderSetup(Path.Combine(folder, language.Tag + ".png"), language);
        }
    }

    private static void RenderSetup(string image, Language language)
    {
        var window = new SetupWindow(preview: true, language: language);
        var view = window.Surface;
        view.Measure(new Size(994, 680));
        view.Arrange(new Rect(0, 0, 994, 680));
        view.UpdateLayout();
        var bitmap = new RenderTargetBitmap(1491, 1020, 144, 144, PixelFormats.Pbgra32);
        bitmap.Render(view);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(image);
        encoder.Save(stream);
        window.Close();
    }

}
