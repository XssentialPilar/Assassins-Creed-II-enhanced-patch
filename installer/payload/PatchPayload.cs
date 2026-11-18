using System.IO;

namespace Ac2Setup;

public static class PatchPayload
{
    public static Stream Open() =>
        typeof(PatchPayload).Assembly.GetManifestResourceStream("Ac2Setup.Patch.zip")
        ?? throw new FileNotFoundException("The private installer DLL does not contain the patch.");
}
