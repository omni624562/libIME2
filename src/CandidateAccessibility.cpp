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

#include "CandidateAccessibility.h"
#include "ComObject.h"

#include <new>

// newer than some SDK headers' guards (Windows 10 1703+)
#ifndef UIA_PositionInSetPropertyId
#define UIA_PositionInSetPropertyId 30152
#endif
#ifndef UIA_SizeOfSetPropertyId
#define UIA_SizeOfSetPropertyId 30153
#endif

namespace Ime {

// AutomationId Narrator looks for on IME conversion candidate lists
static const wchar_t kCandidateListAutomationId[] = L"IME_Candidate_Window";

// ---------------------------------------------------------------------------
// UIAutomationCore.dll, resolved at run time

static const UiaFunctions* g_testFunctions = nullptr;
static UiaFunctions g_realFunctions = {};
static volatile LONG g_realFunctionsState = 0; // 0 = not tried, 1 = resolved, 2 = unavailable

static bool resolveFrom(HMODULE module, UiaFunctions* out) {
    out->returnRawElementProvider = reinterpret_cast<decltype(out->returnRawElementProvider)>(
        ::GetProcAddress(module, "UiaReturnRawElementProvider"));
    out->hostProviderFromHwnd = reinterpret_cast<decltype(out->hostProviderFromHwnd)>(
        ::GetProcAddress(module, "UiaHostProviderFromHwnd"));
    out->raiseAutomationEvent = reinterpret_cast<decltype(out->raiseAutomationEvent)>(
        ::GetProcAddress(module, "UiaRaiseAutomationEvent"));
    out->raiseStructureChangedEvent = reinterpret_cast<decltype(out->raiseStructureChangedEvent)>(
        ::GetProcAddress(module, "UiaRaiseStructureChangedEvent"));
    out->raisePropertyChangedEvent = reinterpret_cast<decltype(out->raisePropertyChangedEvent)>(
        ::GetProcAddress(module, "UiaRaiseAutomationPropertyChangedEvent"));
    out->clientsAreListening = reinterpret_cast<decltype(out->clientsAreListening)>(
        ::GetProcAddress(module, "UiaClientsAreListening"));
    out->disconnectProvider = reinterpret_cast<decltype(out->disconnectProvider)>(
        ::GetProcAddress(module, "UiaDisconnectProvider"));
    return out->returnRawElementProvider && out->hostProviderFromHwnd &&
        out->raiseAutomationEvent && out->raisePropertyChangedEvent &&
        out->clientsAreListening;
}

const UiaFunctions* uiaFunctions(bool load) {
    if (g_testFunctions)
        return g_testFunctions;
    LONG state = g_realFunctionsState;
    if (state == 1)
        return &g_realFunctions;
    if (state == 2)
        return nullptr;
    // Not loaded by anyone and not asked to load: stay out of the process.
    if (!load && !::GetModuleHandleW(L"UIAutomationCore.dll"))
        return nullptr;
    // Load (or pin, when already loaded) the system copy. The reference is
    // kept for the life of the process so the resolved pointers stay valid.
    wchar_t path[MAX_PATH];
    UINT len = ::GetSystemDirectoryW(path, MAX_PATH);
    static const wchar_t kDll[] = L"\\UIAutomationCore.dll";
    if (len == 0 || len + (sizeof(kDll) / sizeof(wchar_t)) > MAX_PATH)
        return nullptr;
    wcscpy_s(path + len, MAX_PATH - len, kDll);
    HMODULE module = ::LoadLibraryW(path);
    if (!module) {
        ::InterlockedExchange(&g_realFunctionsState, 2);
        return nullptr;
    }
    // TSF runs per thread; two UI threads may get here at once. Both resolve
    // the same addresses, so the race is harmless.
    UiaFunctions resolved = {};
    if (!resolveFrom(module, &resolved)) {
        ::InterlockedExchange(&g_realFunctionsState, 2);
        return nullptr;
    }
    g_realFunctions = resolved;
    ::InterlockedExchange(&g_realFunctionsState, 1);
    return &g_realFunctions;
}

void setUiaFunctionsForTesting(const UiaFunctions* functions) {
    g_testFunctions = functions;
}

// ---------------------------------------------------------------------------
// providers

static HRESULT setBstr(VARIANT* v, const std::wstring& s) {
    v->bstrVal = ::SysAllocStringLen(s.c_str(), static_cast<UINT>(s.length()));
    if (!v->bstrVal)
        return E_OUTOFMEMORY;
    v->vt = VT_BSTR;
    return S_OK;
}

static void setI4(VARIANT* v, int value) {
    v->vt = VT_I4;
    v->lVal = value;
}

static void setBool(VARIANT* v, bool value) {
    v->vt = VT_BOOL;
    v->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
}

static void toUiaRect(const RECT& rc, UiaRect* out) {
    out->left = rc.left;
    out->top = rc.top;
    out->width = rc.right - rc.left;
    out->height = rc.bottom - rc.top;
}

class CandidateListProvider:
    public ComObject<
        ComInterface<IRawElementProviderSimple>,
        ComInterface<IRawElementProviderFragment>,
        ComInterface<IRawElementProviderFragmentRoot>,
        ComInterface<ISelectionProvider>> {
public:
    explicit CandidateListProvider(CandidateAccessibility* owner): owner_(owner) {}

    CandidateAccessibility* owner() const { return owner_; }
    void detach() { owner_ = nullptr; }

    // IRawElementProviderSimple
    STDMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        // no ProviderOptions_UseComThreading: UIA calls us on the window's thread
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    STDMETHODIMP GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (patternId == UIA_SelectionPatternId) {
            *pRetVal = static_cast<ISelectionProvider*>(this);
            AddRef();
        }
        return S_OK;
    }

