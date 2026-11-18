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
        if (e.Args is ["--verify-package", var archive])
        {
            try
            {
                PatchFiles.VerifyEmbeddedPackage(archive);
                Shutdown();
            }
            catch (Exception) { Shutdown(1); }
            return;
        }
        if (e.Args is ["--prompt-preview", var artwork])
        {
            try
            {
                File.WriteAllBytes(artwork, PatchFiles.PromptPreview());
                Shutdown();
            }
            catch (Exception) { Shutdown(1); }
            return;
        }
        if (e.Args is ["--preview", var image])
        {
            var view = new SetupWindow(preview: true).Surface;
            view.Measure(new Size(962, 624));
            view.Arrange(new Rect(0, 0, 962, 624));
            view.UpdateLayout();
            var bitmap = new RenderTargetBitmap(1443, 936, 144, 144, PixelFormats.Pbgra32);
            bitmap.Render(view);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using (var stream = File.Create(image)) encoder.Save(stream);
            Shutdown();
            return;
        }
        if (e.Args is ["--verify", var fixture])
        {
            try
            {
                PatchFiles.VerifyRoundTrip(fixture);
                Shutdown();
            }
            catch (Exception error)
            {
                File.WriteAllText(Path.Combine(fixture, "setup-verification-error.txt"), error.ToString());
                Shutdown(1);
            }
            return;
        }
        if (e.Args is ["--verify-drivers"])
        {
            // Downloads and verifies the pinned packages without launching them.
            _ = VerifyDrivers();
            return;
        }
        new SetupWindow().Show();
    }

    private async Task VerifyDrivers()
    {
        try
        {
            await WirelessSetup.VerifyDownloads(CancellationToken.None);
            Shutdown();
        }
        catch
        {
            Shutdown(1);
        }
    }
}
