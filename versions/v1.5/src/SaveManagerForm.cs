using System;
using System.IO;
using System.Drawing;
using System.Diagnostics;
using System.Windows.Forms;

public sealed class SaveManagerForm : Form
{
    private readonly string folder;
    private readonly ListBox slots;
    private readonly Label status;
    private readonly Button export, clear;

    public SaveManagerForm() : this(PatchEngine.SaveDirectory) { }

    internal SaveManagerForm(string saveFolder)
    {
        folder = saveFolder;
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Microsoft YaHei UI", 10F);
        Text = "管理五槽存档";
        ClientSize = new Size(680, 480);
        MinimumSize = new Size(600, 450);
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.FromArgb(247, 248, 250);
        Padding = new Padding(24);
        var close = new Button { Name = "CloseSaveManager", Text = "关闭", AutoSize = true,
            MinimumSize = new Size(112, 38), DialogResult = DialogResult.Cancel, Dock = DockStyle.Bottom };
        var content = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        Controls.Add(content);
        Controls.Add(close);
        var layout = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, RowCount = 5 };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        for (int row = 0; row < 5; row++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        content.Controls.Add(layout);
        layout.Controls.Add(new Label { Text = "选择存档槽后导入、导出、重命名或清空进度。覆盖和清空前会自动备份。\r\n存档操作立即生效。修改前请退出游戏。名称最多12个字符，与游戏内共用。",
            AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 12) }, 0, 0);
        slots = new ListBox { Name = "ManagedSaveSlots", Dock = DockStyle.Fill, Height = 155,
            IntegralHeight = false, HorizontalScrollbar = true, Margin = new Padding(0, 0, 0, 12) };
        layout.Controls.Add(slots, 0, 1);
        var actions = new FlowLayoutPanel { Name = "SaveManagerActions", Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
        layout.Controls.Add(actions, 0, 2);
        AddAction(actions, "导入到所选槽…", "ImportIntoSlot", delegate { Import(false); });
        export = AddAction(actions, "导出所选槽…", "ExportSaveSlot", Export);
        clear = AddAction(actions, "清空所选槽", "ClearSaveSlot", Clear);
        AddAction(actions, "重命名…", "RenameSaveSlot", delegate { Run(Rename); });
        AddAction(actions, "从备份恢复…", "RestoreSaveBackup", delegate { Import(true); });
        AddAction(actions, "打开备份目录", "OpenSaveBackups", delegate {
            Run(delegate {
                string backups = Path.Combine(folder, "Backups");
                if (!Directory.Exists(backups)) { status.Text = "还没有自动备份。覆盖或清空槽位时会生成备份。"; return; }
                Process.Start(new ProcessStartInfo(backups) { UseShellExecute = true });
            });
        });
        AddAction(actions, "刷新", "RefreshSaveSlots", ReloadSlots);
        status = new Label { Name = "SaveManagerStatus", AutoSize = true, Dock = DockStyle.Fill,
            ForeColor = Color.FromArgb(40, 56, 79), Margin = new Padding(0, 12, 0, 12) };
        layout.Controls.Add(status, 0, 3);
        layout.Controls.Add(new Label { Text = "存档目录  " + folder, AutoSize = true, Dock = DockStyle.Fill,
            ForeColor = Color.FromArgb(97, 110, 126), Margin = new Padding(0, 0, 0, 12) }, 0, 4);
        slots.SelectedIndexChanged += delegate { UpdateActions(); };
        CancelButton = close;
        ReloadSlots();
        ResumeLayout(true);
    }

    private Button AddAction(FlowLayoutPanel parent, string text, string name, Action action)
    {
        var button = new Button { Text = text, Name = name, AutoSize = true, MinimumSize = new Size(112, 38),
            Margin = new Padding(0, 0, 10, 8) };
        button.Click += delegate { action(); };
        parent.Controls.Add(button);
        return button;
    }