    STDMETHODIMP GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        ::VariantInit(pRetVal);
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        try {
            switch (propertyId) {
            case UIA_ControlTypePropertyId:
                setI4(pRetVal, UIA_ListControlTypeId);
                break;
            case UIA_AutomationIdPropertyId:
                return setBstr(pRetVal, kCandidateListAutomationId);
            case UIA_NamePropertyId:
                return setBstr(pRetVal, owner_->name());
            case UIA_IsKeyboardFocusablePropertyId:
            case UIA_HasKeyboardFocusPropertyId:
                // keyboard focus stays in the application's text field
                setBool(pRetVal, false);
                break;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId:
            case UIA_IsSelectionPatternAvailablePropertyId:
                setBool(pRetVal, true);
                break;
            case UIA_IsOffscreenPropertyId:
                setBool(pRetVal, !owner_->shown());
                break;
            default:
                break; // VT_EMPTY: UIA falls back to the host provider / defaults
            }
        }
        catch (...) {
            ::VariantClear(pRetVal);
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    STDMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        const UiaFunctions* uia = uiaFunctions(false);
        HWND hwnd = owner_->hwnd();
        if (!uia || !hwnd)
            return S_OK;
        return uia->hostProviderFromHwnd(hwnd, pRetVal);
    }

    // IRawElementProviderFragment
    STDMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        int n = owner_->count();
        int index = -1;
        if (direction == NavigateDirection_FirstChild && n > 0)
            index = 0;
        else if (direction == NavigateDirection_LastChild && n > 0)
            index = n - 1;
        // parent and siblings of the root come from the window (host provider)
        if (index < 0)
            return S_OK;
        return itemFragment(index, pRetVal);
    }

    STDMETHODIMP GetRuntimeId(SAFEARRAY** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr; // the hosting window provides it
        return owner_ ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP get_BoundingRectangle(UiaRect* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = UiaRect();
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        RECT rc;
        HWND hwnd = owner_->hwnd();
        if (hwnd && ::GetWindowRect(hwnd, &rc))
            toUiaRect(rc, pRetVal);
        return S_OK;
    }

    STDMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }

    STDMETHODIMP SetFocus() {
        // the candidate window never takes the keyboard focus
        return owner_ ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }

    // IRawElementProviderFragmentRoot
    STDMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        POINT pt = { static_cast<LONG>(x), static_cast<LONG>(y) };
        for (int i = 0, n = owner_->count(); i < n; ++i) {
            RECT rc;
            if (owner_->itemRect(i, &rc) && ::PtInRect(&rc, pt))
                return itemFragment(i, pRetVal);
        }
        *pRetVal = static_cast<IRawElementProviderFragment*>(this);
        AddRef();
        return S_OK;
    }

    STDMETHODIMP GetFocus(IRawElementProviderFragment** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr; // focus is in the application, not in the list
        return S_OK;
    }

    // ISelectionProvider
    STDMETHODIMP GetSelection(SAFEARRAY** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!owner_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        int sel = owner_->selection();
        bool hasSelection = sel >= 0 && sel < owner_->count();
        SAFEARRAY* array = ::SafeArrayCreateVector(VT_UNKNOWN, 0, hasSelection ? 1 : 0);
        if (!array)
            return E_OUTOFMEMORY;
        if (hasSelection) {
            IRawElementProviderSimple* item = owner_->createItemProvider(sel);
            if (!item) {
                ::SafeArrayDestroy(array);
                return E_OUTOFMEMORY;
            }
            LONG index = 0;
            HRESULT hr = ::SafeArrayPutElement(array, &index, item); // AddRefs
            item->Release();
            if (FAILED(hr)) {
                ::SafeArrayDestroy(array);
                return hr;
            }
        }
        *pRetVal = array;
        return S_OK;
    }

    STDMETHODIMP get_CanSelectMultiple(BOOL* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = FALSE;
        return S_OK;
    }

    STDMETHODIMP get_IsSelectionRequired(BOOL* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = FALSE;
        return S_OK;
    }

