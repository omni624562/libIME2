#pragma once

// A C++ exception must not leave a COM method: TSF calls these methods on the
// thread of the application the user is typing in, and an exception that
// unwinds into it ends that application. Every STDMETHODIMP entry point wraps
// its body in these two macros; an exception answers E_FAIL, and whatever the
// method did before the throw stays done (see EditSession::DoEditSession).
#define IME_COM_GUARD_BEGIN try {
#define IME_COM_GUARD_END } catch (...) { return E_FAIL; }
