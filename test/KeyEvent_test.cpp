// Unit tests for KeyEvent's accessors of the WM_KEYDOWN/WM_KEYUP lParam fields.
//
// scanCode() used to mask bits 16-23 without shifting them down, so the cast to
// unsigned char always gave 0: the backend could never tell left Shift (0x2A)
// from right Shift (0x36).

#include <windows.h>
#include <gtest/gtest.h>

#include "KeyEvent.h"

namespace {

LPARAM makeLParam(unsigned repeat, unsigned scanCode, bool extended, bool keyUp) {
    LPARAM lp = (repeat & 0xffff) | ((scanCode & 0xff) << 16);
    if (extended)
        lp |= (1 << 24);
    if (keyUp)
        lp |= (LPARAM)(1u << 30) | (LPARAM)(1u << 31);
    return lp;
}

TEST(KeyEventTest, ScanCodeOfLeftAndRightShift) {
    Ime::KeyEvent left(WM_KEYDOWN, VK_SHIFT, makeLParam(1, 0x2A, false, false));
    Ime::KeyEvent right(WM_KEYUP, VK_SHIFT, makeLParam(1, 0x36, false, true));
    EXPECT_EQ(0x2A, left.scanCode());
    EXPECT_EQ(0x36, right.scanCode());
}

TEST(KeyEventTest, OtherLParamFields) {
    Ime::KeyEvent ev(WM_KEYDOWN, VK_RIGHT, makeLParam(3, 0x4D, true, false));
    EXPECT_EQ(3, ev.repeatCount());
    EXPECT_EQ(0x4D, ev.scanCode());
    EXPECT_TRUE(ev.isExtended());

    Ime::KeyEvent plain(WM_KEYDOWN, 'A', makeLParam(1, 0x1E, false, false));
    EXPECT_EQ(0x1E, plain.scanCode());
    EXPECT_FALSE(plain.isExtended());
}

TEST(KeyEventTest, HighScanCodeBitsDoNotLeakIntoOtherFields) {
    // every scan code value survives the round trip and does not change repeatCount
    for (unsigned scan = 0; scan <= 0xff; ++scan) {
        Ime::KeyEvent ev(WM_KEYDOWN, 'Q', makeLParam(1, scan, false, false));
        EXPECT_EQ(scan, ev.scanCode());
        EXPECT_EQ(1, ev.repeatCount());
    }
}

}  // namespace
