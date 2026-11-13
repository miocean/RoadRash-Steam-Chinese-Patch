using System;
using System.Drawing;
using System.Windows.Forms;

public sealed class InstalledFeatures
{
    public bool Music, Brands, Saves, Wide;
}

public sealed class InstalledModsForm : Form
{
    private readonly string directory;
    private readonly CheckBox music, brands, saves, wide;
    private readonly Label status;
    private readonly Button apply;
    public string Result { get; private set; }
    public InstalledFeatures SelectedFeatures
    {
        get { return new InstalledFeatures { Music = music.Checked, Brands = brands.Checked,
            Saves = saves.Checked, Wide = wide.Checked }; }
    }

    public InstalledModsForm(string gameDirectory)
    {
        directory = gameDirectory;
        InstalledFeatures installed = PatchEngine.GetInstalledFeatures(directory);
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Microsoft YaHei UI", 10F);
        Text = "管理已安装功能";
        ClientSize = new Size(620, 410);
        MinimumSize = new Size(636, 449);
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.FromArgb(247, 248, 250);
        Padding = new Padding(24);
        var content = new Panel { Name = "ModContent", Dock = DockStyle.Fill, AutoScroll = true, Margin = Padding.Empty };
        Controls.Add(content);
        var layout = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, RowCount = 6, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        for (int row = 0; row < 6; row++) layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        content.Controls.Add(layout);
        layout.Controls.Add(new Label { Text = "已安装的可选功能", AutoSize = true,
            Font = new Font(Font.FontFamily, 16F, FontStyle.Bold), Margin = new Padding(0, 0, 0, 12) }, 0, 0);
        layout.Controls.Add(new Label { Text = "取消勾选并应用，可回退相应功能。重新勾选可以启用。",
            AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 0, 0, 12) }, 0, 1);
        var options = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true,
            FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        music = new CheckBox { Name = "MusicOption", Text = "音乐双语译名", Checked = installed.Music, AutoSize = true, Margin = new Padding(0, 5, 0, 5) };
        brands = new CheckBox { Name = "BrandsOption", Text = "车辆译名（虚构品牌＋车型）", Checked = installed.Brands, AutoSize = true, Margin = new Padding(0, 5, 0, 5) };
        saves = new CheckBox { Name = "SavesOption", Text = "游戏内五槽存档", Checked = installed.Saves, AutoSize = true, Margin = new Padding(0, 5, 0, 5) };
        wide = new CheckBox { Name = "WideOption", Text = "保持比例的宽屏扩展", Checked = installed.Wide, AutoSize = true, Margin = new Padding(0, 5, 0, 5) };
        options.Controls.AddRange(new Control[] { music, brands, saves, wide });
        layout.Controls.Add(options, 0, 2);
        layout.Controls.Add(new Label { Text = "核心汉化可在主窗口点击“还原英文”回退。存档进度保留于用户目录。",
            AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 12, 0, 0), ForeColor = Color.FromArgb(97, 110, 126) }, 0, 4);
        status = new Label { Name = "ModStatus", AutoSize = true, Dock = DockStyle.Fill,
            Margin = new Padding(0, 10, 0, 8), ForeColor = Color.FromArgb(168, 49, 49) };
        layout.Controls.Add(status, 0, 5);
        var manageSaves = new Button { Name = "ManageSaveSlots", Text = "管理五槽存档…", AutoSize = true,
            MinimumSize = new Size(160, 38), Margin = new Padding(0, 4, 0, 12) };
        manageSaves.Click += delegate { using (var manager = new SaveManagerForm()) manager.ShowDialog(this); };
        layout.Controls.Add(manageSaves, 0, 3);
        var footer = new TableLayoutPanel { Name = "ModActionFooter", Dock = DockStyle.Bottom, AutoSize = true,
            ColumnCount = 1, RowCount = 2, Margin = new Padding(0, 12, 0, 0) };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        footer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        footer.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        Controls.Add(footer);
        var actions = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true,
            MinimumSize = new Size(0, 44), ColumnCount = 3, RowCount = 1, Margin = Padding.Empty };
        actions.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        actions.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        actions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
        actions.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        apply = new Button { Name = "ApplyChanges", Text = "应用变更", AutoSize = true, MinimumSize = new Size(112, 38), Margin = new Padding(0, 0, 12, 0) };
        var cancel = new Button { Name = "CancelChanges", Text = "取消", DialogResult = DialogResult.Cancel,
            AutoSize = true, MinimumSize = new Size(112, 38), Margin = Padding.Empty };
        actions.Controls.Add(apply, 0, 0); actions.Controls.Add(cancel, 1, 0);
        footer.Controls.Add(actions, 0, 0);
        footer.Controls.Add(new Label { Text = "汉化作者  miocean-晓风", AutoSize = true, Margin = new Padding(0, 12, 0, 0),
            ForeColor = Color.FromArgb(97, 110, 126) }, 0, 1);
        apply.Click += delegate {
            apply.Enabled = false;
            UseWaitCursor = true;
            try {
                InstalledFeatures selected = SelectedFeatures;
                Result = PatchEngine.Apply(directory, false, selected.Music, selected.Brands, selected.Saves, selected.Wide);
                DialogResult = DialogResult.OK;
                Close();
            }
            catch (Exception error) { status.Text = error.Message; content.ScrollControlIntoView(status); }
            finally { apply.Enabled = true; UseWaitCursor = false; }
        };
        AcceptButton = apply;
        CancelButton = cancel;
        ResumeLayout(true);
    }
}
