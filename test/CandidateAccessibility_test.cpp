// Accessibility of the candidate window:
// - the UI Automation tree (list + items, names, selection) that Narrator reads,
//   checked through a recording stand-in for UIAutomationCore.dll so the events
//   raised on show / cursor move / new page / hide can be asserted;
// - the same tree through the real UI Automation client API (end to end:
//   WM_GETOBJECT, host provider, runtime ids);
// - ITfCandidateListUIElement, the TSF view of the same list (UI-less hosts).
//
// No TSF session, IME or visible window is needed.

#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "CandidateWindow.h"
#include "KeyEvent.h"

namespace {

struct ReleaseWindow {
    void operator()(Ime::CandidateWindow* window) const { window->Release(); }
};
using WindowPtr = std::unique_ptr<Ime::CandidateWindow, ReleaseWindow>;

template <typename T>
struct ComRelease {
    void operator()(T* p) const { if (p) p->Release(); }
};
template <typename T>
using Com = std::unique_ptr<T, ComRelease<T>>;

std::wstring bstrProperty(IRawElementProviderSimple* provider, PROPERTYID id, HRESULT* hrOut = nullptr) {
    VARIANT v;
    ::VariantInit(&v);
    HRESULT hr = provider->GetPropertyValue(id, &v);
    if (hrOut)
        *hrOut = hr;
    std::wstring result = (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal) ? std::wstring(v.bstrVal) : L"";
    ::VariantClear(&v);
    return result;
}

int intProperty(IRawElementProviderSimple* provider, PROPERTYID id) {
    VARIANT v;
    ::VariantInit(&v);
    int result = -1;
    if (SUCCEEDED(provider->GetPropertyValue(id, &v)) && v.vt == VT_I4)
        result = v.lVal;
    ::VariantClear(&v);
    return result;
}

bool isSelected(IRawElementProviderSimple* item) {
    IUnknown* pattern = nullptr;
    if (FAILED(item->GetPatternProvider(UIA_SelectionItemPatternId, &pattern)) || !pattern)
        return false;
    ISelectionItemProvider* selectionItem = nullptr;
    pattern->QueryInterface(IID_PPV_ARGS(&selectionItem));
    pattern->Release();
    BOOL selected = FALSE;
    if (selectionItem) {
        selectionItem->get_IsSelected(&selected);
        selectionItem->Release();
    }
    return selected != FALSE;
}

// --- recording stand-in for UIAutomationCore.dll ---------------------------

struct RaisedEvent {
    EVENTID id;
    std::wstring name;        // Name of the element at the time it was raised
    bool selected;            // its SelectionItem.IsSelected
};

struct Recorder {
    bool listening = true;
    std::vector<RaisedEvent> events;
    std::vector<std::wstring> nameChanges; // new values of Name property changes
    int returnedProviders = 0;
    int releasedForHwnd = 0;
    int disconnected = 0;
};
Recorder* g_recorder = nullptr;

LRESULT WINAPI fakeReturnRawElementProvider(HWND, WPARAM, LPARAM, IRawElementProviderSimple* provider) {
    if (provider)
        ++g_recorder->returnedProviders;
    else
        ++g_recorder->releasedForHwnd;
    return 4242;
}
HRESULT WINAPI fakeHostProviderFromHwnd(HWND, IRawElementProviderSimple** out) {
    *out = nullptr;
    return S_OK;
}
HRESULT WINAPI fakeRaiseAutomationEvent(IRawElementProviderSimple* provider, EVENTID id) {
    g_recorder->events.push_back({ id, bstrProperty(provider, UIA_NamePropertyId), isSelected(provider) });
    return S_OK;
}
HRESULT WINAPI fakeRaiseStructureChangedEvent(IRawElementProviderSimple*, StructureChangeType, int*, int) {
    return S_OK;
}
HRESULT WINAPI fakeRaisePropertyChangedEvent(IRawElementProviderSimple*, PROPERTYID id, VARIANT, VARIANT newValue) {
    if (id == UIA_NamePropertyId && newValue.vt == VT_BSTR)
        g_recorder->nameChanges.push_back(newValue.bstrVal);
    return S_OK;
}
BOOL WINAPI fakeClientsAreListening() {
    return g_recorder->listening;
}
HRESULT WINAPI fakeDisconnectProvider(IRawElementProviderSimple*) {
    ++g_recorder->disconnected;
    return S_OK;
}

const Ime::UiaFunctions kFakeUia = {
    fakeReturnRawElementProvider, fakeHostProviderFromHwnd, fakeRaiseAutomationEvent,
    fakeRaiseStructureChangedEvent, fakeRaisePropertyChangedEvent, fakeClientsAreListening,
    fakeDisconnectProvider,
};

class CandidateAccessibilityTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        Ime::Window::registerClass(::GetModuleHandleW(NULL));
    }

    void SetUp() override {
        g_recorder = &recorder_;
        Ime::setUiaFunctionsForTesting(&kFakeUia);
    }
    void TearDown() override {
        Ime::setUiaFunctionsForTesting(nullptr);
        g_recorder = nullptr;
    }

    static void fill(Ime::CandidateWindow& window, const std::vector<std::wstring>& items) {
        window.clear();
        window.setHeader(L"大易 人"); // 大易 人
        const wchar_t keys[] = L"1234567890";
        for (size_t i = 0; i < items.size(); ++i)
            window.add(items[i], keys[i % 10]);
        window.recalculateSize();
    }

    WindowPtr makeWindow(const std::vector<std::wstring>& items, int selection = 0) {
        WindowPtr window(new Ime::CandidateWindow(nullptr, nullptr));
        fill(*window, items);
        window->setCurrentSel(selection);
        return window;
    }

    static Com<IRawElementProviderSimple> child(IRawElementProviderSimple* root, NavigateDirection direction) {
        IRawElementProviderFragment* fragment = nullptr;
        root->QueryInterface(IID_PPV_ARGS(&fragment));
        IRawElementProviderFragment* next = nullptr;
        if (fragment) {
            fragment->Navigate(direction, &next);
            fragment->Release();
        }
        IRawElementProviderSimple* simple = nullptr;
        if (next) {
            next->QueryInterface(IID_PPV_ARGS(&simple));
            next->Release();
        }
        return Com<IRawElementProviderSimple>(simple);
    }

    static std::vector<Com<IRawElementProviderSimple>> children(IRawElementProviderSimple* root) {
        std::vector<Com<IRawElementProviderSimple>> result;
        Com<IRawElementProviderSimple> item = child(root, NavigateDirection_FirstChild);
        while (item) {
            Com<IRawElementProviderSimple> next = child(item.get(), NavigateDirection_NextSibling);
            result.push_back(std::move(item));
            item = std::move(next);
        }
        return result;
    }

    Recorder recorder_;
};

