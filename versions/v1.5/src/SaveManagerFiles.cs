using System;
using System.IO;
using System.Diagnostics;
using System.Text;

public sealed class SaveSlotInfo
{
    public int Number;
    public bool Exists;
    public long Length;
    public DateTime Modified;
    public string Name;
    public override string ToString()
    {
        if (!Exists) return Number + "  " + Name + "    空";
        return Number + "  " + Name + "    " + (Length == 160 ? "已有进度" : "文件长度异常") +
            "    " + Modified.ToString("yyyy-MM-dd HH:mm:ss");
    }
}

public static partial class PatchEngine
{
    public const int SaveSlotCount = 5;

    public static string SaveDirectory
    {
        get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RoadRash-Chinese", "Saves"); }
    }

    private static string ManagedSlotPath(string folder, int slot)
    {
        if (slot < 1 || slot > SaveSlotCount) throw new ArgumentOutOfRangeException("slot");
        RejectLink(folder);
        string path = Path.Combine(folder, "Slot" + slot + ".RRS");
        RejectLink(path);
        RejectLink(path + ".bak");
        RejectLink(path + ".lock");
        RejectLink(Path.ChangeExtension(path, ".name"));
        RejectLink(Path.ChangeExtension(path, ".name") + ".bak");
        return path;
    }

    internal static SaveSlotInfo[] ReadSaveSlots(string folder)
    {
        var slots = new SaveSlotInfo[SaveSlotCount];
        for (int slot = 1; slot <= SaveSlotCount; slot++)
        {
            var file = new FileInfo(ManagedSlotPath(folder, slot));
            slots[slot - 1] = new SaveSlotInfo { Number = slot, Name = ReadSlotName(folder, slot), Exists = file.Exists,
                Length = file.Exists ? file.Length : 0, Modified = file.Exists ? file.LastWriteTime : DateTime.MinValue };
        }
        return slots;
    }

    private static void RequireGameClosedForSaves()
    {
        foreach (Process process in Process.GetProcessesByName("RoadRash95"))
        { process.Dispose(); throw new IOException("请先退出游戏，再导入、清空、重命名或恢复存档。"); }
    }

    internal static string ReadSlotName(string folder, int slot)
    {
        string path = Path.ChangeExtension(ManagedSlotPath(folder, slot), ".name");
        if (!File.Exists(path) || new FileInfo(path).Length > 48) return "存档 " + slot;
        try { return ValidateSlotName(new UTF8Encoding(false, true).GetString(File.ReadAllBytes(path))); }
        catch (ArgumentException) { return "存档 " + slot; }
    }

    private static string ValidateSlotName(string name)
    {
        name = name.Trim();
        if (name.Length == 0 || name.Length > 12) throw new ArgumentException("存档名称请输入1至12个字符。");
        foreach (char character in name)
            if (Char.IsControl(character)) throw new ArgumentException("存档名称不能包含换行或控制字符。");
        return name;
    }

    internal static void RenameSaveSlot(string folder, int slot, string name)
    {
        RequireGameClosedForSaves();
        byte[] bytes = new UTF8Encoding(false, true).GetBytes(ValidateSlotName(name));
        string path = ManagedSlotPath(folder, slot);
        using (var slotLock = LockManagedSlot(path))
        {
            ArchiveManagedSlot(folder, path);
            WriteSaveBytes(Path.ChangeExtension(path, ".name"), bytes);
        }
    }

