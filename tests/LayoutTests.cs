using System;
using System.IO;
using System.Drawing;
using System.Windows.Forms;
using System.Collections.Generic;
public static class LayoutTests
{
    static void RememberFonts(Control root, List<KeyValuePair<Control, Font>> fonts)
    {
        fonts.Add(new KeyValuePair<Control, Font>(root, root.Font));
        foreach (Control child in root.Controls) RememberFonts(child, fonts);
    }
    [STAThread]
    public static int Main(string[] args)
    {
        try
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Directory.CreateDirectory(args[0]);
            foreach (float factor in new[] { 1F, 1.25F, 1.5F, 2F })
            using (var form = new InstallerForm())
            {
                form.StartPosition = FormStartPosition.Manual;
                form.Location = new Point(-30000, -30000);
                form.ShowInTaskbar = false;
                form.Show();
                Application.DoEvents();
                form.CreateControl();
                form.PerformLayout();
                var fonts = new List<KeyValuePair<Control, Font>>();
                RememberFonts(form, fonts);
                form.AutoScaleMode = AutoScaleMode.None;
                form.SuspendLayout();
                form.Scale(new SizeF(factor, factor));
                foreach (var entry in fonts) entry.Key.Font = new Font(entry.Value.FontFamily, entry.Value.Size * factor, entry.Value.Style);
                form.ResumeLayout(true);
                Application.DoEvents();
                var title = (Label)form.Controls.Find("HeaderTitle", true)[0];
                var required = TextRenderer.MeasureText(title.Text, title.Font, Size.Empty, TextFormatFlags.SingleLine);
                if (title.Height < required.Height || title.Width < required.Width)
                    throw new Exception("Title clips at simulated scale " + factor + ": " + title.Size + " / " + required);
                if (title.Bottom > title.Parent.ClientSize.Height || title.Right > title.Parent.ClientSize.Width)
                    throw new Exception("Title exceeds header at simulated scale " + factor);
                var log = (RichTextBox)form.Controls.Find("ResultLog", true)[0];
                var footer = (Label)form.Controls.Find("Footer", true)[0];
                var status = (Label)form.Controls.Find("Status", true)[0];
                if (log.Top < status.Bottom || log.Bottom > footer.Top || footer.Bottom > footer.Parent.ClientSize.Height)
                    throw new Exception("Result/footer overlap or exceed body at simulated scale " + factor);
                if (log.ClientSize.Height < log.Font.Height * 3)
                    throw new Exception("Result viewport too short at simulated scale " + factor);
                if (log.GetPositionFromCharIndex(0).Y < 0 || log.GetPositionFromCharIndex(log.Text.IndexOf('\n') + 1).Y + log.Font.Height > log.ClientSize.Height)
                    throw new Exception("Initial result text clips at simulated scale " + factor);
                var footerSize = TextRenderer.MeasureText(footer.Text, footer.Font, new Size(footer.Width, Int32.MaxValue), TextFormatFlags.WordBreak);
                if (footer.Height < footerSize.Height) throw new Exception("Footer text clips at simulated scale " + factor);
                string output = Path.Combine(args[0], "installer-scale-" + (factor * 100).ToString("0") + ".png");
                using (var image = new Bitmap(form.Width, form.Height)) { form.DrawToBitmap(image, new Rectangle(Point.Empty, image.Size)); image.Save(output); }
                Console.WriteLine("PASS simulated scale {0}%: font={1:0.0}pt title={2} measured={3}", factor * 100, title.Font.Size, title.Size, required);
                log.Text = "操作未完成，请查看下方说明\n" + new string('测', 300) + "\n最后一行";
                log.Select(log.TextLength, 0); log.ScrollToCaret();
                if (log.GetPositionFromCharIndex(log.TextLength).Y + log.Font.Height > log.ClientSize.Height)
                    throw new Exception("Last result line cannot be scrolled into view at scale " + factor);
                log.Select(0, 0); log.ScrollToCaret();
                if (log.GetPositionFromCharIndex(0).Y < 0) throw new Exception("First result line not restored at scale " + factor);
                Console.WriteLine("PASS result viewport, multiline scrolling and footer at {0}%", factor * 100);
            }
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
