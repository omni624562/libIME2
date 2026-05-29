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
#include <vector>
#include "ComObject.h"

namespace Ime {

class TextService;
class EditSession;
class KeyEvent;

// TODO: make the candidate window looks different in immersive mode
class CandidateWindow:
    public ImeWindow,
    public ComObject<ComInterface<ITfCandidateListUIElement>> {
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
        message_ = message;
        recalculateSize();
        refresh();
    }

    void setItems(const std::vector<std::wstring>& items, const std::vector<wchar_t>& selKeys) {
        items_ = items;
        selKeys_ = selKeys;
        recalculateSize();
        refresh();
    }

    void add(std::wstring item, wchar_t selKey) {
        items_.push_back(item);
        selKeys_.push_back(selKey);
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
        header_ = header;
        recalculateSize();
        refresh();
    }

    const std::wstring& pageInfo() const {
        return pageInfo_;
    }

    void setPageInfo(const std::wstring& info) {
        pageInfo_ = info;
        recalculateSize();
        refresh();
    }

    void setModernStyle(bool modern) {
        modernStyle_ = modern;
        recalculateSize();
        refresh();
    }

    void setTheme(COLORREF panelBg, COLORREF panelBorder, COLORREF textPrimary, COLORREF textSecondary, COLORREF highlightBg, COLORREF highlightBorder, COLORREF highlightText) {
        panelBg_ = panelBg;
        panelBorder_ = panelBorder;
        textPrimary_ = textPrimary;
        textSecondary_ = textSecondary;
        highlightBg_ = highlightBg;
        highlightBorder_ = highlightBorder;
        highlightText_ = highlightText;
        refresh();
    }

    void setSpacing(int contentMargin, int textMargin, int borderRadius) {
        contentMargin_ = contentMargin;
        textMargin_ = textMargin;
        borderRadius_ = borderRadius;
        recalculateSize();
        refresh();
    }

    void setStableWidth(bool stable, int minWidth);
    void resetStableWidth();
    void setMaxWidth(bool wrapToMaxWidth, int maxWidth);

protected:
    LRESULT wndProc(UINT msg, WPARAM wp , LPARAM lp);
    void onPaint(WPARAM wp, LPARAM lp);
    void paintItem(HDC hDC, int i, int x, int y);
    void itemRect(int i, RECT& rect);
    int headerHeight(HDC hDC) const;
    int modernCandidateRowHeight() const;

protected: // COM object should not be deleted directly. calling Release() instead.
    ~CandidateWindow(void);

private:
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

    bool modernStyle_;
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
    bool stableWidth_;
    int minStableWidth_;
    int stableWidthPx_;
    bool wrapToMaxWidth_;
    int maxWidth_;
};

}

#endif
