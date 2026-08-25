#include "tray_icon.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace bt {
namespace {

// Widest content the tray ever shows; the font is fitted to these so the icon
// never changes size while the percentage counts down.
constexpr std::wstring_view kWidestSamples[] = {L"88", L"FL"};

constexpr int kFallbackIconSize = 16;
constexpr int kMinimumEmSize = 6;

// Fitting the widest content edge to edge makes the digits look oversized next
// to the shell's own tray glyphs, which all keep a margin inside their box.
// Leaving a tenth of the box unused lands between that and the noticeably small
// result of rendering large text and letting the shell scale the bitmap down.
// Do not trade this margin for a heavier weight either: a smaller em with more
// stroke fills in the counters of 8 and 0 at tray sizes and reads worse than
// both. The three constants here were settled by comparing builds side by side
// in the tray, and they interact -- changing one means re-checking the others.
constexpr int kFillPercent = 90;

// Rasterize this many times larger and average back down. Hinting can snap
// stems to the pixel grid but never a diagonal, so 7 came out as a stack of
// hard steps when drawn straight at icon size; averaging a larger raster gives
// that stroke a real gradient. Past 2 the extra coverage levels stop being
// visible at this size, and each step costs a larger intermediate bitmap.
constexpr int kSupersample = 2;

// Rasterized coverage is linear, and using it as alpha directly leaves the
// glyphs looking thinner than the text the shell draws: ClearType biases
// partial coverage upward to compensate for how light strokes on a dark
// background read. Supersampling makes that worse -- the box filter spreads a
// stem that hinting used to land on one solid pixel across two partial ones,
// so the peak the eye reads as "solid" drops -- which is why this sits well
// above the 1.8 or so ClearType itself uses. Calibrated against white text on a
// dark taskbar; the light theme's black text needs the opposite bias, so the
// same curve thickens it slightly, which measured out as acceptable.
constexpr int kCoverageGammaPercent = 220;

} // namespace

IconRenderer::IconRenderer(COLORREF text_color)
    : text_color_(text_color), dc_(CreateCompatibleDC(nullptr)) {
    // Baked into a table so the per-pixel loop stays a lookup.
    constexpr double exponent = 100.0 / kCoverageGammaPercent;
    for (int i = 0; i < 256; ++i) {
        const double corrected = std::pow(i / 255.0, exponent) * 255.0 + 0.5;
        coverage_curve_[i] = static_cast<BYTE>(std::min(corrected, 255.0));
    }
}

void IconRenderer::ensure_font(UINT dpi) {
    if (font_ && dpi == font_dpi_) {
        return;
    }

    // The tray scales its icons with DPI, so the em size has to follow instead
    // of being a constant tuned at 100%.
    icon_width_ = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    icon_height_ = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    if (icon_width_ <= 0 || icon_height_ <= 0) {
        icon_width_ = icon_height_ = kFallbackIconSize;
    }
    // Everything below fits and draws at the supersampled size; render() box
    // filters that back down to the icon size.
    width_ = icon_width_ * kSupersample;
    height_ = icon_height_ * kSupersample;

    // Segoe UI by name rather than the shell's lfMessageFont: on Chinese Windows
    // that resolves to Microsoft YaHei UI, which ships embedded bitmap strikes
    // covering exactly the small ppem sizes a tray icon lands on. GDI prefers a
    // matching strike over the outlines and offers no way to opt out, so the
    // glyphs come back hard-edged whatever lfQuality says -- which is why the
    // aliasing appeared only below a certain em size, where the strikes end.
    // Segoe UI carries no strikes, covers the digits and "FL", and is the face
    // the shell renders its own tray text with.
    LOGFONTW base{};
    base.lfWeight = FW_NORMAL;
    base.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(base.lfFaceName, L"Segoe UI");
    // Grayscale AA rather than ClearType: the icon is composited over an
    // arbitrary taskbar through its alpha channel, and subpixel coverage would
    // show up as colored fringes on the glyph edges.
    base.lfQuality = ANTIALIASED_QUALITY;

    if (!dc_) {
        return;
    }

    const int width_budget = std::max(width_ * kFillPercent / 100, 1);
    const int height_budget = std::max(height_ * kFillPercent / 100, 1);

    for (int em = height_; em >= kMinimumEmSize; --em) {
        base.lfHeight = -em;
        unique_font candidate(CreateFontIndirectW(&base));
        if (!candidate) {
            continue;
        }

        const HGDIOBJ previous = SelectObject(dc_.get(), candidate.get());
        TEXTMETRICW text_metrics{};
        int widest = 0;
        if (GetTextMetricsW(dc_.get(), &text_metrics)) {
            for (const std::wstring_view sample : kWidestSamples) {
                SIZE size{};
                if (GetTextExtentPoint32W(dc_.get(), sample.data(), static_cast<int>(sample.size()), &size)) {
                    widest = std::max(widest, static_cast<int>(size.cx));
                }
            }
        }
        SelectObject(dc_.get(), previous);

        // Digits and "FL" never rise above the cap line, so the internal
        // leading (room reserved for accents) must not count against the fit.
        const int cap_height = text_metrics.tmAscent - text_metrics.tmInternalLeading;
        if (widest > 0 && widest <= width_budget && cap_height > 0 && cap_height <= height_budget) {
            font_ = std::move(candidate);
            font_dpi_ = dpi;
            baseline_ = (height_ + cap_height) / 2;
            return;
        }
    }
}

