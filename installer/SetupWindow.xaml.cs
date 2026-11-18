using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Ac2Setup;

public partial class SetupWindow : Window
{
    private bool busy;

    public SetupWindow() : this(preview: false) {}

    internal SetupWindow(bool preview, Language? language = null)
    {
        InitializeComponent();
        VersionLabel.Text = $"v{PatchFiles.Version}";
        LanguagePicker.ItemsSource = Languages.All;
        LanguagePicker.SelectedItem = language ?? Languages.Current;
        PromptMode.SelectedIndex = preview || PatchFiles.EnhancedPrompts ? 0 : 1;
        GameFolder.Text = preview ? @"C:\SteamLibrary\steamapps\common\Assassin's Creed 2" : PatchFiles.FindGame();
    }

    internal Grid Surface => Page;

    private void ChangeLanguage(object sender, SelectionChangedEventArgs e)
    {
        if (LanguagePicker.SelectedItem is not Language language) return;
        Languages.Current = language;
        Page.DataContext = language.Strings;
        Page.FlowDirection = language.Tag == "ar" ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
        VersionLabel.FlowDirection = FlowDirection.LeftToRight;
        Status.Text = Languages.Text("Ready");
    }

    private void BrowseFolder(object sender, RoutedEventArgs e)
    {
        using var picker = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = Languages.Text("Folder"),
            SelectedPath = GameFolder.Text,
            ShowNewFolderButton = false,
        };
        if (picker.ShowDialog() == System.Windows.Forms.DialogResult.OK) GameFolder.Text = picker.SelectedPath;
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
                Title = $"{Languages.Text("Artwork")} · v{PatchFiles.Version}",
                Width = Math.Min(1300, SystemParameters.WorkArea.Width),
                Height = Math.Min(900, SystemParameters.WorkArea.Height),
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Content = new ScrollViewer { Content = artwork, VerticalScrollBarVisibility = ScrollBarVisibility.Auto },
            };
            display.ShowDialog();
        }
        catch (Exception error) { Status.Text = Languages.Text("Failure") + " " + error.Message; }
    }

    private async void InstallPatch(object sender, RoutedEventArgs e)
    {
        var folder = GameFolder.Text;
        bool wireless = InstallPs3.IsChecked == true;
        bool enhancedPrompts = PromptMode.SelectedIndex == 0;
        await RunOperation(async () =>
        {
            string backup = await Task.Run(() => PatchFiles.Install(folder));
            string installed = Languages.Text(string.IsNullOrEmpty(backup) ? "AlreadyInstalled" : "Installed");
            try { PatchFiles.SetEnhancedPrompts(enhancedPrompts); }
            catch (IOException error) { return $"{installed} {error.Message}"; }
            catch (UnauthorizedAccessException) { return $"{installed} {Languages.Text("SettingsFailure")}"; }
            if (!wireless) return installed + " " + Languages.Text("Launch");
            bool restart = await WirelessSetup.Install(new Progress<string>(text => Status.Text = text));
            return installed + " " + Languages.Text(restart ? "DriverRestart" : "DriverPair");
        });
    }

    private async void RestorePatch(object sender, RoutedEventArgs e)
    {
        var folder = GameFolder.Text;
        await RunOperation(async () =>
        {
            await Task.Run(() => PatchFiles.Restore(folder));
            return Languages.Text("Restored");
        });
    }

    private async Task RunOperation(Func<Task<string>> work)
    {
        busy = true;
        InstallButton.IsEnabled = RestoreButton.IsEnabled = BrowseButton.IsEnabled =
            GameFolder.IsEnabled = InstallPs3.IsEnabled = PromptMode.IsEnabled = PromptArtworkButton.IsEnabled =
                LanguagePicker.IsEnabled = false;
        Status.Text = Languages.Text("Working");
        Progress.Visibility = Visibility.Visible;
        Progress.IsIndeterminate = true;
        try { Status.Text = await work(); }
        catch (UnauthorizedAccessException) { Status.Text = Languages.Text("Permission"); }
        catch (Exception error) { Status.Text = Languages.Text("Failure") + " " + error.Message; }
        finally
        {
            busy = false;
            InstallButton.IsEnabled = RestoreButton.IsEnabled = BrowseButton.IsEnabled =
                GameFolder.IsEnabled = InstallPs3.IsEnabled = PromptMode.IsEnabled = PromptArtworkButton.IsEnabled =
                    LanguagePicker.IsEnabled = true;
            Progress.Visibility = Visibility.Collapsed;
        }
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (busy) e.Cancel = true;
        base.OnClosing(e);
    }
}
