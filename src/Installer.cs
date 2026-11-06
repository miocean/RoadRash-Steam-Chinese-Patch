using System;
using System.IO;
using System.Drawing;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;

public static class PatchEngine
{
    public const string OriginalHash = "2309d5b30560103c7a8b3b97e626877fb89fd85164f82186082dc1c665466e70";
    public const string PayloadHash = "72719efdf007c46ca50c7b40b30c170a26d335595010af98839cfa0563e81d26";
    public const string ExeHash = "9e40e25737149ab095061b7154797a35495faea97644fafc5a1e616f1400c968";
    public static string Hash(string file)
    {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(file))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }
    public static byte[] Payload()
    {
        using (var stream = typeof(PatchEngine).Assembly.GetManifestResourceStream("RoadRash.ZH.DLL"))
        using (var data = new MemoryStream())
        {
            if (stream == null) throw new IOException("安装器内置补丁缺失。");
            stream.CopyTo(data);
            byte[] bytes = data.ToArray();
            using (var sha = SHA256.Create())
                if (BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant() != PayloadHash)
                    throw new IOException("安装器内置补丁校验失败，请重新获取完整安装器。");
            return bytes;
        }
    }
    private static void RejectLink(string file)
    {
        if ((File.Exists(file) || Directory.Exists(file)) && (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("所选目录中存在链接或重定向：" + file + "\r\n请选择实际游戏目录。");
    }
    private static string Root(string directory)
    {
        if (String.IsNullOrWhiteSpace(directory)) throw new IOException("请先选择游戏安装目录。");
        string root = Path.GetFullPath(directory.Trim().Trim('"'));
        if (!Directory.Exists(root)) throw new IOException("所选文件夹不存在。");
        foreach (string p in new[] { root, Path.Combine(root, "RoadRash95.exe"), Path.Combine(root, "TEXT"), Path.Combine(root, "TEXT", "ENU.DLL"), Path.Combine(root, "Codex-ZH-backup-v1"), Path.Combine(root, "Codex-ZH-backup-v1", "ENU.DLL") }) RejectLink(p);
        if (!File.Exists(Path.Combine(root, "RoadRash95.exe")) || !File.Exists(Path.Combine(root, "TEXT", "ENU.DLL")))
            throw new IOException("目录不正确。请选择同时包含 RoadRash95.exe 和 TEXT 文件夹的游戏根目录。");
        return root;
    }
    public static string Check(string directory)
    {
        string root = Root(directory);
        if (Hash(Path.Combine(root, "RoadRash95.exe")) != ExeHash)
            throw new IOException("游戏主程序版本不匹配，此补丁适用于制作时的 Steam 30 周年纪念版。未修改任何游戏文件。");
        string current = Hash(Path.Combine(root, "TEXT", "ENU.DLL"));
        if (current == OriginalHash) return "检测通过：英文原版，可安装汉化。";
        if (current != PayloadHash) throw new IOException("语言文件与适配版本不符。未修改任何游戏文件。");
        string backup = Path.Combine(root, "Codex-ZH-backup-v1", "ENU.DLL");
        if (!File.Exists(backup) || Hash(backup) != OriginalHash)
            throw new IOException("已安装汉化，但未找到有效的英文备份。请先通过 Steam 恢复官方文件。");
        return "检测通过：已安装本汉化，英文备份完好，可一键还原。";
    }
    public static string Apply(string directory, bool restore)
    {
        string root = Root(directory);
        foreach (Process process in Process.GetProcessesByName("RoadRash95"))
        {
            process.Dispose();
            throw new IOException("请先完全退出暴力摩托，再安装或还原。");
        }
        string target = Path.Combine(root, "TEXT", "ENU.DLL");
        string backupDir = Path.Combine(root, "Codex-ZH-backup-v1");
        string backup = Path.Combine(backupDir, "ENU.DLL");
        string current = Hash(target);
        byte[] bytes;
        string expected;
        if (restore)
        {
            if (current == OriginalHash) return "当前已是英文原版，无需重复还原。";
            if (current != PayloadHash) throw new IOException("当前语言文件已有其他改动，为避免覆盖，本次还原已取消。");
            if (!File.Exists(backup) || Hash(backup) != OriginalHash) throw new IOException("未找到有效的英文备份，无法还原。");
            bytes = File.ReadAllBytes(backup);
            expected = OriginalHash;
        }
        else
        {
            Check(root);
            bytes = Payload();
            if (File.Exists(backup))
            {
                if (Hash(backup) != OriginalHash) throw new IOException("备份文件与英文原版不一致。本次安装已取消，不会覆盖备份。");
            }
            else
            {
                if (current != OriginalHash) throw new IOException("原始备份缺失，请先用 Steam 恢复官方文件。");
                Directory.CreateDirectory(backupDir);
                File.Copy(target, backup, false);
                if (Hash(backup) != OriginalHash) throw new IOException("英文备份校验失败，安装已取消。");
            }
            if (current == PayloadHash) return "已安装此版本汉化，无需重复安装。英文备份已校验。";
            expected = PayloadHash;
        }
        string stage = Path.Combine(root, "TEXT", "ENU.ZH-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var file = new FileStream(stage, FileMode.CreateNew, FileAccess.Write, FileShare.None)) file.Write(bytes, 0, bytes.Length);
            if (Hash(stage) != expected) throw new IOException("补丁暂存文件校验失败。");
            if (Hash(target) != current) throw new IOException("游戏文件在操作期间发生变化，已停止替换。请关闭游戏后重试。");
            File.Replace(stage, target, null);
            if (Hash(target) != expected) throw new IOException("替换后的文件校验失败。");
        }
        finally { if (File.Exists(stage)) File.Delete(stage); }
        return (restore ? "英文原版已还原。" : "汉化安装成功！从 Steam 正常启动游戏即可。") + "\r\n原始备份：" + backup;
    }
}

public sealed class InstallerForm : Form
{
    private TextBox directory;
    private TextBox log;
    private Label status;
    private Button install, restore, browse, check;
    public InstallerForm()
    {
        Text = "暴力摩托汉化补丁 · v1.1";
        ClientSize = new Size(680, 494);
        MinimumSize = new Size(696, 533);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Microsoft YaHei UI", 10F);
        BackColor = Color.FromArgb(247, 248, 250);
        var header = new Panel { Dock = DockStyle.Top, Height = 105, BackColor = Color.FromArgb(25, 37, 55) };
        header.Controls.Add(new Label { Text = "暴力摩托 · 简体中文补丁", Location = new Point(26, 19), Size = new Size(610, 34), Font = new Font(Font.FontFamily, 20F, FontStyle.Bold), ForeColor = Color.White });
        header.Controls.Add(new Label { Text = "Steam 30 周年纪念版   /   977 条译文 · 480 条人物对话", Location = new Point(28, 67), AutoSize = true, ForeColor = Color.FromArgb(196, 210, 227) });
        Controls.Add(header);
        Controls.Add(new Label { Text = "游戏安装目录", Location = new Point(26, 128), AutoSize = true });
        directory = new TextBox { Location = new Point(28, 158), Size = new Size(508, 29), AccessibleName = "游戏安装目录" };
        string adjacent = AppDomain.CurrentDomain.BaseDirectory;
        directory.Text = File.Exists(Path.Combine(adjacent, "RoadRash95.exe")) ? adjacent : "E:\\SteamLibrary\\steamapps\\common\\RoadRash";
        Controls.Add(directory);
        browse = ButtonAt("浏览…", 548, 155, 104, 34, false);
        browse.Click += delegate {
            using (var dialog = new FolderBrowserDialog()) {
                dialog.Description = "选择包含 RoadRash95.exe 的游戏根目录";
                dialog.ShowNewFolderButton = false;
                if (Directory.Exists(directory.Text)) dialog.SelectedPath = directory.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) { directory.Text = dialog.SelectedPath; Run(false, true); }
            }
        };
        Controls.Add(new Label { Text = "请选择包含 RoadRash95.exe 和 TEXT 文件夹的目录。", Location = new Point(28, 198), AutoSize = true, ForeColor = Color.FromArgb(97, 110, 126) });
        check = ButtonAt("检查目录", 28, 234, 122, 38, false);
        check.Click += delegate { Run(false, true); };
        install = ButtonAt("安装汉化", 166, 234, 150, 38, true);
        install.Click += delegate { Run(false, false); };
        restore = ButtonAt("还原英文", 332, 234, 150, 38, false);
        restore.Click += delegate { Run(true, false); };
        status = new Label { Text = "选择目录后，点击“检查目录”或直接安装。", Location = new Point(28, 294), Size = new Size(624, 28), ForeColor = Color.FromArgb(62, 78, 99) };
        Controls.Add(status);
        log = new TextBox { Location = new Point(28, 326), Size = new Size(624, 106), Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, BackColor = Color.White, BorderStyle = BorderStyle.FixedSingle, Text = "安装时自动校验版本并备份原文件。\r\n仅替换 TEXT\\ENU.DLL，可随时还原。", AccessibleName = "操作结果" };
        Controls.Add(log);
        Controls.Add(new Label { Text = "安装 / 还原前请退出游戏；Steam 中的游戏语言需设为 English。", Location = new Point(28, 451), Size = new Size(624, 26), ForeColor = Color.FromArgb(97, 110, 126), Font = new Font(Font.FontFamily, 9F) });
    }
    private Button ButtonAt(string text, int x, int y, int width, int height, bool primary)
    {
        var button = new Button { Text = text, Location = new Point(x, y), Size = new Size(width, height), FlatStyle = FlatStyle.Flat, Cursor = Cursors.Hand, UseVisualStyleBackColor = false, BackColor = primary ? Color.FromArgb(35, 91, 186) : Color.White, ForeColor = primary ? Color.White : Color.FromArgb(40, 56, 79) };
        button.FlatAppearance.BorderColor = primary ? button.BackColor : Color.FromArgb(204, 211, 220);
        Controls.Add(button);
        return button;
    }
    private void Run(bool restoring, bool checking)
    {
        install.Enabled = restore.Enabled = browse.Enabled = check.Enabled = false;
        UseWaitCursor = true;
        try
        {
            string result = checking ? PatchEngine.Check(directory.Text) : PatchEngine.Apply(directory.Text, restoring);
            status.Text = checking ? "目录检查完成" : restoring ? "还原完成" : "安装完成";
            status.ForeColor = Color.FromArgb(31, 119, 74);
            log.Text = result;
        }
        catch (Exception ex)
        {
            status.Text = "操作未完成，请查看下方说明";
            status.ForeColor = Color.FromArgb(168, 49, 49);
            log.Text = ex is UnauthorizedAccessException ? "没有写入此游戏目录的权限。请关闭游戏，并尝试以管理员身份运行安装器。" : ex.Message;
        }
        finally { install.Enabled = restore.Enabled = browse.Enabled = check.Enabled = true; UseWaitCursor = false; }
    }
    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new InstallerForm());
    }
}
