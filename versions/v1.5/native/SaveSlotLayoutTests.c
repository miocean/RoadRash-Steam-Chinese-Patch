#include "SaveSlots.c"
#include <stdlib.h>

static int assertions;
static void Assert(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
    assertions++;
}

int main(void) {
    const int sizes[][2] = {{640,480}, {800,600}, {2560,1440}, {2560,1600},
        {1920,1080}, {3440,1440}, {5120,1440}, {1080,1920}, {1001,777}};
    BITMAPINFO format = {0};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = 640;
    format.bmiHeader.biHeight = -480;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    BYTE *pixels;
    HBITMAP background = CreateDIBSection(NULL, &format, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    Assert(background != NULL, "Background test bitmap");
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) {
        BYTE *pixel = pixels + (y * 640 + x) * 4;
        pixel[0] = 30 + x % 190; pixel[1] = 40 + y % 180; pixel[2] = 80;
    }
    for (unsigned index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        int width = sizes[index][0], height = sizes[index][1];
        SlotLayout layout = CalculateSlotLayout(width, height, TRUE);
        Assert(layout.canvasWidth >= 640, "Logical canvas accommodates original art");
        Assert(layout.viewport.right == width, "Viewport reaches both horizontal edges");
        if ((LONGLONG)width * 3 >= (LONGLONG)height * 4) {
            Assert(layout.viewport.top == 0 && layout.viewport.bottom == height, "Wide viewport fills client height");
            Assert(abs(MulDiv(layout.canvasWidth, height, 480) - width) <= 2, "Original art and text retain aspect ratio");
        }
        for (int row = 0; row <= SLOT_COUNT; row++) {
            RECT logical = SlotControlRectangle(&layout, row);
            RECT control = SlotViewportRectangle(&layout, logical);
            Assert(control.left >= 0 && control.top >= layout.viewport.top && control.right <= width &&
                control.bottom <= layout.viewport.bottom, "Slot and cancel hit areas remain inside viewport");
            POINT center = {MulDiv(100, width, layout.canvasWidth),
                layout.viewport.top + MulDiv(logical.top + 22,
                    layout.viewport.bottom - layout.viewport.top, 480)};
            Assert(PtInRect(&control, center), "Painted text center matches clickable area");
            Assert(SlotHitTest(&layout, center) == row, "Mouse hit test uses rendered text coordinates");
            POINT topLeft = {control.left, control.top};
            POINT bottomRight = {control.right - 1, control.bottom - 1};
            Assert(SlotHitTest(&layout, topLeft) == row && SlotHitTest(&layout, bottomRight) == row,
                "Mouse hit test covers full control bounds");
            POINT outside = {control.right, control.top};
            Assert(SlotHitTest(&layout, outside) == -1, "Mouse hit test rejects outside edge");
        }
        SlotDialog dialog = {0};
        dialog.layout = layout;
        dialog.background = background;
        HBITMAP frame = PrepareSlotBackground(&dialog);
        Assert(frame != NULL, "Wide background generated independently");
        DIBSECTION dib;
        Assert(GetObjectW(frame, sizeof(dib), &dib) == sizeof(dib), "Wide frame exposes DIB pixels");
        Assert(dib.dsBm.bmWidth == layout.canvasWidth && dib.dsBm.bmHeight == 480, "Frame matches logical canvas");
        const int columns[] = {0, 1, 319, 639};
        for (unsigned sample = 0; sample < sizeof(columns) / sizeof(columns[0]); sample++) {
            int x = sample < 2 ? columns[sample] : layout.canvasWidth - (sample == 2 ? 2 : 1);
            int source = SlotBackgroundColumn(x, layout.canvasWidth);
            BYTE *pixel = (BYTE *)dib.dsBm.bmBits + (470 * layout.canvasWidth + x) * 4;
            Assert(source >= 0 && source < 640 && pixel[0] == 30 + source % 190 && pixel[2] == 80,
                "Extended background has populated edge pixels without cropping original art");
        }
        for (int x = 0; x < 640; x++) {
            int centered = (layout.canvasWidth - 640) / 2 + x;
            Assert(SlotBackgroundColumn(centered, layout.canvasWidth) == x, "Original art columns remain centered");
        }
        DeleteObject(frame);
        printf("PASS %dx%d canvas %dx480, background, proportions and hit areas\n", width, height, layout.canvasWidth);
    }
    for (unsigned index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        int width = sizes[index][0], height = sizes[index][1];
        SlotLayout layout = CalculateSlotLayout(width, height, FALSE);
        int renderedWidth = layout.viewport.right - layout.viewport.left;
        int renderedHeight = layout.viewport.bottom - layout.viewport.top;
        Assert(layout.canvasWidth == 640, "Disabled widescreen uses original logical width");
        Assert(abs(renderedWidth * 3 - renderedHeight * 4) <= 3, "Standard save viewport retains 4:3 proportions");
        Assert(abs(layout.viewport.left - (width - layout.viewport.right)) <= 1 &&
            abs(layout.viewport.top - (height - layout.viewport.bottom)) <= 1, "Standard viewport is centered");
        for (int row = 0; row <= SLOT_COUNT; row++) {
            RECT rectangle = SlotViewportRectangle(&layout, SlotControlRectangle(&layout, row));
            POINT center = {(rectangle.left + rectangle.right) / 2, (rectangle.top + rectangle.bottom) / 2};
            Assert(rectangle.left >= layout.viewport.left && rectangle.right <= layout.viewport.right &&
                rectangle.top >= layout.viewport.top && rectangle.bottom <= layout.viewport.bottom,
                "Standard controls stay within game viewport");
            Assert(SlotHitTest(&layout, center) == row, "Standard mouse hit matches centered control");
        }
        POINT border = {layout.viewport.left - 1, height / 2};
        Assert(SlotHitTest(&layout, border) == -1, "Black border is not a clickable slot");
        printf("PASS standard %dx%d viewport %dx%d\n", width, height, renderedWidth, renderedHeight);
    }
    Assert(!IsWidescreenActive(), "Save module works when wide renderer is not loaded");
    DeleteObject(background);
    Assert(CalculateSlotLayout(0, 0, FALSE).canvasWidth == 0, "Minimized size is ignored");
    Assert(PanelProc(NULL, WM_ERASEBKGND, 0, 0) == 1, "Background erase is suppressed before buffered paint");
    printf("PASS %d assertions\n", assertions);
    return 0;
}
