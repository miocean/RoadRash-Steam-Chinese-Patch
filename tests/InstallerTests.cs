using System;
using System.IO;
using System.Text;
public static class InstallerTests
{
    static void Assert(bool value, string name) { if (!value) throw new Exception(name); Console.WriteLine("PASS " + name); }
    static void Refuses(Action action, string name) { bool failed = false; try { action(); } catch (IOException) { failed = true; } Assert(failed, name); }
    public static int Main(string[] args)
    {
        try
        {
            string source = args[0], root = args[1], originalFile = args[2];
            PatchEngine.ValidateCodePage(936);
            Refuses(delegate { PatchEngine.ValidateCodePage(1252); }, "Western ANSI code page rejected");
            Refuses(delegate { PatchEngine.ValidateCodePage(65001); }, "UTF-8 system code page rejected");
            Directory.CreateDirectory(Path.Combine(root, "TEXT"));
            string exe = Path.Combine(root, "RoadRash95.exe"), dll = Path.Combine(root, "TEXT", "ENU.DLL"), backup = Path.Combine(root, "Codex-ZH-backup-v1", "ENU.DLL");
            File.Copy(Path.Combine(source, "RoadRash95.exe"), exe, true);
            File.Copy(originalFile, dll, true);
            Assert(PatchEngine.Payload().Length == 254464, "embedded payload identity");
            PatchEngine.Check(root);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash && !File.Exists(backup), "check is read only");
            PatchEngine.Apply(root, false);
            Assert(PatchEngine.Hash(dll) == PatchEngine.PayloadHash && PatchEngine.Hash(backup) == PatchEngine.OriginalHash, "install and verified original backup");
            byte[] originalBackup = File.ReadAllBytes(backup);
            DateTime backupTime = File.GetLastWriteTimeUtc(backup);
            PatchEngine.Apply(root, false);
            Assert(File.GetLastWriteTimeUtc(backup) == backupTime, "repeat install preserves backup");
            PatchEngine.Check(root);
            PatchEngine.Apply(root, true);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "restore is byte identical");
            PatchEngine.Apply(root, true);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "repeat restore is safe");
            Refuses(delegate { PatchEngine.Apply(Path.Combine(root, "TEXT"), false); }, "wrong folder rejected");
            byte[] bytes = File.ReadAllBytes(exe); bytes[100] ^= 1; File.WriteAllBytes(exe, bytes);
            Refuses(delegate { PatchEngine.Apply(root, false); }, "wrong executable version rejected");
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "version rejection leaves language unchanged");
            File.Copy(Path.Combine(source, "RoadRash95.exe"), exe, true);
            File.WriteAllText(backup, "unrelated backup", Encoding.UTF8);
            Refuses(delegate { PatchEngine.Apply(root, false); }, "existing unrelated backup protected");
            Assert(File.ReadAllText(backup, Encoding.UTF8) == "unrelated backup" && PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "backup and target unchanged on rejection");
            File.WriteAllBytes(backup, originalBackup);
            File.WriteAllText(dll, "other language modification", Encoding.UTF8);
            Refuses(delegate { PatchEngine.Apply(root, false); }, "other language patch protected during install");
            Refuses(delegate { PatchEngine.Apply(root, true); }, "other language patch protected during restore");
            Assert(File.ReadAllText(dll, Encoding.UTF8) == "other language modification", "unrelated language file unchanged");
            File.Copy(originalFile, dll, true);
            using (var locked = new FileStream(dll, FileMode.Open, FileAccess.Read, FileShare.Read))
                Refuses(delegate { PatchEngine.Apply(root, false); }, "locked language file rejected");
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash && PatchEngine.Hash(backup) == PatchEngine.OriginalHash, "locked file leaves target and backup intact");
            Assert(Directory.GetFiles(Path.Combine(root, "TEXT"), "ENU.ZH-*.tmp").Length == 0, "failed replacement leaves no staged files");
            PatchEngine.Apply(root, false);
            File.WriteAllBytes(exe, new byte[] { 0, 1, 2, 3 });
            PatchEngine.Apply(root, true);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "restore remains available after executable version changes");
            File.Copy(Path.Combine(source, "RoadRash95.exe"), exe, true);
            Console.WriteLine("All installer tests passed. Fixture restored to original English.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
