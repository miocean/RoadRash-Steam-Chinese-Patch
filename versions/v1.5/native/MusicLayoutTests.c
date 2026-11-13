#include <stdio.h>
#include "MusicLayout.c"

static int failures;
static void Check(BOOL condition, const char *name) {
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

int main(void) {
    gameBase = VirtualAlloc(NULL, 0x200000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const char *strings[1100] = {0};
    *(const char ***)(gameBase + 0x1cd49c) = strings;
    char nowPlaying[1024];
    const WCHAR *title = L"正在播放：Swervedriver（急转车手）\n《Last Train to Satansville（开往撒旦镇的末班车）》";
    WideCharToMultiByte(936, 0, title, -1, nowPlaying, sizeof(nowPlaying), NULL, NULL);
    char format[80];
    WideCharToMultiByte(936, 0, L"正在播放：%s\n《%s》", -1, format, sizeof(format), NULL, NULL);
    strings[11] = format;
    originalDrawText = DrawTextA;
    originalDrawTextEx = DrawTextExA;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = CreateBitmap(640, 480, 1, 32, NULL);
    HBITMAP previousBitmap = SelectObject(dc, bitmap);
    HFONT font = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH, L"SimSun");
    HFONT previousFont = SelectObject(dc, font);
    RECT entireBitmap = {0, 0, 640, 480};
    FillRect(dc, &entireBitmap, GetStockObject(WHITE_BRUSH));
    RECT measured = {0, 0, 300, 390};
    RenderMusicText(dc, nowPlaying, -1, &measured, 0x4c50, NULL, FALSE);
    Check(measured.bottom <= 76, "now-playing measurement stays within available height");
    Check(measured.right <= 300, "now-playing measurement keeps fixed width");
    Check(GetCurrentObject(dc, OBJ_FONT) == font, "measurement restores caller font");
    Check(GetPixel(dc, 0, 0) == RGB(255, 255, 255), "measurement never paints screen origin");

    char longTitle[1600];
    memset(longTitle, 'W', sizeof(longTitle) - 1);
    longTitle[sizeof(longTitle) - 1] = 0;
    strings[241] = longTitle;
    RECT area = {100, 100, 360, 130};
    RECT originalArea = area;
    RECT clipBefore;
    GetClipBox(dc, &clipBefore);
    FillRect(dc, &entireBitmap, GetStockObject(WHITE_BRUSH));
    RenderMusicText(dc, longTitle, -1, &area, 0x8d0, NULL, TRUE);
    Check(memcmp(&area, &originalArea, sizeof(RECT)) == 0, "drawing preserves caller rectangle");
    Check(scrollingText[0].source == longTitle, "oversized title enables scrolling without truncation");
    Check(GetPixel(dc, 99, 110) == RGB(255, 255, 255) &&
        GetPixel(dc, 361, 110) == RGB(255, 255, 255), "scrolling stays inside text area");
    Check(GetCurrentObject(dc, OBJ_FONT) == font, "drawing restores caller font");
    RECT clipAfter;
    GetClipBox(dc, &clipAfter);
    Check(EqualRect(&clipBefore, &clipAfter), "drawing restores caller clipping region");

    WCHAR scrollTitle[] = L"whole title";
    scrollingText[0].hash = 0;
    int start = ScrollOffset(dc, longTitle, scrollTitle, 11, 100);
    ScrollingText *item = NULL;
    for (unsigned i = 0; i < 16; ++i) {
        if (scrollingText[i].source == longTitle && scrollingText[i].hash != 0) item = &scrollingText[i];
    }
    item->started = GetTickCount() - 6500;
    int end = ScrollOffset(dc, longTitle, scrollTitle, 11, 100);
    Check(start == 0 && end == 100, "scrolling exposes beginning and end");
    SelectObject(dc, previousFont);
    SelectObject(dc, previousBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