const std::vector<std::wstring> kPage1 = { L"人", L"入", L"八", L"乂", L"亼", L"仌" };
const std::vector<std::wstring> kPage2 = { L"大", L"太", L"犬" };

TEST_F(CandidateAccessibilityTest, ListFollowsMicrosoftImeConvention) {
    auto window = makeWindow(kPage1, 2);
    window->Show(TRUE);
    IRawElementProviderSimple* root = window->accessibility().rootProvider();
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(intProperty(root, UIA_ControlTypePropertyId), UIA_ListControlTypeId);
    EXPECT_EQ(bstrProperty(root, UIA_AutomationIdPropertyId), L"IME_Candidate_Window");
    EXPECT_EQ(bstrProperty(root, UIA_NamePropertyId), L"大易 人");

    auto items = children(root);
    ASSERT_EQ(items.size(), kPage1.size());
    for (size_t i = 0; i < items.size(); ++i) {
        EXPECT_EQ(intProperty(items[i].get(), UIA_ControlTypePropertyId), UIA_ListItemControlTypeId);
        EXPECT_EQ(bstrProperty(items[i].get(), UIA_NamePropertyId), kPage1[i]) << "item " << i;
        EXPECT_EQ(bstrProperty(items[i].get(), UIA_AccessKeyPropertyId), std::wstring(1, L"123456"[i]));
        EXPECT_EQ(intProperty(items[i].get(), 30152 /* PositionInSet */), (int)i + 1);
        EXPECT_EQ(intProperty(items[i].get(), 30153 /* SizeOfSet */), (int)kPage1.size());
        EXPECT_EQ(isSelected(items[i].get()), i == 2) << "item " << i;
    }
    // last child / previous sibling walk the same list backwards
    auto last = child(root, NavigateDirection_LastChild);
    ASSERT_TRUE(last);
    EXPECT_EQ(bstrProperty(last.get(), UIA_NamePropertyId), kPage1.back());
    auto beforeLast = child(last.get(), NavigateDirection_PreviousSibling);
    ASSERT_TRUE(beforeLast);
    EXPECT_EQ(bstrProperty(beforeLast.get(), UIA_NamePropertyId), kPage1[kPage1.size() - 2]);

    // Selection pattern on the list returns the selected candidate
    IUnknown* pattern = nullptr;
    ASSERT_EQ(root->GetPatternProvider(UIA_SelectionPatternId, &pattern), S_OK);
    ASSERT_NE(pattern, nullptr);
    Com<ISelectionProvider> selection;
    {
        ISelectionProvider* p = nullptr;
        pattern->QueryInterface(IID_PPV_ARGS(&p));
        pattern->Release();
        selection.reset(p);
    }
    ASSERT_TRUE(selection);
    SAFEARRAY* selected = nullptr;
    ASSERT_EQ(selection->GetSelection(&selected), S_OK);
    ASSERT_NE(selected, nullptr);
    LONG upper = -1;
    ::SafeArrayGetUBound(selected, 1, &upper);
    ASSERT_EQ(upper, 0);
    IUnknown* first = nullptr;
    LONG index = 0;
    ::SafeArrayGetElement(selected, &index, &first);
    ::SafeArrayDestroy(selected);
    ASSERT_NE(first, nullptr);
    IRawElementProviderSimple* firstSimple = nullptr;
    first->QueryInterface(IID_PPV_ARGS(&firstSimple));
    first->Release();
    ASSERT_NE(firstSimple, nullptr);
    EXPECT_EQ(bstrProperty(firstSimple, UIA_NamePropertyId), kPage1[2]);
    firstSimple->Release();
}

