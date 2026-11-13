using System;
using System.IO;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Diagnostics;

public static partial class PatchEngine
{
    private const string PreviousMusicHash = "4766ea9da064e2df1650d9c46780fa5309384780889d0be2a6601056001048af";
    private const string PreviousMusicBrandsHash = "eb9cfe3dce0befabe142b22934025c1d6a40d1fea047ab860b322082e583ecde";
    private const string EarlierCoreHash = "55128ff42967210bb7b20f05bff95551af8afc05ff98d412788143ba91334a27";
    private const string EarlierBrandsHash = "b7daa3e97e83cd9f709731af30c976ea7cb8ed86c1e9070184ae8c62d11e159e";
    private const string EarlierMusicHash = "24ae647a5a0c1f580a539eaee668f97bf29d4bac827a957b667ec604de0bf3e7";
    private const string EarlierMusicBrandsHash = "5febeb1a3586bc794b2a39a96efc5cc814a3afc9aafd458742381b463311f04b";
    private const string V15BrandsHash = "6cec2426146b5f796ce09c2e87321e85cb11fe763bcbc8cbf16279126707ef67";
    private const string V15MusicBrandsHash = "bc4d3de8ccc66bb4dd49e994fe5e29cb22c08c94da67b505901c8f8bc880b78a";
    private static Dictionary<string, string[]> Components()
    {
        var result = new Dictionary<string, string[]>();
        using (var reader = new StreamReader(Resource("components.tsv"), Encoding.UTF8))
        {
            string line;
            while ((line = reader.ReadLine()) != null)
            {
                string[] parts = line.Split('\t');
                if (parts.Length < 2) throw new IOException("组件清单不完整。");
                result.Add(parts[0], parts);
            }
        }
        return result;
    }

    private static Stream Resource(string name)
    {
        Stream stream = typeof(PatchEngine).Assembly.GetManifestResourceStream("RoadRash." + name);
        if (stream == null) throw new IOException("内置组件缺失 " + name);
        return stream;
    }

