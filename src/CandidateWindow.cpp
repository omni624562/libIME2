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
#include "DrawUtils.h"
#include "TextService.h"
#include "EditSession.h"

#include <algorithm>
#include <cassert>

#include <tchar.h>
#include <windows.h>

using namespace std;

namespace Ime {

CandidateWindow::CandidateWindow(TextService* service, EditSession* session):
    ImeWindow(service),
    shown_(false),
    candPerRow_(1),
    textWidth_(0),
    itemHeight_(0),
    headerHeight_(0),
    headerSpacing_(4),
    currentSel_(0),
    hasResult_(false),
    useCursor_(true),
    selKeyWidth_(0),
    modernStyle_(false),
    textMargin_(6),
    borderRadius_(8),
    panelBackground_(RGB(255, 255, 255)),
    panelBorder_(RGB(218, 221, 227)),
    textPrimary_(RGB(32, 36, 42)),
    textSecondary_(RGB(107, 114, 128)),
    highlightBackground_(RGB(220, 235, 255)),
    highlightBorder_(RGB(156, 199, 255)),
    highlightText_(RGB(11, 58, 117)) {

    if(service->isImmersive()) { // windows 8 app mode
        margin_ = 10;
        rowSpacing_ = 8;
        colSpacing_ = 12;
    }
    else { // desktop mode
        margin_ = 5;
        rowSpacing_ = 4;
        colSpacing_ = 8;
    }

    HWND parent = service->compositionWindow(session);
    create(parent, WS_POPUP|WS_CLIPCHILDREN, WS_EX_TOOLWINDOW|WS_EX_TOPMOST);
}

CandidateWindow::~CandidateWindow(void) {
}

// ITfUIElement
STDMETHODIMP CandidateWindow::GetDescription(BSTR *pbstrDescription) {
    if (!pbstrDescription)
        return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"Candidate window~");
    return S_OK;
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
    /// XXX update all!!!
    *pdwFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetDocumentMgr(ITfDocumentMgr **ppdim) {
    if (!textService_)
        return E_FAIL;
    return textService_->currentContext()->GetDocumentMgr(ppdim);
}

STDMETHODIMP CandidateWindow::GetCount(UINT *puCount) {
    if (!puCount)
        return E_INVALIDARG;
    *puCount = static_cast<UINT>(std::min<size_t>(10, items_.size()));
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetSelection(UINT *puIndex) {
    assert(currentSel_ >= 0);
    if (!puIndex)
        return E_INVALIDARG;
    *puIndex = static_cast<UINT>(currentSel_);
    return S_OK;
}

STDMETHODIMP CandidateWindow::GetString(UINT uIndex, BSTR *pbstr) {
    if (!pbstr)
        return E_INVALIDARG;
    if (uIndex >= items_.size())
        return E_INVALIDARG;
    *pbstr = SysAllocString(items_[uIndex].c_str());
    return S_OK;
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
    /// XXX Do not let app set page indices.
    if (!puIndex)
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
        default:
            return Window::wndProc(msg, wp, lp);
    }
    return 0;
}

void CandidateWindow::onPaint(WPARAM wp, LPARAM lp) {
    // TODO: check isImmersive_, and draw the window differently
    // in Windows 8 app immersive mode to follow windows 8 UX guidelines
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    HDC hDC = ps.hdc;
    HFONT oldFont;
    RECT rc;

    oldFont = (HFONT)SelectObject(hDC, font_);

    GetClientRect(hwnd_,&rc);
    SetTextColor(hDC, GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(hDC, GetSysColor(COLOR_WINDOW));

    int y = margin_;
    if(modernStyle_) {
        SetBkMode(hDC, TRANSPARENT);
        ::FillSolidRect(ps.hdc, &rc, panelBackground_);
        HPEN pen = ::CreatePen(PS_SOLID, 1, panelBorder_);
        HBRUSH brush = ::CreateSolidBrush(panelBackground_);
        HGDIOBJ oldPen = ::SelectObject(hDC, pen);
        HGDIOBJ oldBrush = ::SelectObject(hDC, brush);
        ::RoundRect(hDC, rc.left, rc.top, rc.right, rc.bottom, borderRadius_, borderRadius_);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        ::DeleteObject(brush);
        ::DeleteObject(pen);
    }
    else if(isImmersive()) {
        HPEN pen = ::CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
        HGDIOBJ oldPen = ::SelectObject(hDC, pen);
        ::Rectangle(hDC, rc.left, rc.top, rc.right, rc.bottom);
        ::SelectObject(hDC, oldPen);
        ::DeleteObject(pen);
    }
    else {
        ::FillSolidRect(ps.hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, GetSysColor(COLOR_WINDOW));
        ::Draw3DBorder(hDC, &rc, GetSysColor(COLOR_3DFACE), 0);
    }

    if(modernStyle_ && !header_.empty()) {
        RECT headerRect = {margin_, y, rc.right - margin_, y + headerHeight_};
        ::SetTextColor(hDC, textSecondary_);
        ::DrawTextW(hDC, header_.c_str(), static_cast<int>(header_.length()), &headerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += headerHeight_ + headerSpacing_;
    }

    // paint items
    int col = 0;
    int x = margin_;
    for(int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
        paintItem(hDC, i, x, y);
        ++col; // go to next column
        if(col >= candPerRow_) {
            col = 0;
            x = margin_;
            y += itemHeight_ + rowSpacing_;
        }
        else {
            x += colSpacing_ + selKeyWidth_ + textWidth_ + (modernStyle_ ? textMargin_ * 2 : 0);
        }
    }
    SelectObject(hDC, oldFont);
    EndPaint(hwnd_, &ps);
}

void CandidateWindow::recalculateSize() {
    if(!modernStyle_) {
        if(items_.empty()) {
            resize(margin_ * 2, margin_ * 2);
        }

        HDC hDC = ::GetWindowDC(hwnd());
        int height, width;
        selKeyWidth_ = 0;
        textWidth_ = 0;
        itemHeight_ = 0;

        HGDIOBJ oldFont = ::SelectObject(hDC, font_);
        for(int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
            SIZE selKeySize;
            int lineHeight = 0;
            wchar_t selKey[] = L"?. ";
            selKey[0] = selKeys_[i];
            ::GetTextExtentPoint32W(hDC, selKey, 3, &selKeySize);
            if(selKeySize.cx > selKeyWidth_)
                selKeyWidth_ = selKeySize.cx;

            SIZE candidateSize;
            wstring& item = items_.at(i);
            ::GetTextExtentPoint32W(hDC, item.c_str(), static_cast<int>(item.length()), &candidateSize);
            if(candidateSize.cx > textWidth_)
                textWidth_ = candidateSize.cx;
            int itemHeight = max(candidateSize.cy, selKeySize.cy);
            if(itemHeight > itemHeight_)
                itemHeight_ = itemHeight;
        }
        ::SelectObject(hDC, oldFont);
        ::ReleaseDC(hwnd(), hDC);

        int itemCount = static_cast<int>(items_.size());
        if(itemCount <= candPerRow_) {
            width = itemCount * (selKeyWidth_ + textWidth_);
            if(itemCount > 0)
                width += colSpacing_ * (itemCount - 1);
            width += margin_ * 2;
            height = itemHeight_ + margin_ * 2;
        }
        else {
            width = candPerRow_ * (selKeyWidth_ + textWidth_);
            width += colSpacing_ * (candPerRow_ - 1);
            width += margin_ * 2;
            int rowCount = itemCount / candPerRow_;
            if(itemCount % candPerRow_)
                ++rowCount;
            height = itemHeight_ * rowCount + rowSpacing_ * (rowCount - 1) + margin_ * 2;
        }
        resize(width, height);
        return;
    }

    if(items_.empty()) {
        resize(margin_ * 2, margin_ * 2);
    }

    HDC hDC = ::GetWindowDC(hwnd());
    int height = 0;
    int width = 0;
    selKeyWidth_ = 0;
    textWidth_ = 0;
    itemHeight_ = 0;
    headerHeight_ = 0;

    HGDIOBJ oldFont = ::SelectObject(hDC, font_);
    SIZE headerSize = {0, 0};
    if(!header_.empty()) {
        ::GetTextExtentPoint32W(hDC, header_.c_str(), static_cast<int>(header_.length()), &headerSize);
        headerHeight_ = headerSize.cy;
        width = headerSize.cx;
    }

    vector<wstring>::const_iterator it;
    for(int i = 0, n = static_cast<int>(items_.size()); i < n; ++i) {
        SIZE selKeySize;
        int lineHeight = 0;
        // the selection key string
        wchar_t selKey[] = L"?. ";
        selKey[0] = selKeys_[i];
        ::GetTextExtentPoint32W(hDC, selKey, 3, &selKeySize);
        if(selKeySize.cx > selKeyWidth_)
            selKeyWidth_ = selKeySize.cx;

        // the candidate string
        SIZE candidateSize;
        wstring& item = items_.at(i);
        ::GetTextExtentPoint32W(hDC, item.c_str(), static_cast<int>(item.length()), &candidateSize);
        if(candidateSize.cx > textWidth_)
            textWidth_ = candidateSize.cx;
        int itemHeight = max(candidateSize.cy, selKeySize.cy);
        if(itemHeight > itemHeight_)
            itemHeight_ = itemHeight;
    }
    itemHeight_ += max(2, textMargin_ / 2) * 2;
    ::SelectObject(hDC, oldFont);
    ::ReleaseDC(hwnd(), hDC);

    int candidateWidth = 0;
    int candidateHeight = 0;
    int itemCount = static_cast<int>(items_.size());
    if(itemCount <= candPerRow_) {
        candidateWidth = itemCount * (selKeyWidth_ + textWidth_ + textMargin_ * 2);
        if(itemCount > 0)
            candidateWidth += colSpacing_ * (itemCount - 1);
        candidateHeight = itemHeight_;
    }
    else {
        candidateWidth = candPerRow_ * (selKeyWidth_ + textWidth_ + textMargin_ * 2);
        candidateWidth += colSpacing_ * (candPerRow_ - 1);
        int rowCount = itemCount / candPerRow_;
        if(itemCount % candPerRow_)
            ++rowCount;
        candidateHeight = itemHeight_ * rowCount + rowSpacing_ * (rowCount - 1);
    }
    width = max(width, candidateWidth) + margin_ * 2;
    height = candidateHeight;
    if(headerHeight_ > 0)
        height += headerHeight_ + headerSpacing_;
    height += margin_ * 2;
    resize(width, height);
}

void CandidateWindow::setHeader(std::wstring header) {
    if(header_ != header) {
        header_ = header;
        if(modernStyle_) {
            recalculateSize();
            refresh();
        }
    }
}

void CandidateWindow::setModernStyle(bool enabled) {
    if(modernStyle_ != enabled) {
        modernStyle_ = enabled;
        recalculateSize();
        refresh();
    }
}

void CandidateWindow::setCandPerRow(int n) {
    if(n < 1)
        n = 1;
    if(n != candPerRow_) {
        candPerRow_ = n;
        recalculateSize();
    }
}

void CandidateWindow::setTheme(COLORREF panelBackground,
                               COLORREF panelBorder,
                               COLORREF textPrimary,
                               COLORREF textSecondary,
                               COLORREF highlightBackground,
                               COLORREF highlightBorder,
                               COLORREF highlightText) {
    panelBackground_ = panelBackground;
    panelBorder_ = panelBorder;
    textPrimary_ = textPrimary;
    textSecondary_ = textSecondary;
    highlightBackground_ = highlightBackground;
    highlightBorder_ = highlightBorder;
    highlightText_ = highlightText;
    if(isVisible())
        ::InvalidateRect(hwnd_, NULL, TRUE);
}

void CandidateWindow::setSpacing(int contentMargin, int textMargin, int borderRadius) {
    if(modernStyle_)
        margin_ = max(0, contentMargin);
    textMargin_ = max(0, textMargin);
    borderRadius_ = max(0, borderRadius);
    if(modernStyle_)
        recalculateSize();
    if(modernStyle_ && isVisible())
        ::InvalidateRect(hwnd_, NULL, TRUE);
}

bool CandidateWindow::filterKeyEvent(KeyEvent& keyEvent) {
    // select item with arrow keys
    int oldSel = currentSel_;
    switch(keyEvent.keyCode()) {
    case VK_UP:
        if(currentSel_ - candPerRow_ >=0)
            currentSel_ -= candPerRow_;
        break;
    case VK_DOWN:
        if(currentSel_ + candPerRow_ < static_cast<int>(items_.size()))
            currentSel_ += candPerRow_;
        break;
    case VK_LEFT:
        if(currentSel_ - 1 >=0)
            --currentSel_;
        break;
    case VK_RIGHT:
        if(currentSel_ + 1 < static_cast<int>(items_.size()))
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
        // repaint the old and new items
        RECT rect;
        itemRect(oldSel, rect);
        ::InvalidateRect(hwnd_, &rect, TRUE);
        itemRect(currentSel_, rect);
        ::InvalidateRect(hwnd_, &rect, TRUE);
        return true;
    }
    return false;
}

void CandidateWindow::setCurrentSel(int sel) {
    if(sel >= static_cast<int>(items_.size()))
        sel = 0;
    if (currentSel_ != sel) {
        currentSel_ = sel;
        if (isVisible())
            ::InvalidateRect(hwnd_, NULL, TRUE);
    }
}

void CandidateWindow::clear() {
    items_.clear();
    selKeys_.clear();
    currentSel_ = 0;
    hasResult_ = false;
}

void CandidateWindow::setUseCursor(bool use) {
    useCursor_ = use;
    if(isVisible())
        ::InvalidateRect(hwnd_, NULL, TRUE);
}

void CandidateWindow::paintItem(HDC hDC, int i,  int x, int y) {
    if(!modernStyle_) {
        RECT textRect = {x, y, 0, y + itemHeight_};
        wchar_t selKey[] = L"?. ";
        selKey[0] = selKeys_[i];
        textRect.right = textRect.left + selKeyWidth_;
        COLORREF selKeyColor = RGB(0, 0, 255);
        COLORREF oldColor = ::SetTextColor(hDC, selKeyColor);
        ::ExtTextOut(hDC, textRect.left, textRect.top, ETO_OPAQUE, &textRect, selKey, 3, NULL);
        ::SetTextColor(hDC, oldColor);

        wstring& item = items_.at(i);
        textRect.left += selKeyWidth_;
        textRect.right = textRect.left + textWidth_;
        ::ExtTextOut(hDC, textRect.left, textRect.top, ETO_OPAQUE, &textRect, item.c_str(), static_cast<UINT>(item.length()), NULL);

        if(useCursor_ && i == currentSel_) {
            int left = textRect.left;
            int top = textRect.top;
            int width = textRect.right - left;
            ::BitBlt(hDC, left, top, width, itemHeight_, hDC, left, top, NOTSRCCOPY);
        }
        return;
    }

    RECT itemRc;
    itemRect(i, itemRc);
    itemRc.left = x;
    itemRc.top = y;
    itemRc.right = x + selKeyWidth_ + textWidth_ + textMargin_ * 2;
    itemRc.bottom = y + itemHeight_;

    if(useCursor_ && i == currentSel_) {
        HPEN pen = ::CreatePen(PS_SOLID, 1, highlightBorder_);
        HBRUSH brush = ::CreateSolidBrush(highlightBackground_);
        HGDIOBJ oldPen = ::SelectObject(hDC, pen);
        HGDIOBJ oldBrush = ::SelectObject(hDC, brush);
        ::RoundRect(hDC, itemRc.left, itemRc.top, itemRc.right, itemRc.bottom, borderRadius_ / 2, borderRadius_ / 2);
        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        ::DeleteObject(brush);
        ::DeleteObject(pen);
    }

    int verticalPadding = max(2, textMargin_ / 2);
    RECT textRect = {x + textMargin_, y + verticalPadding, 0, y + itemHeight_ - verticalPadding};
    wchar_t selKey[] = L"?. ";
    selKey[0] = selKeys_[i];
    textRect.right = textRect.left + selKeyWidth_;
    COLORREF selKeyColor = (useCursor_ && i == currentSel_) ? highlightText_ : textSecondary_;
    COLORREF oldColor = ::SetTextColor(hDC, selKeyColor);
    // paint the selection key
    ::ExtTextOut(hDC, textRect.left, textRect.top, 0, &textRect, selKey, 3, NULL);
    ::SetTextColor(hDC, oldColor); // restore text color

    // paint the candidate string
    wstring& item = items_.at(i);
    textRect.left += selKeyWidth_;
    textRect.right = textRect.left + textWidth_;
    ::SetTextColor(hDC, (useCursor_ && i == currentSel_) ? highlightText_ : textPrimary_);
    // paint the candidate string
    ::ExtTextOut(hDC, textRect.left, textRect.top, 0, &textRect, item.c_str(), static_cast<UINT>(item.length()), NULL);
}

void CandidateWindow::itemRect(int i, RECT& rect) {
    int row, col;
    row = i / candPerRow_;
    col = i % candPerRow_;
    if(!modernStyle_) {
        rect.left = margin_ + col * (selKeyWidth_ + textWidth_ + colSpacing_);
        rect.top = margin_ + row * (itemHeight_ + rowSpacing_);
        rect.right = rect.left + (selKeyWidth_ + textWidth_);
        rect.bottom = rect.top + itemHeight_;
        return;
    }

    int topOffset = headerHeight_ > 0 ? headerHeight_ + headerSpacing_ : 0;
    rect.left = margin_ + col * (selKeyWidth_ + textWidth_ + colSpacing_);
    rect.left += col * textMargin_ * 2;
    rect.top = margin_ + topOffset + row * (itemHeight_ + rowSpacing_);
    rect.right = rect.left + (selKeyWidth_ + textWidth_ + textMargin_ * 2);
    rect.bottom = rect.top + itemHeight_;
}


} // namespace Ime
