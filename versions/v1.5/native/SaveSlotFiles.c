// Included in SaveSlots.c so the save feature ships as one independent DLL.
#define SLOT_COUNT 5
typedef HANDLE (WINAPI *CreateFileProc)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *WriteFileProc)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandleProc)(HANDLE);
static CreateFileProc originalCreateFile;
static WriteFileProc originalWriteFile;
static CloseHandleProc originalCloseHandle;
static CRITICAL_SECTION fileLock;
static wchar_t saveFolder[MAX_PATH];
static const char *slotNames[] = {"RRZH-Slot1.RRS", "RRZH-Slot2.RRS", "RRZH-Slot3.RRS", "RRZH-Slot4.RRS", "RRZH-Slot5.RRS"};
typedef struct {
    HANDLE file;
    HANDLE lock;
    BOOL failed;
    wchar_t destination[MAX_PATH];
    wchar_t temporary[MAX_PATH];
} SlotWrite;
static SlotWrite writes[SLOT_COUNT];

static BOOL SlotPath(int slot, wchar_t *path) {
    if (slot < 0 || slot >= SLOT_COUNT || !saveFolder[0]) return FALSE;
    return swprintf_s(path, MAX_PATH, L"%s\\Slot%d.RRS", saveFolder, slot + 1) > 0;
}

static BOOL SlotNamePath(int slot, wchar_t *path) {
    if (!SlotPath(slot, path)) return FALSE;
    wchar_t *extension = wcsrchr(path, L'.');
    return extension && wcscpy_s(extension, MAX_PATH - (extension - path), L".name") == 0;
}

static BOOL ReadSlotName(int slot, wchar_t *name) {
    swprintf_s(name, 13, L"存档 %d", slot + 1);
    wchar_t path[MAX_PATH];
    if (!SlotNamePath(slot, path)) return FALSE;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    char bytes[49] = {0}; DWORD count = 0;
    BOOL read = GetFileSize(file, NULL) <= 48 && ReadFile(file, bytes, 48, &count, NULL);
    CloseHandle(file);
    wchar_t decoded[13] = {0};
    int length = read && count ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, count, decoded, 12) : 0;
    if (!length) return FALSE;
    for (int index = 0; index < length; index++) if (decoded[index] < L' ') return FALSE;
    wcscpy_s(name, 13, decoded);
    return TRUE;
}

static BOOL WriteSlotName(int slot, const wchar_t *name) {
    int length = (int)wcslen(name);
    if (length < 1 || length > 12) return FALSE;
    for (int index = 0; index < length; index++) if (name[index] < L' ') return FALSE;
    char bytes[49];
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, length, bytes, sizeof(bytes), NULL, NULL);
    wchar_t slotPath[MAX_PATH], path[MAX_PATH], lockPath[MAX_PATH], pending[MAX_PATH], backup[MAX_PATH];
    if (!size || !SlotPath(slot, slotPath) || !SlotNamePath(slot, path) ||
        swprintf_s(lockPath, MAX_PATH, L"%s.lock", slotPath) < 0 ||
        swprintf_s(pending, MAX_PATH, L"%s.pending", path) < 0 ||
        swprintf_s(backup, MAX_PATH, L"%s.bak", path) < 0) return FALSE;
    HANDLE lock = CreateFileW(lockPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (lock == INVALID_HANDLE_VALUE) return FALSE;
    BOOL result = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || CopyFileW(path, backup, FALSE);
    HANDLE file = result ? CreateFileW(pending, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    DWORD written = 0;
    result = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, size, &written, NULL) && written == (DWORD)size && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (result) result = MoveFileExW(pending, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!result) DeleteFileW(pending);
    CloseHandle(lock);
    return result;
}

