using System.Text.RegularExpressions;

namespace OnimushaPS5Adapter.Manager;

/// <summary>Translate the stable messages returned by the shared install API.</summary>
internal static class ServiceText
{
    private static readonly (string Pattern, string Key)[] Messages =
    [
        (@"^所选目录不是《鬼武者 剑之道》游戏目录。$", "InstallInvalidGame"),
        (@"^请先退出游戏再安装或修复。$", "InstallExitGame"),
        (@"^版本 (.+) 已安装到游戏目录。$", "InstallComplete"),
        (@"^没有找到已安装的桥接组件。$", "BridgeNotFound"),
        (@"^请先退出游戏再卸载。$", "UninstallExitGame"),
        (@"^桥接组件已卸载。$", "UninstallComplete"),
        (@"^桥接同步校验失败：安装负载缺少 (.+)。$", "SyncPayloadMissing"),
        (@"^桥接同步校验失败：游戏目录缺少 (.+)。$", "SyncGameMissing"),
        (@"^桥接同步校验失败：(.+) 未更新为当前安装包版本，请关闭游戏后重试。$", "SyncVersionMismatch"),
        (@"^桥接同步校验通过：DLL 与辅助程序均已更新为当前安装包版本。$", "SyncComplete"),
        (@"^安装负载缺少文件。$", "PayloadFileMissing"),
        (@"^文件覆盖后校验失败：(.+)$", "FileVerificationFailed"),
        (@"^安装失败：(.*)$", "InstallFailed"),
        (@"^卸载失败：(.*)$", "UninstallFailed"),
        (@"^桥接同步校验失败：(.*)$", "SyncFailed")
    ];

    internal static string Translate(string message)
    {
        if (UiText.Language == "zh-CN") return message;
        return string.Join('\n', message.Replace("\r\n", "\n").Split('\n').Select(TranslateLine));
    }

    private static string TranslateLine(string line)
    {
        foreach (var (pattern, key) in Messages)
        {
            var match = Regex.Match(line, pattern, RegexOptions.CultureInvariant);
            if (!match.Success) continue;
            return UiText.Get(key, match.Groups.Cast<Group>().Skip(1)
                .Select(group => (object?)TranslateLine(group.Value)).ToArray());
        }
        // Preserve Windows and third-party diagnostic details verbatim.
        return line;
    }
}
