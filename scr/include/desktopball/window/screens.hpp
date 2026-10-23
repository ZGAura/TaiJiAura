#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace desktopball {
struct Screen {
    HMONITOR handle{};
    std::wstring id, device;
    RECT bounds{}, work{};
    bool primary{};
};
std::vector<Screen> enumerateScreens();
// Missing selections (including first launch) always fall back to the primary screen.
const Screen* resolveScreen(const std::vector<Screen>& screens, const std::wstring& id);
std::wstring screenTitle(const Screen& screen, size_t index);
// Keep the relative position within the usable desktop, including negative coordinates.
RECT transferScreenRect(RECT rect, RECT sourceWork, RECT targetWork, int width, int height);
}
