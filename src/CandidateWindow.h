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

#ifndef IME_CANDIDATE_WINDOW_H
#define IME_CANDIDATE_WINDOW_H

#include "ImeWindow.h"
#include <string>
#include <unordered_map>
#include <vector>
#include "ComObject.h"
#include "CandidateAccessibility.h"

namespace Ime {

class TextService;
class EditSession;
class KeyEvent;

// TODO: make the candidate window looks different in immersive mode
class CandidateWindow:
    public ImeWindow,
    public ComObject<ComInterface<ITfCandidateListUIElement>>,
    private CandidateAccessibleGeometry {
public:
    CandidateWindow(TextService* service, EditSession* session);

    // ITfUIElement
    STDMETHODIMP GetDescription(BSTR *pbstrDescription);
    STDMETHODIMP GetGUID(GUID *pguid);
    STDMETHODIMP Show(BOOL bShow);
    STDMETHODIMP IsShown(BOOL *pbShow);

    // ITfCandidateListUIElement
    STDMETHODIMP GetUpdatedFlags(DWORD *pdwFlags);
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **ppdim);
    STDMETHODIMP GetCount(UINT *puCount);
    STDMETHODIMP GetSelection(UINT *puIndex);
    STDMETHODIMP GetString(UINT uIndex, BSTR *pstr);
    STDMETHODIMP GetPageIndex(UINT *puIndex, UINT uSize, UINT *puPageCnt);
    STDMETHODIMP SetPageIndex(UINT *puIndex, UINT uPageCnt);
    STDMETHODIMP GetCurrentPage(UINT *puPage);

    const std::vector<std::wstring>& items() const {
        return items_;
    }

    const std::wstring& message() const {
        return message_;
    }

    void setMessage(const std::wstring& message) {
        if (message_ == message)
            return;
        message_ = message;
        recalculateSize();
        refresh();
    }

    void setItems(const std::vector<std::wstring>& items, const std::vector<wchar_t>& selKeys) {
        items_ = items;
        selKeys_ = selKeys;
        updatedFlags_ |= TF_CLUIE_STRING | TF_CLUIE_COUNT | TF_CLUIE_SELECTION;
        recalculateSize();
        refresh();
        syncAccessibility();
    }

    void setTextRows(const std::wstring& message, const std::wstring& header, const std::wstring& pageInfo) {
        message_ = message;
        header_ = header;
        pageInfo_ = pageInfo;
    }

    void add(std::wstring item, wchar_t selKey) {
        items_.push_back(item);
        selKeys_.push_back(selKey);
        updatedFlags_ |= TF_CLUIE_STRING | TF_CLUIE_COUNT;
    }

    void clear();

    int candPerRow() const {
        return candPerRow_;
    }
    void setCandPerRow(int n);

    virtual void recalculateSize();

    bool filterKeyEvent(KeyEvent& keyEvent);

    int currentSel() const {
        return currentSel_;
    }
    void setCurrentSel(int sel);

    wchar_t currentSelKey() const {
        return selKeys_.at(currentSel_);
    }

    bool hasResult() const {
        return hasResult_;
    }

    bool useCursor() const {
        return useCursor_;
    }

    void setUseCursor(bool use);

    const std::wstring& header() const {
        return header_;
    }

    void setHeader(const std::wstring& header) {
        if (header_ == header)
            return;
        header_ = header;
        recalculateSize();
        refresh();
    }

    const std::wstring& pageInfo() const {
        return pageInfo_;
    }

    void setPageInfo(const std::wstring& info) {
        if (pageInfo_ == info)
            return;
        pageInfo_ = info;
        recalculateSize();
        refresh();
    }

    // WIME's theme colors. While a Windows high-contrast theme is active they
    // are kept but not used: the window paints with the system colors instead.
    void setTheme(COLORREF panelBg, COLORREF panelBorder, COLORREF textPrimary, COLORREF textSecondary, COLORREF highlightBg, COLORREF highlightBorder, COLORREF highlightText) {
        const COLORREF theme[ThemeColorCount] = { panelBg, panelBorder, textPrimary, textSecondary, highlightBg, highlightBorder, highlightText };
        bool same = true;
        for (int i = 0; i < ThemeColorCount; ++i)
            same = same && themeColors_[i] == theme[i];
        if (same)
            return;
        for (int i = 0; i < ThemeColorCount; ++i)
            themeColors_[i] = theme[i];
        applyColors();
    }