    private static string BytesHash(byte[] bytes)
    {
        using (var sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }

    private static byte[] Component(string name)
    {
        using (Stream stream = Resource(name))
        using (var data = new MemoryStream())
        {
            stream.CopyTo(data);
            byte[] bytes = data.ToArray();
            if (BytesHash(bytes) != Components()[name][1]) throw new IOException("内置组件校验失败 " + name);
            return bytes;
        }
    }

    private static bool KnownLanguage(string hash)
    {
        if (hash == OriginalHash || hash == LegacyHash || hash == PreviousHash || hash == PayloadHash) return true;
        if (hash == PreviousMusicHash || hash == PreviousMusicBrandsHash) return true;
        if (hash == V15BrandsHash || hash == V15MusicBrandsHash) return true;
        if (hash == EarlierCoreHash || hash == EarlierBrandsHash || hash == EarlierMusicHash || hash == EarlierMusicBrandsHash) return true;
        foreach (var pair in Components())
            if (pair.Key.StartsWith("Language.") && pair.Value[1] == hash) return true;
        return false;
    }

    private static bool KnownExecutable(string hash)
    {
        foreach (var pair in Components())
            if (pair.Key.EndsWith(".delta") && pair.Value[2] == hash) return true;
        return hash == ExeHash ||
            hash == "2199e83d29b8040dfa6c171c29572b59ab09513054ddc6465ed007bde1c5fe32" ||
            hash == "60b437e3657b3c5241f0ff3243a305cd725fe3e2eb7c4c5fc1fd54bd43236de0" ||
            hash == "c218eddcc36ddde4b71f4b416e1f436f55b1e7d4dc645d2a816f92316dabfb1c" ||
            hash == "df8ead3ec57f9f45c2c12860a44a973978531de88819c658c568576c711b2f2b";
    }

    private static string Backup(string root, string filename)
    {
        string path = Path.Combine(root, "Codex-ZH-backup-v1", filename);
        RejectLink(path);
        return path;
    }

    private static void ValidateFiles(string root, bool restoring)
    {
        if (!restoring) ValidateCodePage(GetACP());
        string language = Hash(Path.Combine(root, "TEXT", "ENU.DLL"));
        string executable = Hash(Path.Combine(root, "RoadRash95.exe"));
        if (!KnownLanguage(language)) throw new IOException("语言文件包含未识别的改动。请先用旧汉化安装器还原，或通过Steam验证游戏文件，再安装本补丁。");
        if (!KnownExecutable(executable)) throw new IOException("主程序包含未识别的改动。请先用旧补丁安装器还原，或通过Steam验证游戏文件，再安装本补丁。");
        foreach (string filename in new[] { "ENU.DLL", "RoadRash95.exe" })
        {
            string expected = filename == "ENU.DLL" ? OriginalHash : ExeHash;
            string current = filename == "ENU.DLL" ? language : executable;
            string backup = Backup(root, filename);
            if (File.Exists(backup) && Hash(backup) != expected) throw new IOException("原版备份校验失败 " + backup);
            if (current != expected && !File.Exists(backup)) throw new IOException("原版备份缺失 " + backup);
        }
        if (!restoring) CheckPortraits(root);
    }

    public static string Check(string directory)
    {
        string root = Root(directory);
        if (Directory.Exists(Path.Combine(root, "Codex-ZH-backup-v1", "transaction")))
            throw new IOException("发现未完成的安装事务。请再次安装或还原，安装器会先恢复上次状态。");
        ValidateFiles(root, false);
        ReadNativeOwned(root);
        return "版本与备份检查通过，可直接安装或覆盖升级。请保留原版备份。音乐、虚构品牌、五槽存档和宽屏可分别选择。";
    }

    public static InstalledFeatures GetInstalledFeatures(string directory)
    {
        string root = Root(directory);
        if (Directory.Exists(Path.Combine(root, "Codex-ZH-backup-v1", "transaction")))
            throw new IOException("发现未完成的安装事务。请先在主窗口再次安装或还原，恢复后再管理功能。");
        ValidateFiles(root, true);
        string language = Hash(Path.Combine(root, "TEXT", "ENU.DLL"));
        if (language == OriginalHash) throw new IOException("尚未安装本汉化补丁。请先在主窗口安装，再管理可选功能。");
        var components = Components();
        var owned = ReadNativeOwned(root);
        foreach (var pair in owned)
        {
            string file = Path.Combine(root, pair.Key);
            RejectLink(file);
            if (pair.Key != "RRWideScreen.ini" && File.Exists(file) && Hash(file) != pair.Value)
                throw new IOException("已安装组件被修改，请先检查该文件 " + file);
        }
        bool combined = language == components["Language.MusicBrands"][1] || language == PreviousMusicBrandsHash || language == EarlierMusicBrandsHash || language == V15MusicBrandsHash;
        return new InstalledFeatures {
            Music = combined || language == components["Language.Music"][1] || language == PreviousMusicHash || language == EarlierMusicHash,
            Brands = combined || language == components["Language.Brands"][1] || language == EarlierBrandsHash || language == V15BrandsHash,
            Saves = owned.ContainsKey("RRSaveSlots.dll") && File.Exists(Path.Combine(root, "RRSaveSlots.dll")),
            Wide = owned.ContainsKey("RRWideScreen.dll") && File.Exists(Path.Combine(root, "RRWideScreen.dll"))
        };
    }

    private static byte[] PatchedExecutable(byte[] original, bool saves, bool wide)
    {
        string name = wide ? saves ? "SaveWide.delta" : "Wide.delta" : saves ? "Save.delta" : "Core.delta";
        using (var reader = new BinaryReader(new MemoryStream(Component(name))))
        {
            if (Encoding.ASCII.GetString(reader.ReadBytes(4)) != "RRD1" || reader.ReadInt32() != original.Length)
                throw new IOException("主程序差分格式错误。");
            int length = reader.ReadInt32();
            int count = reader.ReadInt32();
            if (length < original.Length || length > 8 * 1024 * 1024 || count < 0 || count > length)
                throw new IOException("主程序差分范围错误。");
            byte[] result = new byte[length];
            Buffer.BlockCopy(original, 0, result, 0, original.Length);
            for (int index = 0; index < count; index++)
            {
                int offset = reader.ReadInt32();
                int size = reader.ReadInt32();
                if (offset < 0 || size < 0 || offset > length - size) throw new IOException("主程序差分越界。");
                byte[] bytes = reader.ReadBytes(size);
                if (bytes.Length != size) throw new IOException("主程序差分截断。");
                Buffer.BlockCopy(bytes, 0, result, offset, size);
            }
            if (BytesHash(result) != Components()[name][2]) throw new IOException("生成的主程序校验失败。");
            return result;
        }
    }

    private static readonly string[] NativeNames = { "RRMusicLayout.dll", "RRSaveSlots.dll", "RRSaveBackground.bmp",
        "RRWideScreen.dll", "RRWideScreen.ini", "RRWideScreen-LICENSE.txt", "RRWideScreen-NOTICE.txt" };
    private static Dictionary<string, string> ReadNativeOwned(string root)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string marker = Backup(root, "components-owned.tsv");
        if (!File.Exists(marker)) return result;
        foreach (string line in File.ReadAllLines(marker, Encoding.UTF8))
        {
            string[] parts = line.Split('\t');
            if (parts.Length != 2 || Array.IndexOf(NativeNames, parts[0]) < 0 || parts[1].Length != 64)
                throw new IOException("组件安装记录无效。");
            result.Add(parts[0], parts[1]);
        }
        return result;
    }