    internal void ReloadSlots()
    {
        int selected = Math.Max(0, slots.SelectedIndex);
        try
        {
            SaveSlotInfo[] information = PatchEngine.ReadSaveSlots(folder);
            slots.BeginUpdate();
            slots.Items.Clear();
            slots.Items.AddRange(information);
            slots.SelectedIndex = selected;
            slots.EndUpdate();
            UpdateActions();
        }
        catch (Exception error) { status.Text = error.Message; }
    }

    private void UpdateActions()
    {
        var selected = slots.SelectedItem as SaveSlotInfo;
        export.Enabled = selected != null && selected.Exists;
        clear.Enabled = selected != null && selected.Exists;
    }

    private int SelectedSlot { get { return slots.SelectedIndex + 1; } }

    private void Run(Action action)
    {
        try { action(); }
        catch (Exception error) { status.Text = error.Message; }
        finally { ReloadSlots(); }
    }

    private void Import(bool backup)
    {
        using (var dialog = new OpenFileDialog { Title = backup ? "选择要恢复的备份，再导入所选槽位" : "选择原版RRS存档",
            Filter = "Road Rash 存档 (*.RRS)|*.RRS", CheckFileExists = true })
        {
            if (backup)
            {
                string backups = Path.Combine(folder, "Backups");
                if (!Directory.Exists(backups)) { status.Text = "还没有自动备份。"; return; }
                dialog.InitialDirectory = backups;
            }
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            int slot = SelectedSlot;
            Run(delegate {
                SaveSlotInfo selected = PatchEngine.ReadSaveSlots(folder)[slot - 1];
                if (selected.Exists && MessageBox.Show(this, "将覆盖存档槽 " + slot + "。当前进度会先自动备份，是否继续？",
                    "确认导入", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
                status.Text = PatchEngine.ImportSaveIntoSlot(dialog.FileName, folder, slot, backup);
            });
        }
    }

    private void Export()
    {
        int slot = SelectedSlot;
        using (var dialog = new SaveFileDialog { Title = "导出存档槽 " + slot, Filter = "Road Rash 存档 (*.RRS)|*.RRS",
            DefaultExt = "RRS", AddExtension = true, FileName = "RoadRash-Slot" + slot + ".RRS", OverwritePrompt = true })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            Run(delegate { PatchEngine.ExportSaveSlot(folder, slot, dialog.FileName); status.Text = "已导出到 " + dialog.FileName; });
        }
    }

    private void Clear()
    {
        int slot = SelectedSlot;
        if (MessageBox.Show(this, "清空存档槽 " + slot + " 的进度和名称，并自动备份当前内容？",
            "确认清空", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        Run(delegate { status.Text = PatchEngine.ClearSaveSlot(folder, slot); });
    }

    private void Rename()
    {
        int slot = SelectedSlot;
        using (var dialog = new Form { Text = "重命名存档槽 " + slot, ClientSize = new Size(370, 145),
            Font = Font, StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false, MinimizeBox = false })
        {
            var label = new Label { Text = "存档名称（最多12个字符）", AutoSize = true, Location = new Point(20, 15) };
            var input = new TextBox { Name = "SaveNameInput", Text = PatchEngine.ReadSlotName(folder, slot),
                MaxLength = 12, Location = new Point(20, 45), Width = 330 };
            var confirm = new Button { Text = "确定", Location = new Point(140, 95), Size = new Size(100, 32) };
            var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, Location = new Point(250, 95), Size = new Size(100, 32) };
            dialog.Controls.AddRange(new Control[] { label, input, confirm, cancel });
            dialog.AcceptButton = confirm; dialog.CancelButton = cancel;
            confirm.Click += delegate {
                try { PatchEngine.RenameSaveSlot(folder, slot, input.Text); dialog.DialogResult = DialogResult.OK; }
                catch (Exception error) { MessageBox.Show(dialog, error.Message, "名称未保存", MessageBoxButtons.OK, MessageBoxIcon.Information); }
            };
            if (dialog.ShowDialog(this) == DialogResult.OK) { status.Text = "存档槽 " + slot + " 已重命名。"; ReloadSlots(); }
        }
    }
}
