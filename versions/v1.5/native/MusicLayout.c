#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>

typedef int (WINAPI *DrawTextProc)(HDC, LPCSTR, int, LPRECT, UINT);
typedef int (WINAPI *DrawTextExProc)(HDC, LPSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
static DrawTextProc originalDrawText;
static DrawTextExProc originalDrawTextEx;
static BYTE *gameBase;
static const char *videoBuffers[2];
static struct { HFONT source; int height; HFONT resized; } fontCache[48];
static unsigned fontCount;
static HDC measurementDC;
static UINT_PTR scrollTimer;
static HWND timerWindow;

typedef struct {
    const char *source;
    DWORD hash;
    DWORD started;
    DWORD lastDraw;
    HWND window;
} ScrollingText;
static ScrollingText scrollingText[16];

static BOOL IsNowPlaying(LPCSTR text) {
    const char **strings = *(const char ***)(gameBase + 0x1cd49c);
    if (!text || !strings || !strings[11]) return FALSE;
    const char *placeholder = strchr(strings[11], '%');
    return placeholder && placeholder > strings[11] &&
        strncmp(text, strings[11], (size_t)(placeholder - strings[11])) == 0;
}

static BOOL IsMusicText(LPCSTR text) {
    const char **strings = *(const char ***)(gameBase + 0x1cd49c);
    if (!text || !strings) return FALSE;
    if (text == videoBuffers[0] || text == videoBuffers[1] || IsNowPlaying(text)) return TRUE;
    if (text == strings[81] || text == strings[496]) return TRUE;
    for (int id = 225; id <= 254; ++id) {
        if (id >= 237 && id <= 240) continue;
        if (text == strings[id]) return TRUE;
    }
    return FALSE;
}

static HFONT SmallerFont(HFONT original, const LOGFONTA *description, int height) {
    for (unsigned i = 0; i < fontCount; ++i) {
        if (fontCache[i].source == original && fontCache[i].height == height) return fontCache[i].resized;
    }
    if (fontCount == 48) return NULL;
    LOGFONTA resized = *description;
    resized.lfHeight = description->lfHeight < 0 ? -height : height;
    resized.lfWidth = 0;
    HFONT font = CreateFontIndirectA(&resized);
    if (!font) return NULL;
    fontCache[fontCount].source = original;
    fontCache[fontCount].height = height;
    fontCache[fontCount++].resized = font;
    return font;
}

static HWND GameWindow(HDC dc) {
    HWND window = WindowFromDC(dc);
    if (!window) window = FindWindowA(NULL, "Road Rash");
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId() ? window : NULL;
}

static void CALLBACK AnimateText(HWND window, UINT message, UINT_PTR timer, DWORD now) {
    (void)window;
    (void)message;
    BOOL active = FALSE;
    for (unsigned i = 0; i < 16; ++i) {
        ScrollingText *item = &scrollingText[i];
        if (!item->window || now - item->lastDraw > 500 || !IsWindowVisible(item->window)) continue;
        // The game caches menu text on a DirectDraw surface. WM_PAINT only
        // presents that cache, so use its existing item repaint routine.
        const char **strings = *(const char ***)(gameBase + 0x1cd49c);
        BYTE **entries = (BYTE **)(gameBase + 0x1c3fc8);
        const int last = *(int *)(gameBase + 0x1c4048);
        typedef void (__fastcall *RepaintItemProc)(BYTE *, BYTE *);
        RepaintItemProc repaint = (RepaintItemProc)(gameBase + 0x39160);
        for (int index = 0; index <= last && index < 30; ++index) {
            BYTE *entry = entries[index];
            if (!entry) continue;
            const DWORD resource = *(DWORD *)(entry + 8);
            const BOOL matches = resource <= 994 && strings[resource] == item->source;
            const BOOL video = (resource == 160 && item->source == videoBuffers[0]) ||
                (resource == 161 && item->source == videoBuffers[1]);
            if (!matches && !video) continue;
            active = TRUE;
            repaint(entry, *(BYTE **)(gameBase + 0x1c4030));
        }
    }
    if (!active) {
        KillTimer(timerWindow, timer);
        scrollTimer = 0;
        timerWindow = NULL;
    }
}

static int ScrollOffset(HDC dc, LPCSTR source, const WCHAR *text, int length, int overflow) {
    DWORD hash = 2166136261u;
    for (int i = 0; i < length; ++i) hash = (hash ^ text[i]) * 16777619u;
    const DWORD now = GetTickCount();
    ScrollingText *item = NULL;
    ScrollingText *oldest = &scrollingText[0];
    for (unsigned i = 0; i < 16; ++i) {
        if (scrollingText[i].source == source && scrollingText[i].hash == hash) item = &scrollingText[i];
        if (scrollingText[i].lastDraw < oldest->lastDraw) oldest = &scrollingText[i];
    }
    if (!item) item = oldest;
    if (item->source != source || item->hash != hash || now - item->lastDraw > 500) {
        item->source = source;
        item->hash = hash;
        item->started = now;
    }
    item->lastDraw = now;
    item->window = GameWindow(dc);
    if (!scrollTimer && item->window) {
        timerWindow = item->window;
        scrollTimer = SetTimer(timerWindow, 0x52524d55, 50, AnimateText);
    }
    // Hold both ends so the first and final words remain readable. Movement is
    // twenty pixels per second in the game's original 640 x 480 coordinate space.
    DWORD travelTime = (DWORD)overflow * 50;
    DWORD phase = (now - item->started) % (travelTime + 3000);
    if (phase <= 1500) return 0;
    return min(overflow, (int)((phase - 1500) / 50));
}

static int RenderMusicText(HDC dc, LPCSTR text, int length, LPRECT bounds, UINT format,
    LPDRAWTEXTPARAMS parameters, BOOL extended) {
    if (!IsMusicText(text)) {
        return extended ? originalDrawTextEx(dc, (LPSTR)text, length, bounds, format, parameters) :
            originalDrawText(dc, text, length, bounds, format);
    }
    const BOOL measuring = (format & DT_CALCRECT) != 0;
    RECT area = *bounds;
    if (IsNowPlaying(text) && measuring) area.bottom = area.top + 76;
    if (!measuring) area.bottom = min(area.bottom, 476);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    const int bytes = length < 0 ? (int)strlen(text) : length;
    const int count = MultiByteToWideChar(936, 0, text, bytes, NULL, 0);
    WCHAR *unicode = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)count + 1) * sizeof(WCHAR));
    HFONT original = (HFONT)GetCurrentObject(dc, OBJ_FONT);
    LOGFONTA description;
    if (!measurementDC) measurementDC = CreateCompatibleDC(NULL);
    if (!unicode || !measurementDC || count <= 0 || width <= 0 || height <= 0 ||
        !GetObjectA(original, sizeof(description), &description)) {
        if (unicode) HeapFree(GetProcessHeap(), 0, unicode);
        return extended ? originalDrawTextEx(dc, (LPSTR)text, length, bounds, format, parameters) :
            originalDrawText(dc, text, length, bounds, format);
    }
    MultiByteToWideChar(936, 0, text, bytes, unicode, count);
    unicode[count] = 0;
    // DT_TABSTOP reuses high-byte format bits. Never combine it with CALCRECT.
    UINT wrapping = (format & ~(DT_SINGLELINE | DT_MODIFYSTRING | DT_NOCLIP | DT_TABSTOP |
        DT_CALCRECT | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) | DT_WORDBREAK;
    HFONT measuredFont = (HFONT)SelectObject(measurementDC, original);
    HFONT chosen = original;
    RECT measured = {0, 0, width, height};
    const int originalHeight = abs(description.lfHeight);
    const int minimumHeight = min(originalHeight, max(16, originalHeight * 7 / 10));
    for (int size = originalHeight; size >= minimumHeight; --size) {
        HFONT resized = size == originalHeight ? original : SmallerFont(original, &description, size);
        if (!resized) break;
        SelectObject(measurementDC, resized);
        chosen = resized;
        SetRect(&measured, 0, 0, width, height);
        DrawTextW(measurementDC, unicode, count, &measured, wrapping | DT_CALCRECT);
        if (measured.right <= width && measured.bottom <= height) break;
    }
    const BOOL scroll = measured.right > width || measured.bottom > height;
    SIZE extent = {0};
    if (scroll) {
        for (int i = 0; i < count; ++i) {
            if (unicode[i] == '\n' || unicode[i] == '\r') unicode[i] = ' ';
        }
        GetTextExtentPoint32W(measurementDC, unicode, count, &extent);
        wrapping = (wrapping & ~DT_WORDBREAK) | DT_SINGLELINE;
    }
    SelectObject(measurementDC, measuredFont);
    SelectObject(dc, chosen);
    const int saved = SaveDC(dc);
    if (!saved) {
        SelectObject(dc, original);
        HeapFree(GetProcessHeap(), 0, unicode);
        return extended ? originalDrawTextEx(dc, (LPSTR)text, length, bounds, format, parameters) :
            originalDrawText(dc, text, length, bounds, format);
    }
    if (!measuring) {
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        if (scroll) {
            int offset = ScrollOffset(dc, text, unicode, count, max(0, extent.cx - width));
            area.left -= offset;
            area.right = area.left + max(width, extent.cx);
        }
    }
    int result = DrawTextW(dc, unicode, count, &area, wrapping | (measuring ? DT_CALCRECT : 0));
    if (measuring) {
        if (scroll) area.right = area.left + width;
        *bounds = area;
    }
    RestoreDC(dc, saved);
    SelectObject(dc, original);
    HeapFree(GetProcessHeap(), 0, unicode);
    return result;
}

