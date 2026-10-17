//
//    Copyright (C) 2013 - 2020 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//    This library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Library General Public
//    License as published by the Free Software Foundation; either
//    version 2 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Library General Public License for more details.
//
//    You should have received a copy of the GNU Library General Public
//    License along with this library; if not, write to the
//    Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//    Boston, MA  02110-1301, USA.
//

#include "CandidateWindow.h"
#include "TextService.h"
#include "EditSession.h"

#include <algorithm>
#include <cassert>

#include <tchar.h>
#include <windows.h>

using namespace std;

// winuser.h only declares these for WINVER >= 0x0602 (the project targets 0x0600)
#ifndef EVENT_OBJECT_IME_SHOW
#define EVENT_OBJECT_IME_SHOW 0x8027
#endif
#ifndef EVENT_OBJECT_IME_HIDE
#define EVENT_OBJECT_IME_HIDE 0x8028
#endif

namespace Ime {

static COLORREF blendColor(COLORREF a, COLORREF b, int percentB) {
    int percentA = 100 - percentB;
    return RGB(
        (GetRValue(a) * percentA + GetRValue(b) * percentB) / 100,
        (GetGValue(a) * percentA + GetGValue(b) * percentB) / 100,
        (GetBValue(a) * percentA + GetBValue(b) * percentB) / 100
    );
}

static int colorLuma(COLORREF color) {
    return (GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114) / 1000;
}

static int colorContrast(COLORREF a, COLORREF b) {
    int diff = colorLuma(a) - colorLuma(b);
    return diff < 0 ? -diff : diff;
}

static COLORREF readableTextOnColor(COLORREF bg, COLORREF preferred, COLORREF alternate) {
    COLORREF dark = RGB(17, 24, 39);
    COLORREF light = RGB(248, 250, 252);
    COLORREF result = preferred;
    int resultContrast = colorContrast(bg, result);
    int alternateContrast = colorContrast(bg, alternate);

    if (alternateContrast > resultContrast) {
        result = alternate;
        resultContrast = alternateContrast;
    }

    int darkContrast = colorContrast(bg, dark);
    int lightContrast = colorContrast(bg, light);
    if (resultContrast < 72 && darkContrast > resultContrast) {
        result = dark;
        resultContrast = darkContrast;
    }
    if (resultContrast < 72 && lightContrast > resultContrast) {
        result = light;
    }
    return result;
}

static COLORREF readableHeaderValueColor(COLORREF panelBg, COLORREF textPrimary, COLORREF highlightBg, COLORREF highlightText) {
    COLORREF preferred = colorLuma(panelBg) > 165 ? highlightBg : highlightText;
    COLORREF alternate = preferred == highlightBg ? highlightText : highlightBg;
    if (colorContrast(panelBg, preferred) < 70 && colorContrast(panelBg, alternate) > colorContrast(panelBg, preferred))
        preferred = alternate;
    if (colorContrast(panelBg, preferred) < 70 && colorContrast(panelBg, textPrimary) > colorContrast(panelBg, preferred))
        preferred = textPrimary;
    return preferred;
}

static int modernCandidateTextGap(int keyStyle, int textMargin) {
    switch (keyStyle) {
    case CandidateWindow::KeyStyleLeftTag:
        return max(4, textMargin);
    case CandidateWindow::KeyStyleWordFirst:
    case CandidateWindow::KeyStyleSoftCapsule:
    case CandidateWindow::KeyStyleGlowKey:
    case CandidateWindow::KeyStyleMicroTab:
        return max(3, textMargin / 2);
    default:
        return 1;
    }
}

static int modernCandidateKeyMinWidth(int keyStyle, int textMargin) {
    switch (keyStyle) {
    case CandidateWindow::KeyStyleKeycap:
    case CandidateWindow::KeyStyleBadgeMinimal:
    case CandidateWindow::KeyStyleSoftCapsule:
        return max(17, textMargin * 3 + 5);
    case CandidateWindow::KeyStyleLeftTag:
        return max(19, textMargin * 3 + 7);
    case CandidateWindow::KeyStyleAccentDot:
    case CandidateWindow::KeyStyleGlowKey:
    case CandidateWindow::KeyStyleWordAnchor:
        return max(18, textMargin * 3 + 6);
    case CandidateWindow::KeyStyleMonospaceSlot:
        return max(16, textMargin * 3 + 4);
    case CandidateWindow::KeyStyleMicroTab:
        return max(14, textMargin * 3 + 2);
    default:
        return 0;
    }
}

static int modernCandidateKeyFontPercent(int keyStyle) {
    switch (keyStyle) {
    case CandidateWindow::KeyStyleWordFirst:
    case CandidateWindow::KeyStyleMicroTab:
        return 72;
    case CandidateWindow::KeyStyleWordAnchor:
        return 82;
    default:
        return 86;
    }
}

static HFONT createScaledFont(HFONT baseFont, int percent) {
    LOGFONTW logFont;
    if (!baseFont || ::GetObjectW(baseFont, sizeof(logFont), &logFont) != sizeof(logFont))
        return NULL;

    if (logFont.lfHeight != 0) {
        int sign = logFont.lfHeight < 0 ? -1 : 1;
        int height = logFont.lfHeight < 0 ? -logFont.lfHeight : logFont.lfHeight;
        logFont.lfHeight = sign * max(1, height * percent / 100);
    }
    return ::CreateFontIndirectW(&logFont);
}

static int modernCandidateWidthSafety(int textMargin) {
    return max(2, textMargin / 2);
}

// extra horizontal room the header label decoration needs beyond the text
static int headerLabelExtraWidth(int style, int textMargin) {
    switch (style) {
    case CandidateWindow::HeaderLabelBadge:
    case CandidateWindow::HeaderLabelTag:
        return max(3, textMargin / 2 + 1) * 2;
    case CandidateWindow::HeaderLabelBar:
        return 2 + max(3, textMargin / 2);
    default:
        return 0;
    }
}

static int modernCandidateExtraWidth(int keyStyle, int textMargin) {
    int stylePadding = keyStyle == CandidateWindow::KeyStyleLeftTag ? textMargin : 0;
    return textMargin * 2 + modernCandidateTextGap(keyStyle, textMargin) + modernCandidateWidthSafety(textMargin) + stylePadding;
}

static int candidateMessageExtraWidth(int messageStyle, int textMargin, int itemHeight) {
    switch (messageStyle) {
    case CandidateWindow::MessageStyleBar:
        return max(10, textMargin * 2 + 4);
    case CandidateWindow::MessageStyleDot:
        return max(16, textMargin * 2 + 8);
    case CandidateWindow::MessageStyleBadge:
    default:
        return max(22, itemHeight) + textMargin * 2;
    }
}

static void applyCandidateWindowRegion(HWND hwnd, int width, int height, int borderRadius) {
    if (!hwnd)
        return;

    if (borderRadius <= 0 || width <= 0 || height <= 0) {
        ::SetWindowRgn(hwnd, NULL, TRUE);
        return;
    }

    int diameter = borderRadius * 2;
    HRGN region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region && ::SetWindowRgn(hwnd, region, TRUE) == 0)
        ::DeleteObject(region);
}