unique_icon IconRenderer::render(std::wstring_view text, UINT dpi) {
    ensure_font(dpi);
    if (!dc_ || !font_ || text.empty()) {
        return {};
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_; // top-down, so row order matches the loop below
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    unique_bitmap sample(CreateDIBSection(dc_.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!sample || !pixels) {
        return {};
    }

    const HGDIOBJ previous_bitmap = SelectObject(dc_.get(), sample.get());
    const HGDIOBJ previous_font = SelectObject(dc_.get(), font_.get());
    SetBkMode(dc_.get(), TRANSPARENT);
    // GDI leaves the alpha channel untouched, so the glyphs are drawn white on
    // the zero-filled (black) DIB and that coverage becomes the alpha below.
    SetTextColor(dc_.get(), RGB(255, 255, 255));
    SetTextAlign(dc_.get(), TA_LEFT | TA_BASELINE);

    SIZE size{};
    const int length = static_cast<int>(text.size());
    const int x = GetTextExtentPoint32W(dc_.get(), text.data(), length, &size)
                      ? (width_ - static_cast<int>(size.cx)) / 2
                      : 0;
    TextOutW(dc_.get(), x, baseline_, text.data(), length);
    GdiFlush(); // DIB bits are only guaranteed to be written back after this

    SelectObject(dc_.get(), previous_font);
    SelectObject(dc_.get(), previous_bitmap);

    info.bmiHeader.biWidth = icon_width_;
    info.bmiHeader.biHeight = -icon_height_;
    void* icon_pixels = nullptr;
    unique_bitmap color(CreateDIBSection(dc_.get(), &info, DIB_RGB_COLORS, &icon_pixels, nullptr, 0));
    if (!color || !icon_pixels) {
        return {};
    }

    const int red = GetRValue(text_color_);
    const int green = GetGValue(text_color_);
    const int blue = GetBValue(text_color_);
    const int taps = kSupersample * kSupersample;
    const BYTE* const source = static_cast<const BYTE*>(pixels);
    BYTE* pixel = static_cast<BYTE*>(icon_pixels);
    for (int y = 0; y < icon_height_; ++y) {
        for (int x = 0; x < icon_width_; ++x, pixel += 4) {
            // Box filter the supersampled coverage. Hinting can align stems to
            // the pixel grid but never a diagonal, so 7 comes out as a stack of
            // hard steps when rasterized straight at icon size; averaging a
            // larger raster gives that stroke a real gradient instead.
            int sum = 0;
            for (int sy = 0; sy < kSupersample; ++sy) {
                const BYTE* row =
                    source + (static_cast<size_t>(y * kSupersample + sy) * width_ + x * kSupersample) * 4;
                for (int sx = 0; sx < kSupersample; ++sx) {
                    sum += row[sx * 4]; // grayscale AA: all three channels agree
                }
            }
            const int coverage = coverage_curve_[(sum + taps / 2) / taps];
            // Premultiplied BGRA: icons built from a 32bpp DIB are composited the
            // AlphaBlend way, and straight alpha would light up the glyph edges.
            pixel[0] = static_cast<BYTE>((blue * coverage + 127) / 255);
            pixel[1] = static_cast<BYTE>((green * coverage + 127) / 255);
            pixel[2] = static_cast<BYTE>((red * coverage + 127) / 255);
            pixel[3] = static_cast<BYTE>(coverage);
        }
    }

    // A zeroed AND mask leaves visibility entirely to the alpha channel.
    const size_t mask_stride = ((static_cast<size_t>(icon_width_) + 15) / 16) * 2;
    const std::vector<BYTE> mask_bits(mask_stride * static_cast<size_t>(icon_height_), 0);
    unique_bitmap mask(CreateBitmap(icon_width_, icon_height_, 1, 1, mask_bits.data()));
    if (!mask) {
        return {};
    }

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask.get();
    icon_info.hbmColor = color.get();
    return unique_icon(CreateIconIndirect(&icon_info)); // copies both bitmaps
}

} // namespace bt
