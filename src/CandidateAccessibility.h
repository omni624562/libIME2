//
//    Copyright (C) 2026 WIME contributors
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

// UI Automation for the candidate window, following Microsoft's convention for
// IME candidate lists, which Narrator relies on
// (https://learn.microsoft.com/windows/apps/develop/input/input-method-editor-requirements#ime-accessibility):
// - a list whose AutomationId is "IME_Candidate_Window", one list item per
//   candidate, Name = the candidate text;
// - UIA_MenuOpenedEventId / UIA_MenuClosedEventId when the list appears / goes away;
// - UIA_SelectionItem_ElementSelectedEventId when the selected candidate changes,
//   the selected item reporting SelectionItem.IsSelected = TRUE.
//
// Cost when no assistive technology runs: nothing is created until a client
// asks for the window's provider (WM_GETOBJECT), UIAutomationCore.dll is not
// loaded by us unless a screen reader is running or a client already asked,
// and events are only raised while UiaClientsAreListening().

#ifndef IME_CANDIDATE_ACCESSIBILITY_H
#define IME_CANDIDATE_ACCESSIBILITY_H

#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <string>
#include <vector>

namespace Ime {

class CandidateListProvider;

// The UIAutomationCore.dll entry points the candidate window uses, resolved at
// run time so the IME DLL does not import UIAutomationCore.dll into every
// process that has a text field. Tests can substitute their own table.
struct UiaFunctions {
    LRESULT (WINAPI* returnRawElementProvider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
    HRESULT (WINAPI* hostProviderFromHwnd)(HWND, IRawElementProviderSimple**);
    HRESULT (WINAPI* raiseAutomationEvent)(IRawElementProviderSimple*, EVENTID);
    HRESULT (WINAPI* raiseStructureChangedEvent)(IRawElementProviderSimple*, StructureChangeType, int*, int);
    HRESULT (WINAPI* raisePropertyChangedEvent)(IRawElementProviderSimple*, PROPERTYID, VARIANT, VARIANT);
    BOOL (WINAPI* clientsAreListening)();
    HRESULT (WINAPI* disconnectProvider)(IRawElementProviderSimple*);
};

// The table, or null if UIAutomationCore.dll is unavailable. With load=false it
// is only returned once something (a WM_GETOBJECT, a running screen reader)
// already caused it to be loaded.
const UiaFunctions* uiaFunctions(bool load);
// Test hook: replaces the table (null restores the real one).
void setUiaFunctionsForTesting(const UiaFunctions* functions);

// Where the candidate items are on screen; implemented by CandidateWindow.
class CandidateAccessibleGeometry {
public:
    virtual ~CandidateAccessibleGeometry() {}
    virtual HWND accessibleHwnd() = 0;
    // screen coordinates of item i; false if it has no area
    virtual bool accessibleItemRect(int index, RECT* screenRect) = 0;
};

// Snapshot of what the window shows, as announced to assistive technology, and
// the root UIA provider (created on demand). Owned by CandidateWindow and only
// used on its thread (UIA calls hwnd-hosted providers on the window's thread).
class CandidateAccessibility {
public:
    explicit CandidateAccessibility(CandidateAccessibleGeometry* geometry);
    ~CandidateAccessibility();

    // WM_GETOBJECT: answers UiaRootObjectId with the provider. Returns false
    // (message not handled) for other object ids.
    bool handleGetObject(WPARAM wp, LPARAM lp, LRESULT* result);

    // Called when the window content, selection or visibility may have changed.
    // Updates the snapshot (cheap: a few short strings) and raises the UIA
    // events for what changed, only if UIA clients are listening.
    // selection is -1 when no candidate is selected.
    void sync(const std::vector<std::wstring>& items, const std::vector<wchar_t>& keys,
              int selection, const std::wstring& name, bool shown);

    // The window is going away: providers handed out to clients now report
    // UIA_E_ELEMENTNOTAVAILABLE, and UIA is told to drop them.
    void disconnect();

    // snapshot, read by the providers
    int count() const { return static_cast<int>(items_.size()); }
    const std::wstring& itemText(int i) const { return items_[i]; }
    wchar_t itemKey(int i) const { return i < static_cast<int>(keys_.size()) ? keys_[i] : 0; }
    int selection() const { return selection_; }
    const std::wstring& name() const { return name_; }
    bool shown() const { return shown_; }
    // bumped whenever the list of candidates changes (it is part of the items'
    // runtime ids, so a client never confuses a candidate with the one that
    // was at the same position on the previous page)
    unsigned generation() const { return generation_; }
    bool connected() const { return geometry_ != nullptr; }
    HWND hwnd() const;
    bool itemRect(int index, RECT* screenRect) const;

    // the root provider (borrowed reference), created on first use; null
    // after disconnect()
    IRawElementProviderSimple* rootProvider();
    // a provider for item index of the current generation (new reference), or null
    IRawElementProviderSimple* createItemProvider(int index);

    // Whether events would be raised now: UIA is loaded (by a client asking, or
    // because a screen reader runs) and a client listens.
    static bool eventsWanted();

private:
    void raiseEvent(IRawElementProviderSimple* provider, EVENTID id);
    void raiseItemSelected(int index);

    CandidateAccessibleGeometry* geometry_;
    CandidateListProvider* root_;
    bool returnedToUia_;
    std::vector<std::wstring> items_;
    std::vector<wchar_t> keys_;
    int selection_;
    std::wstring name_;
    bool shown_;
    unsigned generation_;
};

} // namespace Ime

#endif
