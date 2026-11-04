// input/native_keymap.cpp — OHOS KeyCode → evdev (阶段 1 N1)
// 逐项移植自 hgs/src/main/ets/common/KeyMap.ets 的 OH2EVDEV 表。
// 修改此表必须同步 KeyMap.ets (ArkTS 后端) — host test 固化黄金值。

#include "native_keymap.h"

namespace hgs {
namespace input {

int32_t OhosKeyCodeToEvdev(int32_t ohosKeyCode) {
    switch (ohosKeyCode) {
        // -- 数字行 --
        case 2000: return 11;   // KEY_0
        case 2001: return 2;
        case 2002: return 3;
        case 2003: return 4;
        case 2004: return 5;
        case 2005: return 6;
        case 2006: return 7;
        case 2007: return 8;
        case 2008: return 9;
        case 2009: return 10;

        // -- 方向键 --
        case 2012: return 103;  // UP
        case 2013: return 108;  // DOWN
        case 2014: return 105;  // LEFT
        case 2015: return 106;  // RIGHT

        // -- 字母 A-Z --
        case 2017: return 30;   // A
        case 2018: return 48;   // B
        case 2019: return 46;   // C
        case 2020: return 32;   // D
        case 2021: return 18;   // E
        case 2022: return 33;   // F
        case 2023: return 34;   // G
        case 2024: return 35;   // H
        case 2025: return 23;   // I
        case 2026: return 36;   // J
        case 2027: return 37;   // K
        case 2028: return 38;   // L
        case 2029: return 50;   // M
        case 2030: return 49;   // N
        case 2031: return 24;   // O
        case 2032: return 25;   // P
        case 2033: return 16;   // Q
        case 2034: return 19;   // R
        case 2035: return 31;   // S
        case 2036: return 20;   // T
        case 2037: return 22;   // U
        case 2038: return 47;   // V
        case 2039: return 17;   // W
        case 2040: return 45;   // X
        case 2041: return 21;   // Y
        case 2042: return 44;   // Z

        // -- 标点/符号 --
        case 2043: return 51;   // COMMA
        case 2044: return 52;   // PERIOD
        case 2056: return 41;   // GRAVE
        case 2057: return 12;   // MINUS
        case 2058: return 13;   // EQUAL
        case 2059: return 26;   // LEFT_BRACKET
        case 2060: return 27;   // RIGHT_BRACKET
        case 2061: return 43;   // BACKSLASH
        case 2062: return 39;   // SEMICOLON
        case 2063: return 40;   // APOSTROPHE
        case 2064: return 53;   // SLASH

        // -- 修饰键 --
        case 2045: return 56;   // LEFTALT
        case 2046: return 100;  // RIGHTALT
        case 2047: return 42;   // LEFTSHIFT
        case 2048: return 54;   // RIGHTSHIFT
        case 2072: return 29;   // LEFTCTRL
        case 2073: return 97;   // RIGHTCTRL
        case 2074: return 58;   // CAPSLOCK
        case 2076: return 125;  // LEFTMETA
        case 2077: return 126;  // RIGHTMETA

        // -- 常用键 --
        case 2049: return 15;   // TAB
        case 2050: return 57;   // SPACE
        case 2054: return 28;   // ENTER
        case 2070: return 1;    // ESC
        case 2055: return 14;   // BACKSPACE
        case 2071: return 111;  // DELETE
        case 2067: return 139;  // MENU

        // -- 导航键 --
        case 2068: return 104;  // PAGEUP
        case 2069: return 109;  // PAGEDOWN
        case 2081: return 102;  // HOME
        case 2082: return 107;  // END
        case 2083: return 110;  // INSERT
        case 2052: return 150;  // WWW
        case 2053: return 155;  // MAIL
        case 2084: return 159;  // FORWARD

        // -- F1-F12 --
        case 2090: return 59;
        case 2091: return 60;
        case 2092: return 61;
        case 2093: return 62;
        case 2094: return 63;
        case 2095: return 64;
        case 2096: return 65;
        case 2097: return 66;
        case 2098: return 67;
        case 2099: return 68;
        case 2100: return 87;   // F11
        case 2101: return 88;   // F12

        // -- 小键盘 --
        case 2102: return 69;   // NUMLOCK
        case 2103: return 82;   // KP0
        case 2104: return 79;   // KP1
        case 2105: return 80;   // KP2
        case 2106: return 81;   // KP3
        case 2107: return 75;   // KP4
        case 2108: return 76;   // KP5
        case 2109: return 77;   // KP6
        case 2110: return 71;   // KP7
        case 2111: return 72;   // KP8
        case 2112: return 73;   // KP9
        case 2113: return 98;   // KPSLASH
        case 2114: return 55;   // KPASTERISK
        case 2115: return 74;   // KPMINUS
        case 2116: return 78;   // KPPLUS
        case 2117: return 83;   // KPDOT
        case 2119: return 96;   // KPENTER

        // -- 媒体键 --
        case 2085: return 207;  // PLAY
        case 2086: return 119;  // PAUSE
        case 2087: return 128;  // STOP
        case 2089: return 167;  // RECORD

        // -- 系统键 --
        case 2051: return 99;   // SYSRQ

        default: return 0;
    }
}

bool IsModifierKeyCode(int32_t ohosKeyCode) {
    switch (ohosKeyCode) {
        case 2045: case 2046:   // ALT L/R
        case 2047: case 2048:   // SHIFT L/R
        case 2072: case 2073:   // CTRL L/R
        case 2076: case 2077:   // META L/R
        case 2074:              // CAPS_LOCK
            return true;
        default:
            return false;
    }
}

} // namespace input
} // namespace hgs