TEST_F(CandidateAccessibilityTest, EventsOnShowCursorMovePageChangeAndHide) {
    auto window = makeWindow(kPage1, 0);
    window->Show(TRUE);
    ASSERT_EQ(recorder_.events.size(), 2u);
    EXPECT_EQ(recorder_.events[0].id, UIA_MenuOpenedEventId);
    EXPECT_EQ(recorder_.events[1].id, UIA_SelectionItem_ElementSelectedEventId);
    EXPECT_EQ(recorder_.events[1].name, kPage1[0]);
    EXPECT_TRUE(recorder_.events[1].selected);

    // cursor moves (the backend's candidateCursor -> setCurrentSel + sync)
    recorder_.events.clear();
    window->setCurrentSel(3);
    window->syncAccessibility();
    ASSERT_EQ(recorder_.events.size(), 1u);
    EXPECT_EQ(recorder_.events[0].id, UIA_SelectionItem_ElementSelectedEventId);
    EXPECT_EQ(recorder_.events[0].name, kPage1[3]);
    EXPECT_TRUE(recorder_.events[0].selected);

    // the same list laid out again (大易 re-sends it to refresh the header):
    // nothing new to announce
    recorder_.events.clear();
    fill(*window, kPage1);
    window->setCurrentSel(3);
    window->syncAccessibility();
    EXPECT_TRUE(recorder_.events.empty());

    // next page: the selected item of the new list is announced, even though
    // the cursor stays at index 0 -> 0
    window->setCurrentSel(0);
    window->syncAccessibility();
    recorder_.events.clear();
    fill(*window, kPage2);
    window->setCurrentSel(0);
    window->syncAccessibility();
    ASSERT_EQ(recorder_.events.size(), 1u);
    EXPECT_EQ(recorder_.events[0].id, UIA_SelectionItem_ElementSelectedEventId);
    EXPECT_EQ(recorder_.events[0].name, kPage2[0]);

    // a backend message becomes the list's name
    recorder_.events.clear();
    window->setMessage(L"載入中");
    window->syncAccessibility();
    ASSERT_EQ(recorder_.nameChanges.size(), 1u);
    EXPECT_EQ(recorder_.nameChanges[0], L"載入中");

    // hidden
    recorder_.events.clear();
    window->Show(FALSE);
    ASSERT_EQ(recorder_.events.size(), 1u);
    EXPECT_EQ(recorder_.events[0].id, UIA_MenuClosedEventId);
}

