using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

namespace Ac2Setup;

public partial class SetupWindow : Window
{
    private bool busy;

    public SetupWindow() : this(preview: false) {}

    internal SetupWindow(bool preview)
    {
        InitializeComponent();
        VersionLabel.Text = $"Version {PatchFiles.Version}";
        PromptMode.SelectedIndex = preview || PatchFiles.EnhancedPrompts ? 0 : 1;
        GameFolder.Text = preview ? @"D:\SteamLibrary\steamapps\common\Assassin's Creed 2" : PatchFiles.FindGame();
        if (preview) InstallPs3.IsChecked = true;
    }

    internal Grid Surface => Page;

    private void BrowseFolder(object sender, RoutedEventArgs e)
    {
        var picker = new OpenFolderDialog { Title = "Choose the Assassin’s Creed II game folder" };
        if (picker.ShowDialog(this) == true) GameFolder.Text = picker.FolderName;
    }

    private void OpenBluetooth(object sender, RoutedEventArgs e)
    {
        Process.Start(new ProcessStartInfo("ms-settings:bluetooth") { UseShellExecute = true });
    }

    private void ShowPromptArtwork(object sender, RoutedEventArgs e)
    {
        try
        {
            using var contents = new MemoryStream(PatchFiles.PromptPreview());
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.StreamSource = contents;
            bitmap.EndInit();

            var artwork = new Image { Source = bitmap, Stretch = Stretch.Uniform, VerticalAlignment = VerticalAlignment.Top };
            RenderOptions.SetBitmapScalingMode(artwork, BitmapScalingMode.HighQuality);
            var display = new Window
            {
                Owner = this,
                Icon = Icon,
                Title = $"Input prompts · {PatchFiles.Version}",
                Width = Math.Min(1300, SystemParameters.WorkArea.Width),
                Height = Math.Min(900, SystemParameters.WorkArea.Height),
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Content = new ScrollViewer { Content = artwork, VerticalScrollBarVisibility = ScrollBarVisibility.Auto },
            };
            display.ShowDialog();
        }
        catch (Exception error) { Status.Text = error.Message; }
    }

    private async void InstallPatch(object sender, RoutedEventArgs e)
    {
        var folder = GameFolder.Text;
        bool wireless = InstallPs3.IsChecked == true;
        bool enhancedPrompts = PromptMode.SelectedIndex == 0;
        await RunOperation(async () =>
        {
            string backup = await Task.Run(() => PatchFiles.Install(folder));
            string installed = string.IsNullOrEmpty(backup)
                ? $"Version {PatchFiles.Version} is already installed."
                : $"Version {PatchFiles.Version} installed.";
            try { PatchFiles.SetEnhancedPrompts(enhancedPrompts); }
            catch (IOException error) { return $"{installed} {error.Message}"; }
            catch (UnauthorizedAccessException) { return $"{installed} Windows denied access to the prompt settings."; }
            string promptMode = enhancedPrompts ? "Assigned keys are selected." : "Original game icons are selected.";
            if (!wireless) return $"{installed} {promptMode} Launch Assassin’s Creed II from Steam.";
            bool restart = await WirelessSetup.Install(new Progress<string>(text => Status.Text = text));
            return restart
                ? "Patch and drivers installed. Restart Windows before connecting DualShock 3 by USB to pair it."
                : "Patch and drivers installed. Connect DualShock 3 by USB to pair it, then choose SXS in DsHidMini Control App.";
        });
    }

    private async void RestorePatch(object sender, RoutedEventArgs e)
    {
        var folder = GameFolder.Text;
        await RunOperation(async () =>
        {
            await Task.Run(() => PatchFiles.Restore(folder));
            return "The files from before the last installation were restored.";
        });
    }

    private async Task RunOperation(Func<Task<string>> work)
    {
        busy = true;
        InstallButton.IsEnabled = RestoreButton.IsEnabled = BrowseButton.IsEnabled =
            GameFolder.IsEnabled = InstallPs3.IsEnabled = PromptMode.IsEnabled = PromptArtworkButton.IsEnabled = false;
        Status.Text = "Checking the game folder and patch files…";
        Progress.Visibility = Visibility.Visible;
        Progress.IsIndeterminate = true;
        try { Status.Text = await work(); }
        catch (UnauthorizedAccessException) { Status.Text = "Windows denied access to the game folder. Run this setup as administrator to install here."; }
        catch (Exception error) { Status.Text = error.Message; }
        finally
        {
            busy = false;
            InstallButton.IsEnabled = RestoreButton.IsEnabled = BrowseButton.IsEnabled =
                GameFolder.IsEnabled = InstallPs3.IsEnabled = PromptMode.IsEnabled = PromptArtworkButton.IsEnabled = true;
            Progress.Visibility = Visibility.Collapsed;
        }
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (busy) e.Cancel = true;
        base.OnClosing(e);
    }
}
