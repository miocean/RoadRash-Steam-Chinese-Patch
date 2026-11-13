#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "SaveSlotFiles.c"
#include "SaveSlotLayout.c"

typedef struct {
    HWND parent;
    HWND panel;
    HWND message;
    HFONT font;
    HFONT titleFont;
    HFONT detailFont;
    HFONT inputFont;
    HBITMAP background;
    HBITMAP shadedBackground;
    SlotLayout layout;
    BOOL saving;
    BOOL widescreen;
    BOOL finished;
    BOOL parentEnabled;
    HWND previousCapture;
    int selected;
    int overwrite;
    int focusedSlot;
    int namingSlot;
    BOOL editingName;
    BOOL saveAfterName;
    BOOL composingName;
    HWND nameEdit;
    wchar_t paths[SLOT_COUNT][MAX_PATH];
} SlotDialog;

static HINSTANCE moduleInstance;
static LONG dialogActive;
static const wchar_t panelClass[] = L"RoadRashChineseSaveSlots";
static WNDPROC originalButtonProc;
static WNDPROC originalNameEditProc;
static void ChooseSlot(SlotDialog *dialog, int slot);
static void BeginSlotName(SlotDialog *dialog, int slot, BOOL saveAfterName);
static void FinishSlotName(SlotDialog *dialog, BOOL accept);

static LRESULT CALLBACK NameEditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SlotDialog *dialog = (SlotDialog *)GetWindowLongPtrW(GetParent(window), GWLP_USERDATA);
    if (dialog && message == WM_IME_STARTCOMPOSITION) dialog->composingName = TRUE;
    if (dialog && message == WM_IME_ENDCOMPOSITION) dialog->composingName = FALSE;
    return CallWindowProcW(originalNameEditProc, window, message, wparam, lparam);
}

static RECT NameControlRectangle(const SlotLayout *layout, int id) {
    int left = layout->canvasWidth / 2 + 5;
    RECT rectangle = {left, 300, layout->canvasWidth - 45, 336};
    if (id == 202) { rectangle.top = 350; rectangle.bottom = 390; rectangle.right = left + 120; }
    if (id == 203) { rectangle.top = 350; rectangle.bottom = 390; rectangle.left = left + 140; }
    return rectangle;
}

static HWND ControlAtPoint(SlotDialog *dialog, POINT point) {
    int row = SlotHitTest(&dialog->layout, point);
    if (row >= 0 && (!dialog->editingName || row == SLOT_COUNT)) {
        HWND control = GetDlgItem(dialog->panel, row == SLOT_COUNT ? IDCANCEL : 101 + row);
        return IsWindowEnabled(control) ? control : NULL;
    }
    for (int id = 201; id <= 203; id++) {
        HWND control = GetDlgItem(dialog->panel, id);
        RECT rectangle = SlotViewportRectangle(&dialog->layout, NameControlRectangle(&dialog->layout, id));
        if (IsWindowVisible(control) && PtInRect(&rectangle, point)) return control;
    }
    return NULL;
}