CandidateWindow::CandidateWindow(TextService* service, EditSession* session):
    ImeWindow(service),
    highContrast_(false),
    updatedFlags_(TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE),
    accessibility_(this),
    shown_(false),
    candPerRow_(1),
    effectiveCandPerRow_(1),
    textWidth_(0),
    itemHeight_(0),
    currentSel_(0),
    hasResult_(false),
    useCursor_(true),
    selKeyWidth_(0),
    panelBg_(RGB(255, 255, 255)),
    panelBorder_(RGB(218, 221, 227)),
    textPrimary_(RGB(32, 36, 42)),
    textSecondary_(RGB(107, 114, 128)),
    highlightBg_(RGB(220, 235, 255)),
    highlightBorder_(RGB(156, 199, 255)),
    highlightText_(RGB(11, 58, 117)),
    contentMargin_(8),
    textMargin_(6),
    borderRadius_(8),
    keyStyle_(KeyStyleKeycap),
    messageStyle_(MessageStyleBadge),
    headerLabelStyle_(HeaderLabelPlain),
    stableWidth_(false),
    minStableWidth_(0),
    stableWidthPx_(0),
    wrapToMaxWidth_(false),
    maxWidth_(0),
    cachedKeyFont_(NULL),
    cachedKeyFontBase_(NULL),
    cachedKeyFontPercent_(0),
    panelBgBrush_(NULL),
    panelBorderPen_(NULL),
    headerDividerPen_(NULL),
    measuredHeaderHeight_(0) {

    if(service && service->isImmersive()) { // windows 8 app mode
        margin_ = 10;
        rowSpacing_ = 8;
        colSpacing_ = 12;
    }
    else { // desktop mode
        margin_ = 5;
        rowSpacing_ = 4;
        colSpacing_ = 8;
    }

    // service/session are always set in the IME; they are null only when a test
    // builds the window stand-alone to render it off-screen (see paint()).
    HWND parent = (service && session) ? service->compositionWindow(session) : NULL;
    create(parent, WS_POPUP|WS_CLIPCHILDREN, WS_EX_TOOLWINDOW|WS_EX_TOPMOST);

    themeColors_[0] = panelBg_;
    themeColors_[1] = panelBorder_;
    themeColors_[2] = textPrimary_;
    themeColors_[3] = textSecondary_;
    themeColors_[4] = highlightBg_;
    themeColors_[5] = highlightBorder_;
    themeColors_[6] = highlightText_;
    updateSystemColors();
}

CandidateWindow::~CandidateWindow(void) {
    // the window still exists here (Window's destructor destroys it), which
    // UIA's clean-up for WM_GETOBJECT providers needs
    accessibility_.disconnect();
    if (cachedKeyFont_)
        ::DeleteObject(cachedKeyFont_);
    releaseThemeBrushes();
}

void CandidateWindow::setFont(HFONT f) {
    if (cachedKeyFont_) {
        ::DeleteObject(cachedKeyFont_);
        cachedKeyFont_ = NULL;
        cachedKeyFontBase_ = NULL;
    }
    textSizeCache_.clear(); // extents were measured with the previous font
    ImeWindow::setFont(f);
}

HFONT CandidateWindow::scaledKeyFont() {
    int percent = modernCandidateKeyFontPercent(keyStyle_);
    if (cachedKeyFont_ && cachedKeyFontBase_ == font_ && cachedKeyFontPercent_ == percent)
        return cachedKeyFont_;
    if (cachedKeyFont_)
        ::DeleteObject(cachedKeyFont_);
    cachedKeyFont_ = createScaledFont(font_, percent);
    cachedKeyFontBase_ = font_;
    cachedKeyFontPercent_ = percent;
    return cachedKeyFont_;
}

HBRUSH CandidateWindow::panelBgBrush() {
    if (!panelBgBrush_)
        panelBgBrush_ = ::CreateSolidBrush(panelBg_);
    return panelBgBrush_;
}

HPEN CandidateWindow::panelBorderPen() {
    if (!panelBorderPen_)
        panelBorderPen_ = ::CreatePen(PS_SOLID, 1, panelBorder_);
    return panelBorderPen_;
}

HPEN CandidateWindow::headerDividerPen() {
    if (!headerDividerPen_)
        headerDividerPen_ = ::CreatePen(PS_SOLID, 1, blend(panelBorder_, panelBg_, 35));
    return headerDividerPen_;
}

HBRUSH CandidateWindow::cachedBrush(COLORREF color) {
    auto it = itemBrushCache_.find(color);
    if (it != itemBrushCache_.end())
        return it->second;
    HBRUSH br = ::CreateSolidBrush(color);
    itemBrushCache_[color] = br;
    return br;
}

HPEN CandidateWindow::cachedPen(COLORREF color, int width) {
    DWORD key = (DWORD)((width & 0xFF) << 24) | (color & 0x00FFFFFF);
    auto it = itemPenCache_.find(key);
    if (it != itemPenCache_.end())
        return it->second;
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    itemPenCache_[key] = pen;
    return pen;
}

void CandidateWindow::releaseItemGdiCache() {
    for (auto& kv : itemBrushCache_)
        ::DeleteObject(kv.second);
    itemBrushCache_.clear();
    for (auto& kv : itemPenCache_)
        ::DeleteObject(kv.second);
    itemPenCache_.clear();
}

void CandidateWindow::releaseThemeBrushes() {
    if (panelBgBrush_) {
        ::DeleteObject(panelBgBrush_);
        panelBgBrush_ = NULL;
    }
    if (panelBorderPen_) {
        ::DeleteObject(panelBorderPen_);
        panelBorderPen_ = NULL;
    }
    if (headerDividerPen_) {
        ::DeleteObject(headerDividerPen_);
        headerDividerPen_ = NULL;
    }
    releaseItemGdiCache();
}

COLORREF CandidateWindow::blend(COLORREF a, COLORREF b, int percentB) const {
    // tints and shades would turn the high-contrast colors into ones the
    // user did not choose
    return highContrast_ ? a : blendColor(a, b, percentB);
}

COLORREF CandidateWindow::readableOn(COLORREF bg, COLORREF preferred, COLORREF alternate) const {
    if (highContrast_)
        return bg == highlightBg_ ? highlightText_ : textPrimary_;
    return readableTextOnColor(bg, preferred, alternate);
}

COLORREF CandidateWindow::accentTextColor() const {
    if (highContrast_)
        return textPrimary_;
    return readableHeaderValueColor(panelBg_, textPrimary_, highlightBg_, highlightText_);
}