    // High contrast: true while the window paints with the system colors
    // (COLOR_WINDOW, COLOR_WINDOWTEXT, COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT).
    bool highContrast() const {
        return highContrast_;
    }
    // Re-reads SPI_GETHIGHCONTRAST and the system colors (done on creation and
    // when Windows broadcasts a color / theme / high-contrast change).
    void updateSystemColors();
    // Forces high-contrast painting on or off (tests).
    void setHighContrast(bool on);

    // Assistive technology: call after a batch of changes (items, selection,
    // header/message, visibility) so screen readers get UI Automation events.
    // Cheap when nothing changed or no client listens.
    void syncAccessibility();
    // the UIA side of the window (tests)
    CandidateAccessibility& accessibility() {
        return accessibility_;
    }

    // ITfCandidateListUIElement::GetUpdatedFlags reports what changed since the
    // last UpdateUIElement; call this after each ITfUIElementMgr::UpdateUIElement.
    void resetUpdatedFlags() {
        updatedFlags_ = 0;
    }

    // shadows ImeWindow::setFont: the base deletes the old HFONT, so cached
    // fonts derived from it must be dropped before the handle can be reused
    void setFont(HFONT f);

    void setSpacing(int contentMargin, int textMargin, int borderRadius) {
        if (
            contentMargin_ == contentMargin &&
            textMargin_ == textMargin &&
            borderRadius_ == borderRadius
        )
            return;
        contentMargin_ = contentMargin;
        textMargin_ = textMargin;
        borderRadius_ = borderRadius;
        recalculateSize();
        refresh();
    }

    enum KeyStyle {
        KeyStyleKeycap = 0,
        KeyStyleDivider = 1,
        KeyStyleQuiet = 2,
        KeyStyleBadgeMinimal = 3,
        KeyStyleAccentDot = 4,
        KeyStyleRail = 5,
        KeyStyleMonospaceSlot = 6,
        KeyStyleWordFirst = 7,
        KeyStyleSoftCapsule = 8,
        KeyStyleLeftTag = 9,
        KeyStyleGlowKey = 10,
        KeyStyleMicroTab = 11,
        KeyStyleWordAnchor = 12
    };

    void setKeyStyle(int keyStyle) {
        if (keyStyle < KeyStyleKeycap)
            keyStyle = KeyStyleKeycap;
        if (keyStyle > KeyStyleWordAnchor)
            keyStyle = KeyStyleWordAnchor;
        if (keyStyle_ == keyStyle)
            return;
        keyStyle_ = keyStyle;
        refresh();
    }

    enum MessageStyle {
        MessageStyleBadge = 0,
        MessageStyleBar = 1,
        MessageStyleDot = 2
    };

    // visual treatment of the IME-name label (e.g. 大易) in the header row
    enum HeaderLabelStyle {
        HeaderLabelPlain = 0,
        HeaderLabelBadge = 1,
        HeaderLabelAccent = 2,
        HeaderLabelTag = 3,
        HeaderLabelBar = 4,
        HeaderLabelUnderline = 5
    };

    void setHeaderLabelStyle(int style) {
        if (style < HeaderLabelPlain)
            style = HeaderLabelPlain;
        if (style > HeaderLabelUnderline)
            style = HeaderLabelUnderline;
        if (headerLabelStyle_ == style)
            return;
        headerLabelStyle_ = style;
        recalculateSize();
        refresh();
    }

    void setMessageStyle(int messageStyle) {
        if (messageStyle < MessageStyleBadge)
            messageStyle = MessageStyleBadge;
        if (messageStyle > MessageStyleDot)
            messageStyle = MessageStyleDot;
        if (messageStyle_ == messageStyle)
            return;
        messageStyle_ = messageStyle;
        recalculateSize();
        refresh();
    }