private:
    HRESULT itemFragment(int index, IRawElementProviderFragment** out) {
        IRawElementProviderSimple* item = owner_->createItemProvider(index);
        if (!item)
            return E_OUTOFMEMORY;
        HRESULT hr = item->QueryInterface(IID_PPV_ARGS(out));
        item->Release();
        return hr;
    }

    CandidateAccessibility* owner_;
};

class CandidateItemProvider:
    public ComObject<
        ComInterface<IRawElementProviderSimple>,
        ComInterface<IRawElementProviderFragment>,
        ComInterface<ISelectionItemProvider>> {
public:
    CandidateItemProvider(CandidateListProvider* root, int index, unsigned generation):
        root_(root), index_(index), generation_(generation) {
        root_->AddRef();
    }

    // IRawElementProviderSimple
    STDMETHODIMP get_ProviderOptions(ProviderOptions* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    STDMETHODIMP GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!alive())
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (patternId == UIA_SelectionItemPatternId) {
            *pRetVal = static_cast<ISelectionItemProvider*>(this);
            AddRef();
        }
        return S_OK;
    }

    STDMETHODIMP GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        ::VariantInit(pRetVal);
        CandidateAccessibility* owner = alive();
        if (!owner)
            return UIA_E_ELEMENTNOTAVAILABLE;
        try {
            switch (propertyId) {
            case UIA_ControlTypePropertyId:
                setI4(pRetVal, UIA_ListItemControlTypeId);
                break;
            case UIA_NamePropertyId:
                // the candidate itself (Microsoft's IME convention); the key
                // that picks it is the access key
                return setBstr(pRetVal, owner->itemText(index_));
            case UIA_AccessKeyPropertyId: {
                wchar_t key = owner->itemKey(index_);
                if (key)
                    return setBstr(pRetVal, std::wstring(1, key));
                break;
            }
            case UIA_AutomationIdPropertyId:
                return setBstr(pRetVal, L"IME_Candidate_" + std::to_wstring(index_ + 1));
            case UIA_PositionInSetPropertyId:
                setI4(pRetVal, index_ + 1);
                break;
            case UIA_SizeOfSetPropertyId:
                setI4(pRetVal, owner->count());
                break;
            case UIA_IsKeyboardFocusablePropertyId:
            case UIA_HasKeyboardFocusPropertyId:
                setBool(pRetVal, false);
                break;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId:
            case UIA_IsEnabledPropertyId:
            case UIA_IsSelectionItemPatternAvailablePropertyId:
                setBool(pRetVal, true);
                break;
            case UIA_IsOffscreenPropertyId:
                setBool(pRetVal, !owner->shown());
                break;
            case UIA_ProcessIdPropertyId:
                setI4(pRetVal, static_cast<int>(::GetCurrentProcessId()));
                break;
            default:
                break;
            }
        }
        catch (...) {
            ::VariantClear(pRetVal);
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    STDMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr; // not hosted in a window of its own
        return S_OK;
    }

    // IRawElementProviderFragment
    STDMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        CandidateAccessibility* owner = alive();
        if (!owner)
            return UIA_E_ELEMENTNOTAVAILABLE;
        int target = -1;
        switch (direction) {
        case NavigateDirection_Parent:
            *pRetVal = static_cast<IRawElementProviderFragment*>(root_);
            root_->AddRef();
            return S_OK;
        case NavigateDirection_NextSibling:
            target = index_ + 1 < owner->count() ? index_ + 1 : -1;
            break;
        case NavigateDirection_PreviousSibling:
            target = index_ - 1;
            break;
        default: // no children
            break;
        }
        if (target < 0)
            return S_OK;
        IRawElementProviderSimple* item = owner->createItemProvider(target);
        if (!item)
            return E_OUTOFMEMORY;
        HRESULT hr = item->QueryInterface(IID_PPV_ARGS(pRetVal));
        item->Release();
        return hr;
    }

    STDMETHODIMP GetRuntimeId(SAFEARRAY** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        if (!alive())
            return UIA_E_ELEMENTNOTAVAILABLE;
        int ids[] = { UiaAppendRuntimeId, static_cast<int>(generation_), index_ + 1 };
        SAFEARRAY* array = ::SafeArrayCreateVector(VT_I4, 0, 3);
        if (!array)
            return E_OUTOFMEMORY;
        for (LONG i = 0; i < 3; ++i) {
            HRESULT hr = ::SafeArrayPutElement(array, &i, &ids[i]);
            if (FAILED(hr)) {
                ::SafeArrayDestroy(array);
                return hr;
            }
        }
        *pRetVal = array;
        return S_OK;
    }

    STDMETHODIMP get_BoundingRectangle(UiaRect* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = UiaRect();
        CandidateAccessibility* owner = alive();
        if (!owner)
            return UIA_E_ELEMENTNOTAVAILABLE;
        RECT rc;
        if (owner->itemRect(index_, &rc))
            toUiaRect(rc, pRetVal);
        return S_OK;
    }

    STDMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = nullptr;
        return S_OK;
    }

    STDMETHODIMP SetFocus() {
        return alive() ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(root_);
        root_->AddRef();
        return S_OK;
    }

    // ISelectionItemProvider: the IME backend owns the selection; a client
    // cannot move it (the user picks with the selection keys / arrows).
    STDMETHODIMP Select() {
        return alive() ? UIA_E_INVALIDOPERATION : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP AddToSelection() {
        return alive() ? UIA_E_INVALIDOPERATION : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP RemoveFromSelection() {
        return alive() ? UIA_E_INVALIDOPERATION : UIA_E_ELEMENTNOTAVAILABLE;
    }

    STDMETHODIMP get_IsSelected(BOOL* pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = FALSE;
        CandidateAccessibility* owner = alive();
        if (!owner)
            return UIA_E_ELEMENTNOTAVAILABLE;
        *pRetVal = owner->selection() == index_ ? TRUE : FALSE;
        return S_OK;
    }

    STDMETHODIMP get_SelectionContainer(IRawElementProviderSimple** pRetVal) {
        if (!pRetVal)
            return E_INVALIDARG;
        *pRetVal = static_cast<IRawElementProviderSimple*>(root_);
        root_->AddRef();
        return S_OK;
    }

protected:
    ~CandidateItemProvider() {
        root_->Release();
    }

private:
    // the snapshot this item still belongs to, or null once the window is gone
    // or shows another list
    CandidateAccessibility* alive() const {
        CandidateAccessibility* owner = root_->owner();
        if (!owner || owner->generation() != generation_ || index_ >= owner->count())
            return nullptr;
        return owner;
    }

    CandidateListProvider* root_;
    int index_;
    unsigned generation_;
};

// ---------------------------------------------------------------------------
// CandidateAccessibility

CandidateAccessibility::CandidateAccessibility(CandidateAccessibleGeometry* geometry):
    geometry_(geometry),
    root_(nullptr),
    returnedToUia_(false),
    selection_(-1),
    shown_(false),
    generation_(1) {
}

CandidateAccessibility::~CandidateAccessibility() {
    disconnect();
}

HWND CandidateAccessibility::hwnd() const {
    return geometry_ ? geometry_->accessibleHwnd() : NULL;
}

bool CandidateAccessibility::itemRect(int index, RECT* screenRect) const {
    return geometry_ && index >= 0 && index < count() && geometry_->accessibleItemRect(index, screenRect);
}

IRawElementProviderSimple* CandidateAccessibility::rootProvider() {
    if (!geometry_)
        return nullptr;
    if (!root_)
        root_ = new (std::nothrow) CandidateListProvider(this);
    return root_;
}

IRawElementProviderSimple* CandidateAccessibility::createItemProvider(int index) {
    if (index < 0 || index >= count() || !rootProvider())
        return nullptr;
    // refcount starts at 1: the caller owns this reference
    return new (std::nothrow) CandidateItemProvider(root_, index, generation_);
}

bool CandidateAccessibility::handleGetObject(WPARAM wp, LPARAM lp, LRESULT* result) {
    // the object id is a 32-bit value; on 64-bit it may arrive zero-extended
    if (static_cast<LONG>(static_cast<DWORD>(lp)) != UiaRootObjectId)
        return false;
    const UiaFunctions* uia = uiaFunctions(true);
    HWND window = hwnd();
    IRawElementProviderSimple* root = rootProvider();
    if (!uia || !window || !root)
        return false;
    returnedToUia_ = true;
    *result = uia->returnRawElementProvider(window, wp, lp, root);
    return true;
}

bool CandidateAccessibility::eventsWanted() {
    const UiaFunctions* uia = uiaFunctions(false);
    if (!uia) {
        // UIA is not in this process yet. Only bring it in for a screen
        // reader; other clients (Inspect, Accessibility Insights) load it by
        // asking for the window's provider.
        BOOL screenReader = FALSE;
        if (!::SystemParametersInfoW(SPI_GETSCREENREADER, 0, &screenReader, 0) || !screenReader)
            return false;
        uia = uiaFunctions(true);
        if (!uia)
            return false;
    }
    return uia->clientsAreListening() != FALSE;
}

void CandidateAccessibility::raiseEvent(IRawElementProviderSimple* provider, EVENTID id) {
    const UiaFunctions* uia = uiaFunctions(false);
    if (uia && provider)
        uia->raiseAutomationEvent(provider, id);
}

void CandidateAccessibility::raiseItemSelected(int index) {
    IRawElementProviderSimple* item = createItemProvider(index);
    if (!item)
        return;
    raiseEvent(item, UIA_SelectionItem_ElementSelectedEventId);
    item->Release();
}

void CandidateAccessibility::sync(const std::vector<std::wstring>& items, const std::vector<wchar_t>& keys,
                                  int selection, const std::wstring& name, bool shown) {
    if (!geometry_)
        return;
    if (selection < 0 || selection >= static_cast<int>(items.size()))
        selection = -1;
    const bool itemsChanged = items != items_ || keys != keys_;
    const bool selectionChanged = selection != selection_;
    const bool nameChanged = name != name_;
    const bool wasShown = shown_;
    std::wstring oldName;

    try {
        if (itemsChanged) {
            items_ = items;
            keys_ = keys;
            ++generation_; // items handed out for the old list go stale
        }
        if (nameChanged) {
            oldName.swap(name_);
            name_ = name;
        }
    }
    catch (...) {
        // out of memory: keep a consistent (empty) snapshot, announce nothing
        items_.clear();
        keys_.clear();
        name_.clear();
        selection_ = -1;
        ++generation_;
        return;
    }
    selection_ = selection;
    shown_ = shown;

    if (!itemsChanged && !selectionChanged && !nameChanged && wasShown == shown)
        return;
    if (!eventsWanted() || !rootProvider())
        return;

    if (shown && !wasShown) {
        raiseEvent(root_, UIA_MenuOpenedEventId);
        if (selection_ >= 0)
            raiseItemSelected(selection_);
    }
    else if (!shown && wasShown) {
        raiseEvent(root_, UIA_MenuClosedEventId);
    }
    else if (shown) {
        if (nameChanged) {
            const UiaFunctions* uia = uiaFunctions(false);
            VARIANT oldValue, newValue;
            ::VariantInit(&oldValue);
            ::VariantInit(&newValue);
            if (uia && SUCCEEDED(setBstr(&oldValue, oldName)) && SUCCEEDED(setBstr(&newValue, name_)))
                uia->raisePropertyChangedEvent(root_, UIA_NamePropertyId, oldValue, newValue);
            ::VariantClear(&oldValue);
            ::VariantClear(&newValue);
        }
        // a new page / new candidates select an item of the new list
        if ((itemsChanged || selectionChanged) && selection_ >= 0)
            raiseItemSelected(selection_);
    }
}

void CandidateAccessibility::disconnect() {
    if (root_) {
        HWND window = hwnd();
        root_->detach(); // items and the root now report UIA_E_ELEMENTNOTAVAILABLE
        const UiaFunctions* uia = uiaFunctions(false);
        if (uia) {
            // documented clean-up for providers returned from WM_GETOBJECT
            if (returnedToUia_ && window)
                uia->returnRawElementProvider(window, 0, 0, nullptr);
            if (uia->disconnectProvider)
                uia->disconnectProvider(root_);
        }
        root_->Release();
        root_ = nullptr;
    }
    returnedToUia_ = false;
    geometry_ = nullptr;
}

} // namespace Ime