void CandidateWindow::applyColors() {
    COLORREF colors[ThemeColorCount];
    if (highContrast_) {
        // the pairs Windows defines for high contrast: window text on window
        // background, highlight text on highlight background
        COLORREF window = ::GetSysColor(COLOR_WINDOW);
        COLORREF windowText = ::GetSysColor(COLOR_WINDOWTEXT);
        COLORREF highlight = ::GetSysColor(COLOR_HIGHLIGHT);
        COLORREF highlightText = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
        colors[0] = window;      // panel background
        colors[1] = windowText;  // panel border
        colors[2] = windowText;  // candidate text
        colors[3] = windowText;  // selection keys, header label (no gray)
        colors[4] = highlight;   // selected candidate background
        colors[5] = highlight;   // selected candidate border
        colors[6] = highlightText;
    }
    else {
        for (int i = 0; i < ThemeColorCount; ++i)
            colors[i] = themeColors_[i];
    }
    COLORREF* targets[ThemeColorCount] = { &panelBg_, &panelBorder_, &textPrimary_, &textSecondary_,
                                           &highlightBg_, &highlightBorder_, &highlightText_ };
    bool changed = false;
    for (int i = 0; i < ThemeColorCount; ++i) {
        if (*targets[i] != colors[i]) {
            *targets[i] = colors[i];
            changed = true;
        }
    }
    if (!changed)
        return;
    releaseThemeBrushes();
    refresh();
}

void CandidateWindow::setHighContrast(bool on) {
    highContrast_ = on;
    applyColors();
    if (hwnd_)
        refresh(); // also when only the system colors changed
}

void CandidateWindow::updateSystemColors() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    bool on = ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
        (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    setHighContrast(on);
}

void CandidateWindow::syncAccessibility() {
    static const std::wstring kDefaultName = L"\x5019\x9078\x5b57"; // "candidates" in Chinese
    // what the window says besides the candidates: a backend message, or the
    // header (IME name and the typed roots)
    const std::wstring& name = !message_.empty() ? message_ : (!header_.empty() ? header_ : kDefaultName);
    // The selected candidate is announced also without a visible cursor: the
    // first candidate is then the one the first selection key picks.
    accessibility_.sync(items_, selKeys_, currentSel_, name, shown_ != FALSE);
}

HWND CandidateWindow::accessibleHwnd() {
    return hwnd_;
}

bool CandidateWindow::accessibleItemRect(int index, RECT* screenRect) {
    if (!hwnd_ || index < 0 || index >= (int)items_.size())
        return false;
    RECT rc;
    itemRect(index, rc);
    POINT corners[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
    ::MapWindowPoints(hwnd_, NULL, corners, 2);
    screenRect->left = corners[0].x;
    screenRect->top = corners[0].y;
    screenRect->right = corners[1].x;
    screenRect->bottom = corners[1].y;
    return rc.right > rc.left && rc.bottom > rc.top;
}

// ITfUIElement
STDMETHODIMP CandidateWindow::GetDescription(BSTR *pbstrDescription) {
    if (!pbstrDescription)
        return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"Candidate window");
    return *pbstrDescription ? S_OK : E_OUTOFMEMORY;
}

// {BD7CCC94-57CD-41D3-A789-AF47890CEB29}
STDMETHODIMP CandidateWindow::GetGUID(GUID *pguid) {
    if (!pguid)
        return E_INVALIDARG;
    *pguid = { 0xbd7ccc94, 0x57cd, 0x41d3, { 0xa7, 0x89, 0xaf, 0x47, 0x89, 0xc, 0xeb, 0x29 } };
    return S_OK;
}

STDMETHODIMP CandidateWindow::Show(BOOL bShow) {
    shown_ = bShow;
    if (shown_)
        show();
    else
        hide();
    // light-dismiss surfaces and assistive technology track IME windows
    // through these (IME requirements: "IME candidate window interaction with
    // light dismiss surfaces"); NotifyWinEvent is cheap without listeners
    if (hwnd_)
        ::NotifyWinEvent(shown_ ? EVENT_OBJECT_IME_SHOW : EVENT_OBJECT_IME_HIDE, hwnd_, OBJID_CLIENT, CHILDID_SELF);
    syncAccessibility();
    return S_OK;
}

STDMETHODIMP CandidateWindow::IsShown(BOOL *pbShow) {
    if (!pbShow)
        return E_INVALIDARG;
    *pbShow = shown_;
    return S_OK;
}

