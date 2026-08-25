#pragma once

#include "win32.h"
#include "win32_raii.h"

#include <string_view>

namespace bt {

// Fitting the widest content edge to edge makes the digits look oversized next
// to the shell's own tray glyphs, which all keep a margin inside their box.
// Leaving a tenth of the box unused lands between that and the noticeably small
// result of rendering large text and letting the shell scale the bitmap down.
// Do not trade this margin for a heavier weight: a smaller em with more stroke
// fills in the counters of 8 and 0 at tray sizes, which reads worse than either.
// Do not raise it much either: past ~91 the fitted em crosses the ppem where
// Segoe UI's gasp table switches to symmetric smoothing, which stops snapping
// the stems to the pixel grid and looks blurry at this size. 90 sits just under
// that step, so it is both the largest and the sharpest option here.
inline constexpr int kDefaultFillPercent = 90;

// Named explicitly rather than taken from lfMessageFont, which is Microsoft
// YaHei UI on Chinese Windows and would bring its embedded bitmap strikes; see
// the face selection in ensure_font().
inline constexpr wchar_t kDefaultFace[] = L"Segoe UI";

// Turns the battery text (L"87", L"FL") into a notification-area sized HICON.
// The fitted font and the measuring DC are kept across renders because a render
// happens on every battery change and creating GDI objects there is pure waste.
class IconRenderer {
public:
    explicit IconRenderer(COLORREF text_color, int fill_percent = kDefaultFillPercent,
                          PCWSTR face = kDefaultFace);

    // Caller owns the icon; empty on failure.
    [[nodiscard]] unique_icon render(std::wstring_view text, UINT dpi);

private:
    void ensure_font(UINT dpi);

    COLORREF text_color_;
    int fill_percent_;
    wchar_t face_[LF_FACESIZE];
    unique_dc dc_;
    unique_font font_;
    UINT font_dpi_ = 0;
    int width_ = 0;
    int height_ = 0;
    int baseline_ = 0;
};

} // namespace bt
