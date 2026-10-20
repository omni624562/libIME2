// Fuzz target for CandidateWindow: what the backend's replies and the user's
// settings (config.json) can put into the candidate window -- any candidate,
// selection key, header, page info and message text, any candidates per row,
// spacing, widths, styles, font size and cursor -- laid out and painted into an
// off-screen bitmap after every step, like the window does on screen.
//
// The input is a little program: each operation is one byte followed by its
// arguments (see the switch below). Strings are UTF-16 code units taken straight
// from the input, so they include lone surrogates, NULs and control characters.
//
// Built two ways (test/CMakeLists.txt):
// - CandidateWindow_fuzz: libFuzzer + AddressSanitizer (-DLIBIME2_FUZZ=ON, x64),
//   run by WIME's fuzz workflow;
// - CandidateWindow_fuzz_replay: an ordinary test (replay_main.cpp) running every
//   saved input in test/fuzz/seeds and test/fuzz/regressions, part of ctest.

#include <windows.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CandidateWindow.h"
#include "KeyEvent.h"
#include "Window.h"

namespace {

class Input {
public:
    Input(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    bool empty() const { return p_ >= end_; }
    uint8_t byte() { return p_ < end_ ? *p_++ : 0; }
    int i32() {
        // mostly small values, sometimes anything at all (the backend sends ints)
        uint8_t kind = byte();
        switch (kind % 4) {
        case 0: return static_cast<int8_t>(byte());
        case 1: return byte() * 8;
        case 2: return static_cast<int16_t>(byte() | (byte() << 8));
        default: {
            uint32_t v = byte() | (byte() << 8) | (byte() << 16) | (static_cast<uint32_t>(byte()) << 24);
            int out;
            std::memcpy(&out, &v, sizeof out);
            return out;
        }
        }
    }
    bool flag() { return byte() & 1; }
    wchar_t wchar() { return static_cast<wchar_t>(byte() | (byte() << 8)); }
    std::wstring wstr() {
        size_t len = byte() % 48;
        std::wstring s;
        for (size_t i = 0; i < len && !empty(); ++i) {
            // half the time a CJK character, so layouts look like real ones
            uint8_t kind = byte();
            s.push_back(kind & 1 ? static_cast<wchar_t>(0x4E00 + (kind >> 1) * 37) : wchar());
        }
        return s;
    }
    COLORREF color() { return RGB(byte(), byte(), byte()); }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

struct ReleaseWindow {
    void operator()(Ime::CandidateWindow* window) const { window->Release(); }
};

// Paint the whole window into a bitmap, as WM_PAINT would (capped: a window
// bigger than this is not drawn, its size is the finding if it matters).
void paint(Ime::CandidateWindow& window) {
    window.recalculateSize();
    RECT rc;
    if (!::GetClientRect(window.hwnd(), &rc))
        return;
    int width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(NULL);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib) {
        HGDIOBJ old = ::SelectObject(mem, dib);
        window.paint(mem, rc);
        ::GdiFlush();
        ::SelectObject(mem, old);
        ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    ::ReleaseDC(NULL, screen);
}

const wchar_t* const kFaces[] = { L"Microsoft JhengHei", L"Segoe UI", L"Segoe UI Emoji", L"No Such Font" };
const UINT kArrowKeys[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, 'A' };

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const bool registered = Ime::Window::registerClass(::GetModuleHandleW(NULL));
    (void)registered;

    Input in(data, size);
    // the font PIMETextService makes from candFontName / candFontSize
    int fontHeight = -static_cast<int>(in.byte() % 96);
    HFONT font = ::CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH,
                               kFaces[in.byte() % 4]);
    std::unique_ptr<Ime::CandidateWindow, ReleaseWindow> window(new Ime::CandidateWindow(nullptr, nullptr));
    window->setFont(font); // the window owns it now (ImeWindow::setFont)

    for (int ops = 0; !in.empty() && ops < 96; ++ops) {
        switch (in.byte() % 24) {
        case 0: case 1: case 2: {
            std::wstring item = in.wstr();
            window->add(item, in.wchar());
            break;
        }
        case 3: window->setHeader(in.wstr()); break;
        case 4: window->setPageInfo(in.wstr()); break;
        case 5: window->setMessage(in.wstr()); break;
        case 6: {
            std::wstring message = in.wstr(), header = in.wstr();
            window->setTextRows(message, header, in.wstr());
            break;
        }
        case 7: {
            std::vector<std::wstring> items;
            std::vector<wchar_t> keys;
            size_t count = in.byte() % 20, keyCount = in.byte() % 20;
            for (size_t i = 0; i < count; ++i) items.push_back(in.wstr());
            for (size_t i = 0; i < keyCount; ++i) keys.push_back(in.wchar());
            window->setItems(items, keys);
            break;
        }
        case 8: window->setCandPerRow(in.i32()); break;
        case 9: {
            int content = in.i32(), text = in.i32();
            window->setSpacing(content, text, in.i32());
            break;
        }
        case 10: window->setKeyStyle(in.i32()); break;
        case 11: window->setMessageStyle(in.i32()); break;
        case 12: window->setHeaderLabelStyle(in.i32()); break;
        case 13: {
            bool stable = in.flag();
            window->setStableWidth(stable, in.i32());
            break;
        }
        case 14: {
            bool wrap = in.flag();
            window->setMaxWidth(wrap, in.i32());
            break;
        }
        case 15: window->setUseCursor(in.flag()); break;
        case 16: window->setCurrentSel(in.i32()); break;
        case 17: {
            Ime::KeyEvent key(WM_KEYDOWN, kArrowKeys[in.byte() % 6], 1);
            window->filterKeyEvent(key);
            break;
        }
        case 18: window->clear(); break;
        case 19: window->setHighContrast(in.flag()); break;
        case 20: {
            COLORREF c[7];
            for (auto& color : c) color = in.color();
            window->setTheme(c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
            break;
        }
        case 21: window->seedStableWidth(in.i32()); break;
        case 22: window->resetStableWidth(); break;
        default: paint(*window); break;
        }
    }
    paint(*window);
    window->syncAccessibility();
    window.reset();
    return 0;
}
