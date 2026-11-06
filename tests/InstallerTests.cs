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
            string portraits = Path.Combine(root, "IMAGES", "CHARS");
            Directory.CreateDirectory(portraits);
            string[] bases = { "AXLE", "BOSE", "CYDNEY", "JON", "PEARL", "RHONDA", "JIM", "MIKE" };
            foreach (string name in bases) foreach (string mood in new[] { "F", "H", "N" })
                File.Copy(Path.Combine(source, "IMAGES", "CHARS", name + "-" + mood + ".RRI"), Path.Combine(portraits, name + "-" + mood + ".RRI"), true);
            Assert(PatchEngine.Payload().Length == 263680, "embedded payload identity");
            PatchEngine.Check(root);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash && !File.Exists(backup), "check is read only");
            PatchEngine.Apply(root, false);
            Assert(PatchEngine.Hash(dll) == PatchEngine.PayloadHash && PatchEngine.Hash(backup) == PatchEngine.OriginalHash, "install and verified original backup");
            Assert(Directory.GetFiles(portraits, "*.RRI").Length == 48, "all 24 localized portraits created");
            Assert(PatchEngine.Hash(Path.Combine(portraits, "庄臣-N.RRI")) == PatchEngine.Hash(Path.Combine(portraits, "JON-N.RRI")), "legacy name portrait matches speaker");
            byte[] originalBackup = File.ReadAllBytes(backup);
            DateTime backupTime = File.GetLastWriteTimeUtc(backup);
            PatchEngine.Apply(root, false);
            Assert(File.GetLastWriteTimeUtc(backup) == backupTime, "repeat install preserves backup");
            PatchEngine.Check(root);
            PatchEngine.Apply(root, true);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "restore is byte identical");
            Assert(Directory.GetFiles(portraits, "*.RRI").Length == 24, "restore removes only created portrait copies");
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
            Assert(Directory.GetFiles(portraits, "*.RRI").Length == 24, "failed language replacement rolls back portrait copies");
            PatchEngine.Apply(root, false);
            File.WriteAllBytes(exe, new byte[] { 0, 1, 2, 3 });
            PatchEngine.Apply(root, true);
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "restore remains available after executable version changes");
            File.Copy(Path.Combine(source, "RoadRash95.exe"), exe, true);
            File.Copy(args[3], dll, true);
            PatchEngine.Check(root); PatchEngine.Apply(root, false);
            Assert(PatchEngine.Hash(dll) == PatchEngine.PayloadHash && PatchEngine.Hash(backup) == PatchEngine.OriginalHash, "v1.0-v1.2 upgrade preserves original backup");
            PatchEngine.Apply(root, true);
            string alias = Path.Combine(portraits, "庄臣-N.RRI"), sourcePortrait = Path.Combine(portraits, "JON-N.RRI");
            File.Copy(sourcePortrait, alias, false);
            PatchEngine.Apply(root, false); PatchEngine.Apply(root, true);
            Assert(File.Exists(alias) && PatchEngine.Hash(alias) == PatchEngine.Hash(sourcePortrait), "pre-existing identical portrait is not owned or removed");
            File.WriteAllText(alias, "other portrait");
            Refuses(delegate { PatchEngine.Apply(root, false); }, "conflicting portrait copy protected");
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "portrait conflict leaves original language unchanged");
            File.Delete(alias);
            File.Move(sourcePortrait, sourcePortrait + ".test-backup");
            try { Refuses(delegate { PatchEngine.Apply(root, false); }, "missing original portrait rejected before installation"); }
            finally { File.Move(sourcePortrait + ".test-backup", sourcePortrait); }
            PatchEngine.Apply(root, false);
            File.WriteAllText(alias, "user edited portrait");
            PatchEngine.Apply(root, true);
            Assert(File.ReadAllText(alias) == "user edited portrait", "restore preserves user-modified portrait copy");
            Assert(PatchEngine.Hash(dll) == PatchEngine.OriginalHash, "language restore succeeds while preserving modified portrait");
            File.Delete(alias); PatchEngine.Apply(root, true);
            Assert(!File.Exists(Path.Combine(root, "Codex-ZH-backup-v1", "portraits-v1.3.txt")), "portrait ownership record cleared after cleanup");
            Console.WriteLine("All installer tests passed. Fixture restored to original English.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
}