static HBITMAP PrepareSlotBackground(SlotDialog *dialog) {
    BITMAPINFO format = {0};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = 640;
    format.bmiHeader.biHeight = -480;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    BYTE *originalPixels = NULL, *pixels = NULL;
    HDC display = GetDC(dialog->parent);
    HBITMAP originalFrame = CreateDIBSection(display, &format, DIB_RGB_COLORS, (void **)&originalPixels, NULL, 0);
    format.bmiHeader.biWidth = dialog->layout.canvasWidth;
    HBITMAP frame = CreateDIBSection(display, &format, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    HDC canvas = CreateCompatibleDC(display);
    HDC source = CreateCompatibleDC(display);
    if (originalFrame && frame && canvas && source) {
        HGDIOBJ oldCanvas = SelectObject(canvas, originalFrame);
        RECT bounds = {0, 0, 640, 480};
        FillRect(canvas, &bounds, GetStockObject(BLACK_BRUSH));
        if (dialog->background) {
            HGDIOBJ oldSource = SelectObject(source, dialog->background);
            BITMAP bitmap;
            GetObject(dialog->background, sizeof(bitmap), &bitmap);
            StretchBlt(canvas, 0, 0, 640, 480, source, 0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
            SelectObject(source, oldSource);
        }
        GdiFlush();
        int width = dialog->layout.canvasWidth;
        int middle = width / 2;
        for (int y = 0; y < 480; y++) for (int x = 0; x < width; x++) {
            BYTE *pixel = pixels + (y * width + x) * 4;
            int column = SlotBackgroundColumn(x, width);
            memcpy(pixel, originalPixels + (y * 640 + column) * 4, 4);
            BOOL panel = (x < middle + 30 && y < 112) ||
                (x >= middle - 5 && x < width - 30 && y >= 140 && y < 410) ||
                (x < middle - 10 && y >= 124 && y < 376) ||
                (x < 165 && y >= 395 && y < 450);
            if (panel) for (int channel = 0; channel < 3; channel++) pixel[channel] = pixel[channel] * 2 / 5;
        }
        SelectObject(canvas, oldCanvas);
    } else {
        if (frame) DeleteObject(frame);
        frame = NULL;
    }
    if (originalFrame) DeleteObject(originalFrame);
    if (canvas) DeleteDC(canvas);
    if (source) DeleteDC(source);
    ReleaseDC(dialog->parent, display);
    return frame;
}

static BOOL IsWidescreenActive(void) {
    // A leftover DLL on disk does not enable the option. Inspect this process.
    HMODULE renderer = GetModuleHandleW(L"RRWideScreen.dll");
    if (!renderer || !GetProcAddress(renderer, "RRWideInitialize")) return FALSE;
    wchar_t config[MAX_PATH];
    DWORD length = GetModuleFileNameW(renderer, config, MAX_PATH);
    if (!length || length >= MAX_PATH) return FALSE;
    wchar_t *filename = wcsrchr(config, L'\\');
    if (!filename || wcscpy_s(filename + 1, MAX_PATH - (filename + 1 - config), L"RRWideScreen.ini") != 0)
        return FALSE;
    return GetPrivateProfileIntW(L"Graphics", L"Enabled", 1, config) != 0;
}

static void ResizeSlotPanel(SlotDialog *dialog) {
    RECT bounds;
    GetClientRect(dialog->panel, &bounds);
    SlotLayout layout = CalculateSlotLayout(bounds.right, bounds.bottom, dialog->widescreen);
    if (!layout.canvasWidth) return;
    SlotLayout previous = dialog->layout;
    BOOL changed = layout.canvasWidth != dialog->layout.canvasWidth;
    dialog->layout = layout;
    if (changed) {
        HBITMAP frame = PrepareSlotBackground(dialog);
        if (!frame) { dialog->layout = previous; return; }
        DeleteObject(dialog->shadedBackground);
        dialog->shadedBackground = frame;
    }
    for (int row = 0; row <= SLOT_COUNT; row++) {
        HWND control = GetDlgItem(dialog->panel, row == SLOT_COUNT ? IDCANCEL : 101 + row);
        RECT rectangle = SlotViewportRectangle(&layout, SlotControlRectangle(&layout, row));
        if (control) MoveWindow(control, rectangle.left, rectangle.top,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, FALSE);
    }
    for (int id = 201; id <= 204; id++) {
        HWND control = GetDlgItem(dialog->panel, id);
        RECT rectangle = SlotViewportRectangle(&layout, NameControlRectangle(&layout, id));
        if (control) MoveWindow(control, rectangle.left, rectangle.top,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, FALSE);
    }
    if (dialog->nameEdit) {
        HFONT font = CreateFontW(-MulDiv(18, layout.viewport.bottom - layout.viewport.top, 480), 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, L"SimSun");
        if (font) {
            SendMessageW(dialog->nameEdit, WM_SETFONT, (WPARAM)font, TRUE);
            if (dialog->inputFont) DeleteObject(dialog->inputFont);
            dialog->inputFont = font;
        }
    }
    InvalidateRect(dialog->panel, NULL, FALSE);
}

static LRESULT CALLBACK SlotButtonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        SlotDialog *dialog = (SlotDialog *)GetWindowLongPtrW(GetParent(window), GWLP_USERDATA);
        int id = GetDlgCtrlID(window);
        if (dialog && message == WM_SETFOCUS && id >= 101 && id < 101 + SLOT_COUNT) dialog->focusedSlot = id - 101;
        InvalidateRect(GetParent(window), NULL, FALSE);
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(window, GetParent(window), &point, 1);
        if (message == WM_LBUTTONUP && GetCapture() == window) ReleaseCapture();
        SendMessageW(GetParent(window), message, wparam, MAKELPARAM(point.x, point.y));
        return 0;
    }
    return CallWindowProcW(originalButtonProc, window, message, wparam, lparam);
}