    private static FileStream LockManagedSlot(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        return new FileStream(path + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
    }

    private static void WriteSaveBytes(string destination, byte[] bytes)
    {
        string temporary = destination + ".import-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { file.Write(bytes, 0, bytes.Length); file.Flush(true); }
            if (File.Exists(destination)) File.Replace(temporary, destination, null);
            else File.Move(temporary, destination);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    // Keep the current slot and the game's previous-save backup before changing either file.
    private static string ArchiveManagedSlot(string folder, string path)
    {
        string namePath = Path.ChangeExtension(path, ".name");
        if (!File.Exists(path) && !File.Exists(path + ".bak") && !File.Exists(namePath)) return null;
        string backups = Path.Combine(folder, "Backups");
        RejectLink(backups);
        Directory.CreateDirectory(backups);
        string archive = Path.Combine(backups, DateTime.Now.ToString("yyyyMMdd-HHmmss-fff") + "-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(archive);
        foreach (string source in new[] { path, path + ".bak", namePath, namePath + ".bak" })
        {
            if (!File.Exists(source)) continue;
            string name = Path.GetFileName(source);
            if (name.EndsWith(".RRS.bak", StringComparison.OrdinalIgnoreCase)) name = name.Substring(0, name.Length - 8) + "-previous.RRS";
            else if (name.EndsWith(".name.bak", StringComparison.OrdinalIgnoreCase)) name = name.Substring(0, name.Length - 9) + "-previous.name";
            using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var output = new FileStream(Path.Combine(archive, name), FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { input.CopyTo(output); output.Flush(true); }
        }
        return archive;
    }

    internal static string ImportSaveIntoSlot(string filename, string folder, int slot, bool restoreName = false)
    {
        RequireGameClosedForSaves();
        byte[] bytes;
        using (var input = new FileStream(filename, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var reader = new BinaryReader(input))
        {
            if (input.Length != 160) throw new IOException("所选文件长度与本版原生RRS存档不符。");
            bytes = reader.ReadBytes(160);
            if (bytes.Length != 160) throw new IOException("存档读取不完整。");
        }
        byte[] restoredName = null;
        string sourceName = Path.ChangeExtension(filename, ".name");
        if (restoreName && File.Exists(sourceName))
        {
            if (new FileInfo(sourceName).Length > 48) throw new IOException("备份名称文件长度异常。");
            string name = ValidateSlotName(new UTF8Encoding(false, true).GetString(File.ReadAllBytes(sourceName)));
            restoredName = new UTF8Encoding(false, true).GetBytes(name);
        }
        string path = ManagedSlotPath(folder, slot);
        using (var slotLock = LockManagedSlot(path))
        {
            string archive = ArchiveManagedSlot(folder, path);
            string namePath = Path.ChangeExtension(path, ".name");
            byte[] previousName = File.Exists(namePath) ? File.ReadAllBytes(namePath) : null;
            bool nameChanged = false;
            try
            {
                if (restoredName != null) { WriteSaveBytes(namePath, restoredName); nameChanged = true; }
                WriteSaveBytes(path, bytes);
            }
            catch
            {
                if (nameChanged)
                {
                    if (previousName != null) WriteSaveBytes(namePath, previousName);
                    else File.Delete(namePath);
                }
                throw;
            }
            return "已导入存档槽 " + slot + "。" + (archive == null ? "" : "\r\n原进度已备份到 " + archive);
        }
    }

    internal static string ClearSaveSlot(string folder, int slot)
    {
        RequireGameClosedForSaves();
        string path = ManagedSlotPath(folder, slot);
        using (var slotLock = LockManagedSlot(path))
        {
            string archive = ArchiveManagedSlot(folder, path);
            File.Delete(path);
            File.Delete(path + ".bak");
            File.Delete(Path.ChangeExtension(path, ".name"));
            File.Delete(Path.ChangeExtension(path, ".name") + ".bak");
            return "已清空存档槽 " + slot + "。" + (archive == null ? "" : "\r\n原进度已备份到 " + archive);
        }
    }

    internal static void ExportSaveSlot(string folder, int slot, string filename)
    {
        string path = ManagedSlotPath(folder, slot);
        string destination = Path.GetFullPath(filename);
        string managedFolder = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (destination.StartsWith(managedFolder, StringComparison.OrdinalIgnoreCase))
            throw new IOException("请将存档导出到五槽存档目录之外的位置。");
        RejectLink(destination);
        using (var slotLock = LockManagedSlot(path))
        using (var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var snapshot = new MemoryStream())
        {
            input.CopyTo(snapshot);
            WriteSaveBytes(destination, snapshot.ToArray());
        }
    }
}
