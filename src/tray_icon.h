#pragma once

#include "win32.h"
#include "win32_raii.h"

#include <string_view>

namespace bt {

// Turns the battery text (L"87", L"FL") into a notification-area sized HICON.
// The fitted font and the measuring DC are kept across renders because a render
// happens on every battery change and creating GDI objects there is pure waste.
class IconRenderer {
public:
    explicit IconRenderer(COLORREF text_color);

    // Caller owns the icon; empty on failure.
    [[nodiscard]] unique_icon render(std::wstring_view text, UINT dpi);

private:
    void ensure_font(UINT dpi);

    COLORREF text_color_;
    unique_dc dc_;
    unique_font font_;
    UINT font_dpi_ = 0;
    int width_ = 0;
    int height_ = 0;
    int baseline_ = 0;
};

} // namespace bt