static BOOL InitializeSlotFiles(void) {
    InitializeCriticalSection(&fileLock);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, saveFolder))) return FALSE;
    if (wcscat_s(saveFolder, MAX_PATH, L"\\RoadRash-Chinese") != 0) return FALSE;
    if (!CreateDirectoryW(saveFolder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    if (wcscat_s(saveFolder, MAX_PATH, L"\\Saves") != 0) return FALSE;
    return CreateDirectoryW(saveFolder, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int SlotNumber(LPCSTR filename) {
    if (!filename) return -1;
    const char *basename = strrchr(filename, '\\');
    if (basename) filename = basename + 1;
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        if (lstrcmpiA(filename, slotNames[slot]) == 0) return slot;
    }
    return -1;
}

static HANDLE WINAPI CreateSlotFile(LPCSTR filename, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    int slot = SlotNumber(filename);
    if (slot < 0) return originalCreateFile(filename, access, sharing, security, disposition, flags, templateFile);
    wchar_t destination[MAX_PATH];
    if (!SlotPath(slot, destination)) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    if (!(access & GENERIC_WRITE))
        return CreateFileW(destination, access, sharing, security, disposition, flags, templateFile);

    EnterCriticalSection(&fileLock);
    SlotWrite *write = &writes[slot];
    wchar_t lockPath[MAX_PATH];
    if (write->file || swprintf_s(lockPath, MAX_PATH, L"%s.lock", destination) < 0) {
        LeaveCriticalSection(&fileLock);
        SetLastError(ERROR_SHARING_VIOLATION);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE lock = CreateFileW(lockPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (lock == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&fileLock); return INVALID_HANDLE_VALUE; }
    wchar_t backup[MAX_PATH], backupTemporary[MAX_PATH];
    BOOL prepared = wcscpy_s(write->destination, MAX_PATH, destination) == 0 &&
        swprintf_s(write->temporary, MAX_PATH, L"%s.pending", destination) > 0;
    if (prepared && GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES) {
        prepared = swprintf_s(backup, MAX_PATH, L"%s.bak", destination) > 0 &&
            swprintf_s(backupTemporary, MAX_PATH, L"%s.bak.pending", destination) > 0 &&
            CopyFileW(destination, backupTemporary, FALSE) &&
            MoveFileExW(backupTemporary, backup, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    HANDLE file = prepared ? CreateFileW(write->temporary, access, sharing, security,
        CREATE_ALWAYS, flags, templateFile) : INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(lock);
        ZeroMemory(write, sizeof(*write));
        LeaveCriticalSection(&fileLock);
        SetLastError(error);
        return INVALID_HANDLE_VALUE;
    }
    write->file = file;
    write->lock = lock;
    write->failed = FALSE;
    LeaveCriticalSection(&fileLock);
    return file;
}

static BOOL WINAPI WriteSlotFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped) {
    BOOL result = originalWriteFile(file, buffer, bytes, written, overlapped);
    EnterCriticalSection(&fileLock);
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        if (writes[slot].file && writes[slot].file == file && (!result || !written || *written != bytes)) writes[slot].failed = TRUE;
    }
    LeaveCriticalSection(&fileLock);
    return result;
}

static BOOL WINAPI CloseSlotFile(HANDLE file) {
    EnterCriticalSection(&fileLock);
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        SlotWrite *write = &writes[slot];
        if (!write->file || write->file != file) continue;
        DWORD high = 0;
        BOOL complete = !write->failed && GetFileSize(file, &high) == 160 && high == 0 && FlushFileBuffers(file);
        BOOL closed = originalCloseHandle(file);
        BOOL committed = complete && closed && MoveFileExW(write->temporary, write->destination,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        DWORD error = committed ? ERROR_SUCCESS : GetLastError();
        if (!committed) DeleteFileW(write->temporary);
        CloseHandle(write->lock);
        ZeroMemory(write, sizeof(*write));
        LeaveCriticalSection(&fileLock);
        SetLastError(committed ? ERROR_SUCCESS : error ? error : ERROR_WRITE_FAULT);
        return committed;
    }
    LeaveCriticalSection(&fileLock);
    return originalCloseHandle(file);
}
