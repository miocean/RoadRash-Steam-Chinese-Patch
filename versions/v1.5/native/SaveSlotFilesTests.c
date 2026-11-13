#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "SaveSlotFiles.c"

static int failures;
static void Check(BOOL condition, const char *name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

int main(void) {
    InitializeCriticalSection(&fileLock);
    wcscpy_s(saveFolder, MAX_PATH, L"E:\\Codex files\\逆向工程\\analysis\\roadrash-next\\test-save-中文-\xd83d\xdeb2");
    CreateDirectoryW(saveFolder, NULL);
    originalCreateFile = CreateFileA;
    originalWriteFile = WriteFile;
    originalCloseHandle = CloseHandle;
    const char *name = slotNames[0];
    BYTE expected[160], readback[160];
    memset(expected, 0x31, sizeof(expected));
    DWORD written = 0, read = 0;
    HANDLE file = CreateSlotFile(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    Check(file != INVALID_HANDLE_VALUE, "open slot in Unicode path including non-ACP character");
    Check(WriteSlotFile(file, expected, sizeof(expected), &written, NULL) && written == sizeof(expected), "write complete native save");
    Check(CloseSlotFile(file), "commit complete save atomically");
    file = CreateSlotFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check(file != INVALID_HANDLE_VALUE && ReadFile(file, readback, 160, &read, NULL) &&
        read == 160 && memcmp(expected, readback, 160) == 0, "read slot through ASCII game filename");
    CloseSlotFile(file);
    file = CreateSlotFile(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE second = CreateSlotFile(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    Check(second == INVALID_HANDLE_VALUE, "concurrent write to same slot is rejected");
    WriteSlotFile(file, expected, 80, &written, NULL);
    Check(!CloseSlotFile(file), "incomplete write is rejected");
    file = CreateSlotFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ZeroMemory(readback, sizeof(readback));
    Check(ReadFile(file, readback, 160, &read, NULL) && read == 160 &&
        memcmp(expected, readback, 160) == 0, "incomplete write preserves previous save");
    CloseSlotFile(file);
    memset(expected, 0x32, sizeof(expected));
    file = CreateSlotFile(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    WriteSlotFile(file, expected, 160, &written, NULL);
    Check(CloseSlotFile(file), "retry succeeds after failed write");
    wchar_t backup[MAX_PATH];
    swprintf_s(backup, MAX_PATH, L"%s\\Slot1.RRS.bak", saveFolder);
    file = CreateFileW(backup, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ReadFile(file, readback, 160, &read, NULL);
    CloseHandle(file);
    Check(read == 160 && readback[0] == 0x31 && readback[159] == 0x31, "overwrite retains previous complete backup");
    Check(SlotNumber("E:\\Game\\RRZH-Slot2.RRS") == 1 && SlotNumber("another.RRS") == -1,
        "slot mapping handles game-normalized paths and leaves other names alone");
    for (int slot = 3; slot < SLOT_COUNT; slot++) {
        memset(expected, 0x40 + slot, sizeof(expected));
        file = CreateSlotFile(slotNames[slot], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        Check(file != INVALID_HANDLE_VALUE && WriteSlotFile(file, expected, 160, &written, NULL) && CloseSlotFile(file),
            "new fourth/fifth slot commits independently");
        file = CreateSlotFile(slotNames[slot], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        Check(file != INVALID_HANDLE_VALUE && ReadFile(file, readback, 160, &read, NULL) && read == 160 &&
            memcmp(expected, readback, 160) == 0, "new fourth/fifth slot loads through native filename");
        CloseSlotFile(file);
    }
    Check(SlotNumber("RRZH-Slot5.RRS") == 4 && SlotNumber("RRZH-Slot6.RRS") == -1,
        "five-slot filename bounds are exact");
    wchar_t displayName[13], namePath[MAX_PATH];
    Check(WriteSlotName(4, L"城市冠军") && ReadSlotName(4, displayName) && lstrcmpW(displayName, L"城市冠军") == 0,
        "Chinese slot name persists in UTF8 sidecar");
    SlotNamePath(4, namePath);
    file = CreateFileW(namePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BYTE nameBytes[48];
    Check(file != INVALID_HANDLE_VALUE && ReadFile(file, nameBytes, 48, &read, NULL) && read == 12 && nameBytes[0] == 0xe5,
        "native name format matches managed UTF8 storage");
    CloseHandle(file);
    Check(!WriteSlotName(4, L"1234567890123") && ReadSlotName(4, displayName) && lstrcmpW(displayName, L"城市冠军") == 0,
        "overlong name preserves existing name");
    wchar_t lockName[MAX_PATH];
    SlotPath(4, lockName); wcscat_s(lockName, MAX_PATH, L".lock");
    file = CreateFileW(lockName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    Check(file != INVALID_HANDLE_VALUE && !WriteSlotName(4, L"并发修改"), "concurrent name changes are rejected");
    CloseHandle(file);
    DeleteCriticalSection(&fileLock);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