    private static byte[] SaveBackground(string root)
    {
        string source = Path.Combine(root, "IMAGES", "RESTROOM", "REST1.RRI");
        RejectLink(Path.Combine(root, "IMAGES", "RESTROOM")); RejectLink(source);
        if (!File.Exists(source) || Hash(source) != "9ab755c87ce07e62bc34384024b57ef720448314dc859638de6706b895ce5186")
            throw new IOException("五槽存档所用原版背景缺失或版本不匹配。");
        using (Image image = Image.FromFile(source))
        using (var bitmap = new Bitmap(image))
        using (var data = new MemoryStream())
        {
            bitmap.Save(data, ImageFormat.Bmp);
            return data.ToArray();
        }
    }

    private static byte[] Lines(IEnumerable<string> lines)
    {
        var rows = new List<string>(lines);
        return rows.Count == 0 ? new byte[0] : Encoding.UTF8.GetBytes(String.Join("\r\n", rows.ToArray()) + "\r\n");
    }

    private static void PlanNative(string root, bool restore, bool saves, bool wide, Dictionary<string, byte[]> plan)
    {
        var owned = ReadNativeOwned(root);
        var next = new List<string>();
        foreach (string name in NativeNames)
        {
            string target = Path.Combine(root, name); RejectLink(target);
            bool wideFile = name.StartsWith("RRWideScreen");
            bool wanted = !restore && (name == "RRMusicLayout.dll" || (wideFile ? wide : saves));
            // The renderer writes resolution preferences back to its own INI.
            // Keep user changes across both option switches and restoration.
            if (name == "RRWideScreen.ini" && owned.ContainsKey(name) && File.Exists(target) && Hash(target) != owned[name])
            { next.Add(name + "\t" + owned[name]); continue; }
            if (File.Exists(target) && (!owned.ContainsKey(name) || Hash(target) != owned[name]))
                throw new IOException("组件位置已有其他文件或被修改，请先移开后重试 " + target);
            if (wanted)
            {
                byte[] bytes = name == "RRSaveBackground.bmp" ? SaveBackground(root) : Component(name);
                plan[target] = bytes;
                next.Add(name + "\t" + BytesHash(bytes));
            }
            else if (owned.ContainsKey(name)) plan[target] = null;
        }
        string marker = Backup(root, "components-owned.tsv");
        plan[marker] = next.Count == 0 ? null : Lines(next);
    }

    private static int PlanPortraits(string root, bool restore, Dictionary<string, byte[]> plan)
    {
        var owned = ReadOwned(root);
        int retained = 0;
        if (restore)
        {
            var remaining = new List<string>();
            foreach (string record in owned)
            {
                string[] parts = record.Split('\t');
                string target = Path.Combine(root, "IMAGES", "CHARS", parts[0]); RejectLink(target);
                if (!File.Exists(target)) continue;
                if (Hash(target) == parts[1]) plan[target] = null;
                else { remaining.Add(record); retained++; }
            }
            owned = remaining;
        }
        else
        {
            foreach (string[] row in Portraits(root))
            {
                if (File.Exists(row[1])) continue;
                plan[row[1]] = File.ReadAllBytes(row[0]);
                string record = Path.GetFileName(row[1]) + "\t" + row[2];
                if (!owned.Contains(record)) owned.Add(record);
            }
        }
        plan[PortraitMarker(root)] = owned.Count == 0 ? null : Lines(owned);
        return retained;
    }

