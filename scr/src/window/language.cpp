#include <desktopball/ui/language.hpp>
#include <utility>

namespace desktopball {
Language parseLanguage(std::wstring_view code) {
    return code==L"en-US" || code==L"en" ? Language::english : Language::chinese;
}
const wchar_t* languageCode(Language language) {
    return language==Language::english ? L"en-US" : L"zh-CN";
}
const wchar_t* menuFontFamily(Language language) {
    return language==Language::english ? L"Times New Roman" : L"SimSun";
}
std::wstring menuText(Language language, std::wstring_view text) {
    if (language==Language::chinese) return std::wstring(text);
    using Entry = std::pair<std::wstring_view,std::wstring_view>;
    static constexpr Entry entries[]{
        {L"桌面监控 · 轻量悬浮",L"Lightweight desktop monitor"},
        {L"TaiJiAura — 右键设置显示",L"TaiJiAura — Right-click for settings"},
        {L"显示设置",L"Display settings"},
        {L"显示内容",L"Metrics"},
        {L"最多四项；满四项后替换最后一项",L"Up to 4 metrics; new selections replace the last when full"},
        {L"程序已在运行。",L"TaiJiAura is already running."},
        {L"收起后显示",L"Collapsed metric"},
        {L"选择贴边收起时的监控指标",L"Choose the metric shown when collapsed"},
        {L"显示位置",L"Display position"},
        {L"未检测到显示屏",L"No displays detected"},
        {L"语言",L"Language"},
        {L"整体大小",L"Size"},
        {L"图案、文字与网速同步缩放",L"Scale graphics, text and network speeds together"},
        {L"默认",L"Default"},
        {L"显示轮廓",L"Show outline"},
        {L"仅展开胶囊：玻璃描边与内侧高光",L"Glass border and inner highlight for the expanded capsule"},
        {L"贴边自动收起",L"Auto-collapse at edges"},
        {L"靠近屏幕左 / 右边缘时收起",L"Collapse near the left or right edge"},
        {L"立即展开",L"Expand now"},
        {L"运行控制",L"Controls"},
        {L"渲染方式",L"Renderer"},
        {L"默认使用 CPU，可手动切换",L"CPU by default; switch manually"},
        {L"CPU 渲染",L"CPU rendering"},
        {L"GPU 渲染",L"GPU rendering"},
        {L"使用独显或集成显卡",L"Use a dedicated or integrated GPU"},
        {L"未检测到可用 GPU",L"No compatible GPU detected"},
        {L"胶囊帧率",L"Capsule frame rate"},
        {L"展开胶囊：手动选择，受屏幕刷新率限制",L"Expanded capsule; limited by the display refresh rate"},
        {L"烟流帧率",L"Smoke frame rate"},
        {L"仅收起后的竖向烟流；独立于胶囊帧率",L"Collapsed smoke only; independent of the capsule frame rate"},
        {L"开机自动启动",L"Start with Windows"},
        {L"登录当前 Windows 用户后启动",L"Start when the current Windows user signs in"},
        {L"继续动画",L"Resume animation"},
        {L"暂停动画",L"Pause animation"},
        {L"隐藏胶囊",L"Hide capsule"},
        {L"显示胶囊",L"Show capsule"},
        {L"退出 TaiJiAura",L"Exit TaiJiAura"},
        {L"CPU 占用率",L"CPU utilization"},
        {L"内存占用率",L"Memory utilization"},
        {L"PDH 平台热区温度",L"PDH platform thermal zone temperature"},
        {L"无法更新开机启动设置，错误代码：",L"Could not update startup settings. Error code: "}
    };
    for (const auto& [chinese,english] : entries) if (text==chinese) return std::wstring(english);
    const auto separator=text.find(L' ');
    if (separator!=std::wstring_view::npos) {
        const auto prefix=text.substr(0,separator);
        const bool gpuPrefix=prefix.starts_with(L"GPU") || prefix.starts_with(L"IGPU");
        if (gpuPrefix) {
            const auto suffix=text.substr(separator+1);
            constexpr Entry metrics[]{
                {L"占用率 · ",L"utilization · "},
                {L"显存占用率 · ",L"VRAM utilization · "},
                {L"温度 · ",L"temperature · "}};
            for (const auto& [chinese,english] : metrics) if (suffix.starts_with(chinese))
                return std::wstring(prefix)+L" "+std::wstring(english)+std::wstring(suffix.substr(chinese.size()));
        }
    }
    if (text.starts_with(L"显存 · ")) return L"VRAM · "+std::wstring(text.substr(5));
    if (text.starts_with(L"GPU 占用率 · ")) return L"GPU utilization · "+std::wstring(text.substr(10));
    if (text.starts_with(L"GPU 温度 · ")) return L"GPU temperature · "+std::wstring(text.substr(9));
    if (text.starts_with(L"显存占用率 · ")) return L"VRAM utilization · "+std::wstring(text.substr(8));
    if (text.starts_with(L"显示屏")) {
        auto suffix = text.substr(3);
        constexpr std::wstring_view primary = L"（主显示屏）";
        if (suffix.ends_with(primary)) return L"Display "+std::wstring(suffix.substr(0,suffix.size()-primary.size()))+L" (Primary)";
        return L"Display "+std::wstring(suffix);
    }
    if (text.ends_with(L" 项")) return std::wstring(text.substr(0,text.size()-2))+(text==L"1 项" ? L" item" : L" items");
    return std::wstring(text); // Hardware names and language names remain recognizable.
}
}
