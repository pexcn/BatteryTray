#pragma once

#include "win32.h"
#include "win32_raii.h"

#include <dwrite_1.h> // IDWriteBitmapRenderTarget1, for grayscale antialiasing

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
                          PCWSTR face = kDefaultFace, int supersample = 1, int gamma_percent = 100,
                          bool use_dwrite = false);

    // Caller owns the icon; empty on failure.
    [[nodiscard]] unique_icon render(std::wstring_view text, UINT dpi);

    // TEMPORARY: the face GDI settled on, which is not necessarily the one asked
    // for; only valid after a render.
    [[nodiscard]] PCWSTR resolved_face() const { return resolved_face_; }

    // TEMPORARY: whether DirectWrite actually came up, since it falls back to
    // GDI silently.
    [[nodiscard]] bool dwrite_active() const { return dwrite_face_ && dwrite_target_; }

private:
    void ensure_font(UINT dpi);
    void ensure_dwrite();
    // False when DirectWrite is unavailable or refuses the run, so the caller
    // can fall back to GDI.
    bool draw_with_dwrite(std::wstring_view text);

    COLORREF text_color_;
    int fill_percent_;
    wchar_t face_[LF_FACESIZE];
    wchar_t resolved_face_[LF_FACESIZE] = {}; // TEMPORARY: see resolved_face()
    int supersample_;
    BYTE coverage_curve_[256]; // gamma correction applied to the rasterized coverage
    bool use_dwrite_;
    unique_dc dc_;
    unique_font font_;
    // Rebuilt with the font, since both are tied to the fitted em size.
    com_ptr<IDWriteFactory> dwrite_factory_;
    com_ptr<IDWriteGdiInterop> dwrite_interop_;
    com_ptr<IDWriteRenderingParams> dwrite_params_;
    com_ptr<IDWriteFontFace> dwrite_face_;
    com_ptr<IDWriteBitmapRenderTarget> dwrite_target_;
    int em_size_ = 0;
    UINT font_dpi_ = 0;
    int icon_width_ = 0;  // what the tray gets
    int icon_height_ = 0;
    int width_ = 0;       // what the glyphs are rasterized into, supersample_ times larger
    int height_ = 0;
    int baseline_ = 0;
};

} // namespace bt