    // Every replaced file has a durable pre-install snapshot. A later run rolls
    // back an interrupted transaction before it attempts another configuration.
    private static void Recover(string root)
    {
        string folder = Backup(root, "transaction");
        if (!Directory.Exists(folder)) return;
        RejectLink(folder);
        string manifest = Path.Combine(folder, "journal.tsv"); RejectLink(manifest);
        if (File.Exists(manifest))
        {
            string[] records = File.ReadAllLines(manifest, Encoding.UTF8);
            for (int index = records.Length - 1; index >= 0; index--)
            {
                string[] parts = records[index].Split('\t');
                if (parts.Length != 4) throw new IOException("安装恢复记录无效。");
                string target = SafeTransactionTarget(root, parts[0]);
                string current = File.Exists(target) ? Hash(target) : "-";
                if (current == parts[1]) continue;
                if (current != parts[2]) throw new IOException("待恢复文件被另行修改，请保留备份并人工检查 " + target);
                if (parts[1] == "-") File.Delete(target);
                else
                {
                    string snapshot = Path.Combine(folder, parts[3]); RejectLink(snapshot);
                    if (Path.GetFileName(parts[3]) != parts[3] || Hash(snapshot) != parts[1]) throw new IOException("恢复快照校验失败。");
                    ReplaceBytes(target, File.ReadAllBytes(snapshot));
                }
            }
        }
        // This exact workspace-local directory is created and owned by this installer.
        foreach (string file in Directory.GetFiles(folder)) { RejectLink(file); File.Delete(file); }
        Directory.Delete(folder);
    }