    void setStableWidth(bool stable, int minWidth);
    void resetStableWidth();
    void setMaxWidth(bool wrapToMaxWidth, int maxWidth);

    // the grown stable width, so callers can carry it across window recreation
    int stableWidthPx() const {
        return stableWidthPx_;
    }
    void seedStableWidth(int px);

    // Draw the whole window into hDC (clientRect = the window's client area).
    // onPaint() uses it with the BeginPaint DC; tests use it with a memory DC.
    void paint(HDC hDC, const RECT& clientRect);

protected:
    LRESULT wndProc(UINT msg, WPARAM wp , LPARAM lp);
    void onPaint(WPARAM wp, LPARAM lp);
    void paintItem(HDC hDC, int i, int x, int y);
    void itemRect(int i, RECT& rect);
    int headerHeight(HDC hDC) const;
    int modernCandidateRowHeight() const;
    HFONT scaledKeyFont();
    HBRUSH panelBgBrush();
    HPEN panelBorderPen();
    HPEN headerDividerPen();
    void releaseThemeBrushes();
    // copies the theme or, in high contrast, the system colors into the
    // colors used for painting
    void applyColors();
    // color helpers that keep the system colors exact in high contrast
    COLORREF blend(COLORREF a, COLORREF b, int percentB) const;
    COLORREF readableOn(COLORREF bg, COLORREF preferred, COLORREF alternate) const;
    COLORREF accentTextColor() const;

    // CandidateAccessibleGeometry
    HWND accessibleHwnd() override;
    bool accessibleItemRect(int index, RECT* screenRect) override;

protected: // COM object should not be deleted directly. calling Release() instead.
    ~CandidateWindow(void);

private:
    enum { ThemeColorCount = 7 };
    COLORREF themeColors_[ThemeColorCount]; // as set by setTheme()
    bool highContrast_;
    DWORD updatedFlags_; // TF_CLUIE_* changed since the last UpdateUIElement
    CandidateAccessibility accessibility_;

    BOOL shown_;

    int selKeyWidth_;
    int textWidth_;
    int itemHeight_;
    int candPerRow_;
    int effectiveCandPerRow_;
    int colSpacing_;
    int rowSpacing_;
    std::vector<wchar_t> selKeys_;
    std::vector<std::wstring> items_;
    std::wstring message_;
    int currentSel_;
    bool hasResult_;
    bool useCursor_;
    std::wstring header_;
    std::wstring pageInfo_;

    COLORREF panelBg_;
    COLORREF panelBorder_;
    COLORREF textPrimary_;
    COLORREF textSecondary_;
    COLORREF highlightBg_;
    COLORREF highlightBorder_;
    COLORREF highlightText_;
    int contentMargin_;
    int textMargin_;
    int borderRadius_;
    int keyStyle_;
    int messageStyle_;
    int headerLabelStyle_;
    bool stableWidth_;
    int minStableWidth_;
    int stableWidthPx_;
    bool wrapToMaxWidth_;
    int maxWidth_;

    HBRUSH cachedBrush(COLORREF color);
    HPEN cachedPen(COLORREF color, int width = 1);
    void releaseItemGdiCache();

    // paint caches: scaled selection-key font and theme GDI objects are
    // reused across paints instead of being created per item per paint
    HFONT cachedKeyFont_;
    HFONT cachedKeyFontBase_;
    int cachedKeyFontPercent_;
    HBRUSH panelBgBrush_;
    HPEN panelBorderPen_;
    HPEN headerDividerPen_;
    // Item-level brush/pen caches: keyed by color (brushes) and color|width<<24 (pens).
    // Entries live until releaseThemeBrushes() clears them on theme change.
    std::unordered_map<COLORREF, HBRUSH> itemBrushCache_;
    std::unordered_map<DWORD, HPEN> itemPenCache_;
    int measuredHeaderHeight_; // refreshed by recalculateSize()
    // candidate text extents measured with the current font; IMEs show the
    // same candidates over and over, so skip GetTextExtentPoint32W on hits
    std::unordered_map<std::wstring, SIZE> textSizeCache_;
};

}

#endif