static int WINAPI DrawMusicText(HDC dc, LPCSTR text, int length, LPRECT bounds, UINT format) {
    return RenderMusicText(dc, text, length, bounds, format, NULL, FALSE);
}

static int WINAPI DrawMusicTextEx(HDC dc, LPSTR text, int length, LPRECT bounds,
    UINT format, LPDRAWTEXTPARAMS parameters) {
    return RenderMusicText(dc, text, length, bounds, format, parameters, TRUE);
}

static BOOL HookImport(DWORD rva, void *replacement, void **previous) {
    void **entry = (void **)(gameBase + rva);
    DWORD protection;
    if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &protection)) return FALSE;
    *previous = *entry;
    *entry = replacement;
    VirtualProtect(entry, sizeof(void *), protection, &protection);
    return TRUE;
}

__declspec(dllexport) void __cdecl RRMusicInitialize(void) {
    if (gameBase) return;
    gameBase = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)gameBase;
    IMAGE_NT_HEADERS32 *pe = (IMAGE_NT_HEADERS32 *)(gameBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(pe);
    for (WORD i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        if (memcmp(section[i].Name, ".rrzh", 5) != 0) continue;
        videoBuffers[0] = (char *)(gameBase + section[i].VirtualAddress + 128);
        videoBuffers[1] = (char *)(gameBase + section[i].VirtualAddress + 256);
    }
    if (!HookImport(0x21c74c, (void *)DrawMusicText, (void **)&originalDrawText)) return;
    HookImport(0x21c750, (void *)DrawMusicTextEx, (void **)&originalDrawTextEx);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (scrollTimer) KillTimer(timerWindow, scrollTimer);
        if (measurementDC) DeleteDC(measurementDC);
        for (unsigned i = 0; i < fontCount; ++i) DeleteObject(fontCache[i].resized);
    }
    return TRUE;
}