TEST_F(CandidateAccessibilityTest, ArrowKeysInTheWindowAnnounceTheNewSelection) {
    auto window = makeWindow(kPage1, 0);
    window->setCandPerRow(6);
    window->Show(TRUE);
    recorder_.events.clear();
    Ime::KeyEvent right(WM_KEYDOWN, VK_RIGHT, 0);
    EXPECT_TRUE(window->filterKeyEvent(right));
    ASSERT_EQ(recorder_.events.size(), 1u);
    EXPECT_EQ(recorder_.events[0].name, kPage1[1]);
}

TEST_F(CandidateAccessibilityTest, NoEventsAndNoProviderWorkWithoutListeners) {
    recorder_.listening = false;
    auto window = makeWindow(kPage1, 0);
    window->Show(TRUE);
    window->setCurrentSel(2);
    window->syncAccessibility();
    fill(*window, kPage2);
    window->syncAccessibility();
    window->Show(FALSE);
    EXPECT_TRUE(recorder_.events.empty());
    EXPECT_TRUE(recorder_.nameChanges.empty());
    EXPECT_EQ(recorder_.returnedProviders, 0);
}

TEST_F(CandidateAccessibilityTest, ProviderIsHandedOutOnlyForUiaRootObjectId) {
    auto window = makeWindow(kPage1, 0);
    EXPECT_EQ(::SendMessageW(window->hwnd(), WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId)), 4242);
    EXPECT_EQ(recorder_.returnedProviders, 1);
    // MSAA requests still get the system's default proxy (DefWindowProc)
    ::SendMessageW(window->hwnd(), WM_GETOBJECT, 0, static_cast<LPARAM>(OBJID_CLIENT));
    EXPECT_EQ(recorder_.returnedProviders, 1);
}

TEST_F(CandidateAccessibilityTest, ElementsGoStaleWithTheirListAndWindow) {
    auto window = makeWindow(kPage1, 0);
    window->Show(TRUE);
    ::SendMessageW(window->hwnd(), WM_GETOBJECT, 0, static_cast<LPARAM>(UiaRootObjectId));
    IRawElementProviderSimple* root = window->accessibility().rootProvider();
    root->AddRef(); // a client keeps the list
    Com<IRawElementProviderSimple> keptRoot(root);
    auto oldItems = children(root);
    ASSERT_EQ(oldItems.size(), kPage1.size());

    // new page: items of the old one are no longer available (and their
    // runtime ids differ from the new ones)
    IRawElementProviderFragment* oldFragment = nullptr;
    oldItems[0]->QueryInterface(IID_PPV_ARGS(&oldFragment));
    SAFEARRAY* oldId = nullptr;
    ASSERT_EQ(oldFragment->GetRuntimeId(&oldId), S_OK);
    fill(*window, kPage2);
    window->syncAccessibility();
    HRESULT hr = S_OK;
    bstrProperty(oldItems[0].get(), UIA_NamePropertyId, &hr);
    EXPECT_EQ(hr, UIA_E_ELEMENTNOTAVAILABLE);
    auto newItems = children(root);
    ASSERT_EQ(newItems.size(), kPage2.size());
    IRawElementProviderFragment* newFragment = nullptr;
    newItems[0]->QueryInterface(IID_PPV_ARGS(&newFragment));
    SAFEARRAY* newId = nullptr;
    ASSERT_EQ(newFragment->GetRuntimeId(&newId), S_OK);
    int oldGeneration = 0, newGeneration = 0;
    LONG at = 1;
    ::SafeArrayGetElement(oldId, &at, &oldGeneration);
    ::SafeArrayGetElement(newId, &at, &newGeneration);
    EXPECT_NE(oldGeneration, newGeneration);
    int appendId = 0;
    at = 0;
    ::SafeArrayGetElement(newId, &at, &appendId);
    EXPECT_EQ(appendId, UiaAppendRuntimeId);
    ::SafeArrayDestroy(oldId);
    ::SafeArrayDestroy(newId);
    oldFragment->Release();
    newFragment->Release();

    // window destroyed while a client still holds the list and an item
    window.reset();
    EXPECT_EQ(recorder_.disconnected, 1);
    EXPECT_EQ(recorder_.releasedForHwnd, 1) << "UiaReturnRawElementProvider(hwnd, 0, 0, NULL) not called";
    bstrProperty(root, UIA_NamePropertyId, &hr);
    EXPECT_EQ(hr, UIA_E_ELEMENTNOTAVAILABLE);
    bstrProperty(newItems[0].get(), UIA_NamePropertyId, &hr);
    EXPECT_EQ(hr, UIA_E_ELEMENTNOTAVAILABLE);
    // the last references go away without touching the destroyed window
    newItems.clear();
    oldItems.clear();
    EXPECT_EQ(keptRoot.release()->Release(), 0u);
}

