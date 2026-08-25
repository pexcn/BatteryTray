#include "tray_icon.h"

#include <algorithm>
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
// Do not trade this margin for a heavier weight: a smaller em with more stroke
// fills in the counters of 8 and 0 at tray sizes, which reads worse than either.
constexpr int kFillPercent = 90;

} // namespace

IconRenderer::IconRenderer(COLORREF text_color)
    : text_color_(text_color), dc_(CreateCompatibleDC(nullptr)) {}

void IconRenderer::ensure_font(UINT dpi) {
    if (font_ && dpi == font_dpi_) {
        return;
    }

    // The tray scales its icons with DPI, so the em size has to follow instead
    // of being a constant tuned at 100%.
    width_ = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    height_ = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    if (width_ <= 0 || height_ <= 0) {
        width_ = height_ = kFallbackIconSize;
    }

    LOGFONTW base{};
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        base = metrics.lfMessageFont;
    } else {
        base.lfWeight = FW_NORMAL;
        base.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(base.lfFaceName, L"Segoe UI");
    }
    base.lfWidth = 0;
    base.lfEscapement = 0;
    base.lfOrientation = 0;
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
    unique_bitmap color(CreateDIBSection(dc_.get(), &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
    if (!color || !pixels) {
        return {};
    }

    const HGDIOBJ previous_bitmap = SelectObject(dc_.get(), color.get());
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

    const int red = GetRValue(text_color_);
    const int green = GetGValue(text_color_);
    const int blue = GetBValue(text_color_);
    BYTE* pixel = static_cast<BYTE*>(pixels);
    for (int i = 0, count = width_ * height_; i < count; ++i, pixel += 4) {
        const int coverage = pixel[0]; // grayscale AA: all three channels agree
        // Premultiplied BGRA: icons built from a 32bpp DIB are composited the
        // AlphaBlend way, and straight alpha would light up the glyph edges.
        pixel[0] = static_cast<BYTE>((blue * coverage + 127) / 255);
        pixel[1] = static_cast<BYTE>((green * coverage + 127) / 255);
        pixel[2] = static_cast<BYTE>((red * coverage + 127) / 255);
        pixel[3] = static_cast<BYTE>(coverage);
    }

    // A zeroed AND mask leaves visibility entirely to the alpha channel.
    const size_t mask_stride = ((static_cast<size_t>(width_) + 15) / 16) * 2;
    const std::vector<BYTE> mask_bits(mask_stride * static_cast<size_t>(height_), 0);
    unique_bitmap mask(CreateBitmap(width_, height_, 1, 1, mask_bits.data()));
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
