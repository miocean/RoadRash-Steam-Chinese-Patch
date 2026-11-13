#include "SaveSlots.c"
#include <stdlib.h>

static SlotDialog dialog;
static int assertions;
static int ownerReadyAtDestroy;

static void Assert(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
    assertions++;
}

static LRESULT CALLBACK TestPanelProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) {
        ownerReadyAtDestroy = IsWindowEnabled(dialog.parent) &&
            GetActiveWindow() == dialog.parent && GetFocus() == dialog.parent;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int main(void) {
    WNDCLASSW windowClass = {0};
    windowClass.hInstance = GetModuleHandleW(NULL);
    windowClass.lpfnWndProc = TestPanelProc;
    windowClass.lpszClassName = L"RoadRashSaveReturnTest";
    Assert(RegisterClassW(&windowClass), "Register regression window");
    dialog.parent = CreateWindowW(L"STATIC", L"Save return regression", WS_POPUP,
        20, 20, 320, 240, NULL, NULL, windowClass.hInstance, NULL);
    Assert(dialog.parent != NULL, "Create game owner fixture");
    ShowWindow(dialog.parent, SW_SHOWNOACTIVATE);
    for (int attempt = 0; attempt < 5; attempt++) {
        dialog.panel = CreateWindowW(windowClass.lpszClassName, L"Slots", WS_POPUP,
            20, 20, 320, 240, dialog.parent, NULL, windowClass.hInstance, NULL);
        Assert(dialog.panel != NULL, "Create owned slot fixture");
        dialog.parentEnabled = TRUE;
        EnableWindow(dialog.parent, FALSE);
        ShowWindow(dialog.panel, SW_SHOW);
        SetActiveWindow(dialog.panel);
        ownerReadyAtDestroy = FALSE;
        DismissSlots(&dialog);
        Assert(ownerReadyAtDestroy, "Enabled active owner restored before popup destruction");
        Assert(GetActiveWindow() == dialog.parent && GetFocus() == dialog.parent,
            "Closing slots leaves game owner active and focused");
        Assert(!IsWindow(dialog.panel), "Slot popup fully released");
    }
    DestroyWindow(dialog.parent);
    printf("PASS %d return-transition assertions\n", assertions);
    return 0;
}