// --- ITfCandidateListUIElement ------------------------------------------------

TEST_F(CandidateAccessibilityTest, TsfCandidateListElementReportsTheWholePage) {
    std::vector<std::wstring> many;
    for (int i = 0; i < 12; ++i)
        many.push_back(std::wstring(1, static_cast<wchar_t>(0x4e00 + i)));
    auto window = makeWindow(many, 11);
    UINT count = 0;
    ASSERT_EQ(window->GetCount(&count), S_OK);
    EXPECT_EQ(count, 12u) << "used to be capped at 10";
    BSTR text = nullptr;
    ASSERT_EQ(window->GetString(11, &text), S_OK);
    EXPECT_EQ(std::wstring(text), many[11]);
    ::SysFreeString(text);
    EXPECT_EQ(window->GetString(12, &text), E_INVALIDARG);

    UINT sel = 99;
    EXPECT_EQ(window->GetSelection(&sel), S_OK);
    EXPECT_EQ(sel, 11u);
    window->setUseCursor(false);
    EXPECT_EQ(window->GetSelection(&sel), S_FALSE) << "no cursor = no selection";

    UINT pages = 0;
    EXPECT_EQ(window->GetPageIndex(nullptr, 0, &pages), S_OK);
    EXPECT_EQ(pages, 1u);
    UINT start[1] = { 7 };
    EXPECT_EQ(window->GetPageIndex(start, 1, &pages), S_OK);
    EXPECT_EQ(start[0], 0u);
    UINT current = 9;
    EXPECT_EQ(window->GetCurrentPage(&current), S_OK);
    EXPECT_EQ(current, 0u);
    UINT good[1] = { 0 };
    UINT bad[1] = { 3 };
    EXPECT_EQ(window->SetPageIndex(good, 1), S_OK);
    EXPECT_EQ(window->SetPageIndex(bad, 1), E_INVALIDARG);
}

TEST_F(CandidateAccessibilityTest, TsfUpdatedFlagsTrackChangesSinceLastUpdate) {
    auto window = makeWindow(kPage1, 0);
    DWORD flags = 0;
    window->GetUpdatedFlags(&flags);
    EXPECT_EQ(flags & (TF_CLUIE_STRING | TF_CLUIE_COUNT | TF_CLUIE_SELECTION),
              (DWORD)(TF_CLUIE_STRING | TF_CLUIE_COUNT | TF_CLUIE_SELECTION));
    window->resetUpdatedFlags();
    window->GetUpdatedFlags(&flags);
    EXPECT_EQ(flags, 0u);
    window->setCurrentSel(4);
    window->GetUpdatedFlags(&flags);
    EXPECT_EQ(flags, (DWORD)TF_CLUIE_SELECTION);
    window->resetUpdatedFlags();
    fill(*window, kPage2);
    window->GetUpdatedFlags(&flags);
    EXPECT_TRUE(flags & TF_CLUIE_STRING);
    EXPECT_TRUE(flags & TF_CLUIE_COUNT);
}