static void ChooseSlot(SlotDialog *dialog, int slot) {
    if (slot < 0 || slot >= SLOT_COUNT || dialog->editingName) return;
    BOOL exists = GetFileAttributesW(dialog->paths[slot]) != INVALID_FILE_ATTRIBUTES;
    if (!dialog->saving && !exists) return;
    SetFocus(GetDlgItem(dialog->panel, 101 + slot));
    wchar_t name[13];
    if (dialog->saving && !exists && !ReadSlotName(slot, name)) {
        BeginSlotName(dialog, slot, TRUE);
        return;
    }
    if (dialog->saving && exists && dialog->overwrite != slot) {
        dialog->overwrite = slot;
        SetWindowTextW(dialog->message, L"该槽已有进度。再次点击同一存档槽确认覆盖，或选择其他槽。 ");
        InvalidateRect(dialog->panel, NULL, FALSE);
        return;
    }
    dialog->selected = slot;
    dialog->finished = TRUE;
}

static void UpdateSlotLabel(SlotDialog *dialog, int slot) {
    wchar_t label[180], name[13];
    BOOL named = ReadSlotName(slot, name);
    wchar_t heading[40];
    if (named) swprintf_s(heading, 40, L"%d  %s", slot + 1, name);
    else swprintf_s(heading, 40, L"存档 %d", slot + 1);
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(dialog->paths[slot], GetFileExInfoStandard, &info)) {
        FILETIME local; SYSTEMTIME time;
        FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &time);
        swprintf_s(label, 180, L"%s\n%02u-%02u %02u:%02u", heading,
            time.wMonth, time.wDay, time.wHour, time.wMinute);
    } else swprintf_s(label, 180, L"%s\n空", heading);
    SetWindowTextW(GetDlgItem(dialog->panel, 101 + slot), label);
}

static void BeginSlotName(SlotDialog *dialog, int slot, BOOL saveAfterName) {
    if (slot < 0 || slot >= SLOT_COUNT) return;
    dialog->namingSlot = slot;
    dialog->editingName = TRUE;
    dialog->saveAfterName = saveAfterName;
    wchar_t name[13]; ReadSlotName(slot, name);
    SetWindowTextW(dialog->nameEdit, name);
    ShowWindow(GetDlgItem(dialog->panel, 201), SW_HIDE);
    ShowWindow(GetDlgItem(dialog->panel, 202), SW_SHOW);
    ShowWindow(GetDlgItem(dialog->panel, 203), SW_SHOW);
    ShowWindow(dialog->nameEdit, SW_SHOW);
    SetWindowTextW(dialog->message, saveAfterName ? L"为新存档设置名称（最多12字）。\n\n按 Enter 确认并保存，Esc 返回。" :
        L"修改存档名称（最多12字）。\n\n按 Enter 确认，Esc 返回。");
    SetFocus(dialog->nameEdit);
    SendMessageW(dialog->nameEdit, EM_SETSEL, 0, -1);
    InvalidateRect(dialog->panel, NULL, FALSE);
}