    private static string SafeTransactionTarget(string root, string relative)
    {
        string target = Path.GetFullPath(Path.Combine(root, relative));
        string prefix = root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!target.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) throw new IOException("安装恢复路径越界。");
        string current = target;
        while (current.Length > root.Length) { RejectLink(current); current = Path.GetDirectoryName(current); }
        return target;
    }

    private static void ReplaceBytes(string target, byte[] bytes)
    {
        if (bytes == null) { if (File.Exists(target)) File.Delete(target); return; }
        string stage = target + ".rrzh-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var file = new FileStream(stage, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { file.Write(bytes, 0, bytes.Length); file.Flush(true); }
            if (File.Exists(target)) File.Replace(stage, target, null);
            else File.Move(stage, target);
        }
        finally { if (File.Exists(stage)) File.Delete(stage); }
    }

    private static void Commit(string root, Dictionary<string, byte[]> plan)
    {
        string folder = Backup(root, "transaction");
        Directory.CreateDirectory(folder);
        var records = new List<string>();
        var changed = new List<KeyValuePair<string, byte[]>>();
        try
        {
            foreach (var pair in plan)
            {
                byte[] previous = File.Exists(pair.Key) ? File.ReadAllBytes(pair.Key) : null;
                string before = previous == null ? "-" : BytesHash(previous);
                if (pair.Key == Path.Combine(root, "TEXT", "ENU.DLL") && !KnownLanguage(before))
                    throw new IOException("语言文件在安装准备期间发生变化。");
                if (pair.Key == Path.Combine(root, "RoadRash95.exe") && !KnownExecutable(before))
                    throw new IOException("主程序在安装准备期间发生变化。");
                string after = pair.Value == null ? "-" : BytesHash(pair.Value);
                if (before == after) continue;
                string filename = records.Count + ".bak";
                if (previous != null) ReplaceBytes(Path.Combine(folder, filename), previous);
                string relative = pair.Key.Substring(root.TrimEnd(Path.DirectorySeparatorChar).Length + 1);
                records.Add(relative + "\t" + before + "\t" + after + "\t" + filename);
                changed.Add(pair);
            }
            ReplaceBytes(Path.Combine(folder, "journal.tsv"), Lines(records));
            for (int index = 0; index < changed.Count; index++)
            {
                var pair = changed[index];
                string[] record = records[index].Split('\t');
                string current = File.Exists(pair.Key) ? Hash(pair.Key) : "-";
                if (current != record[1]) throw new IOException("安装期间文件发生变化 " + pair.Key);
                ReplaceBytes(pair.Key, pair.Value);
                if ((File.Exists(pair.Key) ? Hash(pair.Key) : "-") != record[2]) throw new IOException("安装后校验失败 " + pair.Key);
            }
            File.Delete(Path.Combine(folder, "journal.tsv"));
        }
        catch
        {
            Recover(root);
            throw;
        }
        foreach (string file in Directory.GetFiles(folder)) File.Delete(file);
        Directory.Delete(folder);
    }

    public static string Apply(string directory, bool restore)
    { return Apply(directory, restore, false, false, false); }

    public static string WidescreenLicense()
    {
        return Encoding.UTF8.GetString(Component("RRWideScreen-NOTICE.txt")) + "\r\n" +
            Encoding.UTF8.GetString(Component("RRWideScreen-LICENSE.txt"));
    }

    public static string ImportSave(string filename)
    {
        string folder = SaveDirectory;
        return ImportSaveToFolder(filename, folder);
    }

    internal static string ImportSaveToFolder(string filename, string folder)
    {
        byte[] bytes;
        using (var source = new FileStream(filename, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var reader = new BinaryReader(source))
        {
            if (source.Length != 160) throw new IOException("所选文件长度与本版原生RRS存档不符。");
            bytes = reader.ReadBytes(160);
            if (bytes.Length != 160) throw new IOException("存档读取不完整。");
        }
        Directory.CreateDirectory(folder);
        RejectLink(folder);
        for (int slot = 1; slot <= SaveSlotCount; slot++)
        {
            string destination = Path.Combine(folder, "Slot" + slot + ".RRS");
            string lockPath = destination + ".lock";
            RejectLink(destination); RejectLink(lockPath);
            using (var slotLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
            {
                if (File.Exists(destination)) continue;
                string temporary = destination + ".import-" + Guid.NewGuid().ToString("N");
                try
                {
                    using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    { file.Write(bytes, 0, bytes.Length); file.Flush(true); }
                    File.Move(temporary, destination);
                }
                finally { if (File.Exists(temporary)) File.Delete(temporary); }
                return "旧存档已复制到存档槽 " + slot + "。启用五槽存档后，在游戏内选择读取进度。";
            }
        }
        throw new IOException("五个存档槽都已有进度。请打开存档管理，选择槽位后导入或先清空槽位。");
    }

    public static string Apply(string directory, bool restore, bool music, bool brands, bool saves)
    { return Apply(directory, restore, music, brands, saves, false); }

    public static string Apply(string directory, bool restore, bool music, bool brands, bool saves, bool wide)
    {
        string root = Root(directory).TrimEnd(Path.DirectorySeparatorChar);
        foreach (Process process in Process.GetProcessesByName("RoadRash95"))
        { process.Dispose(); throw new IOException("请先退出游戏，再安装或还原。"); }
        string backupFolder = Path.Combine(root, "Codex-ZH-backup-v1");
        Directory.CreateDirectory(backupFolder);
        string lockPath = Backup(root, "install.lock");
        using (var installLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
        {
            Recover(root);
            ValidateFiles(root, restore);
            foreach (string filename in new[] { "ENU.DLL", "RoadRash95.exe" })
            {
                string source = filename == "ENU.DLL" ? Path.Combine(root, "TEXT", filename) : Path.Combine(root, filename);
                string backup = Backup(root, filename);
                if (!File.Exists(backup))
                {
                    byte[] bytes = File.ReadAllBytes(source);
                    string expected = filename == "ENU.DLL" ? OriginalHash : ExeHash;
                    if (BytesHash(bytes) != expected) throw new IOException("原版文件在备份前发生变化。");
                    string stage = backup + "." + Guid.NewGuid().ToString("N") + ".tmp";
                    try { ReplaceBytes(stage, bytes); File.Move(stage, backup); }
                    finally { if (File.Exists(stage)) File.Delete(stage); }
                }
            }
            var plan = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            string languageName = music ? brands ? "Language.MusicBrands" : "Language.Music" : brands ? "Language.Brands" : "Language.Core";
            plan[Path.Combine(root, "TEXT", "ENU.DLL")] = restore ? File.ReadAllBytes(Backup(root, "ENU.DLL")) : Component(languageName);
            byte[] original = File.ReadAllBytes(Backup(root, "RoadRash95.exe"));
            plan[Path.Combine(root, "RoadRash95.exe")] = restore ? original : PatchedExecutable(original, saves, wide);
            PlanNative(root, restore, saves, wide, plan);
            int retained = PlanPortraits(root, restore, plan);
            Commit(root, plan);
            return restore ? "英文语言与原版主程序已还原。" + (retained == 0 ? "" : "已保留另行修改的立绘副本。") :
                "v1.5安装完成。\r\n音乐译名 " + (music ? "开" : "关") + "　车辆译名 " + (brands ? "开" : "关") + "　五槽存档 " + (saves ? "开" : "关") +
                "　宽屏扩展 " + (wide ? "开" : "关") + "\r\n从 Steam 正常启动即可。";
        }
    }
}