// --- end to end through the real UI Automation client ------------------------

// The client runs on another thread (as a screen reader runs in another
// process); this thread pumps messages so UIA can call the providers on the
// window's thread, as in the IME.
TEST(CandidateAccessibilityRealUiaTest, ClientSeesListItemsAndSelection) {
    Ime::Window::registerClass(::GetModuleHandleW(NULL));
    Ime::setUiaFunctionsForTesting(nullptr);
    WindowPtr window(new Ime::CandidateWindow(nullptr, nullptr));
    window->setHeader(L"大易 人");
    const wchar_t keys[] = L"123456";
    for (size_t i = 0; i < kPage1.size(); ++i)
        window->add(kPage1[i], keys[i]);
    window->recalculateSize();
    window->setCurrentSel(1);
    window->Show(TRUE);
    window->syncAccessibility();
    HWND hwnd = window->hwnd();

    struct Result {
        HRESULT hr = E_FAIL;
        std::wstring automationId, name;
        int controlType = 0;
        std::vector<std::wstring> itemNames;
        std::wstring selectedName;
    } result;
    std::atomic<bool> done(false);

    std::thread client([&]() {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation* uia = nullptr;
        result.hr = ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
        if (SUCCEEDED(result.hr)) {
            IUIAutomationElement* list = nullptr;
            result.hr = uia->ElementFromHandle(hwnd, &list);
            if (SUCCEEDED(result.hr) && list) {
                BSTR s = nullptr;
                if (SUCCEEDED(list->get_CurrentAutomationId(&s)) && s) { result.automationId = s; ::SysFreeString(s); }
                if (SUCCEEDED(list->get_CurrentName(&s)) && s) { result.name = s; ::SysFreeString(s); }
                list->get_CurrentControlType(&result.controlType);
                IUIAutomationCondition* all = nullptr;
                uia->CreateTrueCondition(&all);
                IUIAutomationElementArray* items = nullptr;
                if (all && SUCCEEDED(list->FindAll(TreeScope_Children, all, &items)) && items) {
                    int n = 0;
                    items->get_Length(&n);
                    for (int i = 0; i < n; ++i) {
                        IUIAutomationElement* item = nullptr;
                        if (SUCCEEDED(items->GetElement(i, &item)) && item) {
                            if (SUCCEEDED(item->get_CurrentName(&s)) && s) { result.itemNames.push_back(s); ::SysFreeString(s); }
                            item->Release();
                        }
                    }
                    items->Release();
                }
                if (all)
                    all->Release();
                IUIAutomationSelectionPattern* selection = nullptr;
                if (SUCCEEDED(list->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&selection))) && selection) {
                    IUIAutomationElementArray* selected = nullptr;
                    if (SUCCEEDED(selection->GetCurrentSelection(&selected)) && selected) {
                        IUIAutomationElement* item = nullptr;
                        if (SUCCEEDED(selected->GetElement(0, &item)) && item) {
                            if (SUCCEEDED(item->get_CurrentName(&s)) && s) { result.selectedName = s; ::SysFreeString(s); }
                            item->Release();
                        }
                        selected->Release();
                    }
                    selection->Release();
                }
                list->Release();
            }
            uia->Release();
        }
        ::CoUninitialize();
        done = true;
    });

    // pump until the client is done (bounded)
    for (int i = 0; i < 1000 && !done; ++i) {
        MSG msg;
        while (::PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    client.join();

    ASSERT_EQ(result.hr, S_OK);
    EXPECT_EQ(result.automationId, L"IME_Candidate_Window");
    EXPECT_EQ(result.controlType, UIA_ListControlTypeId);
    EXPECT_EQ(result.name, L"大易 人");
    EXPECT_EQ(result.itemNames, kPage1);
    EXPECT_EQ(result.selectedName, kPage1[1]);
    window->Show(FALSE);
}

}  // namespace
