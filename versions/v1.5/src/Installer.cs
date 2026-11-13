using System;
using System.IO;
using System.Drawing;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using Microsoft.Win32;

public static partial class PatchEngine
{
    public const string OriginalHash = "2309d5b30560103c7a8b3b97e626877fb89fd85164f82186082dc1c665466e70";
    public const string LegacyHash = "72719efdf007c46ca50c7b40b30c170a26d335595010af98839cfa0563e81d26";
    public const string PreviousHash = "ebb8b2c7168907730a3d7a57638646918581f48cfed4e20cb3afad7c4df8c1cb";
    public const string ExeHash = "9e40e25737149ab095061b7154797a35495faea97644fafc5a1e616f1400c968";
    public const string PayloadHash = "8b01a166135e760a5f0351d755b4a216dc9f4ccd84e0faf71a6baca058fe4674";
    [DllImport("kernel32.dll")] public static extern uint GetACP();
    public static void ValidateCodePage(uint codePage)
    {
        if (codePage != 936)
            throw new IOException("当前系统的非 Unicode 程序代码页为 " + codePage + "，此汉化需要简体中文代码页 936。\r\n为避免游戏出现乱码，本次操作已停止。请在兼容环境中使用；安装器不会自动修改系统区域设置。还原英文不受此限制。");
    }
    public static string Hash(string file)
    {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(file))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }
    public static byte[] Payload() { return Component("Language.Core"); }
    private static void RejectLink(string file)
    {
        if ((File.Exists(file) || Directory.Exists(file)) && (File.GetAttributes(file) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("所选目录中存在链接或重定向：" + file + "\r\n请选择实际游戏目录。");
    }
    private static string[][] Portraits(string root)
    {
        var rows = new List<string[]>();
        string folder = Path.Combine(root, "IMAGES", "CHARS");
        RejectLink(Path.Combine(root, "IMAGES")); RejectLink(folder);
        using (var stream = typeof(PatchEngine).Assembly.GetManifestResourceStream("RoadRash.Portraits"))
        using (var reader = new StreamReader(stream, Encoding.UTF8))
        {
            string line;
            while ((line = reader.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                string[] row = line.Split('\t');
                if (row.Length != 3 || Path.GetFileName(row[0]) != row[0] || Path.GetFileName(row[1]) != row[1]) throw new IOException("立绘映射清单无效。");
                rows.Add(new[] { Path.Combine(folder, row[0]), Path.Combine(folder, row[1]), row[2] });
            }
        }
        if (rows.Count != 24) throw new IOException("立绘映射清单不完整。");
        return rows.ToArray();
    }
    private static void CheckPortraits(string root)
    {
        foreach (var row in Portraits(root))
        {
            RejectLink(row[0]); RejectLink(row[1]);
            if (!File.Exists(row[0]) || Hash(row[0]) != row[2]) throw new IOException("原版车手立绘缺失或版本不匹配：" + row[0]);
            if (File.Exists(row[1]) && Hash(row[1]) != row[2]) throw new IOException("中文立绘路径已有其他文件，不会覆盖：" + row[1]);
        }
        ReadOwned(root);
    }
    private static string PortraitMarker(string root) { return Path.Combine(root, "Codex-ZH-backup-v1", "portraits-v1.3.txt"); }
    private static List<string> ReadOwned(string root)
    {
        string marker = PortraitMarker(root); RejectLink(marker);
        var owned = new List<string>();
        if (!File.Exists(marker)) return owned;
        var valid = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var row in Portraits(root)) {
            string name = Path.GetFileName(row[1]);
            valid.Add(name + "\t" + row[2]);
            valid.Add(name.Replace("荘臣", "庄臣") + "\t" + row[2]);
        }
        foreach (string line in File.ReadAllLines(marker, Encoding.UTF8))
            if (!valid.Contains(line) || owned.Contains(line)) throw new IOException("立绘安装记录无效，操作已停止。"); else owned.Add(line);
        return owned;
    }
    private static List<string[]> CreatePortraits(string root)
    {
        var created = new List<string[]>();
        var owned = ReadOwned(root);
        try
        {
            foreach (var row in Portraits(root))
            {
                if (File.Exists(row[1])) continue;
                File.Copy(row[0], row[1], false); created.Add(row);
                if (Hash(row[1]) != row[2]) throw new IOException("中文立绘副本校验失败。");
                string record = Path.GetFileName(row[1]) + "\t" + row[2];
                if (!owned.Contains(record)) owned.Add(record);
            }
            if (created.Count != 0) File.WriteAllLines(PortraitMarker(root), owned.ToArray(), Encoding.UTF8);
            return created;
        }
        catch { RollbackPortraits(root, created); throw; }
    }
    private static void RollbackPortraits(string root, List<string[]> created)
    {
        foreach (var row in created) if (File.Exists(row[1]) && Hash(row[1]) == row[2]) File.Delete(row[1]);
        string marker = PortraitMarker(root);
        if (File.Exists(marker))
        {
            var owned = ReadOwned(root);
            foreach (var row in created) owned.Remove(Path.GetFileName(row[1]) + "\t" + row[2]);
            if (owned.Count == 0) File.Delete(marker); else File.WriteAllLines(marker, owned.ToArray(), Encoding.UTF8);
        }
    }
    private static string RemovePortraits(string root)
    {
        var owned = ReadOwned(root); var retained = new List<string>();
        foreach (string record in owned)
        {
            var parts = record.Split('\t'); string file = Path.Combine(root, "IMAGES", "CHARS", parts[0]);
            RejectLink(file);
            if (!File.Exists(file)) continue;
            if (Hash(file) == parts[1]) File.Delete(file); else retained.Add(record);
        }
        string marker = PortraitMarker(root);
        if (File.Exists(marker)) { if (retained.Count == 0) File.Delete(marker); else File.WriteAllLines(marker, retained.ToArray(), Encoding.UTF8); }
        return retained.Count == 0 ? "" : "\r\n有" + retained.Count + "个立绘副本被另行修改，已保留。";
    }
    public static string SetDefaultNickname()
    {
        using (var hive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry32))
        using (var key = hive.CreateSubKey(@"SOFTWARE\Electronic Arts\RoadRash 95"))
        {
            string current = key.GetValue("Name") as string;
            if (!String.IsNullOrWhiteSpace(current)) return "\r\n已保留你原有的玩家昵称。";
            key.SetValue("Name", "刘沛松", RegistryValueKind.String);
            return "\r\n默认玩家昵称已设为刘沛松，可在游戏内修改。";
        }
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

}

public sealed class InstallerForm : Form
{
    private TextBox directory;
    private RichTextBox log;
    private Label status;
    private Button install, restore, browse, check;
    private CheckBox defaultNickname, musicNames, brandNames, saveSlots, widescreen;
    public InstallerForm()
    {
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Microsoft YaHei UI", 10F);
        Text = "暴力摩托汉化补丁 · v1.5";
        ClientSize = new Size(760, 760);
        MinimumSize = new Size(776, 799);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(247, 248, 250);
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(layout);
        var header = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty, Padding = new Padding(26, 18, 26, 18), BackColor = Color.FromArgb(25, 37, 55) };
        header.Controls.Add(new Label { Name = "HeaderTitle", Text = "暴力摩托 · 简体中文补丁", AutoSize = true, Margin = new Padding(0, 0, 0, 8), Font = new Font(Font.FontFamily, 20F, FontStyle.Bold), ForeColor = Color.White });
        header.Controls.Add(new Label { Text = "Steam 30 周年纪念版   /   经典译名 · 音乐MV · 长文本滚动", AutoSize = true, Margin = Padding.Empty, ForeColor = Color.FromArgb(196, 210, 227) });
        header.Controls.Add(new Label { Name = "PatchAuthor", Text = "汉化作者  miocean-晓风", AutoSize = true, Margin = new Padding(0, 6, 0, 0), ForeColor = Color.FromArgb(196, 210, 227) });
        layout.Controls.Add(header, 0, 0);
        var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 8, Padding = new Padding(26, 18, 26, 12), Margin = Padding.Empty };
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < 8; row++) body.RowStyles.Add(new RowStyle(row == 6 ? SizeType.Percent : SizeType.AutoSize, row == 6 ? 100 : 0));
        layout.Controls.Add(body, 0, 1);
        body.Controls.Add(new Label { Text = "游戏安装目录", AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 6) }, 0, 0);
        var pathRow = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 1, Margin = Padding.Empty };
        pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        body.Controls.Add(pathRow, 0, 1);
        directory = new TextBox { Dock = DockStyle.Fill, Margin = new Padding(0, 4, 12, 0), AccessibleName = "游戏安装目录" };
        string adjacent = AppDomain.CurrentDomain.BaseDirectory;
        string standard = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steamapps", "common", "RoadRash");
        directory.Text = File.Exists(Path.Combine(adjacent, "RoadRash95.exe")) ? adjacent : File.Exists(Path.Combine(standard, "RoadRash95.exe")) ? standard : "";
        pathRow.Controls.Add(directory, 0, 0);
        browse = MakeButton("浏览…", false);
        browse.Margin = Padding.Empty;
        pathRow.Controls.Add(browse, 1, 0);
        browse.Click += delegate {
            using (var dialog = new FolderBrowserDialog()) {
                dialog.Description = "选择包含 RoadRash95.exe 的游戏根目录";
                dialog.ShowNewFolderButton = false;
                if (Directory.Exists(directory.Text)) dialog.SelectedPath = directory.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) { directory.Text = dialog.SelectedPath; Run(false, true); }
            }
        };
        body.Controls.Add(new Label { Text = "请选择包含 RoadRash95.exe 和 TEXT 文件夹的目录。", Dock = DockStyle.Fill, AutoSize = true, Margin = new Padding(0, 8, 0, 14), ForeColor = Color.FromArgb(97, 110, 126) }, 0, 2);
        var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, Margin = Padding.Empty };
        body.Controls.Add(actions, 0, 3);
        check = MakeButton("检查目录", false);
        actions.Controls.Add(check);
        check.Click += delegate { Run(false, true); };
        install = MakeButton("安装汉化", true);
        actions.Controls.Add(install);
        install.Click += delegate { Run(false, false); };
        restore = MakeButton("还原英文", false);
        actions.Controls.Add(restore);
        restore.Click += delegate { Run(true, false); };
        status = new Label { Name = "Status", Text = "选择目录后，点击“检查目录”或直接安装。", AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 14, 0, 8), ForeColor = Color.FromArgb(62, 78, 99) };
        defaultNickname = new CheckBox { Text = "昵称为空时设为刘沛松（保留已有昵称）", AutoSize = true, Dock = DockStyle.Fill, Checked = true, Margin = new Padding(0, 10, 0, 0) };
        var options = new TableLayoutPanel { Name = "OptionalComponents", Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 2, RowCount = 4, Margin = Padding.Empty };
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50F));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50F));
        for (int row = 0; row < 4; row++) options.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        musicNames = new CheckBox { Text = "音乐译名（英文原文＋中文）", AutoSize = true };
        brandNames = new CheckBox { Text = "车辆译名（虚构品牌＋车型）", AutoSize = true };
        saveSlots = new CheckBox { Text = "游戏内五槽存档（可独立勾选）", AutoSize = true };
        widescreen = new CheckBox { Text = "宽屏扩展（第三方非商业许可组件）", AutoSize = true };
        foreach (var option in new[] { musicNames, brandNames, saveSlots, widescreen, defaultNickname })
            option.Margin = new Padding(0, 5, 12, 5);
        options.Controls.Add(musicNames, 0, 0); options.Controls.Add(brandNames, 1, 0);
        options.Controls.Add(saveSlots, 0, 1); options.Controls.Add(widescreen, 1, 1);
        options.Controls.Add(defaultNickname, 0, 2); options.SetColumnSpan(defaultNickname, 2);
        var optionActions = new FlowLayoutPanel { Name = "OptionActions", Dock = DockStyle.Fill, AutoSize = true, WrapContents = false, Margin = new Padding(0, 4, 0, 0) };
        options.Controls.Add(optionActions, 0, 3); options.SetColumnSpan(optionActions, 2);

        var importSave = MakeButton("导入／管理存档…", false);
        optionActions.Controls.Add(importSave);
        var wideLicense = MakeButton("宽屏组件许可", false);
        optionActions.Controls.Add(wideLicense);
        var manageMods = MakeButton("管理已安装功能", false);
        manageMods.Name = "ManageInstalledMods";
        optionActions.Controls.Add(manageMods);
        manageMods.Click += delegate {
            try {
                using (var manager = new InstalledModsForm(directory.Text)) {
                    if (manager.ShowDialog(this) != DialogResult.OK) return;
                    var selected = manager.SelectedFeatures;
                    musicNames.Checked = selected.Music; brandNames.Checked = selected.Brands;
                    saveSlots.Checked = selected.Saves; widescreen.Checked = selected.Wide;
                    log.Text = manager.Result;
                    status.Text = "功能变更已应用";
                    status.ForeColor = Color.FromArgb(31, 119, 74);
                }
            }
            catch (Exception error) { log.Text = error.Message; }
            log.Select(0, 0); log.ScrollToCaret();
        };
        wideLicense.Click += delegate { log.Text = PatchEngine.WidescreenLicense(); log.Select(0, 0); log.ScrollToCaret(); };
        importSave.Name = "ManageSaveSlots";
        importSave.Click += delegate {
            using (var manager = new SaveManagerForm()) manager.ShowDialog(this);
        };
        body.Controls.Add(options, 0, 4);
        body.Controls.Add(status, 0, 5);
        log = new RichTextBox { Name = "ResultLog", Dock = DockStyle.Fill, MinimumSize = new Size(0, 112), Margin = new Padding(0, 0, 0, 12), ReadOnly = true, DetectUrls = false, WordWrap = true, ScrollBars = RichTextBoxScrollBars.Vertical, BackColor = Color.White, BorderStyle = BorderStyle.FixedSingle, Text = "安装时自动校验版本并备份英文语言文件。\r\n更新语言与显示修复，并生成中文立绘副本。四项可选功能可分别启用。\r\n汉化作者  miocean-晓风。宽屏组件附作者署名和非商业许可。\r\n已支持的旧版可直接覆盖安装，原版备份需保留。", AccessibleName = "操作结果" };
        body.Controls.Add(log, 0, 6);
        body.Controls.Add(new Label { Name = "Footer", MaximumSize = new Size(708, 0), Text = "安装或还原前请退出游戏，Steam游戏语言设为English。\r\n已支持的旧版可直接覆盖安装。遇到未识别改动时，请先用旧安装器还原或通过Steam验证文件。", AutoSize = true, Dock = DockStyle.Fill, Margin = Padding.Empty, ForeColor = Color.FromArgb(97, 110, 126), Font = new Font(Font.FontFamily, 9F) }, 0, 7);
        ResumeLayout(true);
    }
    private Button MakeButton(string text, bool primary)
    {
        var button = new Button { Text = text, AutoSize = true, MinimumSize = new Size(112, 38), Padding = new Padding(12, 3, 12, 3), Margin = new Padding(0, 0, 12, 0), FlatStyle = FlatStyle.Flat, Cursor = Cursors.Hand, UseVisualStyleBackColor = false, BackColor = primary ? Color.FromArgb(35, 91, 186) : Color.White, ForeColor = primary ? Color.White : Color.FromArgb(40, 56, 79) };
        button.FlatAppearance.BorderColor = primary ? button.BackColor : Color.FromArgb(204, 211, 220);
        return button;
    }
    private void Run(bool restoring, bool checking)
    {
        install.Enabled = restore.Enabled = browse.Enabled = check.Enabled = false;
        UseWaitCursor = true;
        try
        {
            string result = checking ? PatchEngine.Check(directory.Text) : PatchEngine.Apply(directory.Text, restoring, musicNames.Checked, brandNames.Checked, saveSlots.Checked, widescreen.Checked);
            if (!checking && !restoring && defaultNickname.Checked)
            {
                try { result += PatchEngine.SetDefaultNickname(); }
                catch { result += "\r\n昵称设置未能写入，不影响汉化；可在游戏内自行设置。"; }
            }
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
        finally { log.Select(0, 0); log.ScrollToCaret(); install.Enabled = restore.Enabled = browse.Enabled = check.Enabled = true; UseWaitCursor = false; }
    }
    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new InstallerForm());
    }
}