// ITfCandidateListUIElement
STDMETHODIMP CandidateWindow::GetUpdatedFlags(DWORD *pdwFlags) {
    if (!pdwFlags)
        return E_INVALIDARG;
    // what changed since the last UpdateUIElement (all bits the first time,
    // as the UILess mode overview requires); the IME resets it after each
    // UpdateUIElement with resetUpdatedFlags()
    *pdwFlags = updatedFlags_;
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetDocumentMgr(ITfDocumentMgr **ppdim) {
    if (!textService_)
        return E_FAIL;
    auto context = textService_->currentContext();
    if (!context)
        return E_FAIL;
    return context->GetDocumentMgr(ppdim);
}

STDMETHODIMP CandidateWindow::GetCount(UINT *puCount) {
    if (!puCount)
        return E_INVALIDARG;
    // the window holds exactly the current page of the backend's list
    *puCount = static_cast<UINT>(items_.size());
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetSelection(UINT *puIndex) {
    if (!puIndex)
        return E_INVALIDARG;
    *puIndex = 0;
    // S_FALSE: "the candidate list does not have a selection" (no cursor shown)
    if (!useCursor_ || currentSel_ < 0 || currentSel_ >= (int)items_.size())
        return S_FALSE;
    *puIndex = static_cast<UINT>(currentSel_);
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetString(UINT uIndex, BSTR *pbstr) {
    if (!pbstr)
        return E_INVALIDARG;
    if (uIndex >= items_.size())
        return E_INVALIDARG;
    *pbstr = SysAllocStringLen(items_[uIndex].c_str(), static_cast<UINT>(items_[uIndex].length()));
    return *pbstr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateWindow::GetPageIndex(UINT *puIndex, UINT uSize, UINT *puPageCnt) {
    /// XXX Always return the same single page index.
    if (!puPageCnt)
        return E_INVALIDARG;
    *puPageCnt = 1;
    if (puIndex) {
        if (uSize < *puPageCnt) {
            return E_INVALIDARG;
        }
        puIndex[0] = 0;
    }
    return S_OK;
}

STDMETHODIMP CandidateWindow::SetPageIndex(UINT *puIndex, UINT uPageCnt) {
    // The list is a single page (the backend pages it); the only layout an
    // application can ask for is that one page starting at 0.
    if (!puIndex || uPageCnt != 1 || puIndex[0] != 0)
        return E_INVALIDARG;
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetCurrentPage(UINT *puPage) {
    if (!puPage)
        return E_INVALIDARG;
    *puPage = 0;
    return S_OK;
}

LRESULT CandidateWindow::wndProc(UINT msg, WPARAM wp , LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            onPaint(wp, lp);
            break;
        case WM_ERASEBKGND:
            return TRUE;
            break;
        case WM_LBUTTONDOWN:
            onLButtonDown(wp, lp);
            break;
        case WM_MOUSEMOVE:
            onMouseMove(wp, lp);
            break;
        case WM_LBUTTONUP:
            onLButtonUp(wp, lp);
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_GETOBJECT: {
            // UI Automation (screen readers); the provider is created here,
            // on the first request, not with the window
            LRESULT result = 0;
            if (accessibility_.handleGetObject(wp, lp, &result))
                return result;
            return Window::wndProc(msg, wp, lp);
        }
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            updateSystemColors();
            return Window::wndProc(msg, wp, lp);
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETHIGHCONTRAST)
                updateSystemColors();
            return Window::wndProc(msg, wp, lp);
        case WM_DESTROY:
            accessibility_.disconnect();
            return Window::wndProc(msg, wp, lp);
        default:
            return Window::wndProc(msg, wp, lp);
    }
    return 0;
}

void CandidateWindow::onPaint(WPARAM wp, LPARAM lp) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    paint(ps.hdc, rc);
    EndPaint(hwnd_, &ps);
}

// Separate from onPaint() so the window can also be rendered into a memory DC
// (off-screen rendering tests), without BeginPaint or a visible window.
void CandidateWindow::paint(HDC hDC, const RECT& clientRect) {
    // TODO: check isImmersive_, and draw the window differently
    // in Windows 8 app immersive mode to follow windows 8 UX guidelines
    HFONT oldFont;
    RECT rc = clientRect;

    oldFont = (HFONT)SelectObject(hDC, font_);

    SetTextColor(hDC, textPrimary_);
    SetBkColor(hDC, panelBg_);

    // Draw rounded modern background and border (cached theme objects)
    HGDIOBJ oldBrush = ::SelectObject(hDC, panelBgBrush());
    HGDIOBJ oldPen = ::SelectObject(hDC, panelBorderPen());

    ::RoundRect(hDC, rc.left, rc.top, rc.right, rc.bottom, borderRadius_ * 2, borderRadius_ * 2);

    ::SelectObject(hDC, oldBrush);
    ::SelectObject(hDC, oldPen);

    // paint header row (label text left-aligned, page info right-aligned)
    int headerHeight = this->headerHeight(hDC);
    if (!header_.empty() || !pageInfo_.empty()) {
        COLORREF headerLabelColor = textSecondary_;
        COLORREF headerValueColor = accentTextColor();
        COLORREF oldColor = ::SetTextColor(hDC, headerLabelColor);
        ::SetBkMode(hDC, TRANSPARENT);

        int rowTop = 0;
        int rowBottom = headerHeight;
        int pageInfoLeft = rc.right - margin_ - textMargin_;
        if (!pageInfo_.empty()) {
            SIZE piSize;
            ::GetTextExtentPoint32W(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &piSize);
            pageInfoLeft -= piSize.cx;
        }

        if (!header_.empty()) {
            std::wstring label = L"";
            std::wstring value = header_;
            size_t separator = header_.find(L' ');
            if (separator != std::wstring::npos && separator + 1 < header_.length()) {
                label = header_.substr(0, separator);
                value = header_.substr(separator + 1);
            }

            int textX = margin_ + textMargin_;
            RECT labelRect = { textX, rowTop, pageInfoLeft - textMargin_, rowBottom };
            if (!label.empty()) {
                SIZE labelSize;
                ::GetTextExtentPoint32W(hDC, label.c_str(), (int)label.length(), &labelSize);
                int labelStyle = headerLabelStyle_;
                int advance = labelSize.cx;

                if (labelStyle == HeaderLabelBadge || labelStyle == HeaderLabelTag) {
                    int padX = max(3, textMargin_ / 2 + 1);
                    int padY = max(1, textMargin_ / 3);
                    RECT chipRc = { labelRect.left, rowTop + padY, labelRect.left + labelSize.cx + padX * 2, rowBottom - padY };
                    COLORREF chipBg = labelStyle == HeaderLabelBadge ? blend(panelBg_, highlightBg_, 26) : panelBg_;
                    COLORREF chipBorder = labelStyle == HeaderLabelBadge
                        ? blend(highlightBorder_, chipBg, 35)
                        : blend(textSecondary_, panelBg_, 55);
                    HGDIOBJ oldChipBrush = ::SelectObject(hDC, cachedBrush(chipBg));
                    HGDIOBJ oldChipPen = ::SelectObject(hDC, cachedPen(chipBorder));
                    int chipRadius = max(4, min(borderRadius_, (int)(chipRc.bottom - chipRc.top) / 2));
                    ::RoundRect(hDC, chipRc.left, chipRc.top, chipRc.right, chipRc.bottom, chipRadius, chipRadius);
                    ::SelectObject(hDC, oldChipBrush);
                    ::SelectObject(hDC, oldChipPen);
                    ::SetTextColor(hDC, labelStyle == HeaderLabelBadge
                        ? readableOn(chipBg, highlightText_, textPrimary_)
                        : headerLabelColor);
                    RECT chipTextRc = { chipRc.left + padX, rowTop, chipRc.left + padX + labelSize.cx, rowBottom };
                    ::DrawTextW(hDC, label.c_str(), (int)label.length(), &chipTextRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    advance = labelSize.cx + padX * 2;
                }
                else if (labelStyle == HeaderLabelBar) {
                    int barGap = max(3, textMargin_ / 2);
                    int barInset = max(2, textMargin_ / 2);
                    RECT barRc = { labelRect.left, rowTop + barInset, labelRect.left + 2, rowBottom - barInset };
                    ::FillRect(hDC, &barRc, cachedBrush(headerValueColor));
                    ::SetTextColor(hDC, headerLabelColor);
                    RECT barTextRc = { labelRect.left + 2 + barGap, rowTop, labelRect.right, rowBottom };
                    ::DrawTextW(hDC, label.c_str(), (int)label.length(), &barTextRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    advance = 2 + barGap + labelSize.cx;
                }
                else if (labelStyle == HeaderLabelAccent) {
                    ::SetTextColor(hDC, headerValueColor);
                    ::DrawTextW(hDC, label.c_str(), (int)label.length(), &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                else {
                    ::SetTextColor(hDC, headerLabelColor);
                    ::DrawTextW(hDC, label.c_str(), (int)label.length(), &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (labelStyle == HeaderLabelUnderline) {
                        HGDIOBJ oldUnderlinePen = ::SelectObject(hDC, cachedPen(headerValueColor));
                        int underlineY = rowBottom - max(2, textMargin_ / 2);
                        ::MoveToEx(hDC, labelRect.left, underlineY, NULL);
                        ::LineTo(hDC, labelRect.left + labelSize.cx, underlineY);
                        ::SelectObject(hDC, oldUnderlinePen);
                    }
                }

                labelRect.left += advance + textMargin_;
            }

            ::SetTextColor(hDC, headerValueColor);
            ::DrawTextW(hDC, value.c_str(), (int)value.length(), &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        if (!pageInfo_.empty()) {
            SIZE piSize;
            ::GetTextExtentPoint32W(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &piSize);
            // right-aligned, vertically centered in the header row
            int piX = pageInfoLeft;
            RECT piRect = { piX, rowTop, rc.right - margin_, rowBottom };
            ::SetTextColor(hDC, headerLabelColor);
            ::DrawTextW(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &piRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        HGDIOBJ oldPen = ::SelectObject(hDC, headerDividerPen());
        int dividerY = max(0, headerHeight - 1);
        ::MoveToEx(hDC, 1, dividerY, NULL);
        ::LineTo(hDC, rc.right - 1, dividerY);
        ::SelectObject(hDC, oldPen);

        ::SetBkMode(hDC, OPAQUE);
        ::SetTextColor(hDC, oldColor);
    }

    if (!message_.empty()) {
        int messageTop = headerHeight > 0 ? headerHeight + textMargin_ : margin_;
        RECT messageRect = {
            margin_ + textMargin_,
            messageTop,
            rc.right - margin_ - textMargin_,
            rc.bottom - margin_
        };

        int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
        COLORREF oldTextColor = ::SetTextColor(hDC, textPrimary_);
        COLORREF accent = accentTextColor();
        COLORREF messageText = colorContrast(panelBg_, accent) >= 62 ? accent : textPrimary_;
        COLORREF messageBg = colorLuma(panelBg_) > 165 ? blend(panelBg_, accent, 8) : blend(panelBg_, accent, 13);
        RECT rowRect = messageRect;
        rowRect.bottom = min(rowRect.bottom, rowRect.top + modernCandidateRowHeight());

        if (messageStyle_ == MessageStyleBadge) {
            HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(messageBg));
            HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(messageBg));
            ::RoundRect(hDC, rowRect.left, rowRect.top, rowRect.right, rowRect.bottom, max(4, borderRadius_), max(4, borderRadius_));
            ::SelectObject(hDC, oldBrush);
            ::SelectObject(hDC, oldPen);

            int badgeSize = min(static_cast<int>(rowRect.bottom - rowRect.top) - max(2, textMargin_ / 2), max(18, itemHeight_));
            RECT badgeRect = {
                rowRect.left + textMargin_,
                rowRect.top + ((rowRect.bottom - rowRect.top) - badgeSize) / 2,
                rowRect.left + textMargin_ + badgeSize,
                rowRect.top + ((rowRect.bottom - rowRect.top) + badgeSize) / 2
            };
            oldBrush = ::SelectObject(hDC, cachedBrush(blend(accent, panelBg_, 12)));
            oldPen = ::SelectObject(hDC, cachedPen(blend(accent, panelBg_, 5)));
            ::RoundRect(hDC, badgeRect.left, badgeRect.top, badgeRect.right, badgeRect.bottom, max(4, badgeSize / 2), max(4, badgeSize / 2));
            ::SelectObject(hDC, oldBrush);
            ::SelectObject(hDC, oldPen);

            COLORREF badgeText = (!highContrast_ && colorContrast(accent, highlightText_) >= 60) ? highlightText_ : panelBg_;
            ::SetTextColor(hDC, badgeText);
            ::DrawTextW(hDC, L"!", 1, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            messageRect.left = badgeRect.right + textMargin_;
        }
        else if (messageStyle_ == MessageStyleBar) {
            HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(accent, max(2, textMargin_ / 2)));
            int barX = rowRect.left + max(1, textMargin_ / 3);
            ::MoveToEx(hDC, barX, rowRect.top + max(3, textMargin_ / 2), NULL);
            ::LineTo(hDC, barX, rowRect.bottom - max(3, textMargin_ / 2));
            ::SelectObject(hDC, oldPen);
            messageRect.left += max(10, textMargin_ * 2);
        }
        else {
            int dotSize = max(6, min(10, itemHeight_ / 2));
            RECT dotRect = {
                rowRect.left + textMargin_,
                rowRect.top + ((rowRect.bottom - rowRect.top) - dotSize) / 2,
                rowRect.left + textMargin_ + dotSize,
                rowRect.top + ((rowRect.bottom - rowRect.top) + dotSize) / 2
            };
            HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(accent));
            HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(accent));
            ::Ellipse(hDC, dotRect.left, dotRect.top, dotRect.right, dotRect.bottom);
            ::SelectObject(hDC, oldBrush);
            ::SelectObject(hDC, oldPen);
            messageRect.left = dotRect.right + textMargin_;
        }
        ::SetTextColor(hDC, messageText);
        ::DrawTextW(hDC, message_.c_str(), (int)message_.length(), &messageRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SetBkMode(hDC, oldBkMode);
        ::SetTextColor(hDC, oldTextColor);
        SelectObject(hDC, oldFont);
        return;
    }

    // paint items
    int col = 0;
    int x = margin_, y = headerHeight > 0 ? headerHeight + textMargin_ : margin_;
    int columnsPerRow = max(1, effectiveCandPerRow_);
    for(int i = 0, n = items_.size(); i < n; ++i) {
        paintItem(hDC, i, x, y);
        ++col; // go to next column
        if(col >= columnsPerRow) {
            col = 0;
            x = margin_;
            y += modernCandidateRowHeight() + rowSpacing_;
        }
        else {
            x += colSpacing_ + selKeyWidth_ + textWidth_ + modernCandidateExtraWidth(keyStyle_, textMargin_);
        }
    }
    SelectObject(hDC, oldFont);
}

void CandidateWindow::recalculateSize() {
    margin_ = contentMargin_;
    rowSpacing_ = max(0, textMargin_ / 2);
    colSpacing_ = max(6, textMargin_ + 2);

    HDC hDC = ::GetWindowDC(hwnd());
    int height = 0;
    int width = 0;
    selKeyWidth_ = 0;
    textWidth_ = 0;
    itemHeight_ = 0;

    HGDIOBJ oldFont = ::SelectObject(hDC, font_);
    TEXTMETRIC textMetrics;
    ::GetTextMetrics(hDC, &textMetrics);
    int fontLineHeight = textMetrics.tmHeight + textMetrics.tmExternalLeading;
    vector<wstring>::const_iterator it;
    for(int i = 0, n = items_.size(); i < n; ++i) {
        SIZE selKeySize;
        int lineHeight = 0;
        // the selection key
        wchar_t selKey = selKeys_[i];
        ::GetTextExtentPoint32W(hDC, &selKey, 1, &selKeySize);
        if(selKeySize.cx > selKeyWidth_)
            selKeyWidth_ = selKeySize.cx;

        // the candidate string (extents cached per string for the current font)
        SIZE candidateSize;
        wstring& item = items_.at(i);
        auto cachedSize = textSizeCache_.find(item);
        if (cachedSize != textSizeCache_.end()) {
            candidateSize = cachedSize->second;
        }
        else {
            ::GetTextExtentPoint32W(hDC, item.c_str(), item.length(), &candidateSize);
            if (textSizeCache_.size() >= 4096)
                textSizeCache_.clear();
            textSizeCache_.emplace(item, candidateSize);
        }
        if(candidateSize.cx > textWidth_)
            textWidth_ = candidateSize.cx;
        int itemHeight = max(candidateSize.cy, selKeySize.cy);
        if(itemHeight > itemHeight_)
            itemHeight_ = itemHeight;
    }
    if (itemHeight_ < fontLineHeight)
        itemHeight_ = fontLineHeight;

    // measure header (reuse the same DC); cache it so itemRect() does not
    // need a fresh window DC on every selection move
    int headerHeight = this->headerHeight(hDC);
    measuredHeaderHeight_ = headerHeight;
    int headerWidth = 0;
    int pageInfoWidth = 0;
    if (!pageInfo_.empty()) {
        SIZE pageInfoSize;
        ::GetTextExtentPoint32W(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &pageInfoSize);
        pageInfoWidth = pageInfoSize.cx;
    }

    if (!header_.empty()) {
        SIZE headerSize;
        ::GetTextExtentPoint32W(hDC, header_.c_str(), (int)header_.length(), &headerSize);
        // header row must fit both the label text and the page-info text
        headerWidth = headerSize.cx + (pageInfoWidth > 0 ? colSpacing_ * 2 + pageInfoWidth : 0);
        headerWidth += headerLabelExtraWidth(headerLabelStyle_, textMargin_);
    }
    else if (pageInfoWidth > 0) {
        // page info with no header: size the row to fit the page info alone
        headerWidth = pageInfoWidth;
    }

    int messageWidth = 0;
    int messageHeight = 0;
    if (!message_.empty()) {
        SIZE messageSize;
        ::GetTextExtentPoint32W(hDC, message_.c_str(), (int)message_.length(), &messageSize);
        messageWidth = messageSize.cx;
        messageHeight = max(static_cast<int>(messageSize.cy), fontLineHeight);
    }

    ::SelectObject(hDC, oldFont);
    ::ReleaseDC(hwnd(), hDC);

    selKeyWidth_ = max(selKeyWidth_, modernCandidateKeyMinWidth(keyStyle_, textMargin_));

    int extraItemPadding = modernCandidateExtraWidth(keyStyle_, textMargin_);
    int modernRowHeight = modernCandidateRowHeight();
    int headerGap = headerHeight > 0 ? textMargin_ : 0;
    int topPadding = headerHeight + headerGap;
    int bottomPadding = margin_;

    int itemStride = selKeyWidth_ + textWidth_ + extraItemPadding;
    int effectiveCandPerRow = max(1, candPerRow_);
    if (wrapToMaxWidth_ && maxWidth_ > 0 && itemStride > 0 && !items_.empty()) {
        int contentLimit = max(1, maxWidth_ - margin_ * 2);
        int maxColumns = (contentLimit + colSpacing_) / (itemStride + colSpacing_);
        effectiveCandPerRow = max(1, min(effectiveCandPerRow, maxColumns));
    }
    effectiveCandPerRow_ = effectiveCandPerRow;

    if (!message_.empty()) {
        int messageRowHeight = messageHeight + textMargin_ * 2;
        width = messageWidth + margin_ * 2 + textMargin_ * 2;
        width += candidateMessageExtraWidth(messageStyle_, textMargin_, itemHeight_);
        width = max(width, headerWidth + margin_ * 2);
        height = topPadding + messageRowHeight + bottomPadding;
    }
    else if(items_.empty()) {
        width = headerWidth > 0 ? headerWidth + margin_ * 2 : margin_ * 2;
        height = headerHeight > 0 ? topPadding + modernRowHeight + bottomPadding : margin_ * 2;
    }
    else {
        int columnCount = min((int)items_.size(), effectiveCandPerRow_);
        width = columnCount * itemStride;
        width += colSpacing_ * (columnCount - 1);
        width += margin_ * 2;
        width = max(width, headerWidth + margin_ * 2);
        int rowCount = (int)items_.size() / effectiveCandPerRow_;
        if(items_.size() % effectiveCandPerRow_)
            ++rowCount;
        height = topPadding + modernRowHeight * rowCount + rowSpacing_ * (rowCount - 1) + bottomPadding;
    }
    if (wrapToMaxWidth_ && maxWidth_ > 0) {
        int maxWindowWidth = max(maxWidth_, minStableWidth_);
        width = min(width, maxWindowWidth);
    }
    if (stableWidth_) {
        int minWidth = max(0, minStableWidth_);
        if (stableWidthPx_ < minWidth)
            stableWidthPx_ = minWidth;
        if (width > stableWidthPx_)
            stableWidthPx_ = width;
        if (wrapToMaxWidth_ && maxWidth_ > 0) {
            int maxWindowWidth = max(maxWidth_, minWidth);
            stableWidthPx_ = min(stableWidthPx_, maxWindowWidth);
        }
        width = stableWidthPx_;
    }
    resize(width, height);
    applyCandidateWindowRegion(hwnd_, width, height, borderRadius_);
}

void CandidateWindow::setCandPerRow(int n) {
    if(n != candPerRow_) {
        candPerRow_ = n;
        recalculateSize();
    }
}

bool CandidateWindow::filterKeyEvent(KeyEvent& keyEvent) {
    // select item with arrow keys
    int oldSel = currentSel_;
    int columnsPerRow = max(1, effectiveCandPerRow_);
    int itemCount = (int)items_.size();
    switch(keyEvent.keyCode()) {
    case VK_UP:
        if(currentSel_ - columnsPerRow >=0)
            currentSel_ -= columnsPerRow;
        break;
    case VK_DOWN:
        if(currentSel_ + columnsPerRow < itemCount)
            currentSel_ += columnsPerRow;
        break;
    case VK_LEFT:
        if(currentSel_ - 1 >=0)
            --currentSel_;
        break;
    case VK_RIGHT:
        if(currentSel_ + 1 < itemCount)
            ++currentSel_;
        break;
    case VK_RETURN:
        hasResult_ = true;
        return true;
    default:
        return false;
    }
    // if currently selected item is changed, redraw
    if(currentSel_ != oldSel) {
        // repaint old and new items in one pass; onPaint always repaints the
        // full background within the clip, so skip the erase step
        RECT oldRect, newRect, unionRect;
        itemRect(oldSel, oldRect);
        itemRect(currentSel_, newRect);
        ::UnionRect(&unionRect, &oldRect, &newRect);
        ::InvalidateRect(hwnd_, &unionRect, FALSE);
        updatedFlags_ |= TF_CLUIE_SELECTION;
        syncAccessibility();
        return true;
    }
    return false;
}

void CandidateWindow::setCurrentSel(int sel) {
    if(sel < 0 || sel >= (int)items_.size())
        sel = 0;
    if (currentSel_ != sel) {
        currentSel_ = sel;
        updatedFlags_ |= TF_CLUIE_SELECTION;
        if (isVisible())
            ::InvalidateRect(hwnd_, NULL, FALSE); // onPaint repaints the full background
    }
}

void CandidateWindow::clear() {
    items_.clear();
    selKeys_.clear();
    message_.clear();
    currentSel_ = 0;
    hasResult_ = false;
    updatedFlags_ |= TF_CLUIE_STRING | TF_CLUIE_COUNT | TF_CLUIE_SELECTION;
}

void CandidateWindow::setUseCursor(bool use) {
    if (useCursor_ == use)
        return;
    useCursor_ = use;
    updatedFlags_ |= TF_CLUIE_SELECTION; // GetSelection() turns S_OK / S_FALSE
    if(isVisible())
        ::InvalidateRect(hwnd_, NULL, TRUE);
}

void CandidateWindow::setStableWidth(bool stable, int minWidth) {
    minWidth = max(0, minWidth);
    if (stableWidth_ == stable && minStableWidth_ == minWidth)
        return;
    if (stableWidth_ != stable || minStableWidth_ != minWidth)
        stableWidthPx_ = 0;
    stableWidth_ = stable;
    minStableWidth_ = minWidth;
    recalculateSize();
    refresh();
}

void CandidateWindow::resetStableWidth() {
    stableWidthPx_ = 0;
}

void CandidateWindow::seedStableWidth(int px) {
    if (px > stableWidthPx_)
        stableWidthPx_ = px;
}

void CandidateWindow::setMaxWidth(bool wrapToMaxWidth, int maxWidth) {
    maxWidth = max(0, maxWidth);
    if (wrapToMaxWidth_ == wrapToMaxWidth && maxWidth_ == maxWidth)
        return;
    if (wrapToMaxWidth_ != wrapToMaxWidth || maxWidth_ != maxWidth)
        stableWidthPx_ = 0;
    wrapToMaxWidth_ = wrapToMaxWidth;
    maxWidth_ = maxWidth;
    recalculateSize();
    refresh();
}

int CandidateWindow::headerHeight(HDC hDC) const {
    if (header_.empty() && pageInfo_.empty())
        return 0;

    SIZE headerSize = { 0, 0 };
    SIZE pageInfoSize = { 0, 0 };
    if (!header_.empty())
        ::GetTextExtentPoint32W(hDC, header_.c_str(), (int)header_.length(), &headerSize);
    if (!pageInfo_.empty())
        ::GetTextExtentPoint32W(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &pageInfoSize);

    int textHeight = max(headerSize.cy, pageInfoSize.cy);
    TEXTMETRIC textMetrics;
    ::GetTextMetrics(hDC, &textMetrics);
    textHeight = max(textHeight, static_cast<int>(textMetrics.tmHeight + textMetrics.tmExternalLeading));
    return textHeight + textMargin_ * 2;
}

int CandidateWindow::modernCandidateRowHeight() const {
    return itemHeight_ + textMargin_ * 2;
}

void CandidateWindow::paintItem(HDC hDC, int i,  int x, int y) {
    RECT itemRc;
    itemRect(i, itemRc);
    ::InflateRect(&itemRc, 0, -max(1, textMargin_ / 3));

    bool isSelected = (useCursor_ && i == currentSel_);
    // high contrast: the system highlight colors, exactly
    COLORREF selectedBg = highContrast_ ? highlightBg_ : blend(panelBg_, highlightBg_, 28);
    COLORREF selectedFg = readableOn(selectedBg, highlightText_, textPrimary_);
    COLORREF selectedMutedFg = blend(selectedFg, selectedBg, 18);
    COLORREF selectedBadgeBg = blend(selectedBg, highlightBg_, colorLuma(panelBg_) > 165 ? 42 : 30);
    COLORREF selectedBadgeFg = readableOn(selectedBadgeBg, highlightText_, textPrimary_);
    COLORREF selectedBorder = highContrast_ ? highlightBg_ : colorContrast(selectedBg, highlightBorder_) >= 38
        ? blend(highlightBorder_, selectedBg, 28)
        : blend(selectedFg, selectedBg, 40);

    // Fill background of the item
    if (isSelected) {
        HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(selectedBg));
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(selectedBorder));
        ::RoundRect(hDC, itemRc.left, itemRc.top, itemRc.right, itemRc.bottom, borderRadius_ * 2, borderRadius_ * 2);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
    }

    if (keyStyle_ == KeyStyleLeftTag) {
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(isSelected ? blend(selectedFg, selectedBg, 22) : blend(textSecondary_, panelBg_, 82)));
        HGDIOBJ oldBrush = ::SelectObject(hDC, ::GetStockObject(HOLLOW_BRUSH));
        ::RoundRect(hDC, itemRc.left, itemRc.top, itemRc.right, itemRc.bottom, max(4, borderRadius_), max(4, borderRadius_));
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
    }

    if (keyStyle_ == KeyStyleRail) {
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(isSelected ? blend(selectedFg, selectedBg, 15) : blend(textSecondary_, panelBg_, 70), 2));
        int railInset = max(4, textMargin_);
        int railX = itemRc.left + max(2, textMargin_ / 2);
        ::MoveToEx(hDC, railX, itemRc.top + railInset, NULL);
        ::LineTo(hDC, railX, itemRc.bottom - railInset);
        ::SelectObject(hDC, oldPen);
    }

    // Draw selection key
    wchar_t selKey[] = L"?. ";
    selKey[0] = selKeys_[i];
    int textGap = modernCandidateTextGap(keyStyle_, textMargin_);
    bool wordFirst = keyStyle_ == KeyStyleWordFirst;
    RECT keyRc = { itemRc.left + textMargin_, itemRc.top, itemRc.left + textMargin_ + selKeyWidth_, itemRc.bottom };
    RECT textRc = { keyRc.right + textGap, itemRc.top, itemRc.right - textMargin_, itemRc.bottom };
    if (wordFirst) {
        textRc.left = itemRc.left + textMargin_;
        textRc.right = min(itemRc.right - textMargin_ - selKeyWidth_ - textGap, textRc.left + textWidth_);
        keyRc.left = textRc.right + textGap;
        keyRc.right = min(itemRc.right - textMargin_, keyRc.left + selKeyWidth_);
    }

    COLORREF keyColor = isSelected ? selectedFg : textSecondary_;
    if (keyStyle_ == KeyStyleQuiet || keyStyle_ == KeyStyleWordAnchor)
        keyColor = isSelected ? selectedMutedFg : blend(textSecondary_, panelBg_, 45);
    else if (keyStyle_ == KeyStyleGlowKey)
        keyColor = isSelected ? selectedFg : blend(textSecondary_, panelBg_, 30);
    else if (keyStyle_ == KeyStyleWordFirst)
        keyColor = isSelected ? selectedMutedFg : blend(textSecondary_, panelBg_, 38);

    int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
    COLORREF oldTextColor = ::SetTextColor(hDC, keyColor);
    HFONT keyFont = scaledKeyFont();
    HGDIOBJ oldKeyFont = keyFont ? ::SelectObject(hDC, keyFont) : NULL;

    if (keyStyle_ == KeyStyleKeycap) {
        if (isSelected) {
            RECT badgeRc = keyRc;
            ::InflateRect(&badgeRc, 0, -max(1, textMargin_ / 2));
            COLORREF badgeBg = selectedBadgeBg;
            COLORREF badgeBorder = blend(selectedBorder, badgeBg, 28);
            HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(badgeBg));
            HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(badgeBorder));
            int badgeRadius = max(3, min(borderRadius_, max(3, static_cast<int>(badgeRc.bottom - badgeRc.top) / 2)));
            ::RoundRect(hDC, badgeRc.left, badgeRc.top, badgeRc.right + 1, badgeRc.bottom, badgeRadius, badgeRadius);
            ::SelectObject(hDC, oldBrush);
            ::SelectObject(hDC, oldPen);
            ::SetTextColor(hDC, selectedBadgeFg);
        }
        else {
            ::SetTextColor(hDC, keyColor);
        }
        ::DrawTextW(hDC, selKey, 1, &keyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (keyStyle_ == KeyStyleBadgeMinimal || keyStyle_ == KeyStyleSoftCapsule || keyStyle_ == KeyStyleLeftTag) {
        RECT badgeRc = keyRc;
        if (keyStyle_ == KeyStyleLeftTag) {
            badgeRc.left = itemRc.left + max(1, textMargin_ / 2);
            badgeRc.right = badgeRc.left + selKeyWidth_;
            keyRc = badgeRc;
        }
        ::InflateRect(&badgeRc, 0, -max(1, textMargin_ / 2));

        COLORREF badgeBg = keyStyle_ == KeyStyleSoftCapsule
            ? (isSelected ? blend(selectedBg, highlightBg_, 24) : blend(panelBg_, textSecondary_, 10))
            : (isSelected ? selectedBadgeBg : panelBg_);
        COLORREF badgeBorder = keyStyle_ == KeyStyleSoftCapsule
            ? (isSelected ? blend(selectedBorder, badgeBg, 38) : badgeBg)
            : (isSelected ? blend(selectedBorder, badgeBg, 28) : blend(textSecondary_, panelBg_, 62));

        HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(badgeBg));
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(badgeBorder));
        int badgeHeight = static_cast<int>(badgeRc.bottom - badgeRc.top);
        int badgeRadius = keyStyle_ == KeyStyleSoftCapsule ? badgeHeight : max(3, min(borderRadius_, max(3, badgeHeight / 2)));
        ::RoundRect(hDC, badgeRc.left, badgeRc.top, badgeRc.right + 1, badgeRc.bottom, badgeRadius, badgeRadius);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        ::SetTextColor(hDC, isSelected ? readableOn(badgeBg, selectedBadgeFg, selectedFg) : keyColor);
        ::DrawTextW(hDC, selKey, 1, &keyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (keyStyle_ == KeyStyleAccentDot) {
        RECT markerRc = {
            keyRc.left + 1,
            keyRc.top + (keyRc.bottom - keyRc.top) / 2 - (isSelected ? 5 : 2),
            keyRc.left + (isSelected ? 5 : 4),
            keyRc.top + (keyRc.bottom - keyRc.top) / 2 + (isSelected ? 6 : 2)
        };
        COLORREF markerColor = isSelected ? selectedFg : blend(highlightBorder_, panelBg_, 20);
        HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(markerColor));
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(markerColor));
        ::RoundRect(hDC, markerRc.left, markerRc.top, markerRc.right, markerRc.bottom, 4, 4);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        RECT keyTextRc = keyRc;
        keyTextRc.left += 6;
        ::DrawTextW(hDC, selKey, 1, &keyTextRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (keyStyle_ == KeyStyleMicroTab) {
        RECT tabRc = keyRc;
        tabRc.top += max(1, textMargin_ / 2);
        tabRc.bottom = min(tabRc.bottom, tabRc.top + max(11, itemHeight_ / 2));
        tabRc.left += 1;
        tabRc.right -= 1;
        COLORREF tabBg = isSelected ? selectedBadgeBg : blend(panelBg_, textSecondary_, 11);
        HGDIOBJ oldBrush = ::SelectObject(hDC, cachedBrush(tabBg));
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(isSelected ? blend(selectedBorder, tabBg, 28) : tabBg));
        ::RoundRect(hDC, tabRc.left, tabRc.top, tabRc.right + 1, tabRc.bottom, 4, 4);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        ::SetTextColor(hDC, isSelected ? readableOn(tabBg, selectedBadgeFg, selectedFg) : keyColor);
        ::DrawTextW(hDC, selKey, 1, &tabRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else {
        UINT keyFormat = keyStyle_ == KeyStyleDivider ? DT_CENTER : DT_LEFT;
        if (keyStyle_ == KeyStyleMonospaceSlot)
            keyFormat = DT_CENTER;
        UINT keyVerticalFormat = keyStyle_ == KeyStyleWordFirst ? DT_TOP : DT_VCENTER;
        if (keyStyle_ == KeyStyleGlowKey && isSelected) {
            COLORREF glowColor = blend(selectedFg, selectedBg, 30);
            ::SetTextColor(hDC, glowColor);
            RECT glowRc = keyRc;
            ::OffsetRect(&glowRc, 1, 0);
            ::DrawTextW(hDC, selKey, 1, &glowRc, keyFormat | keyVerticalFormat | DT_SINGLELINE);
            glowRc = keyRc;
            ::OffsetRect(&glowRc, -1, 0);
            ::DrawTextW(hDC, selKey, 1, &glowRc, keyFormat | keyVerticalFormat | DT_SINGLELINE);
            ::SetTextColor(hDC, keyColor);
        }
        if (keyStyle_ == KeyStyleWordFirst)
            keyRc.top += max(2, textMargin_ / 2);
        ::DrawTextW(hDC, selKey, 1, &keyRc, keyFormat | keyVerticalFormat | DT_SINGLELINE);
        if (keyStyle_ == KeyStyleDivider) {
            HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(isSelected ? blend(selectedFg, selectedBg, 26) : blend(panelBorder_, textSecondary_, 35)));
            int dividerInset = max(3, textMargin_ / 2);
            int dividerX = keyRc.right - max(3, textMargin_);
            dividerX = max(static_cast<int>(keyRc.left) + 1, min(dividerX, static_cast<int>(keyRc.right) - 1));
            ::MoveToEx(hDC, dividerX, keyRc.top + dividerInset, NULL);
            ::LineTo(hDC, dividerX, keyRc.bottom - dividerInset);
            ::SelectObject(hDC, oldPen);
        }
    }

    if (keyFont) {
        ::SelectObject(hDC, oldKeyFont); // cached font is reused, not deleted
    }

    // Draw candidate text
    wstring& item = items_.at(i);
    COLORREF textColor = isSelected ? selectedFg : textPrimary_;
    ::SetTextColor(hDC, textColor);
    ::DrawTextW(hDC, item.c_str(), (int)item.length(), &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (keyStyle_ == KeyStyleWordAnchor) {
        SIZE itemSize;
        ::GetTextExtentPoint32W(hDC, item.c_str(), (int)item.length(), &itemSize);
        HGDIOBJ oldPen = ::SelectObject(hDC, cachedPen(isSelected ? blend(selectedFg, selectedBg, 20) : blend(textSecondary_, panelBg_, 45)));
        int anchorY = textRc.bottom - max(3, textMargin_ / 2);
        ::MoveToEx(hDC, textRc.left, anchorY, NULL);
        ::LineTo(hDC, min(textRc.left + itemSize.cx, textRc.right), anchorY);
        ::SelectObject(hDC, oldPen);
    }

    ::SetTextColor(hDC, oldTextColor);
    ::SetBkMode(hDC, oldBkMode);
}

void CandidateWindow::itemRect(int i, RECT& rect) {
    int row, col;
    int columnsPerRow = max(1, effectiveCandPerRow_);
    row = i / columnsPerRow;
    col = i % columnsPerRow;
    int extraItemPadding = modernCandidateExtraWidth(keyStyle_, textMargin_);
    rect.left = margin_ + col * (selKeyWidth_ + textWidth_ + colSpacing_ + extraItemPadding);

    // header height was measured by the last recalculateSize(); content
    // changes always pass through it before items are hit-tested/painted
    int headerHeight = 0;
    if (!header_.empty() || !pageInfo_.empty()) {
        headerHeight = measuredHeaderHeight_;
    }

    int headerGap = headerHeight > 0 ? textMargin_ : 0;
    int rowHeight = modernCandidateRowHeight();
    rect.top = headerHeight + headerGap + row * (rowHeight + rowSpacing_);
    rect.right = rect.left + (selKeyWidth_ + textWidth_ + extraItemPadding);
    rect.bottom = rect.top + rowHeight;
}


} // namespace Ime
