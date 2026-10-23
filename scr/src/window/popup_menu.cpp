#include <desktopball/ui/popup_menu.hpp>
#include <algorithm>
#include <stdexcept>
#include <dwmapi.h>
#include <exception>

namespace desktopball {
namespace {
constexpr COLORREF background = RGB(245,245,245), hover = RGB(225,225,225);
constexpr COLORREF primary = RGB(48,48,48), muted = RGB(108,108,108);
constexpr COLORREF accent = RGB(76,76,76), line = RGB(196,196,196);
constexpr COLORREF glassEdge = RGB(172,172,172), glassHighlight = RGB(255,255,255);
constexpr BYTE popupOpacity=240;
thread_local UINT popupDpi{};
thread_local bool roundingPopup{};
thread_local HWND popupOwner{};
thread_local UINT_PTR popupSequence{};
thread_local const std::function<bool(UINT)>* popupAction{};
thread_local HMENU activeMenu{};
thread_local UINT highlightedCommand{};
thread_local bool suppressMouseRelease{};
thread_local std::exception_ptr popupError;
constexpr UINT stylePopupMessage=WM_APP+2;
LRESULT CALLBACK persistentMenu(int code, WPARAM wparam, LPARAM lparam) {
    if (code==MSGF_MENU && popupAction && *popupAction) {
        const auto& message = *reinterpret_cast<const MSG*>(lparam);
        if (message.message==WM_LBUTTONUP && suppressMouseRelease) {
            suppressMouseRelease=false; return TRUE;
        }
        const bool keyboard = message.message==WM_KEYDOWN && (message.wParam==VK_RETURN || message.wParam==VK_SPACE);
        const bool mouse = message.message==WM_LBUTTONDOWN || message.message==WM_LBUTTONUP || message.message==WM_LBUTTONDBLCLK;
        if (keyboard || mouse) {
            UINT command = keyboard ? highlightedCommand : 0;
            if (mouse && activeMenu) {
                const int position = MenuItemFromPoint(popupOwner,activeMenu,message.pt);
                if (position>=0) {
                    const UINT state = GetMenuState(activeMenu,position,MF_BYPOSITION);
                    if (!(state & (MF_DISABLED | MF_GRAYED | MF_POPUP))) command = GetMenuItemID(activeMenu,position);
                }
            }
            try {
                if (command && (*popupAction)(command)) {
                    if (mouse) suppressMouseRelease = message.message!=WM_LBUTTONUP;
                    return TRUE;
                }
            } catch (...) { popupError=std::current_exception(); EndMenu(); return TRUE; }
        }
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
LRESULT CALLBACK roundPopup(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && popupOwner && !roundingPopup) {
        const auto& message=*reinterpret_cast<const CWPSTRUCT*>(lparam);
        const bool positioned=message.message == WM_WINDOWPOSCHANGED;
        if (positioned || (message.message == WM_SHOWWINDOW && message.wParam)) {
            wchar_t name[32]{}; GetClassNameW(message.hwnd,name,32);
            if (wcscmp(name,L"#32768")==0) {
                // Hooks run before native popup setup finishes. Queue the decoration
                // so changing the region/style cannot re-enter submenu initialization.
                PostMessageW(popupOwner,stylePopupMessage,popupSequence,reinterpret_cast<LPARAM>(message.hwnd));
            }
        }
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
void fill(HDC dc, RECT rect, COLORREF color) {
    const auto brush = CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush);
}
}
bool PopupMenu::handleMessage(UINT message, WPARAM sequence, LPARAM target) {
    if (message==WM_INITMENUPOPUP) {
        activeMenu=reinterpret_cast<HMENU>(sequence); highlightedCommand=0;
    } else if (message==WM_MENUSELECT) {
        activeMenu=reinterpret_cast<HMENU>(target);
        highlightedCommand = HIWORD(sequence)!=0xffff && !(HIWORD(sequence) & (MF_POPUP | MF_DISABLED | MF_GRAYED)) ? LOWORD(sequence) : 0;
    }
    if (message!=stylePopupMessage) return false;
    const auto window=reinterpret_cast<HWND>(target);
    if (!popupOwner || sequence!=popupSequence || !IsWindowVisible(window) ||
        GetWindowThreadProcessId(window,nullptr)!=GetCurrentThreadId()) return true;
    wchar_t name[32]{}; GetClassNameW(window,name,32);
    if (wcscmp(name,L"#32768")!=0) return true;
    RECT rect{}; GetWindowRect(window,&rect);
    if (rect.right<=rect.left || rect.bottom<=rect.top) return true;
    roundingPopup=true;
    const int corner=MulDiv(12,static_cast<int>(popupDpi),96);
    auto region=CreateRoundRectRgn(0,0,rect.right-rect.left+1,rect.bottom-rect.top+1,corner*2,corner*2);
    if (!SetWindowRgn(window,region,TRUE)) DeleteObject(region);
    const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(window,GWL_EXSTYLE,style | WS_EX_LAYERED);
    SetLayeredWindowAttributes(window,0,popupOpacity,LWA_ALPHA);
    const DWORD preference=2; const BOOL dark=FALSE;
    DwmSetWindowAttribute(window,33,&preference,sizeof(preference));
    DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
    DwmSetWindowAttribute(window,34,&line,sizeof(line));
    // Layered composition needs fresh content after the native layout has settled.
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    roundingPopup=false;
    return true;
}
struct PopupMenu::Row {
    const PopupMenu* style;
    HMENU menu;
    UINT id;
    std::wstring title, detail, value;
    Kind kind;
    bool checked, enabled, submenu, danger;
    HFONT font{};
    int width{}, height{}, top{};
};
int PopupMenu::px(int value) const { return MulDiv(value,static_cast<int>(dpi_),96); }
PopupMenu::PopupMenu(UINT dpi, Language language) : dpi_(dpi), language_(language) {
    auto font = [&](int size, int weight, const wchar_t* family) {
        return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
    };
    const auto family = menuFontFamily(language_);
    font_ = font(10,FW_NORMAL,family); small_ = font(8,FW_NORMAL,family); heading_ = font(11,FW_SEMIBOLD,family);
    alternate_ = font(10,FW_NORMAL,menuFontFamily(language_==Language::chinese ? Language::english : Language::chinese));
    LOGFONTW struck{}; GetObjectW(font_,sizeof(struck),&struck); struck.lfStrikeOut=TRUE;
    strike_=CreateFontIndirectW(&struck);
    background_ = CreateSolidBrush(background);
}
PopupMenu::~PopupMenu() {
    for (auto menu : roots_) DestroyMenu(menu);
    DeleteObject(font_); DeleteObject(small_); DeleteObject(heading_); DeleteObject(alternate_); DeleteObject(strike_); DeleteObject(background_);
}
HMENU PopupMenu::create() {
    HMENU menu = CreatePopupMenu();
    if (!menu) throw std::runtime_error("Could not create settings menu");
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND | MIM_STYLE; info.hbrBack = background_;
    info.dwStyle = MNS_NOCHECK; SetMenuInfo(menu,&info);
    roots_.push_back(menu);
    return menu;
}
void PopupMenu::add(HMENU menu, UINT id, std::wstring title, std::wstring detail,
    bool checked, bool enabled, HMENU submenu, std::wstring value, bool danger, Kind kind, bool strike, bool fullTitle) {
    title = menuText(language_,title); detail = menuText(language_,detail); value = menuText(language_,value);
    auto row = std::make_unique<Row>(Row{this,menu,id,std::move(title),std::move(detail),std::move(value),
        kind,checked,enabled,submenu != nullptr,danger});
    row->font = ((row->title==L"English" && language_==Language::chinese) ||
        (row->title==L"中文" && language_==Language::english)) ? alternate_ : font_;
    if (strike) row->font=strike_;
    row->height = px(kind == Kind::separator ? 4 : kind == Kind::section ? 13 :
        kind == Kind::header ? 24 : 20);
    for (int i=0;i<GetMenuItemCount(menu);++i) {
        MENUITEMINFOW previous{sizeof(previous)}; previous.fMask=MIIM_DATA;
        if (GetMenuItemInfoW(menu,i,TRUE,&previous) && previous.dwItemData)
            row->top+=reinterpret_cast<const Row*>(previous.dwItemData)->height;
    }
    HDC dc = GetDC(nullptr);
    const auto old = SelectObject(dc,row->font);
    SIZE titleSize{}, valueSize{}, detailSize{};
    GetTextExtentPoint32W(dc,row->title.c_str(),static_cast<int>(row->title.size()),&titleSize);
    GetTextExtentPoint32W(dc,row->value.c_str(),static_cast<int>(row->value.size()),&valueSize);
    SelectObject(dc,small_);
    GetTextExtentPoint32W(dc,row->detail.c_str(),static_cast<int>(row->detail.size()),&detailSize);
    SelectObject(dc,old); ReleaseDC(nullptr,dc);
    row->width = std::clamp(static_cast<int>(titleSize.cx+valueSize.cx)+px(40),px(138),px(fullTitle ? 500 : 200));
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STRING | MIIM_STATE;
    item.fType = MFT_OWNERDRAW; item.wID = id;
    item.dwItemData = reinterpret_cast<ULONG_PTR>(row.get()); item.dwTypeData = row->title.data();
    item.fState = enabled && kind == Kind::action ? MFS_ENABLED : MFS_DISABLED;
    if (checked) item.fState |= MFS_CHECKED;
    if (submenu) { item.fMask |= MIIM_SUBMENU; item.hSubMenu = submenu; }
    if (!InsertMenuItemW(menu,static_cast<UINT>(-1),TRUE,&item)) throw std::runtime_error("Could not add settings menu row");
    if (submenu) roots_.erase(std::remove(roots_.begin(),roots_.end(),submenu),roots_.end());
    rows_.push_back(std::move(row));
}
void PopupMenu::separator(HMENU menu) { add(menu,0,L"",{},false,false,nullptr,{},false,Kind::separator); }
void PopupMenu::updateState(UINT id, bool checked, bool enabled) {
    for (auto& row : rows_) if (row->id==id) {
        row->checked=checked; row->enabled=enabled;
        MENUITEMINFOW state{sizeof(state)}; state.fMask=MIIM_STATE;
        const UINT existing=GetMenuState(row->menu,id,MF_BYCOMMAND);
        state.fState=(existing & MF_HILITE) | (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
        if (!SetMenuItemInfoW(row->menu,id,FALSE,&state)) throw std::runtime_error("Could not update menu selection");
    }
}
void PopupMenu::updateValue(UINT id, std::wstring value) {
    for (auto& row : rows_) if (row->id==id) row->value=menuText(language_,value);
}
void PopupMenu::redraw() const {
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
        wchar_t name[32]{}; GetClassNameW(window,name,32);
        if (IsWindowVisible(window) && wcscmp(name,L"#32768")==0)
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return TRUE;
    },0);
}
UINT PopupMenu::track(HMENU menu, HWND owner, POINT point, const std::function<bool(UINT)>& persistentAction) const {
    popupDpi=dpi_;
    popupOwner=owner; ++popupSequence;
    popupAction=&persistentAction; activeMenu=menu; highlightedCommand=0; suppressMouseRelease=false; popupError=nullptr;
    const HHOOK hook=SetWindowsHookExW(WH_CALLWNDPROC,roundPopup,nullptr,GetCurrentThreadId());
    const HHOOK inputHook=persistentAction ? SetWindowsHookExW(WH_MSGFILTER,persistentMenu,nullptr,GetCurrentThreadId()) : nullptr;
    if (persistentAction && !inputHook) {
        if (hook) UnhookWindowsHookEx(hook);
        popupAction=nullptr; popupOwner=nullptr; popupDpi=0;
        throw std::runtime_error("Could not enable continuous metric selection");
    }
    // The native fade animation overwrites per-window alpha; keep our fixed glass opacity.
    const UINT selected=TrackPopupMenu(menu,TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NOANIMATION,point.x,point.y,0,owner,nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    if (inputHook) UnhookWindowsHookEx(inputHook);
    popupDpi=0;
    popupOwner=nullptr;
    popupAction=nullptr; activeMenu=nullptr;
    if (popupError) { auto error=popupError; popupError=nullptr; std::rethrow_exception(error); }
    return selected;
}
bool PopupMenu::measure(MEASUREITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_MENU || !item->itemData) return false;
    const auto& row = *reinterpret_cast<const Row*>(item->itemData);
    item->itemWidth = row.width; item->itemHeight = row.height; return true;
}
bool PopupMenu::draw(DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_MENU || !item->itemData) return false;
    const auto& row = *reinterpret_cast<const Row*>(item->itemData);
    const auto& s = *row.style; HDC dc = item->hDC; const RECT rect = item->rcItem;
    const int saved = SaveDC(dc); SetBkMode(dc,TRANSPARENT);
    // Clip every row against the same rounded shell, including hover backgrounds.
    MENUITEMINFOW last{sizeof(last)}; last.fMask=MIIM_DATA;
    const int count=GetMenuItemCount(row.menu);
    const Row* lastRow=&row;
    if (count>0 && GetMenuItemInfoW(row.menu,count-1,TRUE,&last) && last.dwItemData)
        lastRow=reinterpret_cast<const Row*>(last.dwItemData);
    const LONG menuTop=rect.top-row.top;
    const RECT shell{rect.left,menuTop,rect.right,menuTop+lastRow->top+lastRow->height};
    IntersectClipRect(dc,rect.left,rect.top,rect.right,rect.bottom);
    auto shellRegion=CreateRoundRectRgn(shell.left,shell.top,shell.right+1,shell.bottom+1,s.px(24),s.px(24));
    ExtSelectClipRgn(dc,shellRegion,RGN_AND); DeleteObject(shellRegion);
    auto glassFrame = [&] {
        const auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        auto outer=CreatePen(PS_SOLID,std::max(1,s.px(1)),glassEdge);
        const auto oldPen=SelectObject(dc,outer);
        RoundRect(dc,shell.left,shell.top,shell.right,shell.bottom,s.px(24),s.px(24));
        auto inner=CreatePen(PS_SOLID,std::max(1,s.px(1)),glassHighlight);
        SelectObject(dc,inner);
        const int inset=std::max(1,s.px(1));
        RoundRect(dc,shell.left+inset,shell.top+inset,shell.right-inset,shell.bottom-inset,s.px(22),s.px(22));
        SelectObject(dc,oldPen); SelectObject(dc,oldBrush);
        DeleteObject(outer); DeleteObject(inner);
    };
    // A continuous white-to-gray wash runs down the entire menu.
    for (int y=0;y<rect.bottom-rect.top;y+=2) {
        const float shade=std::clamp(static_cast<float>(row.top+y)/s.px(240),0.0f,1.0f);
        const int gray=static_cast<int>(251-15*shade);
        const auto color=RGB(gray,gray,gray);
        fill(dc,{rect.left,rect.top+y,rect.right,std::min(rect.top+y+2,rect.bottom)},color);
    }
    if (row.kind == Kind::header) {
        RECT glint{rect.left+s.px(10),rect.top+s.px(1),rect.right-s.px(10),rect.top+s.px(2)};
        fill(dc,glint,RGB(255,255,255));
    }
    auto box = [&](int l,int t,int r,int b) { return RECT{rect.left+s.px(l),rect.top+s.px(t),rect.right-s.px(r),rect.bottom-s.px(b)}; };
    if (row.kind == Kind::separator) {
        RECT rule = box(7,2,7,1); rule.bottom = rule.top+std::max(1,s.px(1)); fill(dc,rule,line);
        glassFrame(); RestoreDC(dc,saved); return true;
    }
    const bool selected = row.enabled && row.kind == Kind::action && (item->itemState & ODS_SELECTED);
    if (selected) {
        RECT active = box(2,1,2,1);
        auto brush = CreateSolidBrush(row.danger ? RGB(242,225,225) : hover);
        const auto oldBrush=SelectObject(dc,brush), oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
        RoundRect(dc,active.left,active.top,active.right,active.bottom,s.px(12),s.px(12));
        SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(brush);
    }
    const bool header = row.kind == Kind::header, section = row.kind == Kind::section;
    auto ink = row.danger ? RGB(148,62,62) : row.enabled || header ? primary : muted;
    if (!row.enabled && !header && !section) ink = RGB(148,148,148);
    SetTextColor(dc,section ? muted : ink);
    SelectObject(dc,header ? s.heading_ : section ? s.small_ : row.font);
    RECT title = box(header || section ? 8 : 18,0,18,0);
    if (!row.value.empty()) {
        SelectObject(dc,s.small_); SIZE valueSize{};
        GetTextExtentPoint32W(dc,row.value.c_str(),static_cast<int>(row.value.size()),&valueSize);
        title.right-=valueSize.cx+s.px(8);
        SelectObject(dc,header ? s.heading_ : section ? s.small_ : row.font);
    }
    DrawTextW(dc,row.title.c_str(),-1,&title,DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!row.value.empty()) {
        SelectObject(dc,s.small_); SetTextColor(dc,accent);
        RECT value = box(18,0,row.submenu ? 20 : 8,0);
        DrawTextW(dc,row.value.c_str(),-1,&value,DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    if (row.checked) {
        const int x=rect.left+s.px(9), y=(rect.top+rect.bottom)/2;
        auto pen = CreatePen(PS_SOLID,std::max(1,s.px(1)),accent); SelectObject(dc,pen);
        MoveToEx(dc,x-s.px(3),y,nullptr); LineTo(dc,x-s.px(1),y+s.px(2)); LineTo(dc,x+s.px(3),y-s.px(3));
        SelectObject(dc,GetStockObject(BLACK_PEN)); DeleteObject(pen);
    }
    if (row.submenu) {
        const int x=rect.right-s.px(8), y=(rect.top+rect.bottom)/2;
        auto pen=CreatePen(PS_SOLID,std::max(1,s.px(1)),muted); SelectObject(dc,pen);
        MoveToEx(dc,x-s.px(2),y-s.px(4),nullptr); LineTo(dc,x+s.px(2),y); LineTo(dc,x-s.px(2),y+s.px(4));
        SelectObject(dc,GetStockObject(BLACK_PEN)); DeleteObject(pen);
    }
    glassFrame(); RestoreDC(dc,saved); return true;
}
SIZE PopupMenu::preview(HMENU menu, HDC target, POINT origin, int selectedIndex) {
    SIZE size{};
    const int count=GetMenuItemCount(menu);
    for (int i=0;i<count;++i) {
        MENUITEMINFOW info{sizeof(info)}; info.fMask=MIIM_DATA;
        GetMenuItemInfoW(menu,i,TRUE,&info);
        const auto& row=*reinterpret_cast<const Row*>(info.dwItemData);
        size.cx=std::max(size.cx,static_cast<LONG>(row.width)); size.cy+=row.height;
    }
    if (!count) return size;
    int y=origin.y;
    MENUITEMINFOW first{sizeof(first)}; first.fMask=MIIM_DATA; GetMenuItemInfoW(menu,0,TRUE,&first);
    const auto& style=*reinterpret_cast<const Row*>(first.dwItemData)->style;
    const int saved=SaveDC(target);
    auto outline=CreateRoundRectRgn(origin.x,origin.y,origin.x+size.cx+1,origin.y+size.cy+1,style.px(24),style.px(24));
    ExtSelectClipRgn(target,outline,RGN_AND); DeleteObject(outline);
    for (int i=0;i<count;++i) {
        MENUITEMINFOW info{sizeof(info)}; info.fMask=MIIM_DATA; GetMenuItemInfoW(menu,i,TRUE,&info);
        const auto& row=*reinterpret_cast<const Row*>(info.dwItemData);
        DRAWITEMSTRUCT item{}; item.CtlType=ODT_MENU; item.hDC=target; item.itemData=info.dwItemData;
        item.itemState=i == selectedIndex ? ODS_SELECTED : 0;
        item.rcItem={origin.x,y,origin.x+size.cx,y+row.height}; draw(&item); y+=row.height;
    }
    RestoreDC(target,saved);
    return size;
}
}
