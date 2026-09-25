// Off-screen rendering tests for CandidateWindow.
//
// The window is built without a TextService, drawn with paint() into a 32-bit
// memory DC, and the pixels are inspected. This needs no TSF session, no visible
// window and no DWM, so it also runs on CI. It guards the regressions that were
// only ever noticed by eye before: the header row not being drawn, and the theme
// colors (panel background, selection highlight) being ignored.

#include <windows.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "CandidateWindow.h"

namespace {

const COLORREF kSentinel = RGB(1, 2, 3);  // pre-fill: anything left like this was never painted

struct Theme {
    COLORREF panelBg, panelBorder, textPrimary, textSecondary, highlightBg, highlightBorder, highlightText;
};
// Same values as PIMEClient.cpp candidateThemeColors() "sepiadim" and "graphite".
const Theme kSepiaDim = { RGB(40, 37, 31), RGB(93, 86, 74), RGB(235, 226, 211), RGB(185, 173, 154),
                          RGB(109, 101, 71), RGB(149, 138, 99), RGB(248, 239, 217) };
const Theme kGraphite = { RGB(18, 20, 26), RGB(68, 74, 87), RGB(243, 245, 250), RGB(174, 181, 196),
                          RGB(65, 105, 215), RGB(111, 146, 235), RGB(237, 243, 255) };

struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<COLORREF> px;  // row-major, top-down

    COLORREF at(int x, int y) const { return px[y * width + x]; }

    size_t count(COLORREF c, int y0 = 0, int y1 = -1) const {
        if (y1 < 0) y1 = height;
        size_t n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width; ++x)
                if (at(x, y) == c) ++n;
        return n;
    }

    // pixels in rows [y0, y1) that are none of the given colors (i.e. text/glyphs)
    size_t countOther(std::initializer_list<COLORREF> known, int y0, int y1) const {
        size_t n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width; ++x) {
                COLORREF c = at(x, y);
                bool isKnown = false;
                for (COLORREF k : known) isKnown = isKnown || c == k;
                if (!isKnown) ++n;
            }
        return n;
    }

    COLORREF dominant() const {
        std::map<COLORREF, size_t> histogram;
        for (COLORREF c : px) ++histogram[c];
        COLORREF best = 0;
        size_t bestCount = 0;
        for (auto& kv : histogram)
            if (kv.second > bestCount) { best = kv.first; bestCount = kv.second; }
        return best;
    }
};

struct ReleaseWindow {
    void operator()(Ime::CandidateWindow* window) const { window->Release(); }
};
using WindowPtr = std::unique_ptr<Ime::CandidateWindow, ReleaseWindow>;

class CandidateWindowRenderTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        Ime::Window::registerClass(::GetModuleHandleW(NULL));
        font_ = ::CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                              DEFAULT_PITCH, L"Microsoft JhengHei");
    }
    static void TearDownTestSuite() { ::DeleteObject(font_); }

    // A candidate window like the one 大易 shows: 6 per row, first candidate selected.
    WindowPtr makeWindow(bool modern, const Theme& theme, const std::wstring& header, const std::wstring& pageInfo) {
        WindowPtr window(new Ime::CandidateWindow(nullptr, nullptr));
        window->setFont(font_);
        window->setCandPerRow(6);
        window->setTheme(theme.panelBg, theme.panelBorder, theme.textPrimary, theme.textSecondary,
                         theme.highlightBg, theme.highlightBorder, theme.highlightText);
        window->setSpacing(6, 4, 6);
        window->setModernStyle(modern);
        const wchar_t* items[] = { L"人", L"入", L"八", L"乂", L"亼", L"仌" };
        const wchar_t keys[] = L"123456";
        for (int i = 0; i < 6; ++i)
            window->add(items[i], keys[i]);
        window->setHeader(header);
        window->setPageInfo(pageInfo);
        window->recalculateSize();
        window->setCurrentSel(0);
        return window;
    }

    static Pixels render(Ime::CandidateWindow& window) {
        RECT rc;
        ::GetClientRect(window.hwnd(), &rc);
        Pixels out;
        out.width = rc.right - rc.left;
        out.height = rc.bottom - rc.top;
        if (out.width <= 0 || out.height <= 0)
            return out;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = out.width;
        bi.bmiHeader.biHeight = -out.height;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = ::GetDC(NULL);
        HDC mem = ::CreateCompatibleDC(screen);
        HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBitmap = ::SelectObject(mem, dib);

        HBRUSH sentinel = ::CreateSolidBrush(kSentinel);
        ::FillRect(mem, &rc, sentinel);
        ::DeleteObject(sentinel);

        window.paint(mem, rc);
        ::GdiFlush();

        const DWORD* p = static_cast<const DWORD*>(bits);
        out.px.resize(static_cast<size_t>(out.width) * out.height);
        for (size_t i = 0; i < out.px.size(); ++i)  // DIB pixels are 0x00RRGGBB
            out.px[i] = RGB((p[i] >> 16) & 0xff, (p[i] >> 8) & 0xff, p[i] & 0xff);

        ::SelectObject(mem, oldBitmap);
        ::DeleteObject(dib);
        ::DeleteDC(mem);
        ::ReleaseDC(NULL, screen);
        return out;
    }

    static HFONT font_;
};