static void FinishSlotName(SlotDialog *dialog, BOOL accept) {
    if (accept) {
        wchar_t name[13]; GetWindowTextW(dialog->nameEdit, name, 13);
        size_t length = wcslen(name);
        while (length && name[length - 1] <= L' ') name[--length] = 0;
        wchar_t *start = name;
        while (*start && *start <= L' ') start++;
        if (!WriteSlotName(dialog->namingSlot, start)) {
            SetWindowTextW(dialog->message, L"名称未保存。请输入1至12个字符，或稍后重试。\n\nEsc 返回。");
            SetFocus(dialog->nameEdit);
            InvalidateRect(dialog->panel, NULL, FALSE);
            return;
        }
        UpdateSlotLabel(dialog, dialog->namingSlot);
    }
    dialog->editingName = FALSE;
    ShowWindow(dialog->nameEdit, SW_HIDE);
    ShowWindow(GetDlgItem(dialog->panel, 202), SW_HIDE);
    ShowWindow(GetDlgItem(dialog->panel, 203), SW_HIDE);
    ShowWindow(GetDlgItem(dialog->panel, 201), SW_SHOW);
    SetWindowTextW(dialog->message, L"选择一个存档槽。\n\nF2 重命名，Esc 返回。");
    SetFocus(GetDlgItem(dialog->panel, 101 + dialog->namingSlot));
    if (accept && dialog->saveAfterName) { dialog->selected = dialog->namingSlot; dialog->finished = TRUE; }
    InvalidateRect(dialog->panel, NULL, FALSE);
}

static void ActivateNameControl(SlotDialog *dialog, int id) {
    if (id == 201) BeginSlotName(dialog, dialog->focusedSlot, FALSE);
    if (id == 202) FinishSlotName(dialog, TRUE);
    if (id == 203) FinishSlotName(dialog, FALSE);
}

static LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SlotDialog *dialog = (SlotDialog *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        dialog = (SlotDialog *)((CREATESTRUCTW *)lparam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)dialog);
    }
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND control = ControlAtPoint(dialog, point);
            if (!dialog->editingName && control && GetFocus() != control) SetFocus(control);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            HWND control = ControlAtPoint(dialog, point);
            if (control) {
                int id = GetDlgCtrlID(control);
                if (id == IDCANCEL) dialog->finished = TRUE;
                else if (id >= 201 && id <= 203) ActivateNameControl(dialog, id);
                else ChooseSlot(dialog, id - 101);
            }
            return 0;
        }
        case WM_SIZE:
            if (dialog && dialog->panel) ResizeSlotPanel(dialog);
            return 0;
        case WM_DRAWITEM:
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wparam) >= 101 && LOWORD(wparam) < 101 + SLOT_COUNT) ChooseSlot(dialog, LOWORD(wparam) - 101);
            if (LOWORD(wparam) == IDCANCEL) dialog->finished = TRUE;
            if (LOWORD(wparam) >= 201 && LOWORD(wparam) <= 203) ActivateNameControl(dialog, LOWORD(wparam));
            return 0;
        case WM_CLOSE:
            dialog->finished = TRUE;
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wparam, GetDlgCtrlID((HWND)lparam) == 100 ? RGB(244, 84, 27) : RGB(245, 245, 235));
            SetBkColor((HDC)wparam, RGB(0, 0, 0));
            return (LRESULT)GetStockObject(BLACK_BRUSH);
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wparam, RGB(245, 245, 235));
            SetBkColor((HDC)wparam, RGB(0, 0, 0));
            return (LRESULT)GetStockObject(BLACK_BRUSH);
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC target = BeginPaint(window, &paint);
            RECT bounds;
            GetClientRect(window, &bounds);
            HDC device = CreateCompatibleDC(target);
            HBITMAP output = CreateCompatibleBitmap(target, bounds.right, bounds.bottom);
            if (!device || !output) {
                if (device) DeleteDC(device);
                if (output) DeleteObject(output);
                EndPaint(window, &paint);
                return 0;
            }
            HGDIOBJ previousOutput = SelectObject(device, output);
            HBRUSH background = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(device, &bounds, background);
            DeleteObject(background);
            if (dialog->shadedBackground) {
                HDC source = CreateCompatibleDC(device);
                HBITMAP frame = (HBITMAP)CopyImage(dialog->shadedBackground, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
                HGDIOBJ previous = SelectObject(source, frame);
                SetBkMode(source, TRANSPARENT);
                SelectObject(source, dialog->titleFont);
                SetTextColor(source, RGB(244, 84, 27));
                int middle = dialog->layout.canvasWidth / 2;
                RECT title = {32, 35, middle + 30, 100};
                DrawTextW(source, dialog->saving ? L"保存进度" : L"读取进度", -1, &title, DT_LEFT);
                SelectObject(source, dialog->font);
                wchar_t label[300];
                GetWindowTextW(dialog->message, label, 300);
                RECT description = {middle + 5, 150, dialog->layout.canvasWidth - 45, 260};
                SetTextColor(source, RGB(245, 245, 235));
                DrawTextW(source, label, -1, &description, DT_LEFT | DT_WORDBREAK);
                for (int row = 0; row <= SLOT_COUNT; row++) {
                    HWND control = GetDlgItem(window, row == SLOT_COUNT ? IDCANCEL : 101 + row);
                    GetWindowTextW(control, label, 300);
                    BOOL selected = GetFocus() == control;
                    SetTextColor(source, !IsWindowEnabled(control) ? RGB(125, 125, 125) :
                        selected ? RGB(255, 235, 0) : RGB(245, 245, 235));
                    int top = SlotControlRectangle(&dialog->layout, row).top + 2;
                    wchar_t *detail = wcschr(label, L'\n');
                    if (detail) *detail++ = 0;
                    RECT text = {45, top, middle - 15, top + 22};
                    SelectObject(source, dialog->font);
                    DrawTextW(source, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    if (detail) {
                        RECT secondLine = {45, top + 23, middle - 15, top + 42};
                        SelectObject(source, dialog->detailFont);
                        DrawTextW(source, detail, -1, &secondLine, DT_LEFT | DT_SINGLELINE);
                    }
                    if (selected) {
                        int center = top + 20;
                        POINT points[] = {{5, center - 3}, {13, center - 3}, {13, center - 7},
                            {23, center}, {13, center + 7}, {13, center + 3}, {5, center + 3}};
                        HBRUSH arrow = CreateSolidBrush(RGB(255, 235, 0));
                        HGDIOBJ oldBrush = SelectObject(source, arrow);
                        HGDIOBJ oldPen = SelectObject(source, GetStockObject(NULL_PEN));
                        Polygon(source, points, 7);
                        SelectObject(source, oldBrush);
                        SelectObject(source, oldPen);
                        DeleteObject(arrow);
                    }
                }
                SelectObject(source, dialog->font);
                for (int id = 201; id <= 203; id++) {
                    HWND control = GetDlgItem(window, id);
                    if (!IsWindowVisible(control)) continue;
                    GetWindowTextW(control, label, 300);
                    SetTextColor(source, GetFocus() == control ? RGB(255, 235, 0) : RGB(245, 245, 235));
                    RECT rectangle = NameControlRectangle(&dialog->layout, id);
                    DrawTextW(source, label, -1, &rectangle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
                RECT viewport = dialog->layout.viewport;
                SetStretchBltMode(device, HALFTONE);
                SetBrushOrgEx(device, 0, 0, NULL);
                StretchBlt(device, viewport.left, viewport.top, viewport.right - viewport.left,
                    viewport.bottom - viewport.top, source, 0, 0, dialog->layout.canvasWidth, 480, SRCCOPY);
                SelectObject(source, previous);
                DeleteObject(frame);
                DeleteDC(source);
            }
            BitBlt(target, 0, 0, bounds.right, bounds.bottom, device, 0, 0, SRCCOPY);
            SelectObject(device, previousOutput);
            DeleteObject(output);
            DeleteDC(device);
            EndPaint(window, &paint);
            if (dialog->editingName) InvalidateRect(dialog->nameEdit, NULL, FALSE);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HWND AddControl(SlotDialog *dialog, const wchar_t *className, const wchar_t *text,
                       DWORD style, int id, int row) {
    RECT rectangle = SlotViewportRectangle(&dialog->layout, SlotControlRectangle(&dialog->layout, row));
    BOOL button = lstrcmpW(className, L"BUTTON") == 0;
    if (button) style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
        rectangle.left, rectangle.top, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        dialog->panel, (HMENU)(INT_PTR)id, moduleInstance, NULL);
    if (control) SendMessageW(control, WM_SETFONT, (WPARAM)dialog->font, TRUE);
    if (control && button) {
        WNDPROC previous = (WNDPROC)SetWindowLongPtrW(control, GWLP_WNDPROC, (LONG_PTR)SlotButtonProc);
        if (!originalButtonProc) originalButtonProc = previous;
    }
    return control;
}

static BOOL PreparePaths(SlotDialog *dialog) {
    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        if (!SlotPath(slot, dialog->paths[slot])) return FALSE;
    }
    return TRUE;
}

static void DismissSlots(SlotDialog *dialog) {
    // Restore the enabled owner while the popup still covers it. Destroying an
    // active popup with a disabled owner can activate an unrelated desktop window.
    if (dialog->parentEnabled && IsWindow(dialog->parent)) EnableWindow(dialog->parent, TRUE);
    if (dialog->panel && IsWindow(dialog->parent)) {
        SetActiveWindow(dialog->parent);
        SetFocus(dialog->parent);
        RedrawWindow(dialog->parent, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    if (dialog->panel) DestroyWindow(dialog->panel);
}

static BOOL ShowSlots(OPENFILENAMEA *request, BOOL saving) {
    if (!request || !request->lpstrFile || !request->nMaxFile) return FALSE;
    if (InterlockedCompareExchange(&dialogActive, 1, 0)) return FALSE;
    SlotDialog dialog = {0};
    dialog.parent = request->hwndOwner;
    dialog.saving = saving;
    dialog.widescreen = IsWidescreenActive();
    dialog.selected = -1;
    dialog.overwrite = -1;
    BOOL result = FALSE;
    if (!IsWindow(dialog.parent) || !PreparePaths(&dialog)) goto cleanup;
    RECT bounds;
    GetClientRect(dialog.parent, &bounds);
    int width = bounds.right;
    int height = bounds.bottom;
    if (width < 320 || height < 240) goto cleanup;
    dialog.layout = CalculateSlotLayout(width, height, dialog.widescreen);
    dialog.font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH, L"SimSun");
    dialog.titleFont = CreateFontW(-50, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"SimSun");
    dialog.detailFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"SimSun");
    wchar_t backgroundFile[MAX_PATH];
    DWORD pathLength = GetModuleFileNameW(moduleInstance, backgroundFile, MAX_PATH);
    if (pathLength > 0 && pathLength < MAX_PATH) {
        wchar_t *filename = wcsrchr(backgroundFile, L'\\');
        if (filename && wcscpy_s(filename + 1, MAX_PATH - (filename + 1 - backgroundFile), L"RRSaveBackground.bmp") == 0)
            dialog.background = (HBITMAP)LoadImageW(NULL, backgroundFile, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    }
    dialog.shadedBackground = PrepareSlotBackground(&dialog);
    POINT origin = {0, 0};
    ClientToScreen(dialog.parent, &origin);
    dialog.panel = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW, panelClass, L"存档槽",
        WS_POPUP, origin.x, origin.y, width, height,
        dialog.parent, NULL, moduleInstance, &dialog);
    if (!dialog.panel) goto cleanup;
    dialog.previousCapture = GetCapture();
    ReleaseCapture();
    dialog.parentEnabled = IsWindowEnabled(dialog.parent);
    if (dialog.parentEnabled) EnableWindow(dialog.parent, FALSE);
    // The original game uses an 8-bit display. Composite text and panels into one RGB frame.
    dialog.message = CreateWindowW(L"STATIC", L"选择一个存档槽。\r\n\r\nF2 重命名，Esc 返回。", WS_CHILD,
        0, 0, 0, 0, dialog.panel, NULL, moduleInstance, NULL);
    HWND firstButton = NULL;
    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        BOOL exists = GetFileAttributesW(dialog.paths[slot]) != INVALID_FILE_ATTRIBUTES;
        HWND button = AddControl(&dialog, L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 101 + slot, slot);
        UpdateSlotLabel(&dialog, slot);
        if (!saving && !exists) EnableWindow(button, FALSE);
        if (!firstButton && (saving || exists)) firstButton = button;
    }
    HWND cancel = AddControl(&dialog, L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON,
        IDCANCEL, SLOT_COUNT);
    AddControl(&dialog, L"BUTTON", L"重命名（F2）", WS_TABSTOP, 201, 0);
    AddControl(&dialog, L"BUTTON", L"确定", WS_TABSTOP, 202, 0);
    AddControl(&dialog, L"BUTTON", L"返回", WS_TABSTOP, 203, 0);
    ShowWindow(GetDlgItem(dialog.panel, 202), SW_HIDE);
    ShowWindow(GetDlgItem(dialog.panel, 203), SW_HIDE);
    dialog.nameEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, dialog.panel, (HMENU)204, moduleInstance, NULL);
    originalNameEditProc = (WNDPROC)SetWindowLongPtrW(dialog.nameEdit, GWLP_WNDPROC, (LONG_PTR)NameEditProc);
    SendMessageW(dialog.nameEdit, WM_SETFONT, (WPARAM)dialog.font, FALSE);
    SendMessageW(dialog.nameEdit, EM_SETLIMITTEXT, 12, 0);
    ResizeSlotPanel(&dialog);
    ShowWindow(dialog.panel, SW_SHOW);
    SetActiveWindow(dialog.panel);
    SetFocus(firstButton ? firstButton : cancel);
    UpdateWindow(dialog.panel);
    MSG message;
    while (!dialog.finished) {
        int status = GetMessageW(&message, NULL, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage((int)message.wParam);
            break;
        }
        if (message.hwnd == dialog.parent && message.message == WM_SIZE) {
            RECT client;
            GetClientRect(dialog.parent, &client);
            POINT origin = {0, 0};
            ClientToScreen(dialog.parent, &origin);
            SetWindowPos(dialog.panel, HWND_TOP, origin.x, origin.y, client.right, client.bottom, SWP_NOACTIVATE);
        }
        if (dialog.editingName) {
            if (dialog.composingName) { TranslateMessage(&message); DispatchMessageW(&message); }
            else if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) FinishSlotName(&dialog, FALSE);
            else if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) FinishSlotName(&dialog, TRUE);
            else if (!IsDialogMessageW(dialog.panel, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_F2) {
            BeginSlotName(&dialog, dialog.focusedSlot, FALSE);
        } else if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            dialog.finished = TRUE;
        } else if (message.message == WM_KEYDOWN && (message.wParam == VK_RETURN || message.wParam == VK_SPACE)) {
            int control = GetDlgCtrlID(GetFocus());
            if (control == IDCANCEL) dialog.finished = TRUE;
            else if (control >= 201 && control <= 203) ActivateNameControl(&dialog, control);
            else if (control >= 101 && control < 101 + SLOT_COUNT) ChooseSlot(&dialog, control - 101);
        } else if (message.message == WM_KEYDOWN && (message.wParam == VK_UP || message.wParam == VK_DOWN)) {
            int control = GetDlgCtrlID(GetFocus());
            int row = control == IDCANCEL ? SLOT_COUNT : control >= 101 && control < 101 + SLOT_COUNT ? control - 101 : 0;
            for (int attempt = 0; attempt <= SLOT_COUNT; attempt++) {
                row = (row + (message.wParam == VK_UP ? SLOT_COUNT : 1)) % (SLOT_COUNT + 1);
                HWND next = GetDlgItem(dialog.panel, row == SLOT_COUNT ? IDCANCEL : 101 + row);
                if (IsWindowEnabled(next)) { SetFocus(next); break; }
            }
        } else if (message.message == WM_KEYDOWN && message.wParam >= '1' && message.wParam <= '1' + SLOT_COUNT - 1) {
            ChooseSlot(&dialog, (int)message.wParam - '1');
        } else if (!IsDialogMessageW(dialog.panel, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (dialog.selected >= 0 && strlen(slotNames[dialog.selected]) < request->nMaxFile &&
        strcpy_s(request->lpstrFile, request->nMaxFile, slotNames[dialog.selected]) == 0) {
        request->nFileOffset = 0;
        request->nFileExtension = (WORD)(strrchr(request->lpstrFile, '.') - request->lpstrFile + 1);
        result = TRUE;
    }
cleanup:
    DismissSlots(&dialog);
    if (dialog.font) DeleteObject(dialog.font);
    if (dialog.titleFont) DeleteObject(dialog.titleFont);
    if (dialog.detailFont) DeleteObject(dialog.detailFont);
    if (dialog.inputFont) DeleteObject(dialog.inputFont);
    if (dialog.background) DeleteObject(dialog.background);
    if (dialog.shadedBackground) DeleteObject(dialog.shadedBackground);
    if (IsWindow(dialog.parent)) {
        InvalidateRect(dialog.parent, NULL, FALSE);
    }
    if (IsWindow(dialog.previousCapture)) SetCapture(dialog.previousCapture);
    InterlockedExchange(&dialogActive, 0);
    return result;
}

static BOOL WINAPI OpenSlot(OPENFILENAMEA *request) { return ShowSlots(request, FALSE); }
static BOOL WINAPI SaveSlot(OPENFILENAMEA *request) { return ShowSlots(request, TRUE); }

__declspec(dllexport) void __cdecl RRSaveInitialize(void) {
    static LONG initializationStarted;
    if (InterlockedCompareExchange(&initializationStarted, 1, 0)) return;
    HMODULE executable = GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)executable;
    IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)((BYTE *)executable + dos->e_lfanew);
    if (headers->OptionalHeader.AddressOfEntryPoint < 0x228000 || headers->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) return;
    if (!InitializeSlotFiles()) return;
    originalCreateFile = CreateFileA;
    originalWriteFile = WriteFile;
    originalCloseHandle = CloseHandle;
    WNDCLASSW windowClass = {0};
    windowClass.lpfnWndProc = PanelProc;
    windowClass.hInstance = moduleInstance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = panelClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    DWORD previous;
    void **imports = (void **)((BYTE *)executable + 0x21c0b4);
    SIZE_T importSpan = 0x21c43c + sizeof(void *) - 0x21c0b4;
    if (!VirtualProtect(imports, importSpan, PAGE_READWRITE, &previous)) return;
    imports[0] = (void *)OpenSlot;
    imports[1] = (void *)SaveSlot;
    DWORD ignored;
    const DWORD locations[] = {0x21c3e8, 0x21c3d8, 0x21c43c};
    void *replacements[] = {(void *)CreateSlotFile, (void *)WriteSlotFile, (void *)CloseSlotFile};
    for (unsigned index = 0; index < 3; ++index) {
        void **entry = (void **)((BYTE *)executable + locations[index]);
        *entry = replacements[index];
    }
    VirtualProtect(imports, importSpan, previous, &ignored);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        moduleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

#ifdef RR_SAVE_PREVIEW
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show) {
    (void)previous;
    (void)show;
    SetProcessDPIAware();
    moduleInstance = instance;
    InitializeCriticalSection(&fileLock);
    DWORD folderLength = GetModuleFileNameW(instance, saveFolder, MAX_PATH);
    wchar_t *previewName = folderLength && folderLength < MAX_PATH ? wcsrchr(saveFolder, L'\\') : NULL;
    if (!previewName || wcscpy_s(previewName + 1, MAX_PATH - (previewName + 1 - saveFolder), L"PreviewSaves") != 0) return 1;
    if (!CreateDirectoryW(saveFolder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 1;
    WNDCLASSW windowClass = {0};
    windowClass.lpfnWndProc = PanelProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.lpszClassName = panelClass;
    if (!RegisterClassW(&windowClass)) return 1;
    int width = 1280, height = 720;
    if (sscanf(commandLine, "%dx%d", &width, &height) != 2 || width < 640 || height < 480) {
        width = 1280; height = 720;
    }
    RECT preview = {0, 0, width, height};
    AdjustWindowRect(&preview, WS_OVERLAPPEDWINDOW, FALSE);
    HWND parent = CreateWindowExW(0, L"STATIC", L"Road Rash 存档界面原型预览",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        preview.right - preview.left, preview.bottom - preview.top,
        NULL, NULL, instance, NULL);
    if (!parent) return 2;
    char filename[MAX_PATH] = {0};
    OPENFILENAMEA request = {0};
    request.lStructSize = sizeof(request);
    request.hwndOwner = parent;
    request.lpstrFile = filename;
    request.nMaxFile = MAX_PATH;
    ShowSlots(&request, TRUE);
    DestroyWindow(parent);
    return 0;
}
#endif
