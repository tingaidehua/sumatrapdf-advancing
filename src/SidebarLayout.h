/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Splitter input is expressed in frame coordinates, but pane width is local
// to the pane. Always measure from the pane's fixed edge so other sidebars do
// not affect the result.
inline int SidebarDxFromCursor(const Rect& sidebarBounds, int cursorX, bool sidebarOnRight) {
    if (sidebarOnRight) {
        return sidebarBounds.x + sidebarBounds.dx - cursorX;
    }
    return cursorX - sidebarBounds.x;
}

// A top toolbar embedded in the custom caption is already included in the
// caption's measured height. Only standalone top/bottom toolbars consume an
// additional frame row.
inline int ToolbarDyOutsideCaption(bool showCaption, bool showToolbar, bool toolbarBottom, int toolbarDy) {
    bool embeddedInCaption = showCaption && showToolbar && !toolbarBottom;
    return embeddedInCaption ? 0 : toolbarDy;
}
