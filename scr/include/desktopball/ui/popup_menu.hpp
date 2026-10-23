#pragma once
#include <windows.h>
#include <desktopball/ui/language.hpp>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace desktopball {
// Owner-drawn native menus retain Win32 keyboard navigation and submenu behavior.
class PopupMenu {
public:
    enum class Kind { action, header, section, separator };
    explicit PopupMenu(UINT dpi, Language language = Language::chinese);
    ~PopupMenu();
    PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;
    HMENU create();
    void add(HMENU menu, UINT id, std::wstring title, std::wstring detail = {},
        bool checked = false, bool enabled = true, HMENU submenu = nullptr,
        std::wstring value = {}, bool danger = false, Kind kind = Kind::action, bool strike = false, bool fullTitle = false);
    void separator(HMENU menu);
    UINT track(HMENU menu, HWND owner, POINT point,
        const std::function<bool(UINT)>& persistentAction = {}) const;
    void updateState(UINT id, bool checked, bool enabled);
    void updateValue(UINT id, std::wstring value);
    void redraw() const;
    // Forward owner messages here to decorate native popups after layout completes.
    static bool handleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    static bool measure(MEASUREITEMSTRUCT* item);
    static bool draw(DRAWITEMSTRUCT* item);
    // Render the same menu rows offscreen for deterministic visual inspection.
    static SIZE preview(HMENU menu, HDC target, POINT origin = {}, int selectedIndex = -1);
private:
    struct Row;
    int px(int value) const;
    UINT dpi_;
    Language language_;
    HFONT font_{}, small_{}, heading_{}, alternate_{}, strike_{};
    HBRUSH background_{};
    std::vector<HMENU> roots_;
    std::vector<std::unique_ptr<Row>> rows_;
};
}
