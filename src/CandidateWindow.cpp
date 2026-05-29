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

static COLORREF blendColor(COLORREF a, COLORREF b, int percentB) {
    int percentA = 100 - percentB;
    return RGB(
        (GetRValue(a) * percentA + GetRValue(b) * percentB) / 100,
        (GetGValue(a) * percentA + GetGValue(b) * percentB) / 100,
        (GetBValue(a) * percentA + GetBValue(b) * percentB) / 100
    );
}

CandidateWindow::CandidateWindow(TextService* service, EditSession* session):
    ImeWindow(service),
    shown_(false),
    candPerRow_(1),
    effectiveCandPerRow_(1),
    textWidth_(0),
    itemHeight_(0),
    currentSel_(0),
    hasResult_(false),
    useCursor_(true),
    selKeyWidth_(0),
    modernStyle_(false),
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
    stableWidth_(false),
    minStableWidth_(0),
    stableWidthPx_(0),
    wrapToMaxWidth_(false),
    maxWidth_(0) {

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
    *puCount = std::min<UINT>(10, items_.size());
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

    if (modernStyle_) {
        SetTextColor(hDC, textPrimary_);
        SetBkColor(hDC, panelBg_);

        // Draw rounded modern background and border
        HBRUSH bgBrush = ::CreateSolidBrush(panelBg_);
        HPEN borderPen = ::CreatePen(PS_SOLID, 1, panelBorder_);
        HGDIOBJ oldBrush = ::SelectObject(hDC, bgBrush);
        HGDIOBJ oldPen = ::SelectObject(hDC, borderPen);

        ::RoundRect(hDC, rc.left, rc.top, rc.right, rc.bottom, borderRadius_ * 2, borderRadius_ * 2);

        ::SelectObject(hDC, oldBrush);
        ::SelectObject(hDC, oldPen);
        ::DeleteObject(bgBrush);
        ::DeleteObject(borderPen);
    } else {
        SetTextColor(hDC, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(hDC, GetSysColor(COLOR_WINDOW));

        // paint window background and border
        // draw a flat black border in Windows 8 app immersive mode
        // draw a 3d border in desktop mode
        if(isImmersive()) {
            HPEN pen = ::CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
            HGDIOBJ oldPen = ::SelectObject(hDC, pen);
            ::Rectangle(hDC, rc.left, rc.top, rc.right, rc.bottom);
            ::SelectObject(hDC, oldPen);
            ::DeleteObject(pen);
        }
        else {
            // draw a 3d border in desktop mode
            ::FillSolidRect(ps.hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, GetSysColor(COLOR_WINDOW));
            ::Draw3DBorder(hDC, &rc, GetSysColor(COLOR_3DFACE), 0);
        }
    }

    // paint header row (label text left-aligned, page info right-aligned)
    int headerHeight = this->headerHeight(hDC);
    if (!header_.empty() || !pageInfo_.empty()) {
        COLORREF headerLabelColor = modernStyle_ ? textSecondary_ : RGB(0, 0, 180);
        COLORREF headerValueColor = modernStyle_ ? highlightText_ : RGB(0, 0, 180);
        COLORREF oldColor = ::SetTextColor(hDC, headerLabelColor);
        if (modernStyle_) {
            ::SetBkMode(hDC, TRANSPARENT);
        }

        int rowTop = modernStyle_ ? 0 : margin_;
        int rowBottom = modernStyle_ ? headerHeight : rowTop + headerHeight;
        int pageInfoLeft = rc.right - margin_ - (modernStyle_ ? textMargin_ : 0);
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

            int textX = margin_ + (modernStyle_ ? textMargin_ : 0);
            RECT labelRect = { textX, rowTop, pageInfoLeft - textMargin_, rowBottom };
            if (!label.empty()) {
                ::SetTextColor(hDC, headerLabelColor);
                ::DrawTextW(hDC, label.c_str(), (int)label.length(), &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SIZE labelSize;
                ::GetTextExtentPoint32W(hDC, label.c_str(), (int)label.length(), &labelSize);
                labelRect.left += labelSize.cx + textMargin_;
            }

            ::SetTextColor(hDC, headerValueColor);
            ::DrawTextW(hDC, value.c_str(), (int)value.length(), &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (!pageInfo_.empty()) {
            SIZE piSize;
            ::GetTextExtentPoint32W(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &piSize);
            // right-aligned, vertically centered in the header row
            int piX = pageInfoLeft;
            RECT piRect = { piX, rowTop, rc.right - margin_, rowBottom };
            ::SetTextColor(hDC, headerLabelColor);
            ::DrawTextW(hDC, pageInfo_.c_str(), (int)pageInfo_.length(), &piRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (modernStyle_) {
            HPEN dividerPen = ::CreatePen(PS_SOLID, 1, blendColor(panelBorder_, panelBg_, 35));
            HGDIOBJ oldPen = ::SelectObject(hDC, dividerPen);
            int dividerY = max(0, headerHeight - 1);
            ::MoveToEx(hDC, 1, dividerY, NULL);
            ::LineTo(hDC, rc.right - 1, dividerY);
            ::SelectObject(hDC, oldPen);
            ::DeleteObject(dividerPen);
        }

        if (modernStyle_) {
            ::SetBkMode(hDC, OPAQUE);
        }
        ::SetTextColor(hDC, oldColor);
    }

    if (!message_.empty()) {
        int messageTop = modernStyle_ && headerHeight > 0 ? headerHeight + textMargin_ : margin_ + headerHeight;
        RECT messageRect = {
            margin_ + (modernStyle_ ? textMargin_ : 0),
            messageTop,
            rc.right - margin_ - (modernStyle_ ? textMargin_ : 0),
            rc.bottom - margin_
        };

        COLORREF oldTextColor = ::SetTextColor(hDC, modernStyle_ ? textPrimary_ : GetSysColor(COLOR_WINDOWTEXT));
        int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
        ::DrawTextW(hDC, message_.c_str(), (int)message_.length(), &messageRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ::SetBkMode(hDC, oldBkMode);
        ::SetTextColor(hDC, oldTextColor);
        SelectObject(hDC, oldFont);
        EndPaint(hwnd_, &ps);
        return;
    }

    // paint items
    int col = 0;
    int x = margin_, y = margin_ + headerHeight;
    if (modernStyle_ && headerHeight > 0)
        y = headerHeight + textMargin_;
    int columnsPerRow = max(1, effectiveCandPerRow_);
    for(int i = 0, n = items_.size(); i < n; ++i) {
        paintItem(hDC, i, x, y);
        ++col; // go to next column
        if(col >= columnsPerRow) {
            col = 0;
            x = margin_;
            y += modernStyle_ ? modernCandidateRowHeight() + rowSpacing_ : itemHeight_ + rowSpacing_;
        }
        else {
            x += colSpacing_ + selKeyWidth_ + textWidth_ + (modernStyle_ ? textMargin_ * 4 : 0);
        }
    }
    SelectObject(hDC, oldFont);
    EndPaint(hwnd_, &ps);
}

void CandidateWindow::recalculateSize() {
    if (modernStyle_) {
        margin_ = contentMargin_;
        rowSpacing_ = max(0, textMargin_ / 2);
        colSpacing_ = max(8, textMargin_ * 3);
    }

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
        // the selection key string
        wchar_t selKey[] = L"?. ";
        selKey[0] = selKeys_[i];
        ::GetTextExtentPoint32W(hDC, selKey, modernStyle_ ? 1 : 3, &selKeySize);
        if(selKeySize.cx > selKeyWidth_)
            selKeyWidth_ = selKeySize.cx;

        // the candidate string
        SIZE candidateSize;
        wstring& item = items_.at(i);
        ::GetTextExtentPoint32W(hDC, item.c_str(), item.length(), &candidateSize);
        if(candidateSize.cx > textWidth_)
            textWidth_ = candidateSize.cx;
        int itemHeight = max(candidateSize.cy, selKeySize.cy);
        if(itemHeight > itemHeight_)
            itemHeight_ = itemHeight;
    }
    if (itemHeight_ < fontLineHeight)
        itemHeight_ = fontLineHeight;

    // measure header (reuse the same DC)
    int headerHeight = this->headerHeight(hDC);
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
        messageHeight = max(messageSize.cy, fontLineHeight);
    }

    ::SelectObject(hDC, oldFont);
    ::ReleaseDC(hwnd(), hDC);

    int extraItemPadding = modernStyle_ ? textMargin_ * 3 : 0;
    int modernRowHeight = modernCandidateRowHeight();
    int headerGap = modernStyle_ && headerHeight > 0 ? textMargin_ : 0;
    int topPadding = modernStyle_ ? headerHeight + headerGap : margin_ + headerHeight;
    int bottomPadding = margin_;

    int itemStride = selKeyWidth_ + textWidth_ + extraItemPadding;
    int effectiveCandPerRow = max(1, candPerRow_);
    if (modernStyle_ && wrapToMaxWidth_ && maxWidth_ > 0 && itemStride > 0 && !items_.empty()) {
        int contentLimit = max(1, maxWidth_ - margin_ * 2);
        int maxColumns = (contentLimit + colSpacing_) / (itemStride + colSpacing_);
        effectiveCandPerRow = max(1, min(effectiveCandPerRow, maxColumns));
    }
    effectiveCandPerRow_ = effectiveCandPerRow;

    if (!message_.empty()) {
        int messageRowHeight = modernStyle_ ? messageHeight + textMargin_ * 2 : messageHeight;
        width = messageWidth + margin_ * 2 + (modernStyle_ ? textMargin_ * 2 : 0);
        width = max(width, headerWidth + margin_ * 2);
        height = topPadding + messageRowHeight + bottomPadding;
    }
    else if(items_.empty()) {
        width = headerWidth > 0 ? headerWidth + margin_ * 2 : margin_ * 2;
        height = headerHeight > 0 ? headerHeight + bottomPadding : margin_ * 2;
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
        height = topPadding + (modernStyle_ ? modernRowHeight : itemHeight_) * rowCount + rowSpacing_ * (rowCount - 1) + bottomPadding;
    }
    if (modernStyle_ && wrapToMaxWidth_ && maxWidth_ > 0) {
        int maxWindowWidth = max(maxWidth_, headerWidth + margin_ * 2);
        maxWindowWidth = max(maxWindowWidth, minStableWidth_);
        width = min(width, maxWindowWidth);
    }
    if (modernStyle_ && stableWidth_) {
        int minWidth = max(0, minStableWidth_);
        if (stableWidthPx_ < minWidth)
            stableWidthPx_ = minWidth;
        if (width > stableWidthPx_)
            stableWidthPx_ = width;
        if (wrapToMaxWidth_ && maxWidth_ > 0) {
            int maxWindowWidth = max(maxWidth_, headerWidth + margin_ * 2);
            maxWindowWidth = max(maxWindowWidth, minWidth);
            stableWidthPx_ = min(stableWidthPx_, maxWindowWidth);
        }
        width = stableWidthPx_;
    }
    resize(width, height);
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
    if(sel >= (int)items_.size())
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
    message_.clear();
    currentSel_ = 0;
    hasResult_ = false;
}

void CandidateWindow::setUseCursor(bool use) {
    useCursor_ = use;
    if(isVisible())
        ::InvalidateRect(hwnd_, NULL, TRUE);
}

void CandidateWindow::setStableWidth(bool stable, int minWidth) {
    minWidth = max(0, minWidth);
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

void CandidateWindow::setMaxWidth(bool wrapToMaxWidth, int maxWidth) {
    maxWidth = max(0, maxWidth);
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
    if (modernStyle_) {
        TEXTMETRIC textMetrics;
        ::GetTextMetrics(hDC, &textMetrics);
        textHeight = max(textHeight, textMetrics.tmHeight + textMetrics.tmExternalLeading);
        return textHeight + textMargin_ * 2;
    }
    return textHeight + margin_;
}

int CandidateWindow::modernCandidateRowHeight() const {
    return itemHeight_ + textMargin_ * 2;
}

void CandidateWindow::paintItem(HDC hDC, int i,  int x, int y) {
    if (modernStyle_) {
        RECT itemRc;
        itemRect(i, itemRc);
        ::InflateRect(&itemRc, 0, -max(1, textMargin_ / 3));

        bool isSelected = (useCursor_ && i == currentSel_);

        // Fill background of the item
        if (isSelected) {
            HBRUSH bgBrush = ::CreateSolidBrush(highlightBg_);
            HPEN borderPen = ::CreatePen(PS_SOLID, 1, highlightBorder_);
            HGDIOBJ oldBrush = ::SelectObject(hDC, bgBrush);
            HGDIOBJ oldPen = ::SelectObject(hDC, borderPen);

            ::RoundRect(hDC, itemRc.left, itemRc.top, itemRc.right, itemRc.bottom, borderRadius_ * 2, borderRadius_ * 2);

            ::SelectObject(hDC, oldBrush);
            ::SelectObject(hDC, oldPen);
            ::DeleteObject(bgBrush);
            ::DeleteObject(borderPen);
        }

        // Draw selection key
        wchar_t selKey[] = L"?. ";
        selKey[0] = selKeys_[i];
        RECT keyRc = { itemRc.left + textMargin_, itemRc.top, itemRc.left + textMargin_ + selKeyWidth_, itemRc.bottom };
        COLORREF keyColor = isSelected ? highlightText_ : textSecondary_;
        COLORREF oldTextColor = ::SetTextColor(hDC, keyColor);
        int oldBkMode = ::SetBkMode(hDC, TRANSPARENT);
        ::DrawTextW(hDC, selKey, 1, &keyRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Draw candidate text
        wstring& item = items_.at(i);
        RECT textRc = { keyRc.right + textMargin_, itemRc.top, itemRc.right - textMargin_, itemRc.bottom };
        COLORREF textColor = isSelected ? highlightText_ : textPrimary_;
        ::SetTextColor(hDC, textColor);
        ::DrawTextW(hDC, item.c_str(), (int)item.length(), &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        ::SetTextColor(hDC, oldTextColor);
        ::SetBkMode(hDC, oldBkMode);
    } else {
        RECT textRect = {x, y, 0, y + itemHeight_};
        wchar_t selKey[] = L"?. ";
        selKey[0] = selKeys_[i];
        textRect.right = textRect.left + selKeyWidth_;
        // FIXME: make the color of strings configurable.
        COLORREF selKeyColor = RGB(0, 0, 255);
        COLORREF oldColor = ::SetTextColor(hDC, selKeyColor);
        // paint the selection key
        ::ExtTextOut(hDC, textRect.left, textRect.top, ETO_OPAQUE, &textRect, selKey, 3, NULL);
        ::SetTextColor(hDC, oldColor); // restore text color

        // paint the candidate string
        wstring& item = items_.at(i);
        textRect.left += selKeyWidth_;
        textRect.right = textRect.left + textWidth_;
        // paint the candidate string
        ::ExtTextOut(hDC, textRect.left, textRect.top, ETO_OPAQUE, &textRect, item.c_str(), item.length(), NULL);

        if(useCursor_ && i == currentSel_) { // invert the selected item
            int left = textRect.left; // - selKeyWidth_;
            int top = textRect.top;
            int width = textRect.right - left;
            int height = itemHeight_;
            ::BitBlt(hDC, left, top, width, itemHeight_, hDC, left, top, NOTSRCCOPY);
        }
    }
}

void CandidateWindow::itemRect(int i, RECT& rect) {
    int row, col;
    int columnsPerRow = max(1, effectiveCandPerRow_);
    row = i / columnsPerRow;
    col = i % columnsPerRow;
    if (modernStyle_) {
        int extraItemPadding = textMargin_ * 3;
        rect.left = margin_ + col * (selKeyWidth_ + textWidth_ + colSpacing_ + extraItemPadding);

        // measure header height
        int headerHeight = 0;
        if (!header_.empty() || !pageInfo_.empty()) {
            HDC hDC = ::GetWindowDC(hwnd());
            HGDIOBJ oldFont = ::SelectObject(hDC, font_);
            headerHeight = this->headerHeight(hDC);
            ::SelectObject(hDC, oldFont);
            ::ReleaseDC(hwnd(), hDC);
        }

        int headerGap = headerHeight > 0 ? textMargin_ : 0;
        int rowHeight = modernCandidateRowHeight();
        rect.top = headerHeight + headerGap + row * (rowHeight + rowSpacing_);
        rect.right = rect.left + (selKeyWidth_ + textWidth_ + extraItemPadding);
        rect.bottom = rect.top + rowHeight;
    } else {
        rect.left = margin_ + col * (selKeyWidth_ + textWidth_ + colSpacing_);

        // measure header height
        int headerHeight = 0;
        if (!header_.empty()) {
            HDC hDC = ::GetWindowDC(hwnd());
            HGDIOBJ oldFont = ::SelectObject(hDC, font_);
            SIZE headerSize;
            ::GetTextExtentPoint32W(hDC, header_.c_str(), (int)header_.length(), &headerSize);
            ::SelectObject(hDC, oldFont);
            ::ReleaseDC(hwnd(), hDC);
            headerHeight = headerSize.cy + margin_;
        }

        rect.top = margin_ + headerHeight + row * (itemHeight_ + rowSpacing_);
        rect.right = rect.left + (selKeyWidth_ + textWidth_);
        rect.bottom = rect.top + itemHeight_;
    }
}


} // namespace Ime
