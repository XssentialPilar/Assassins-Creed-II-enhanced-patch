using System.Globalization;
using System.IO;
using System.Reflection;

namespace Ac2Setup;

internal sealed class Language(string tag, string name, Dictionary<string, string> strings)
{
    public string Tag { get; } = tag;
    public string Name { get; } = name;
    public Dictionary<string, string> Strings { get; } = strings;
    public override string ToString() => Name;
}

internal static class Languages
{
    private static readonly string[] Keys =
    [
        "Title", "Description", "Edition", "Folder", "Browse", "Prompts", "Assigned", "Original",
        "Artwork", "Shortcut", "Wireless", "Pair", "Ps3", "Ps3Details", "Bluetooth", "Ready",
        "Restore", "Install", "Working", "Installed", "AlreadyInstalled", "Restored", "Failure", "Launch",
        "DriverPlatform", "Download", "DriverWindow", "DriverCancelled", "DriverFailure",
        "DriverRestart", "DriverPair",
    ];
    internal static readonly IReadOnlyList<Language> All = ReadAll();
    internal static Language Current { get; set; } = Select(CultureInfo.CurrentUICulture.Name);
    internal static string Text(string key) => Current.Strings[key];
    internal static string Format(string key, params object[] values) =>
        string.Format(CultureInfo.CurrentCulture, Text(key), values);

    internal static Language Select(string tag)
    {
        if (tag.StartsWith("zh-", StringComparison.OrdinalIgnoreCase))
        {
            if (new[] { "zh-Hant", "zh-TW", "zh-HK", "zh-MO" }.Any(
                    variant => tag.StartsWith(variant, StringComparison.OrdinalIgnoreCase)))
                return All.First(language => language.Tag == "zh-Hant");
            return All.First(language => language.Tag == "zh-Hans");
        }
        return All.FirstOrDefault(language => language.Tag.Equals(tag, StringComparison.OrdinalIgnoreCase))
            ?? All.FirstOrDefault(language => language.Tag.Equals(tag.Split('-')[0], StringComparison.OrdinalIgnoreCase))
            ?? (tag.StartsWith("pt", StringComparison.OrdinalIgnoreCase) ? All.First(language => language.Tag == "pt-BR") : All[0]);
    }

    private static IReadOnlyList<Language> ReadAll()
    {
        var result = new List<Language>();
        var assembly = Assembly.GetExecutingAssembly();
        using var errors = Open(assembly, "errors");
        string[] errorKeys = (errors.ReadLine() ?? throw new IOException("Setup error titles are missing.")).Split('|');
        var errorRows = EnumerateLines(errors).Select(line => line.Split('|')).ToDictionary(fields => fields[0]);
        if (errorKeys[0] != "tag" || errorRows.Values.Any(fields => fields.Length != errorKeys.Length))
            throw new IOException("Setup error translations are invalid.");

        using var reader = Open(assembly, "setup");
        if (reader.ReadLine() != "tag|name|" + string.Join("|", Keys))
            throw new IOException("Setup translation columns do not match the interface.");
        foreach (string line in EnumerateLines(reader))
        {
            string[] fields = line.Split('|');
            if (fields.Length != Keys.Length + 2 || fields.Any(string.IsNullOrWhiteSpace))
                throw new IOException($"Invalid setup translation: {fields[0]}.");
            var strings = Keys.Select((key, index) => new { key, value = fields[index + 2] })
                .ToDictionary(item => item.key, item => item.value);
            string[] additions = errorRows[fields[0]];
            for (int index = 1; index < errorKeys.Length; index++)
                strings.Add(errorKeys[index], additions[index]);
            result.Add(new Language(fields[0], fields[1], strings));
        }
        return result;
    }

    private static StreamReader Open(Assembly assembly, string name) =>
        new(assembly.GetManifestResourceStream($"Ac2Setup.locales.{name}.txt")
            ?? throw new IOException($"Setup translations are missing: {name}."));

    private static IEnumerable<string> EnumerateLines(StreamReader reader)
    {
        while (reader.ReadLine() is { } line)
            if (!string.IsNullOrWhiteSpace(line)) yield return line;
    }
}
