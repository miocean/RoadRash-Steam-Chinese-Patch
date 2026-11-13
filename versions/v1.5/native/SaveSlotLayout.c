typedef struct {
    int canvasWidth;
    RECT viewport;
} SlotLayout;

static SlotLayout CalculateSlotLayout(int width, int height, BOOL widescreen) {
    SlotLayout layout = {0};
    if (width <= 0 || height <= 0) return layout;
    layout.canvasWidth = 640;
    layout.viewport.right = width;
    layout.viewport.bottom = height;
    if ((LONGLONG)width * 3 >= (LONGLONG)height * 4) {
        if (widescreen) {
            layout.canvasWidth = (int)(((LONGLONG)width * 480 + height / 2) / height);
        } else {
            int viewportWidth = (int)((LONGLONG)height * 4 / 3);
            layout.viewport.left = (width - viewportWidth) / 2;
            layout.viewport.right = layout.viewport.left + viewportWidth;
        }
    } else {
        int viewportHeight = width * 3 / 4;
        layout.viewport.top = (height - viewportHeight) / 2;
        layout.viewport.bottom = layout.viewport.top + viewportHeight;
    }
    return layout;
}

static RECT SlotControlRectangle(const SlotLayout *layout, int row) {
    int top = row == SLOT_COUNT ? 395 : 132 + row * 48;
    RECT rectangle = {30, top, row == SLOT_COUNT ? 270 : layout->canvasWidth / 2 - 10, top + 44};
    return rectangle;
}

static RECT SlotViewportRectangle(const SlotLayout *layout, RECT rectangle) {
    int width = layout->viewport.right - layout->viewport.left;
    int height = layout->viewport.bottom - layout->viewport.top;
    RECT result = {
        layout->viewport.left + MulDiv(rectangle.left, width, layout->canvasWidth),
        layout->viewport.top + MulDiv(rectangle.top, height, 480),
        layout->viewport.left + MulDiv(rectangle.right, width, layout->canvasWidth),
        layout->viewport.top + MulDiv(rectangle.bottom, height, 480)
    };
    return result;
}

static int SlotHitTest(const SlotLayout *layout, POINT point) {
    for (int row = 0; row <= SLOT_COUNT; row++) {
        RECT rectangle = SlotViewportRectangle(layout, SlotControlRectangle(layout, row));
        if (PtInRect(&rectangle, point)) return row;
    }
    return -1;
}

static int SlotBackgroundColumn(int column, int canvasWidth) {
    int reflected = (column - (canvasWidth - 640) / 2) % 1280;
    if (reflected < 0) reflected += 1280;
    return reflected < 640 ? reflected : 1279 - reflected;
}