HFONT CandidateWindowRenderTest::font_ = NULL;

TEST_F(CandidateWindowRenderTest, ModernStyleFillsPanelWithThemeBackground) {
    for (const Theme* theme : { &kSepiaDim, &kGraphite }) {
        auto window = makeWindow(true, *theme, L"大易 人", L"1/1");
        Pixels px = render(*window);
        ASSERT_GT(px.width, 0);
        ASSERT_GT(px.height, 0);
        EXPECT_EQ(px.dominant(), theme->panelBg) << "panel background is not the theme color";
        // Only the rounded corners may stay unpainted.
        EXPECT_LT(px.count(kSentinel), px.px.size() / 20);
    }
}

// The selected candidate's background is a tint of the panel toward the theme's
// highlight color (CandidateWindow blends them), so look for pixels whose every
// channel lies between the two - and none when there is no selection cursor.
static bool tintBetween(COLORREF c, COLORREF from, COLORREF to) {
    if (c == from)
        return false;
    auto within = [](int v, int a, int b) { return v >= std::min(a, b) && v <= std::max(a, b); };
    return within(GetRValue(c), GetRValue(from), GetRValue(to)) &&
           within(GetGValue(c), GetGValue(from), GetGValue(to)) &&
           within(GetBValue(c), GetBValue(from), GetBValue(to));
}

TEST_F(CandidateWindowRenderTest, ModernStyleHighlightsCurrentSelectionWithThemeColor) {
    auto selected = makeWindow(true, kSepiaDim, L"大易 人", L"1/1");
    auto noCursor = makeWindow(true, kSepiaDim, L"大易 人", L"1/1");
    noCursor->setUseCursor(false);
    Pixels a = render(*selected);
    Pixels b = render(*noCursor);
    ASSERT_EQ(a.width, b.width);
    ASSERT_EQ(a.height, b.height);
    // Pixels that only differ because of the selection, and now show the tint:
    // that is the highlighted item's background. (Other elements such as the
    // header badge use tints too, so compare the two renders instead of counting.)
    size_t highlightPixels = 0;
    for (size_t i = 0; i < a.px.size(); ++i)
        if (a.px[i] != b.px[i] && tintBetween(a.px[i], kSepiaDim.panelBg, kSepiaDim.highlightBg))
            ++highlightPixels;
    EXPECT_GT(highlightPixels, 200u) << "selecting a candidate did not draw a highlight in the theme color";
    // and another theme's colors must not leak in
    // (exact colors only: the two themes' tint ranges overlap)
    EXPECT_EQ(a.count(kGraphite.panelBg), 0u);
    EXPECT_EQ(a.count(kGraphite.highlightBg), 0u);
}

TEST_F(CandidateWindowRenderTest, HeaderRowIsDrawnAboveTheCandidates) {
    auto withHeader = makeWindow(true, kSepiaDim, L"大易 人", L"1/1");
    auto withoutHeader = makeWindow(true, kSepiaDim, L"", L"");
    Pixels a = render(*withHeader);
    Pixels b = render(*withoutHeader);
    ASSERT_GT(a.height, 0);
    ASSERT_GT(b.height, 0);
    const int headerBand = a.height - b.height;
    ASSERT_GT(headerBand, 8) << "setting a header did not add a header row";
    // The extra band at the top must actually contain the header text (glyph
    // pixels that are neither background, border nor unpainted).
    EXPECT_GT(a.countOther({ kSepiaDim.panelBg, kSepiaDim.panelBorder, kSentinel }, 2, headerBand - 1), 20u)
        << "header row is present but empty";
    // Same width rules either way, so the header must not squeeze the candidates.
    EXPECT_GE(a.width, b.width);
}

TEST_F(CandidateWindowRenderTest, ClassicStyleStillPaintsCandidates) {
    auto window = makeWindow(false, kSepiaDim, L"", L"");
    Pixels px = render(*window);
    ASSERT_GT(px.height, 0);
    EXPECT_EQ(px.count(kSentinel), 0u) << "classic style left part of the window unpainted";
    const COLORREF background = ::GetSysColor(COLOR_WINDOW);
    EXPECT_EQ(px.dominant(), background);
    EXPECT_GT(px.countOther({ background }, 2, px.height - 2), 50u) << "no candidate text drawn";
}

}  // namespace
