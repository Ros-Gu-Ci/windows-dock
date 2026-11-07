// dock/src/main.cpp
// ---------------------------------------------------------------------------
// Windows Dock —— v1.12
//   原生 C++20 / Win32 / Direct2D / DirectComposition
//
//   * 四边停靠：底部 / 顶部 / 左侧 / 右侧，自适应任意分辨率与任务栏
//   * 材质：毛玻璃(Acrylic) / 透明 / 深色实心 / 浅色实心
//   * 托盘图标 + 右键菜单（位置、外观、图标大小、开机自启、刷新、退出）
//   * 九宫格设置面板 + 日期时间信息块
//   * 增删：拖放追加、右键移除、末尾「＋」添加
//   * 文件夹二级栏：把游戏/工具归到一个图标里
//   * 内置模块（时间 / 九宫格 / ＋）可单独隐藏
//   * 真实图标（IShellItemImageFactory）+ 悬停弹簧放大
//   * 便携配置（exe 同目录）；空闲 0 帧
// ---------------------------------------------------------------------------

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <uiautomation.h>

#include <wrl/client.h>

#include <gdiplus.h>

#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"DockWindowClass";
constexpr wchar_t kPopupClassName[] = L"DockPopupWindow";
constexpr wchar_t kMutexName[] = L"Local\\Dock.SingleInstance";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Dock";

constexpr int kPadX = 14;
constexpr int kPadY = 12;
constexpr int kGap = 10;
constexpr int kMargin = 8;
// edgeOffset 的允许上限：再大就会让"贴边"的语义失效，见 README 里的说明
constexpr int kEdgeOffsetMax = 200;
constexpr UINT_PTR kAnimTimerId = 1;
constexpr UINT_PTR kFullscreenTimerId = 2;
constexpr UINT_PTR kSmartTimerId = 3;
constexpr UINT_PTR kClockTimerId = 4;
constexpr UINT_PTR kPopupWatchTimerId = 5;
constexpr UINT_PTR kSystemTrayTimerId = 6;
constexpr UINT_PTR kRevealTimerId = 7;
constexpr UINT_PTR kGroupWatchTimerId = 8;
constexpr UINT_PTR kGroupAnimTimerId = 9;          // 文件夹面板入场/退场动画
constexpr UINT_PTR kPopupAnimTimerId = 10;         // 九宫格面板入场/退场动画
constexpr UINT kAnimIntervalMs = 15;
constexpr UINT kTrayMsg = WM_APP + 10;
constexpr UINT kFullscreenCheckMsg = WM_APP + 11;
constexpr UINT kFullscreenPollMs = 1000;
constexpr UINT kSmartPollMs = 700;
constexpr UINT kSmartRevealPollMs = 150;
constexpr UINT kClockPollMs = 15000;
constexpr UINT kPopupWatchMs = 80;
constexpr UINT kSystemTrayPollMs = 220;
constexpr UINT kRevealPollMs = 100;     // 边缘热区检测频率（只在收起状态下运行）
constexpr DWORD kRevealDwellMs = 450;   // 需在边缘停稳多久才浮出，避免误触
constexpr DWORD kRevealLeaveMs = 350;   // 鼠标离开后多久重新收起
constexpr int kHotEdgePx = 1;           // 热区厚度：只有真正顶到屏幕边缘才算
constexpr int kRevealStillPx = 3;       // 停留期间允许的抖动范围
constexpr DWORD kRevealCooldownMs = 800;  // 边缘处按下鼠标后，这段时间内不唤出
constexpr DWORD kManualRevealMs = 3000;   // 遮挡中手动唤出后，3 秒无操作自动收起

// 九宫格面板（物理像素会在绘制时乘以 DPI 缩放）
constexpr float kPopupTileW = 86.0f;
constexpr float kPopupTileH = 82.0f;
constexpr float kPopupGap = 8.0f;
constexpr float kPopupPad = 14.0f;
constexpr float kPopupHeader = 46.0f;
constexpr float kPopupShadow = 14.0f;

constexpr float kHoverScale = 1.28f;
constexpr float kSpringK = 170.0f;
constexpr float kSpringD = 21.0f;

enum : UINT {
    kCmdToggle = 1,
    kCmdMatAcrylic = 10,
    kCmdMatTransparent = 11,
    kCmdMatDark = 12,
    kCmdMatLight = 13,
    kCmdSizeSmall = 20,
    kCmdSizeMedium = 21,
    kCmdSizeLarge = 22,
    kCmdAutoStart = 30,
    kCmdAdd = 40,
    kCmdOpenFolder = 41,
    kCmdRefreshIcons = 42,
    kCmdReflection = 43,
    kCmdTaskbar = 44,
    kCmdHideFullscreen = 45,
    kCmdSmartHide = 46,
    kCmdAddFolder = 47,
    kCmdOpenTray = 48,
    kCmdEdgeReveal = 49,
    kCmdAlwaysHide = 51,
    kCmdExit = 50,
    kCmdEdgeBottom = 60,
    kCmdEdgeTop = 61,
    kCmdEdgeLeft = 62,
    kCmdEdgeRight = 63,
    kCmdShowClock = 70,
    kCmdShowCenter = 71,
    kCmdShowAdd = 72,
    // 时序 / 边缘留白：档位命令（区间编号，从 300 起，避开 100 起的“移除第 N 项”）
    kCmdDwellBase = 300,     // 80 / 150 / 300 / 450
    kCmdRetractBase = 310,   // 300 / 500 / 750 / 1000 / 1250 / 1500
    kCmdOffsetBase = 320,    // 0 / 8 / 16 / 20 / 24 / 28 / 32 / 36 / 48 / 64
    kCmdHotEdgeGrow = 330,
    // 悬停标签：背景黑白二选一、不透明度档位、文字描边
    kCmdLabelBgBase = 340,       // 0 黑 / 1 白
    kCmdLabelAlphaBase = 350,    // 0 / 20 / 40 / 60 / 80 / 100 %
    kCmdLabelAlphaCustom = 356,  // 自定义…（弹输入框）
    kCmdLabelOutline = 360,
    kCmdRemoveBase = 100,
};

// 九宫格面板内部动作，与托盘菜单命令分开，避免和“移除第 N 项”的编号空间冲突
enum : int {
    kPopupQuickSettings = 0,
    kPopupTray,
    kPopupAdd,
    kPopupMaterial,
    kPopupSize,
    kPopupEdge,
    kPopupTaskbar,
    kPopupSmartHide,
    kPopupFullscreen,
    kPopupCount,
};

struct __declspec(uuid("bcc18b79-ba16-442f-80c4-8a59c30c463b")) IShellItemImageFactory2
    : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetImage(SIZE size, int flags, HBITMAP* phbm) = 0;
};

struct ACCENT_POLICY_ {
    int accentState;
    int accentFlags;
    int gradientColor;
    int animationId;
};
struct WINDOWCOMPOSITIONATTRIBDATA_ {
    int attrib;
    void* pvData;
    size_t cbData;
};
enum { WCA_ACCENT_POLICY_ = 19 };
enum { ACCENT_DISABLED_ = 0, ACCENT_ENABLE_BLURBEHIND_ = 3, ACCENT_ENABLE_ACRYLICBLURBEHIND_ = 4 };

struct Settings {
    int material = 0;   // 0 毛玻璃 1 透明 2 深色实心 3 浅色实心
    int iconSize = 44;
    int edge = 0;       // 0 底 1 顶 2 左 3 右
    bool autoStart = false;
    bool reflection = true;  // 图标倒影
    bool hideFullscreen = true;  // 全屏应用时自动隐藏
    bool smartHide = true;       // 被窗口挡住时自动收起
    bool alwaysHide = false;     // 常隐：没有遮挡也收起，只有贴边停稳才浮出
    bool showClock = true;       // 日期时间块
    bool showCenter = true;      // 九宫格设置
    bool showAdd = true;         // 「＋」添加位
    bool edgeReveal = true;      // 自动收起后，鼠标顶到屏幕边缘可唤出
    int  revealDwellMs = (int)kRevealDwellMs;   // 边缘停稳多久才浮出（默认 450）
    int  retractMs     = (int)kManualRevealMs;  // 浮出后无操作多久收回（默认 3000）
    int  edgeOffset    = 0;                     // 距屏幕边缘的额外留白（像素），0 = 贴边
    bool hotEdgeGrow   = false;                 // 触发热区是否延伸到 Dock 上沿（填满空档）

    int  labelBg       = 0;                     // 悬停标签背景：0 黑 1 白
    int  labelAlphaPct = 90;                    // 悬停标签背景不透明度 0..100（默认接近原观感 232/255）
    bool labelOutline  = false;                 // 悬停标签文字描边：低不透明度时保证可读
};

struct ItemAnim {
    float scale = 1.0f;
    float vel = 0.0f;
};

struct DockState {
    HWND hwnd = nullptr;
    std::vector<std::wstring> items;
    std::vector<ComPtr<ID2D1Bitmap1>> icons;
    std::vector<ComPtr<ID2D1Bitmap1>> reflections;   // 预渲染的倒影（含渐隐 alpha）
    float opacity = 1.0f;                            // 整体淡入 / 淡出
    float opacityTarget = 1.0f;
    std::vector<ItemAnim> anims;

    int hover = -1;
    int pressIndex = -1;    // 鼠标按下时的槽位（用于拖动排序）
    bool dragging = false;  // 是否已超过拖动阈值
    bool suppressClick = false;  // 捕获丢失后的抬起事件不再触发启动
    POINT pressPt{};
    float dpiScale = 1.0f;
    bool animating = false;
    bool hidden = false;

    UINT width = 240;
    UINT height = 80;
    int availLen = 0;   // 沿停靠方向的可用长度（物理像素），用于自动收缩防止溢出屏幕

    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> targetBitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1RoundedRectangleGeometry> panelGeo;   // 面板圆角裁剪用
    ComPtr<ID2D1Layer> panelLayer;
    float panelGeoW = 0.0f;
    float panelGeoH = 0.0f;
    float panelGeoR = 0.0f;
    ComPtr<IDCompositionDevice> compDevice;
    ComPtr<IDCompositionTarget> compTarget;
    ComPtr<IDCompositionVisual> compVisual;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> clockTime;
    ComPtr<IDWriteTextFormat> clockDate;

    bool taskbarHidden = false;
    bool fsHidden = false;       // 因全屏应用自动隐藏
    bool fsSuppressed = false;   // 本次全屏里用户手动唤出过，期内不再自动隐藏
    HWND fsWnd = nullptr;        // 触发隐藏的那个全屏窗口
    bool fsTimerOn = false;
    HWINEVENTHOOK fsHook = nullptr;

    // 智能收起：桌面有窗口压住 Dock 时自动淡出，只在边缘留一条热区
    bool autoHidden = false;
    bool smartTimerOn = false;
    bool hoverRevealed = false;   // 因鼠标靠近而临时浮出（遮挡仍在）
    DWORD revealHideAt = 0;       // 临时浮出的自动收起时间点
    bool menuOpen = false;        // 右键菜单打开期间不自动收起
    bool winShown = true;         // 收起动画结束后彻底 SW_HIDE，桌面不留残影
    float slide = 0.0f;           // 0=完全显示, 1=完全收起（收起滑出动画）
    RECT baseRect{};              // 未收起时的窗口矩形
    bool workAreaSaved = false;
    RECT savedWorkArea{};

    // 系统托盘面板：隐藏任务栏时临时借出任务栏，面板收起后自动收回
    bool trayPeek = false;
    bool trayTimerOn = false;
    bool trayOverflowSeen = false;
    DWORD trayPeekStart = 0;
    HWND trayOverflow = nullptr;

    // 边缘唤出：只有鼠标停在 Dock 朝屏幕一侧的窄条上并停留一小段时间才浮出。
    bool revealTimerOn = false;
    bool edgeArmed = false;
    DWORD edgeEnterTick = 0;
    DWORD revealLeaveTick = 0;
    POINT edgeAnchorPt{};         // 停留判定的起始点，移动超过阈值就重新计时
    DWORD revealCooldownUntil = 0;

    bool panelsShown = false;
    int popupHover = -1;

    NOTIFYICONDATAW nid{};
    HICON trayIcon = nullptr;
    UINT taskbarCreatedMsg = 0;

    // 日期时间插槽：只在分钟变化时刷新一次
    int clockMinute = -1;
    int clockDay = -1;
    bool clockTimerOn = false;
    int clockFormatSize = -1;

    // 布局缓存：悬停动画每 15ms 重绘，避免每帧重复跑 LayoutMetrics
    int cacheBox = -1;
    int cacheGap = -1;
    int cacheRefl = -1;
    int cacheAvail = -1;
    int cacheIconSize = -1;
    int cacheSlotCount = -1;
    int cacheEdge = -1;
    bool cacheReflection = false;
    bool cacheShowClock = true;
    bool cacheShowCenter = true;
    bool cacheShowAdd = true;
    float cacheDpi = 0.0f;
};

// 文件夹二级栏：一个 dock 图标里收纳多个游戏 / 工具
struct GroupState {
    HWND hwnd = nullptr;
    bool shown = false;
    std::wstring root;                  // 当前浏览路径
    std::wstring title;
    std::vector<std::wstring> paths;
    std::vector<bool> isDir;
    std::vector<ComPtr<ID2D1Bitmap1>> icons;
    std::vector<std::wstring> parents;  // 导航栈
    int hover = -1;
    int scrollRow = 0;
    UINT w = 0;
    UINT h = 0;
    bool watchOn = false;
    bool prevMouseDown = false;
    bool prevEscapeDown = false;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> opacityFx;   // DComp 的透明度要通过效果组设置
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteTextFormat> titleFmt;
    ComPtr<IDWriteTextFormat> labelFmt;
    ComPtr<IDWriteTextFormat> captionFmt;
    ComPtr<ID2D1StrokeStyle> stroke;
    float formatDpi = 0.0f;
};

// 文件夹面板的过渡动画：只改 DComp 视觉对象的透明度/缩放，不重绘内容
struct GroupAnimState {
    bool running = false;
    bool closing = false;
    bool timerOn = false;
    float t = 0.0f;
};
GroupAnimState gGroupAnim;
GroupAnimState gPopupAnim;      // 九宫格面板复用同一套状态结构

struct PopupState {
    HWND hwnd = nullptr;
    bool shown = false;
    int hover = -1;
    UINT w = 0;
    UINT h = 0;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionEffectGroup> opacityFx;   // 九宫格面板的透明度效果组
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> caption;
    ComPtr<IDWriteTextFormat> label;
    ComPtr<ID2D1StrokeStyle> roundStroke;
    bool watchOn = false;
    bool prevMouseDown = false;
    bool prevEscapeDown = false;
    float opacity = 1.0f;
    float opacityTarget = 1.0f;
    bool animTimerOn = false;
    float formatDpi = 0.0f;

};

DockState g;
PopupState gPopup;
GroupState gGroup;
Settings s;
bool gDebug = false;
// ------------------------------------------------------------------- misc ---
std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    const size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring(L".") : p.substr(0, cut);
}

void Dbg(const wchar_t* fmt, ...) {
    if (!gDebug) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    const std::wstring path = ExeDir() + L"\\dock-debug.log";
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return;

    const int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8;
    if (need > 1) {
        utf8.resize(static_cast<size_t>(need - 1));
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), need, nullptr, nullptr);
    }
    out << utf8 << "\n";
}


std::wstring Utf8ToWide(const std::string& in) {
    if (in.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()), out.data(), need);
    return out;
}

std::string WideToUtf8(const std::wstring& in) {
    if (in.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.c_str(), static_cast<int>(in.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L'\\');
    std::wstring name = (cut == std::wstring::npos) ? path : path.substr(cut + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

bool IsDirectoryPath(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 二级栏里只列可启动的项目，避免被 dll / 配置 / 缓存文件淹没。
bool IsLaunchableFile(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = name.substr(dot);
    return _wcsicmp(ext.c_str(), L".exe") == 0 || _wcsicmp(ext.c_str(), L".lnk") == 0 ||
           _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0 ||
           _wcsicmp(ext.c_str(), L".url") == 0 || _wcsicmp(ext.c_str(), L".appref-ms") == 0;
}

// ----------------------------------------------------------------- config ---
std::wstring ItemsPath() { return ExeDir() + L"\\dock-items.txt"; }
std::wstring SettingsPath() { return ExeDir() + L"\\dock-settings.ini"; }

void LoadItems() {
    std::ifstream in(ItemsPath(), std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        g.items.push_back(Utf8ToWide(line));
    }
}

void SaveItems() {
    std::ofstream out(ItemsPath(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    for (const auto& item : g.items) out << WideToUtf8(item) << "\n";
}

void LoadSettings() {
    std::ifstream in(SettingsPath(), std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "material") s.material = atoi(val.c_str());
        else if (key == "iconSize") s.iconSize = atoi(val.c_str());
        else if (key == "edge") s.edge = atoi(val.c_str());
        else if (key == "autoStart") s.autoStart = (val == "1");
        else if (key == "reflection") s.reflection = (val == "1");
        else if (key == "hideFullscreen") s.hideFullscreen = (val == "1");
        else if (key == "smartHide") s.smartHide = (val == "1");
        else if (key == "alwaysHide") s.alwaysHide = (val == "1");
        else if (key == "showClock") s.showClock = (val == "1");
        else if (key == "showCenter") s.showCenter = (val == "1");
        else if (key == "showAdd") s.showAdd = (val == "1");
        else if (key == "edgeReveal") s.edgeReveal = (val == "1");
        else if (key == "revealDwellMs") s.revealDwellMs = atoi(val.c_str());
        else if (key == "retractMs") s.retractMs = atoi(val.c_str());
        else if (key == "edgeOffset") s.edgeOffset = atoi(val.c_str());
        else if (key == "hotEdgeGrow") s.hotEdgeGrow = (val == "1");
        else if (key == "labelBg") s.labelBg = atoi(val.c_str());
        else if (key == "labelAlphaPct") s.labelAlphaPct = atoi(val.c_str());
        else if (key == "labelOutline") s.labelOutline = (val == "1");
    }
    if (s.material < 0 || s.material > 3) s.material = 0;
    if (s.iconSize < 28 || s.iconSize > 72) s.iconSize = 44;
    if (s.edge < 0 || s.edge > 3) s.edge = 0;
    if (s.revealDwellMs < 80) s.revealDwellMs = 80;
    if (s.revealDwellMs > 5000) s.revealDwellMs = 5000;
    if (s.retractMs < 300) s.retractMs = 300;
    if (s.retractMs > 20000) s.retractMs = 20000;
    if (s.edgeOffset < 0) s.edgeOffset = 0;
    if (s.edgeOffset > kEdgeOffsetMax) s.edgeOffset = kEdgeOffsetMax;
    if (s.labelBg < 0 || s.labelBg > 1) s.labelBg = 0;
    if (s.labelAlphaPct < 0) s.labelAlphaPct = 0;
    if (s.labelAlphaPct > 100) s.labelAlphaPct = 100;
}

void SaveSettings() {
    std::ofstream out(SettingsPath(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "material=" << s.material << "\n";
    out << "iconSize=" << s.iconSize << "\n";
    out << "edge=" << s.edge << "\n";
    out << "autoStart=" << (s.autoStart ? 1 : 0) << "\n";
    out << "reflection=" << (s.reflection ? 1 : 0) << "\n";
    out << "hideFullscreen=" << (s.hideFullscreen ? 1 : 0) << "\n";
    out << "smartHide=" << (s.smartHide ? 1 : 0) << "\n";
    out << "alwaysHide=" << (s.alwaysHide ? 1 : 0) << "\n";
    out << "showClock=" << (s.showClock ? 1 : 0) << "\n";
    out << "showCenter=" << (s.showCenter ? 1 : 0) << "\n";
    out << "showAdd=" << (s.showAdd ? 1 : 0) << "\n";
    out << "edgeReveal=" << (s.edgeReveal ? 1 : 0) << "\n";
    out << "revealDwellMs=" << s.revealDwellMs << "\n";
    out << "retractMs=" << s.retractMs << "\n";
    out << "edgeOffset=" << s.edgeOffset << "\n";
    out << "hotEdgeGrow=" << (s.hotEdgeGrow ? 1 : 0) << "\n";
    out << "labelBg=" << s.labelBg << "\n";
    out << "labelAlphaPct=" << s.labelAlphaPct << "\n";
    out << "labelOutline=" << (s.labelOutline ? 1 : 0) << "\n";
}

int ItemCount();
int SlotCount();
int ClockIndex();
int CenterIndex();
int AddIndex();
bool Vertical();
int IconBox();
int ReflectionH();
int ReflExtra();
void UpdateWindowGeometry();
void Render();
void HidePopup();
void TogglePopup();
void PositionPopup();
void RenderPopup();
void RefreshPopupIfShown();
void ExecutePopupAction(int action);
void HandleCommand(int cmd);
void ShowLabelFor(int index);
void HideLabel();
void LaunchItem(int index);
LRESULT CALLBACK GroupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void HideGroup();
void StartPopupAnim(bool closing);
void StepPopupAnim();
void FinishHidePopup();
bool IsDockItemGroup(int index);
void OpenGroupAtItem(int index);
void RenderGroup();
void GroupActivate(int index);
void GroupGoBack();
void GroupScroll(int delta);

// ------------------------------------------------------------------ taskbar ---
BOOL CALLBACK TaskbarEnumProc(HWND hwnd, LPARAM hide) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        ShowWindow(hwnd, hide ? SW_HIDE : SW_SHOW);
    }
    return TRUE;
}

// 直接把系统任务栏藏起来（不注入 Explorer），并把工作区扩展到整屏；
// 恢复时把原工作区写回去，避免任务栏回来后窗口布局错乱。
void SetTaskbarHidden(bool hide) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tb) {
        g.taskbarHidden = false;
        return;
    }

    if (hide) {
        if (!g.workAreaSaved) {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &g.savedWorkArea, 0);
            g.workAreaSaved = true;
        }
        ShowWindow(tb, SW_HIDE);
        EnumWindows(TaskbarEnumProc, TRUE);

        RECT full = g.savedWorkArea;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(tb, MONITOR_DEFAULTTOPRIMARY), &mi)) full = mi.rcMonitor;
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, SPIF_SENDCHANGE);
    } else {
        ShowWindow(tb, SW_SHOW);
        EnumWindows(TaskbarEnumProc, FALSE);
        if (g.workAreaSaved) {
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &g.savedWorkArea, SPIF_SENDCHANGE);
            g.workAreaSaved = false;
        }
    }

    g.taskbarHidden = hide;
    UpdateWindowGeometry();
    Render();
}

// --------------------------------------------------------------- autostart ---
void ApplyAutoStart() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (s.autoStart) {
        const std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------- material ---
BOOL SetAccent(HWND hwnd, int state, COLORREF tint, BYTE alpha) {
    using Fn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA_*);
    static Fn fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return FALSE;

    ACCENT_POLICY_ policy{};
    policy.accentState = state;
    policy.accentFlags = 2;
    policy.gradientColor = (static_cast<int>(alpha) << 24) |
                           (static_cast<int>(GetBValue(tint)) << 16) |
                           (static_cast<int>(GetGValue(tint)) << 8) |
                           static_cast<int>(GetRValue(tint));

    WINDOWCOMPOSITIONATTRIBDATA_ data{};
    data.attrib = WCA_ACCENT_POLICY_;
    data.pvData = &policy;
    data.cbData = sizeof(policy);
    return fn(hwnd, &data);
}

// Acrylic 底衬由 DWM 按"窗口矩形"绘制，D2D 的圆角裁剪管不到它：
// 蒙层是圆角、底衬是直角，四角就会露出矩形亮块。
// 用窗口区域把整个窗口（含 DWM 底衬）裁成同一个圆角。
void UpdateWindowRegion() {
    if (!g.hwnd) return;
    if (s.material != 0 || g.width == 0 || g.height == 0) {
        SetWindowRgn(g.hwnd, nullptr, TRUE);
        return;
    }
    const float r = std::round(20.0f * g.dpiScale);
    HRGN rgn = CreateRoundRectRgn(0, 0, static_cast<int>(g.width) + 1,
                                  static_cast<int>(g.height) + 1,
                                  static_cast<int>(r) * 2, static_cast<int>(r) * 2);
    if (rgn) SetWindowRgn(g.hwnd, rgn, TRUE);   // 成功后区域归系统所有，不能 DeleteObject
}

void ApplyMaterial() {
    if (!g.hwnd) return;

    // 用 DWM 自己的圆角：Acrylic 是 DWM 在窗口矩形上画的，D2D 的圆角裁剪管不到它，
    // 只有把窗口本身裁成圆角，毛玻璃的四角才不会是直角（DWM 圆角带抗锯齿）。
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(g.hwnd, 33, &corner, sizeof(corner));
    DWORD borderNone = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g.hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderNone, sizeof(borderNone));

    UpdateWindowRegion();

    if (s.material == 0) {
        // Win11 22H2+：优先用 DWM 系统背景材质（真模糊，且跟随 DWM 圆角一起裁剪）
        const int backdrop = 3;   // DWMSBT_TRANSIENTWINDOW（Acrylic）
        const HRESULT bhr = DwmSetWindowAttribute(g.hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */,
                                                  &backdrop, sizeof(backdrop));
        BOOL light = FALSE;       // 浅色模式，避免系统给深色 Acrylic
        DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &light, sizeof(light));
        // Win10 或 DWM 材质不可用时回退到 accent blur
        BOOL ok = SUCCEEDED(bhr);
        if (!ok) ok = SetAccent(g.hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND_, RGB(246, 248, 252), 0x42);
        if (!ok) ok = SetAccent(g.hwnd, ACCENT_ENABLE_BLURBEHIND_, RGB(246, 248, 252), 0x42);
        Dbg(L"acrylic ok=%d (backdrop hr=0x%08X)", static_cast<int>(ok), static_cast<unsigned>(bhr));
    } else {
        // 必须显式关掉 DWM 背景材质：否则从毛玻璃切到实心后，
        // 系统 Acrylic 底衬仍留在窗口矩形上，四角会露出一圈浅灰"外框"。
        const int none = 1;   // DWMSBT_NONE
        DwmSetWindowAttribute(g.hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &none, sizeof(none));
        SetAccent(g.hwnd, ACCENT_DISABLED_, 0, 0);
    }
}
// ------------------------------------------------------------------- icons ---

// 图标归一化 ---------------------------------------------------------------
// 有些 .lnk（典型是图标资源里没有大尺寸帧的应用）在向系统要"大图"时，系统不放大，
// 而是把原生小图居中贴进请求的画布，四周留空，外圈还带 1px 白边。
// 直接画进图标框就会"图标小一圈 + 一圈白框"。
// 这里按内容包围盒裁掉空白与白边，再等比放大铺满方形画布；判定条件严格（内容占画布
// 不足 85% 才处理），像 Steam / Edge 那种本来就铺满的图标完全不受影响。
// 失败时返回空，调用方继续用原图，行为与改动前一致。
ComPtr<IWICBitmap> NormalizeIconWic(IWICBitmap* src, UINT px) {
    if (!src || !g.wic || px == 0) return nullptr;

    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || sw == 0 || sh == 0) return nullptr;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g.wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return nullptr;

    const UINT stride = sw * 4;
    std::vector<BYTE> buf(static_cast<size_t>(stride) * sh);
    if (FAILED(conv->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data())))
        return nullptr;

    // 内容包围盒：只认真实图案（alpha > 128）。
    // 那圈白边和图标外围的浅阴影 alpha 都在 128 以下，会被一并忽略——这正是我们要的：
    // 实测 110px 下，无畏契约/图吧的图案只有 48x48、47x46，而画布是 110x110；
    // 若按 alpha>16 算，白边和阴影会把包围盒撑满整张画布，判定就会失效。
    int minX = static_cast<int>(sw), minY = static_cast<int>(sh), maxX = -1, maxY = -1;
    for (UINT y = 0; y < sh; ++y) {
        const BYTE* row = buf.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < sw; ++x) {
            const BYTE* p = row + x * 4;                  // BGRA（预乘）
            if (p[3] <= 128) continue;                    // 浅阴影 / 半透明留白 / 白边
            if (static_cast<int>(x) < minX) minX = static_cast<int>(x);
            if (static_cast<int>(x) > maxX) maxX = static_cast<int>(x);
            if (static_cast<int>(y) < minY) minY = static_cast<int>(y);
            if (static_cast<int>(y) > maxY) maxY = static_cast<int>(y);
        }
    }
    if (maxX < minX || maxY < minY) return nullptr;       // 近乎全透明（比如淡色 logo），保持原样

    const UINT cw = static_cast<UINT>(maxX - minX + 1);
    const UINT chh = static_cast<UINT>(maxY - minY + 1);
    // 门槛 65%：实测（110px 画布）无畏契约 60x60、图吧 57x57 会被裁；
    // 其余条目最小的也有 75x75（68%），一个都不会误伤。
    Dbg(L"icon bbox %ux%u in %ux%u", cw, chh, sw, sh);
    if (cw * 100 >= sw * 65 || chh * 100 >= sh * 65) return nullptr;

    ComPtr<IWICBitmap> cropped;
    if (FAILED(g.wic->CreateBitmapFromSourceRect(conv.Get(), static_cast<UINT>(minX),
                                                 static_cast<UINT>(minY), cw, chh, &cropped)))
        return nullptr;

    const double scale = static_cast<double>(px) / static_cast<double>(cw > chh ? cw : chh);
    long nwL = std::lround(cw * scale);
    long nhL = std::lround(chh * scale);
    if (nwL < 1) nwL = 1;
    if (nhL < 1) nhL = 1;
    const UINT nw = static_cast<UINT>(nwL);
    const UINT nh = static_cast<UINT>(nhL);

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(g.wic->CreateBitmapScaler(&scaler))) return nullptr;
    if (FAILED(scaler->Initialize(cropped.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic)))
        return nullptr;

    std::vector<BYTE> scaledBuf(static_cast<size_t>(nw) * 4 * nh);
    if (FAILED(scaler->CopyPixels(nullptr, nw * 4, static_cast<UINT>(scaledBuf.size()), scaledBuf.data())))
        return nullptr;

    ComPtr<IWICBitmap> square;
    if (FAILED(g.wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnDemand, &square)))
        return nullptr;

    WICRect all{ 0, 0, static_cast<INT>(px), static_cast<INT>(px) };
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(square->Lock(&all, WICBitmapLockWrite, &lock))) return nullptr;
    UINT dstStride = 0, dstSize = 0;
    BYTE* dst = nullptr;
    if (FAILED(lock->GetStride(&dstStride))) return nullptr;
    if (FAILED(lock->GetDataPointer(&dstSize, &dst))) return nullptr;
    memset(dst, 0, dstSize);                              // 透明底

    const UINT ox = (px - nw) / 2, oy = (px - nh) / 2;    // 居中放置
    for (UINT y = 0; y < nh; ++y) {
        memcpy(dst + static_cast<size_t>(y + oy) * dstStride + ox * 4,
               scaledBuf.data() + static_cast<size_t>(y) * nw * 4, nw * 4);
    }
    lock.Reset();
    return square;
}


ComPtr<IWICBitmap> LoadIconWic(const std::wstring& path, UINT px) {
    if (!g.wic) return nullptr;

    ComPtr<IShellItemImageFactory2> factory;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr,
                                             __uuidof(IShellItemImageFactory2),
                                             reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr) || !factory) {
        Dbg(L"parse hr=0x%08X %s", hr, path.c_str());
        return nullptr;
    }

    HBITMAP hbm = nullptr;
    const SIZE want{ static_cast<LONG>(px), static_cast<LONG>(px) };
    hr = factory->GetImage(want, 0x4 | 0x1, &hbm);
    if (FAILED(hr) || !hbm) {
        Dbg(L"getimage hr=0x%08X %s", hr, path.c_str());
        return nullptr;
    }

    ComPtr<IWICBitmap> wicBitmap;
    hr = g.wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap);
    DeleteObject(hbm);
    if (FAILED(hr)) {
        Dbg(L"wic hr=0x%08X %s", hr, path.c_str());
        return nullptr;
    }

    // 归一化：把"居中小图 + 白边"的图标裁到内容包围盒再铺满；本来就满的图标原样返回
    if (ComPtr<IWICBitmap> normalized = NormalizeIconWic(wicBitmap.Get(), px)) {
        Dbg(L"icon normalized: %s", path.c_str());
        return normalized;
    }
    return wicBitmap;
}

ComPtr<ID2D1Bitmap1> WicToD2DBitmap(IWICBitmapSource* src) {
    if (!src || !g.dc) return nullptr;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> out;
    if (FAILED(g.dc->CreateBitmapFromWicBitmap(src, &props, &out))) return nullptr;
    return out;
}

// 文件夹条目：画一个系统风格的圆角文件夹，比空色块更易辨认，且无需解码位图。
void DrawFolderGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush,
                     const D2D1_RECT_F& r, float opacity) {
    if (!dc || !brush) return;
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float bodyTop = r.top + h * 0.26f;
    const float tabW = w * 0.46f;
    const float tabH = h * 0.16f;
    const float radius = std::max(2.0f, h * 0.10f);

    brush->SetColor(D2D1::ColorF(0.99f, 0.78f, 0.36f, 0.95f * opacity));
    const D2D1_ROUNDED_RECT tab{
        D2D1::RectF(r.left + w * 0.06f, bodyTop - tabH, r.left + w * 0.06f + tabW, bodyTop + tabH),
        radius, radius };
    dc->FillRoundedRectangle(tab, brush);

    const D2D1_ROUNDED_RECT body{
        D2D1::RectF(r.left + w * 0.04f, bodyTop, r.right - w * 0.04f, r.bottom - h * 0.10f),
        radius, radius };
    brush->SetColor(D2D1::ColorF(1.00f, 0.86f, 0.48f, 0.98f * opacity));
    dc->FillRoundedRectangle(body, brush);
}

// 预渲染"倒影"：垂直翻转 + 自上而下渐隐（预乘 alpha）。
// 放在加载期做，绘制期只是 DrawBitmap，静止时依旧 0 帧、悬停动画零额外开销。
ComPtr<IWICBitmap> MakeReflectionWic(IWICBitmapSource* src, UINT w, UINT h, float maxAlpha) {
    if (!g.wic || !src || w == 0 || h == 0) return nullptr;

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(g.wic->CreateBitmapScaler(&scaler))) return nullptr;
    if (FAILED(scaler->Initialize(src, w, h, WICBitmapInterpolationModeFant))) return nullptr;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g.wic->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    const UINT stride = w * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * h);
    if (FAILED(conv->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
        return nullptr;
    }

    ComPtr<IWICBitmap> out;
    if (FAILED(g.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &out))) {
        return nullptr;
    }

    WICRect rc{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(out->Lock(&rc, WICBitmapLockWrite, &lock))) return nullptr;

    BYTE* dst = nullptr;
    UINT cap = 0, dstStride = 0;
    if (FAILED(lock->GetDataPointer(&cap, &dst)) || FAILED(lock->GetStride(&dstStride))) return nullptr;

    for (UINT y = 0; y < h; ++y) {
        const float t = 1.0f - static_cast<float>(y) / static_cast<float>(h);
        const float a = maxAlpha * t * t;                       // 二次渐隐，越往下越淡
        const BYTE* srow = pixels.data() + static_cast<size_t>(h - 1 - y) * stride;
        BYTE* drow = dst + static_cast<size_t>(y) * dstStride;
        for (UINT x = 0; x < w * 4; ++x) drow[x] = static_cast<BYTE>(srow[x] * a);
    }
    return out;
}

void ReloadIcons() {
    g.icons.clear();
    g.reflections.clear();
    const UINT px = static_cast<UINT>(s.iconSize * 2.0f * g.dpiScale);
    const UINT rh = std::max<UINT>(4, static_cast<UINT>(std::lround(px * 0.42f)));

    for (const auto& path : g.items) {
        ComPtr<IWICBitmap> src = LoadIconWic(path, px);
        if (!src) {
            g.icons.push_back(nullptr);
            g.reflections.push_back(nullptr);
            continue;
        }
        g.icons.push_back(WicToD2DBitmap(src.Get()));

        UINT iw = px, ih = px;
        if (FAILED(src->GetSize(&iw, &ih)) || iw == 0) { iw = px; ih = px; }
        ComPtr<IWICBitmap> refl = MakeReflectionWic(src.Get(), iw, rh, 0.55f);
        g.reflections.push_back(WicToD2DBitmap(refl.Get()));
    }

    g.anims.assign(static_cast<size_t>(SlotCount()), ItemAnim{});
    if (g.hover >= SlotCount()) g.hover = -1;   // 条目减少后避免残留悬停态
}

// ------------------------------------------------------------------ layout ---
int ItemCount() { return static_cast<int>(g.items.size()); }
int ClockIndex() { return s.showClock ? ItemCount() : -1; }              // 日期时间信息块
int CenterIndex() { return s.showCenter ? ItemCount() + (s.showClock ? 1 : 0) : -1; }  // 九宫格设置
int AddIndex() {
    if (!s.showAdd) return -1;
    return ItemCount() + (s.showClock ? 1 : 0) + (s.showCenter ? 1 : 0);  // 「＋」添加位
}
int SlotCount() { return ItemCount() + (s.showClock ? 1 : 0) + (s.showCenter ? 1 : 0) + (s.showAdd ? 1 : 0); }
bool Vertical() { return s.edge == 2 || s.edge == 3; }

int ScaledPx(float v) { return static_cast<int>(std::lround(v * g.dpiScale)); }
int AlongPad() { return ScaledPx(static_cast<float>(kPadX)); }
int CrossPad() { return ScaledPx(static_cast<float>(kPadY)); }
int GapPx() { return ScaledPx(static_cast<float>(kGap)); }
int ClockWidthFor(int box) { return static_cast<int>(std::lround(box * 2.35f)); }
int ClockHeightFor(int box) { return static_cast<int>(std::lround(box * 1.62f)); }
int ClockExtraFor(int box) {
    return Vertical() ? (ClockHeightFor(box) - box) : (ClockWidthFor(box) - box);
}

// 图标格 / 间距 / 倒影高度的统一度量。
//
// 空间不够时的让步顺序（尽量不牺牲观感）：
//   1) 先把间距从 10px 收到最小（竖排时倒影需要纵向空间，收间距最不易察觉）；
//   2) 还放不下，图标与倒影按 0.42 的比例一起等比缩；
//   3) 缩到 24px 底线仍放不下，才把倒影压扁甚至放弃（绝不让图标继续变小到 18px 以下）。
void LayoutMetrics(int* boxOut, int* gapOut, int* reflOut) {
    const int slots = SlotCount();
    if (g.cacheBox > 0 && g.cacheAvail == g.availLen && g.cacheIconSize == s.iconSize &&
        g.cacheSlotCount == slots && g.cacheEdge == s.edge &&
        g.cacheReflection == s.reflection && g.cacheDpi == g.dpiScale &&
        g.cacheShowClock == s.showClock && g.cacheShowCenter == s.showCenter &&
        g.cacheShowAdd == s.showAdd) {
        *boxOut = g.cacheBox;
        *gapOut = g.cacheGap;
        *reflOut = g.cacheRefl;
        return;
    }

    const int desired = ScaledPx(static_cast<float>(s.iconSize));
    const int baseGap = GapPx();
    const int gapMin = std::max(2, ScaledPx(4.0f));
    const int pad = AlongPad();
    const int n = (slots > 0) ? slots : 1;
    const int reflGap = ScaledPx(2.0f);
    const float ratio = 0.42f;
    const bool vertical = Vertical();

    auto reflFor = [&](int box) { return s.reflection ? static_cast<int>(std::lround(box * ratio)) : 0; };
    // 总占用：竖排时倒影在纵向（沿停靠方向），横排时在厚度方向
    auto total = [&](int box, int gap, int refl) {
        const int blk = (refl > 1) ? (refl + reflGap) : 0;
        const int along = 2 * pad + n * box + (n - 1) * gap + (vertical ? n * blk : 0) +
                          (s.showClock ? ClockExtraFor(box) : 0);
        return along + (vertical ? 0 : blk);
    };

    int box = desired;
    int gap = baseGap;
    int refl = reflFor(box);

    if (g.availLen > 0) {
        if (total(box, gap, refl) > g.availLen) {
            // 1) 收间距（每次 1px，最多收到 gapMin）
            while (gap > gapMin && total(box, gap, refl) > g.availLen) --gap;
        }

        if (total(box, gap, refl) > g.availLen) {
            // 2) 图标 + 倒影等比缩（横排时倒影不占沿停靠方向，但同样要缩图标）
            const float fixed = static_cast<float>(2 * pad + (n - 1) * gap) +
                                (vertical ? static_cast<float>(n * reflGap) : 0.0f);
            const float denom = vertical
                ? static_cast<float>(n) * (1.0f + ratio) + 0.55f
                : static_cast<float>(n) + 1.35f;
            const int fit = static_cast<int>(std::floor((g.availLen - fixed) / denom));
            box = std::max(ScaledPx(24.0f), std::min(box, fit));
            refl = reflFor(box);
        }

        if (total(box, gap, refl) > g.availLen) {
            // 3) 牺牲倒影，保住图标
            refl = 0;
        }
        while (total(box, gap, refl) > g.availLen && box > ScaledPx(18.0f)) --box;
        if (s.reflection && refl == 0 && total(box, gap, 0) < g.availLen) {
            // 图标保住后若还有余量，给倒影分一点（至少 3px 才画）
            const int spare = (g.availLen - total(box, gap, 0)) / n - reflGap;
            if (spare >= 3) refl = std::min(reflFor(box), spare);
        }
    }

    g.cacheBox = box;
    g.cacheGap = gap;
    g.cacheRefl = refl;
    g.cacheAvail = g.availLen;
    g.cacheIconSize = s.iconSize;
    g.cacheSlotCount = slots;
    g.cacheEdge = s.edge;
    g.cacheReflection = s.reflection;
    g.cacheDpi = g.dpiScale;
    g.cacheShowClock = s.showClock;
    g.cacheShowCenter = s.showCenter;
    g.cacheShowAdd = s.showAdd;

    *boxOut = box;
    *gapOut = gap;
    *reflOut = refl;
}
int IconBox() { int b = 0, gp = 0, r = 0; LayoutMetrics(&b, &gp, &r); return b; }
int GapPxEff() { int b = 0, gp = 0, r = 0; LayoutMetrics(&b, &gp, &r); return gp; }

// 倒影高度：竖排时随剩余空间自适应，横排时固定 0.42×图标
int ReflectionH() { int b = 0, gp = 0, r = 0; LayoutMetrics(&b, &gp, &r); return r; }

// 倒影占用的额外纵向空间（间隙 + 镜像高度）；没有倒影时为 0
int ReflExtra() {
    const int r = ReflectionH();
    return (r > 1) ? (r + ScaledPx(2.0f)) : 0;
}

// 每格沿停靠方向的步进：竖排时把倒影一起算进去，否则倒影会压到下一个图标
int SlotStep() { return IconBox() + GapPxEff() + (Vertical() ? ReflExtra() : 0); }
int ClockWidth() { return Vertical() ? IconBox() : ClockWidthFor(IconBox()); }
int ClockExtra() { return s.showClock ? ClockExtraFor(IconBox()) : 0; }
int SlotStart(int i) {
    const int clock = ClockIndex();
    return AlongPad() + i * SlotStep() + ((clock >= 0 && i > clock) ? ClockExtra() : 0);
}

int BarLength() {
    const int n = SlotCount();
    return AlongPad() * 2 + (n - 1) * SlotStep() + IconBox() + (Vertical() ? ReflExtra() : 0) + ClockExtra();
}
// 倒影一律朝下，所以厚度方向（竖排时是宽度）不再需要为倒影留位置
int BarThickness() {
    return IconBox() + CrossPad() * 2 +
           (Vertical() ? 0 : (s.reflection ? ReflectionH() + ScaledPx(2.0f) : 0));
}
UINT BoxWidth() { return static_cast<UINT>(Vertical() ? BarThickness() : BarLength()); }
UINT BoxHeight() { return static_cast<UINT>(Vertical() ? BarLength() : BarThickness()); }


D2D1_RECT_F IconRect(int i) {
    const float along = static_cast<float>(SlotStart(i));
    const float cross = static_cast<float>(CrossPad());
    const float box = static_cast<float>(IconBox());
    if (Vertical()) {
        const float height = (i == ClockIndex()) ? static_cast<float>(ClockHeightFor(IconBox())) : box;
        return D2D1::RectF(cross, along, cross + box, along + height);
    }
    const float width = (i == ClockIndex()) ? static_cast<float>(ClockWidth()) : box;
    return D2D1::RectF(along, cross, along + width, cross + box);
}

int HitTest(int x, int y) {
    for (int i = 0; i < SlotCount(); ++i) {
        const D2D1_RECT_F r = IconRect(i);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

D2D1_COLOR_F HashColor(const std::wstring& str) {
    uint32_t h = 2166136261u;
    for (wchar_t c : str) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    const float hue = static_cast<float>(h % 360u);
    const float sat = 0.42f;
    const float val = 0.92f;
    const float c = val * sat;
    const float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
    const float m = val - c;
    float r = 0.0f, gr = 0.0f, b = 0.0f;
    if (hue < 60.0f)       { r = c;  gr = x; }
    else if (hue < 120.0f) { r = x;  gr = c; }
    else if (hue < 180.0f) { gr = c; b = x; }
    else if (hue < 240.0f) { gr = x; b = c; }
    else if (hue < 300.0f) { r = x;  b = c; }
    else                   { r = c;  b = x; }
    return D2D1::ColorF(r + m, gr + m, b + m, 1.0f);
}

// ------------------------------------------------------------------ device ---
HRESULT CreateTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    HRESULT hr = g.swap->GetBuffer(0, __uuidof(IDXGISurface),
                                   reinterpret_cast<void**>(surface.GetAddressOf()));
    if (FAILED(hr)) return hr;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);

    return g.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &g.targetBitmap);
}

HRESULT CreateDeviceResources() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &g.d3d, &got, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &g.d3d, &got, nullptr);
    }
    if (FAILED(hr)) return hr;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.d2dFactory.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()));
    if (FAILED(hr)) g.dwrite.Reset();

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g.d3d.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    hr = g.d2dFactory->CreateDevice(dxgiDevice.Get(), &g.d2dDevice);
    if (FAILED(hr)) return hr;

    hr = g.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc);
    if (FAILED(hr)) return hr;

    // 关键：让 1 个 D2D 单位 == 1 个物理像素。
    // 否则设备上下文会继承工厂的桌面 DPI（150% 缩放下为 144），
    // 使所有绘制被放大 1.5 倍，而窗口尺寸是按像素算的，导致图标溢出。
    g.dc->SetDpi(96.0f, 96.0f);

    hr = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(g.compDevice.GetAddressOf()));
    if (FAILED(hr)) return hr;

    hr = g.compDevice->CreateTargetForHwnd(g.hwnd, TRUE, &g.compTarget);
    if (FAILED(hr)) return hr;

    hr = g.compDevice->CreateVisual(&g.compVisual);
    if (FAILED(hr)) return hr;

    hr = g.compTarget->SetRoot(g.compVisual.Get());
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2),
                            reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = g.width;
    desc.Height = g.height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(g.d3d.Get(), &desc, nullptr, &g.swap);
    if (FAILED(hr)) return hr;

    hr = g.compVisual->SetContent(g.swap.Get());
    if (FAILED(hr)) return hr;

    hr = g.compDevice->Commit();
    if (FAILED(hr)) return hr;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IWICImagingFactory),
                          reinterpret_cast<void**>(g.wic.GetAddressOf()));
    if (FAILED(hr)) g.wic.Reset();

    return CreateTargetBitmap();
}
// ------------------------------------------------------------------ render ---
void PanelStyleFor(int material, D2D1_COLOR_F* fill, D2D1_COLOR_F* border) {
    switch (material) {
    case 0:
        // 系统 Acrylic 在 Win11 上会忽略我们给的色调、用系统深色底，
        // 所以叠一层浅白半透明蒙层把整体压成"浅白毛玻璃"。
        *fill = D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.20f);
        *border = D2D1::ColorF(1, 1, 1, 0.35f);
        break;
    case 1:
        *fill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);      // 透明模式：不画底板
        *border = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);    // 透明模式：不画描边
        break;
    case 2:
        *fill = D2D1::ColorF(0.05f, 0.06f, 0.08f, 0.94f);
        *border = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);    // 实心：不描边，只留圆角
        break;
    default:
        *fill = D2D1::ColorF(0.93f, 0.94f, 0.96f, 0.96f);
        *border = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
        break;
    }
}

void PanelStyle(D2D1_COLOR_F* fill, D2D1_COLOR_F* border) {
    PanelStyleFor(s.material, fill, border);
}

// 面板偏亮时用深色笔（浅色实心 / 浅白毛玻璃），否则用白色笔
float InkFor(int material) { return (material == 3 || material == 0) ? 0.15f : 1.0f; }
float InkW() { return InkFor(s.material); }

void EnsureClockFormats() {
    if (!g.dwrite) return;
    const float base = std::max(10.0f, static_cast<float>(IconBox()) * 0.31f);
    const int box = IconBox();
    if (g.clockTime && g.clockDate && g.clockFormatSize == box) return;
    g.clockTime.Reset();
    g.clockDate.Reset();
    g.clockFormatSize = box;
    if (!g.clockTime) {
        g.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                                   DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, base * 1.12f, L"zh-CN", &g.clockTime);
        if (g.clockTime) {
            g.clockTime->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g.clockTime->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.clockTime->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    if (!g.clockDate) {
        g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                   DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, base * 0.72f, L"zh-CN", &g.clockDate);
        if (g.clockDate) {
            g.clockDate->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g.clockDate->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.clockDate->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
}

void DrawClock(int index, float opacity) {
    if (!g.dc || !g.dwrite) return;
    EnsureClockFormats();
    if (!g.clockTime || !g.clockDate) return;

    const D2D1_RECT_F cell = IconRect(index);
    const float ink = InkW();
    const D2D1_ROUNDED_RECT rr{ cell, 10.0f * g.dpiScale, 10.0f * g.dpiScale };
    g.brush->SetColor(D2D1::ColorF(ink, ink, ink, (index == g.hover ? 0.16f : 0.08f) * opacity));
    g.dc->FillRoundedRectangle(rr, g.brush.Get());

    SYSTEMTIME st{};
    GetLocalTime(&st);
    static const wchar_t* const kWeek[] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
    wchar_t timeBuf[16]{};
    wchar_t dateBuf[40]{};
    swprintf_s(timeBuf, L"%02u:%02u", st.wHour, st.wMinute);
    swprintf_s(dateBuf, L"%u月%u日 %s", st.wMonth, st.wDay, kWeek[st.wDayOfWeek % 7]);

    const float pad = 4.0f * g.dpiScale;
    const float ch = cell.bottom - cell.top;
    g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.95f * opacity));
    if (Vertical()) {
        // 竖排时 Dock 很窄：时间 / 日期 / 星期拆成三行，避免文字被截断
        const D2D1_RECT_F timeRect{ cell.left + pad, cell.top + pad * 0.8f,
                                    cell.right - pad, cell.top + ch * 0.44f };
        const D2D1_RECT_F dayRect{ cell.left + pad, cell.top + ch * 0.40f,
                                   cell.right - pad, cell.top + ch * 0.71f };
        const D2D1_RECT_F weekRect{ cell.left + pad, cell.top + ch * 0.68f,
                                    cell.right - pad, cell.bottom - pad * 0.5f };
        wchar_t dayBuf[16]{};
        swprintf_s(dayBuf, L"%u/%u", st.wMonth, st.wDay);
        g.dc->DrawTextW(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), g.clockTime.Get(), timeRect, g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.68f * opacity));
        g.dc->DrawTextW(dayBuf, static_cast<UINT32>(wcslen(dayBuf)), g.clockDate.Get(), dayRect, g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.52f * opacity));
        g.dc->DrawTextW(kWeek[st.wDayOfWeek % 7], 2, g.clockDate.Get(), weekRect, g.brush.Get());
    } else {
        const D2D1_RECT_F timeRect{ cell.left + pad, cell.top + pad * 0.6f,
                                    cell.right - pad, cell.top + ch * 0.60f };
        const D2D1_RECT_F dateRect{ cell.left + pad, cell.top + ch * 0.53f,
                                    cell.right - pad, cell.bottom - pad * 0.5f };
        g.dc->DrawTextW(timeBuf, static_cast<UINT32>(wcslen(timeBuf)), g.clockTime.Get(), timeRect, g.brush.Get());
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.62f * opacity));
        g.dc->DrawTextW(dateBuf, static_cast<UINT32>(wcslen(dateBuf)), g.clockDate.Get(), dateRect, g.brush.Get());
    }
}

void DrawControlCenter(int index, float scale, float opacity) {
    const D2D1_RECT_F cell = IconRect(index);
    const float cx = (cell.left + cell.right) * 0.5f;
    const float cy = (cell.top + cell.bottom) * 0.5f;
    const float half = (cell.right - cell.left) * 0.5f * scale;
    const float lift = (scale - 1.0f) * static_cast<float>(IconBox()) * 0.35f;
    const float dx = (s.edge == 2) ? lift : ((s.edge == 3) ? -lift : 0.0f);
    const float dy = (s.edge == 0) ? -lift : ((s.edge == 1) ? lift : 0.0f);
    const float radius = 10.0f * g.dpiScale * scale;
    const D2D1_ROUNDED_RECT rr{ D2D1::RectF(cx - half + dx, cy - half + dy, cx + half + dx, cy + half + dy),
                               radius, radius };
    const float ink = InkW();
    g.brush->SetColor(D2D1::ColorF(ink, ink, ink, (index == g.hover ? 0.24f : 0.13f) * opacity));
    g.dc->FillRoundedRectangle(rr, g.brush.Get());

    const float step = half * 0.56f;
    const float dot = std::max(1.6f, half * 0.12f);
    g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.85f * opacity));
    for (int ry = -1; ry <= 1; ++ry) {
        for (int rx = -1; rx <= 1; ++rx) {
            const float px = cx + rx * step + dx;
            const float py = cy + ry * step + dy;
            g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), dot, dot), g.brush.Get());
        }
    }
}

void Render() {
    if (!g.dc || !g.targetBitmap) return;

    Dbg(L"render %u x %u edge=%d box=%d slots=%d op=%.2f", g.width, g.height, s.edge,
        IconBox(), SlotCount(), g.opacity);
    g.dc->SetTarget(g.targetBitmap.Get());
    g.dc->BeginDraw();
    g.dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (!g.brush) {
        if (FAILED(g.dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g.brush))) {
            g.dc->EndDraw();
            return;
        }
    }

    const float w = static_cast<float>(g.width);
    const float h = static_cast<float>(g.height);
    const float op = g.opacity;

    D2D1_COLOR_F fill, border;
    PanelStyle(&fill, &border);
    bool layerPushed = false;   // 只有真的 PushLayer 成功才允许 PopLayer

    // 透明模式下 fill/border 的 alpha 都是 0：整块底板完全不绘制，图标与倒影直接浮在桌面上
    if (fill.a > 0.002f || border.a > 0.002f) {
        fill.a *= op;
        border.a *= op;
        const float radius = std::round(20.0f * g.dpiScale) + 0.5f;
        const D2D1_ROUNDED_RECT panel{ D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), radius, radius };
        if (fill.a > 0.002f) {
            g.brush->SetColor(fill);
            g.dc->FillRoundedRectangle(panel, g.brush.Get());
        }
        if (border.a > 0.002f) {
            g.brush->SetColor(border);
            g.dc->DrawRoundedRectangle(panel, g.brush.Get(), 1.0f);
        }

        // 圆角裁剪：把图标/倒影裁进面板圆角内，避免悬停放大时蹭到四角变成直角
        if (fill.a > 0.002f) {
            // 几何体只在窗口尺寸/圆角变化时重建（ID2D1RoundedRectangleGeometry 没有 setter）
            if (!g.panelGeo || g.panelGeoW != w || g.panelGeoH != h || g.panelGeoR != radius) {
                g.panelGeo.Reset();
                if (FAILED(g.d2dFactory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), radius, radius),
                        &g.panelGeo))) {
                    g.panelGeo.Reset();
                } else {
                    g.panelGeoW = w;
                    g.panelGeoH = h;
                    g.panelGeoR = radius;
                }
            }
            if (g.panelGeo) {
                if (!g.panelLayer) {
                    if (FAILED(g.dc->CreateLayer(nullptr, &g.panelLayer))) g.panelLayer.Reset();
                }
                if (g.panelLayer) {
                    const D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                        D2D1::InfiniteRect(), g.panelGeo.Get(),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    g.dc->PushLayer(lp, g.panelLayer.Get());
                    layerPushed = true;
                }
            }
        }
    }

    const float reflGap = static_cast<float>(ScaledPx(2.0f));
    const float reflH = static_cast<float>(ReflectionH());
    const bool wantRefl = (s.reflection && reflH > 0.5f);

    for (int i = 0; i < SlotCount(); ++i) {
        const D2D1_RECT_F cell = IconRect(i);
        const float cx = (cell.left + cell.right) * 0.5f;
        const float cy = (cell.top + cell.bottom) * 0.5f;

        if (i == ClockIndex()) {
            DrawClock(i, op);
            continue;
        }

        float scale = 1.0f;
        if (i < static_cast<int>(g.anims.size())) scale = g.anims[static_cast<size_t>(i)].scale;

        if (i == CenterIndex()) {
            DrawControlCenter(i, scale, op);
            continue;
        }

        const float half = (cell.right - cell.left) * 0.5f * scale;
        // 悬停放大时朝屏幕内侧轻微抬起，接近 Nexus / macOS 的"浮起"手感
        const float liftPx = (scale - 1.0f) * static_cast<float>(IconBox()) * 0.35f;
        const float dxLift = (s.edge == 2) ? liftPx : ((s.edge == 3) ? -liftPx : 0.0f);
        const float dyLift = (s.edge == 0) ? -liftPx : ((s.edge == 1) ? liftPx : 0.0f);
        const D2D1_RECT_F dst = D2D1::RectF(cx - half + dxLift, cy - half + dyLift,
                                            cx + half + dxLift, cy + half + dyLift);

        if (i < ItemCount()) {
            ID2D1Bitmap1* bmp = (i < static_cast<int>(g.icons.size()))
                                    ? g.icons[static_cast<size_t>(i)].Get()
                                    : nullptr;
            if (bmp) {
                g.dc->DrawBitmap(bmp, dst, op, D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR,
                                 nullptr, nullptr);
            } else {
                const D2D1_ROUNDED_RECT rr{ dst, 10.0f * scale, 10.0f * scale };
                D2D1_COLOR_F c = HashColor(g.items[static_cast<size_t>(i)]);
                c.a *= op;
                g.brush->SetColor(c);
                g.dc->FillRoundedRectangle(rr, g.brush.Get());
            }

            // 倒影：加载期已把翻转 + 渐隐烘进位图，这里只是一次 DrawBitmap
            if (wantRefl && i < static_cast<int>(g.reflections.size()) &&
                g.reflections[static_cast<size_t>(i)]) {
                // 倒影永远在图标下方（镜像的物理方向不随停靠边变化），
                // 竖排时下方空间已由 SlotStep() 预留，不会压到下一个图标
                const D2D1_RECT_F rdst = D2D1::RectF(dst.left, dst.bottom + reflGap,
                                                     dst.right, dst.bottom + reflGap + reflH);
                g.dc->DrawBitmap(g.reflections[static_cast<size_t>(i)].Get(), rdst, op * 0.9f,
                                 D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR, nullptr, nullptr);
            }
        } else if (i == AddIndex()) {
            // 「＋」添加位
            const D2D1_ROUNDED_RECT rr{ dst, 10.0f * scale, 10.0f * scale };
            const float ink = InkW();
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, ((i == g.hover) ? 0.22f : 0.12f) * op));
            g.dc->FillRoundedRectangle(rr, g.brush.Get());

            const float arm = (dst.right - dst.left) * 0.26f;
            const float th = std::max(1.5f, 1.5f * scale);
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.75f * op));
            const D2D1_ROUNDED_RECT hbar{ D2D1::RectF(cx - arm, cy - th, cx + arm, cy + th), th, th };
            const D2D1_ROUNDED_RECT vbar{ D2D1::RectF(cx - th, cy - arm, cx + th, cy + arm), th, th };
            g.dc->FillRoundedRectangle(hbar, g.brush.Get());
            g.dc->FillRoundedRectangle(vbar, g.brush.Get());
        }
    }

    // 必须与 PushLayer 严格配对：透明模式 / 切换尺寸时没有 push 却 pop，
    // D2D 会直接让 EndDraw 失败，整帧丢弃 —— 表现为"切透明后旧画面残留""换边后只剩一个图标"。
    if (layerPushed) g.dc->PopLayer();

    const HRESULT hr = g.dc->EndDraw();
    if (FAILED(hr)) {
        Dbg(L"enddraw fail hr=0x%08X", static_cast<unsigned>(hr));
        // 设备上下文出错后必须重建 device resources，否则后续每帧都会失败
        g.dc->SetTarget(nullptr);
        return;
    }
    g.swap->Present(1, 0);
}

// ------------------------------------------------------------------ layout ---
// 停靠可用区域：任务栏隐藏时直接用显示器整屏，这样"底部"就真的贴到屏幕最底部
RECT DockArea() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (g.taskbarHidden) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        HWND ref = g.hwnd ? g.hwnd : GetDesktopWindow();
        if (GetMonitorInfoW(MonitorFromWindow(ref, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            return mi.rcMonitor;
        }
    }
    return work;
}

// 未收起时的目标矩形（贴边留 kMargin）
RECT BaseDockRect() {
    const RECT work = DockArea();
    const int w = static_cast<int>(g.width);
    const int h = static_cast<int>(g.height);
    RECT r{};
    switch (s.edge) {
    case 0:  // 底部
        r.left = work.left + ((work.right - work.left) - w) / 2;
        r.top = work.bottom - h - kMargin - s.edgeOffset;
        break;
    case 1:  // 顶部
        r.left = work.left + ((work.right - work.left) - w) / 2;
        r.top = work.top + kMargin + s.edgeOffset;
        break;
    case 2:  // 左侧
        r.left = work.left + kMargin + s.edgeOffset;
        r.top = work.top + ((work.bottom - work.top) - h) / 2;
        break;
    default: // 右侧
        r.left = work.right - w - kMargin - s.edgeOffset;
        r.top = work.top + ((work.bottom - work.top) - h) / 2;
        break;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

// 收起位置：把窗口推到屏幕外，只留 kPeekPx 一条热区；
// 这样鼠标触到屏幕边缘时仍能命中窗口，从而"探出"。
void PositionDock() {
    if (!g.hwnd) return;
    const RECT base = BaseDockRect();
    g.baseRect = base;

    // 收起目标：整条完全移出工作区（不留任何可见残影），动画结束后再 SW_HIDE。
    const RECT area = DockArea();
    int hx = base.left;
    int hy = base.top;
    switch (s.edge) {
    case 0: hy = area.bottom; break;
    case 1: hy = area.top - static_cast<int>(g.height); break;
    case 2: hx = area.left - static_cast<int>(g.width); break;
    default: hx = area.right; break;
    }

    const float t = std::max(0.0f, std::min(1.0f, g.slide));
    const int x = static_cast<int>(std::lround(base.left + (hx - base.left) * t));
    const int y = static_cast<int>(std::lround(base.top + (hy - base.top) * t));
    SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, static_cast<int>(g.width), static_cast<int>(g.height),
                 SWP_NOACTIVATE);
}

void UpdateWindowGeometry() {
    const RECT work = DockArea();
    const int avail = Vertical() ? (work.bottom - work.top - 2 * kMargin)
                                 : (work.right - work.left - 2 * kMargin);
    g.availLen = (avail > 0) ? avail : 0;

    g.width = BoxWidth();
    g.height = BoxHeight();
    if (g.hwnd) PositionDock();
    if (gPopup.shown) {
        PositionPopup();
        RenderPopup();
    }
}

// ------------------------------------------------------------------- label ---
// 悬停时浮出应用名（独立分层窗口 + GDI+ 绘制，鼠标穿透、不抢焦点）
// ------------------------------------------------------------------ popup ---
struct PopupAction {
    const wchar_t* title;
    const wchar_t* caption;
};

const PopupAction kPopupActions[kPopupCount] = {
    { L"快捷设置", L"Windows 设置" },
    { L"系统托盘", L"通知区域" },
    { L"添加应用", L"选择 exe / lnk" },
    { L"切换外观", L"玻璃 / 实心" },
    { L"图标大小", L"小 / 中 / 大" },
    { L"停靠位置", L"四边切换" },
    { L"系统任务栏", L"隐藏 / 显示" },
    { L"智能收起", L"避让窗口" },
    { L"全屏隐藏", L"游戏不打扰" },
};

float PopupTileW() { return kPopupTileW * g.dpiScale; }
float PopupTileH() { return kPopupTileH * g.dpiScale; }
float PopupGap() { return kPopupGap * g.dpiScale; }
float PopupPad() { return kPopupPad * g.dpiScale; }
float PopupHeader() { return kPopupHeader * g.dpiScale; }

// 面板底部留白
constexpr float kPopupFooter = 24.0f;

int PopupWidthPx() {
    return static_cast<int>(std::lround(PopupPad() * 2.0f + PopupTileW() * 3.0f + PopupGap() * 2.0f));
}
int PopupHeightPx() {
    return static_cast<int>(std::lround(PopupPad() * 2.0f + PopupHeader() + PopupTileH() * 3.0f +
                                        PopupGap() * 2.0f + kPopupFooter * g.dpiScale));
}
D2D1_RECT_F PopupTileRect(int index) {
    const int col = index % 3;
    const int row = index / 3;
    const float left = PopupPad() + col * (PopupTileW() + PopupGap());
    const float top = PopupPad() + PopupHeader() + row * (PopupTileH() + PopupGap());
    return D2D1::RectF(left, top, left + PopupTileW(), top + PopupTileH());
}

bool PopupHitRect(const D2D1_RECT_F& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void EnsurePopupFormats() {
    if (!g.dwrite) return;
    if (!gPopup.title) {
        g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   14.0f * g.dpiScale, L"zh-CN", &gPopup.title);
        if (gPopup.title) {
            gPopup.title->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            gPopup.title->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            gPopup.title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    if (!gPopup.caption) {
        g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   9.5f * g.dpiScale, L"zh-CN", &gPopup.caption);
        if (gPopup.caption) {
            gPopup.caption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            gPopup.caption->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            gPopup.caption->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    if (!gPopup.label) {
        g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   10.0f * g.dpiScale, L"zh-CN", &gPopup.label);
        if (gPopup.label) {
            gPopup.label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            gPopup.label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            gPopup.label->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
}

void DrawPopupGlyph(int index, D2D1_RECT_F tile, bool active) {
    if (!g.dc || !g.brush) return;
    const float cx = (tile.left + tile.right) * 0.5f;
    const float cy = tile.top + (tile.bottom - tile.top) * 0.33f;
    const float glyph = PopupTileH() * 0.19f;
    const float ink = active ? 0.30f : 0.94f;
    g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 1.0f));
    const float th = std::max(1.5f * g.dpiScale, 1.2f);
    auto square = [&](float half, bool filled) {
        const D2D1_RECT_F r{ cx - half, cy - half, cx + half, cy + half };
        if (filled) g.dc->FillRoundedRectangle(D2D1::RoundedRect(r, 2.5f * g.dpiScale, 2.5f * g.dpiScale), g.brush.Get());
        else g.dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 2.5f * g.dpiScale, 2.5f * g.dpiScale), g.brush.Get(), th);
    };
    auto line = [&](float x1, float y1, float x2, float y2) {
        g.dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g.brush.Get(), th);
    };

    switch (index) {
    case kPopupQuickSettings: {
        square(glyph, false);
        line(cx, cy - glyph * 0.55f, cx, cy + glyph * 0.55f);
        line(cx - glyph * 0.55f, cy, cx + glyph * 0.55f, cy);
        g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), glyph * 0.22f, glyph * 0.22f), g.brush.Get());
        break;
    }
    case kPopupTray: {
        const D2D1_RECT_F box{ cx - glyph, cy - glyph * 0.55f, cx + glyph, cy + glyph * 0.9f };
        g.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.0f * g.dpiScale, 3.0f * g.dpiScale), g.brush.Get(), th);
        line(cx - glyph * 0.55f, cy + glyph * 0.2f, cx + glyph * 0.55f, cy + glyph * 0.2f);
        line(cx - glyph, cy - glyph * 0.1f, cx - glyph * 0.4f, cy - glyph * 0.1f);
        line(cx + glyph * 0.4f, cy - glyph * 0.1f, cx + glyph, cy - glyph * 0.1f);
        break;
    }
    case kPopupAdd: {
        line(cx - glyph * 0.8f, cy, cx + glyph * 0.8f, cy);
        line(cx, cy - glyph * 0.8f, cx, cy + glyph * 0.8f);
        break;
    }
    case kPopupMaterial: {
        // 三层错位玻璃片，比双圆更清楚地表达“材质/外观”
        for (int layer = 2; layer >= 0; --layer) {
            const float off = (layer - 1) * glyph * 0.34f;
            const D2D1_RECT_F sheet{ cx - glyph * 0.68f + off, cy - glyph * 0.78f - off * 0.35f,
                                     cx + glyph * 0.68f + off, cy + glyph * 0.42f - off * 0.35f };
            const D2D1_ROUNDED_RECT sheetRR{ sheet, glyph * 0.22f, glyph * 0.22f };
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.16f + layer * 0.10f));
            g.dc->FillRoundedRectangle(sheetRR, g.brush.Get());
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.92f));
            g.dc->DrawRoundedRectangle(sheetRR, g.brush.Get(), th);
        }
        break;
    }
    case kPopupSize: {
        square(glyph * 0.42f, false);
        const D2D1_RECT_F big{ cx - glyph * 0.25f, cy - glyph * 0.25f, cx + glyph * 0.95f, cy + glyph * 0.95f };
        g.dc->DrawRoundedRectangle(D2D1::RoundedRect(big, 2.0f * g.dpiScale, 2.0f * g.dpiScale), g.brush.Get(), th);
        break;
    }
    case kPopupEdge: {
        const D2D1_RECT_F box{ cx - glyph, cy - glyph * 0.72f, cx + glyph, cy + glyph * 0.72f };
        g.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.0f * g.dpiScale, 2.0f * g.dpiScale), g.brush.Get(), th);
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.38f));
        g.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left, box.bottom - glyph * 0.3f, box.right, box.bottom), 2.0f * g.dpiScale, 2.0f * g.dpiScale), g.brush.Get());
        break;
    }
    case kPopupTaskbar: {
        const D2D1_RECT_F box{ cx - glyph, cy - glyph * 0.65f, cx + glyph, cy + glyph * 0.3f };
        g.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.0f * g.dpiScale, 2.0f * g.dpiScale), g.brush.Get(), th);
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.38f));
        g.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - glyph * 0.25f, box.bottom - glyph * 0.22f, cx + glyph * 0.25f, box.bottom - glyph * 0.08f), 1.0f, 1.0f), g.brush.Get());
        break;
    }
    case kPopupSmartHide: {
        const D2D1_ROUNDED_RECT rr{ D2D1::RectF(cx - glyph, cy - glyph * 0.5f, cx + glyph, cy + glyph * 0.5f), 3.0f * g.dpiScale, 3.0f * g.dpiScale };
        g.dc->DrawRoundedRectangle(rr, g.brush.Get(), th);
        line(cx - glyph * 0.55f, cy + glyph * 0.85f, cx + glyph * 0.3f, cy + glyph * 0.85f);
        break;
    }
    default: {   // kPopupFullscreen
        const D2D1_RECT_F box{ cx - glyph, cy - glyph * 0.7f, cx + glyph, cy + glyph * 0.7f };
        g.dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.0f * g.dpiScale, 2.0f * g.dpiScale), g.brush.Get(), th);
        line(box.left + glyph * 0.35f, box.top + glyph * 0.35f, box.left + glyph * 0.75f, box.top + glyph * 0.35f);
        line(box.left + glyph * 0.35f, box.top + glyph * 0.35f, box.left + glyph * 0.35f, box.top + glyph * 0.75f);
        break;
    }
    }
}

bool PopupActive(int index) {
    switch (index) {
    case kPopupMaterial: return s.material == 0;
    case kPopupTaskbar: return g.taskbarHidden;
    case kPopupSmartHide: return s.smartHide;
    case kPopupFullscreen: return s.hideFullscreen;
    default: return false;
    }
}

std::wstring PopupStateText(int index) {
    static const wchar_t* const kMaterials[] = { L"毛玻璃", L"透明", L"深色", L"浅色" };
    static const wchar_t* const kSizes[] = { L"小", L"中", L"大" };
    static const wchar_t* const kEdges[] = { L"底部", L"顶部", L"左侧", L"右侧" };
    switch (index) {
    case kPopupMaterial: return kMaterials[std::max(0, std::min(3, s.material))];
    case kPopupSize: return kSizes[s.iconSize <= 36 ? 0 : (s.iconSize >= 52 ? 2 : 1)];
    case kPopupEdge: return kEdges[std::max(0, std::min(3, s.edge))];
    case kPopupTaskbar: return g.taskbarHidden ? L"已隐藏" : L"已显示";
    case kPopupSmartHide: return s.smartHide ? L"已开启" : L"已关闭";
    case kPopupFullscreen: return s.hideFullscreen ? L"已开启" : L"已关闭";
    default: return L"";
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 退场动画期间面板逻辑上已关闭：屏蔽鼠标，避免点到正在淡出的窗口
    if (!gPopup.shown && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
    switch (msg) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        int hover = -1;
        for (int i = 0; i < kPopupCount; ++i) {
            if (PopupHitRect(PopupTileRect(i), x, y)) { hover = i; break; }
        }
        if (hover != gPopup.hover) {
            gPopup.hover = hover;
            RenderPopup();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (gPopup.hover != -1) {
            gPopup.hover = -1;
            RenderPopup();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        for (int i = 0; i < kPopupCount; ++i) {
            if (PopupHitRect(PopupTileRect(i), x, y)) {
                ExecutePopupAction(i);
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) HidePopup();
        return 0;
    case WM_DESTROY:
        gPopup.hwnd = nullptr;
        gPopup.shown = false;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsurePopupWindow() {
    if (gPopup.hwnd) return;
    static bool classOnce = false;
    if (!classOnce) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PopupProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClassName;
        RegisterClassExW(&wc);
        classOnce = true;
    }
    gPopup.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kPopupClassName, L"Dock 设置", WS_POPUP,
        0, 0, PopupWidthPx(), PopupHeightPx(), g.hwnd, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!gPopup.hwnd) return;

    if (!g.compDevice) return;
    if (FAILED(g.compDevice->CreateTargetForHwnd(gPopup.hwnd, TRUE, &gPopup.target))) return;
    if (FAILED(g.compDevice->CreateVisual(&gPopup.visual))) return;
    if (FAILED(gPopup.target->SetRoot(gPopup.visual.Get()))) return;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = PopupWidthPx();
    desc.Height = PopupHeightPx();
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g.d3d.As(&dxgiDevice))) return;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf())))) return;
    if (FAILED(factory->CreateSwapChainForComposition(g.d3d.Get(), &desc, nullptr, &gPopup.swap))) return;
    if (FAILED(gPopup.visual->SetContent(gPopup.swap.Get()))) return;
    g.compDevice->Commit();
    gPopup.w = desc.Width;
    gPopup.h = desc.Height;
}

void ResizePopupTarget() {
    if (!gPopup.swap || !g.dc) return;
    const UINT w = static_cast<UINT>(PopupWidthPx());
    const UINT h = static_cast<UINT>(PopupHeightPx());
    if (w == gPopup.w && h == gPopup.h && gPopup.bitmap) return;

    g.dc->SetTarget(nullptr);
    g.dc->Flush();
    gPopup.bitmap.Reset();
    if (FAILED(gPopup.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
    ComPtr<IDXGISurface> surface;
    if (FAILED(gPopup.swap->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf())))) return;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &gPopup.bitmap))) return;
    gPopup.w = w;
    gPopup.h = h;
    if (g.compDevice) g.compDevice->Commit();
}

void PositionPopup() {
    if (!gPopup.hwnd) return;
    RECT dock{};
    if (!GetWindowRect(g.hwnd, &dock)) return;
    const RECT area = DockArea();
    const int w = PopupWidthPx();
    const int h = PopupHeightPx();
    const int gap = ScaledPx(10.0f);
    int x = dock.left + (dock.right - dock.left - w) / 2;
    int y = dock.top - h - gap;
    switch (s.edge) {
    case 1: y = dock.bottom + gap; break;
    case 2: x = dock.right + gap; y = dock.top + (dock.bottom - dock.top - h) / 2; break;
    case 3: x = dock.left - w - gap; y = dock.top + (dock.bottom - dock.top - h) / 2; break;
    default: break;
    }
    if (x < area.left + 4) x = area.left + 4;
    if (x + w > area.right - 4) x = area.right - 4 - w;
    if (y < area.top + 4) y = area.top + 4;
    if (y + h > area.bottom - 4) y = area.bottom - 4 - h;
    SetWindowPos(gPopup.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

void RenderPopup() {
    if (!gPopup.hwnd || !g.dc || !gPopup.bitmap) return;
    if (gPopup.formatDpi != g.dpiScale) {
        gPopup.title.Reset();
        gPopup.caption.Reset();
        gPopup.label.Reset();
        gPopup.formatDpi = g.dpiScale;
    }
    EnsurePopupFormats();

    g.dc->SetTarget(gPopup.bitmap.Get());
    g.dc->BeginDraw();
    g.dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    if (!g.brush) {
        if (FAILED(g.dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g.brush))) {
            g.dc->EndDraw();
            return;
        }
    }

    const float w = static_cast<float>(gPopup.w);
    const float h = static_cast<float>(gPopup.h);
    const float r = 18.0f * g.dpiScale;
    const D2D1_ROUNDED_RECT panel{ D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r, r };
    const float op = gPopup.opacity;

    g.brush->SetColor(D2D1::ColorF(0.045f, 0.050f, 0.065f, 0.94f * op));
    g.dc->FillRoundedRectangle(panel, g.brush.Get());
    g.brush->SetColor(D2D1::ColorF(1, 1, 1, 0.075f * op));
    g.dc->DrawRoundedRectangle(panel, g.brush.Get(), 1.0f);

    const D2D1_RECT_F titleRect{ PopupPad(), PopupPad() * 0.6f,
                                 w - PopupPad(),
                                 PopupPad() * 0.6f + PopupHeader() * 0.75f };
    g.brush->SetColor(D2D1::ColorF(0.97f, 0.97f, 0.99f, 0.98f * op));
    if (gPopup.title) {
        g.dc->DrawTextW(L"设置", 2, gPopup.title.Get(), titleRect, g.brush.Get());
    }

    for (int i = 0; i < kPopupCount; ++i) {
        const D2D1_RECT_F tile = PopupTileRect(i);
        const bool active = PopupActive(i);
        const D2D1_COLOR_F bg = active
            ? D2D1::ColorF(0.94f, 0.72f, 0.76f, 0.96f * op)
            : D2D1::ColorF(1, 1, 1, (i == gPopup.hover ? 0.16f : 0.075f) * op);
        g.brush->SetColor(bg);
        const D2D1_ROUNDED_RECT rr{ tile, 8.0f * g.dpiScale, 8.0f * g.dpiScale };
        g.dc->FillRoundedRectangle(rr, g.brush.Get());
        g.brush->SetColor(active ? D2D1::ColorF(0, 0, 0, 0.10f * op)
                                 : D2D1::ColorF(1, 1, 1, (i == gPopup.hover ? 0.11f : 0.07f) * op));
        g.dc->DrawRoundedRectangle(rr, g.brush.Get(), 1.0f);

        DrawPopupGlyph(i, tile, active);

        const D2D1_RECT_F textRect{ tile.left + 2.0f, tile.bottom - 36.0f * g.dpiScale,
                                    tile.right - 2.0f, tile.bottom - 20.0f * g.dpiScale };
        g.brush->SetColor(active ? D2D1::ColorF(0.14f, 0.12f, 0.13f, 0.96f * op)
                                 : D2D1::ColorF(0.96f, 0.97f, 0.99f, 0.95f * op));
        if (gPopup.label) {
            g.dc->DrawTextW(kPopupActions[i].title,
                            static_cast<UINT32>(wcslen(kPopupActions[i].title)),
                            gPopup.label.Get(), textRect, g.brush.Get());
        }

        const std::wstring state = PopupStateText(i);
        if (!state.empty() && gPopup.caption) {
            const D2D1_RECT_F stateRect{ tile.left + 2.0f, tile.bottom - 20.0f * g.dpiScale,
                                         tile.right - 2.0f, tile.bottom - 2.0f * g.dpiScale };
            g.brush->SetColor(active ? D2D1::ColorF(0.28f, 0.19f, 0.21f, 0.82f * op)
                                     : D2D1::ColorF(0.74f, 0.77f, 0.84f, 0.68f * op));
            g.dc->DrawTextW(state.c_str(), static_cast<UINT32>(state.size()),
                            gPopup.caption.Get(), stateRect, g.brush.Get());
        }
    }

    const HRESULT hr = g.dc->EndDraw();
    if (FAILED(hr)) {
        g.dc->SetTarget(nullptr);
        return;
    }
    gPopup.swap->Present(1, 0);
}

void RefreshPopupIfShown() {
    if (gPopup.shown) RenderPopup();
}

void HidePopup() {
    if (!gPopup.shown && !gPopup.hwnd) return;
    if (gPopupAnim.running && gPopupAnim.closing) return;   // 已经在退场
    gPopup.shown = false;
    gPopup.hover = -1;
    gPopup.prevMouseDown = false;
    gPopup.prevEscapeDown = false;
    if (gPopup.watchOn) {
        gPopup.watchOn = false;
        KillTimer(g.hwnd, kPopupWatchTimerId);
    }
    // 平滑退场：先跑动画，跑完在 FinishHidePopup() 里才真正隐藏窗口
    if (gPopup.hwnd && gPopup.visual) {
        StartPopupAnim(true);
        Dbg(L"popup closing");
        return;
    }
    if (gPopup.hwnd) ShowWindow(gPopup.hwnd, SW_HIDE);
    Dbg(L"popup hidden");
}

// --------------------------------------------------------------- group panel ---
// 文件夹二级栏：沿用九宫格面板的 DComp 小交换链，但内容是目录里的启动项。
constexpr float kGroupTileW = 88.0f;
constexpr float kGroupTileH = 94.0f;
constexpr float kGroupGap = 8.0f;
constexpr float kGroupPad = 14.0f;
constexpr float kGroupHeader = 48.0f;
constexpr float kGroupFooter = 26.0f;
constexpr int kGroupCols = 4;
constexpr int kGroupMaxRows = 4;

float GroupTileW() { return kGroupTileW * g.dpiScale; }
float GroupTileH() { return kGroupTileH * g.dpiScale; }
float GroupGap() { return kGroupGap * g.dpiScale; }
float GroupPad() { return kGroupPad * g.dpiScale; }
float GroupHeader() { return kGroupHeader * g.dpiScale; }

int GroupRowsVisible() {
    const int total = static_cast<int>(gGroup.paths.size());
    const int rows = (total + kGroupCols - 1) / kGroupCols;
    return std::max(1, std::min(rows, kGroupMaxRows));
}

int GroupWidthPx() {
    return static_cast<int>(std::lround(GroupPad() * 2.0f + GroupTileW() * kGroupCols +
                                        GroupGap() * (kGroupCols - 1)));
}
int GroupHeightPx() {
    return static_cast<int>(std::lround(GroupPad() * 2.0f + GroupHeader() +
                                        GroupTileH() * GroupRowsVisible() +
                                        GroupGap() * (GroupRowsVisible() - 1) +
                                        kGroupFooter * g.dpiScale));
}

D2D1_RECT_F GroupTileRect(int index) {
    const int visible = index - gGroup.scrollRow * kGroupCols;
    const int col = visible % kGroupCols;
    const int row = visible / kGroupCols;
    const float left = GroupPad() + col * (GroupTileW() + GroupGap());
    const float top = GroupPad() + GroupHeader() + row * (GroupTileH() + GroupGap());
    return D2D1::RectF(left, top, left + GroupTileW(), top + GroupTileH());
}

int GroupHitTest(int x, int y) {
    const int rows = GroupRowsVisible();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < kGroupCols; ++c) {
            const int index = (gGroup.scrollRow + r) * kGroupCols + c;
            if (index >= static_cast<int>(gGroup.paths.size())) return -1;
            const D2D1_RECT_F tile = GroupTileRect(index);
            if (x >= tile.left && x < tile.right && y >= tile.top && y < tile.bottom) return index;
        }
    }
    return -1;
}

bool ResolveShortcutTarget(const std::wstring& lnk, std::wstring* target) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) {
        return false;
    }
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    if (FAILED(file->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t buf[MAX_PATH * 2]{};
    if (FAILED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, SLGP_RAWPATH))) return false;
    if (!buf[0]) return false;
    *target = buf;
    return true;
}

// 目录 → 二级栏根目录；.lnk 指向目录时也当作文件夹。
bool ResolveGroupRoot(const std::wstring& path, std::wstring* root) {
    if (IsDirectoryPath(path)) {
        *root = path;
        return true;
    }
    if (_wcsicmp(FileNameOf(path).c_str(), L"") != 0 && IsLaunchableFile(path)) {
        std::wstring target;
        if (ResolveShortcutTarget(path, &target) && IsDirectoryPath(target)) {
            *root = target;
            return true;
        }
    }
    return false;
}

bool IsDockItemGroup(int index) {
    if (index < 0 || index >= ItemCount()) return false;
    const std::wstring& path = g.items[static_cast<size_t>(index)];
    if (IsDirectoryPath(path)) return true;
    std::wstring target;
    return ResolveShortcutTarget(path, &target) && IsDirectoryPath(target);
}

std::wstring StripTrailingSlash(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

void LoadGroupEntries() {
    gGroup.paths.clear();
    gGroup.isDir.clear();
    gGroup.icons.clear();
    gGroup.hover = -1;
    gGroup.scrollRow = 0;

    const std::wstring pattern = StripTrailingSlash(gGroup.root) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    struct Entry { std::wstring name; std::wstring path; bool dir; };
    std::vector<Entry> entries;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const std::wstring name = fd.cFileName;
        if (!dir && !IsLaunchableFile(name)) continue;
        entries.push_back({ name, StripTrailingSlash(gGroup.root) + L"\\" + name, dir });
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;                  // 文件夹在前
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    const UINT px = static_cast<UINT>(std::lround(64.0f * g.dpiScale));
    for (const auto& e : entries) {
        gGroup.paths.push_back(e.path);
        gGroup.isDir.push_back(e.dir);
        gGroup.icons.push_back(e.dir ? nullptr : WicToD2DBitmap(LoadIconWic(e.path, px).Get()));
    }
}

void EnsureGroupFormats() {
    if (!g.dwrite) return;
    if (gGroup.formatDpi == g.dpiScale && gGroup.titleFmt) return;
    gGroup.formatDpi = g.dpiScale;
    gGroup.titleFmt.Reset();
    gGroup.labelFmt.Reset();
    gGroup.captionFmt.Reset();

    g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               13.0f * g.dpiScale, L"zh-CN", &gGroup.titleFmt);
    if (gGroup.titleFmt) {
        gGroup.titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        gGroup.titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        gGroup.titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               10.0f * g.dpiScale, L"zh-CN", &gGroup.labelFmt);
    if (gGroup.labelFmt) {
        gGroup.labelFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        gGroup.labelFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        gGroup.labelFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    g.dwrite->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               9.0f * g.dpiScale, L"zh-CN", &gGroup.captionFmt);
    if (gGroup.captionFmt) {
        gGroup.captionFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        gGroup.captionFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        gGroup.captionFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

void EnsureGroupWindow() {
    if (gGroup.hwnd) return;
    static bool classOnce = false;
    if (!classOnce) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GroupProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DockGroupWindow";
        RegisterClassExW(&wc);
        classOnce = true;
    }
    gGroup.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"DockGroupWindow", L"Dock 文件夹", WS_POPUP, 0, 0, GroupWidthPx(), GroupHeightPx(),
        g.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!gGroup.hwnd) return;
    if (!g.compDevice) return;
    if (FAILED(g.compDevice->CreateTargetForHwnd(gGroup.hwnd, TRUE, &gGroup.target))) return;
    if (FAILED(g.compDevice->CreateVisual(&gGroup.visual))) return;
    if (FAILED(gGroup.target->SetRoot(gGroup.visual.Get()))) return;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(GroupWidthPx());
    desc.Height = static_cast<UINT>(GroupHeightPx());
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g.d3d.As(&dxgiDevice))) return;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(factory.GetAddressOf())))) return;
    if (FAILED(factory->CreateSwapChainForComposition(g.d3d.Get(), &desc, nullptr, &gGroup.swap)))
        return;
    if (FAILED(gGroup.visual->SetContent(gGroup.swap.Get()))) return;
    g.compDevice->Commit();
    gGroup.w = desc.Width;
    gGroup.h = desc.Height;
}

void ResizeGroupTarget() {
    if (!gGroup.swap || !g.dc) return;
    const UINT w = static_cast<UINT>(GroupWidthPx());
    const UINT h = static_cast<UINT>(GroupHeightPx());
    if (w == gGroup.w && h == gGroup.h && gGroup.bitmap) return;

    g.dc->SetTarget(nullptr);
    g.dc->Flush();
    gGroup.bitmap.Reset();
    if (FAILED(gGroup.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
    ComPtr<IDXGISurface> surface;
    if (FAILED(gGroup.swap->GetBuffer(0, __uuidof(IDXGISurface),
                                      reinterpret_cast<void**>(surface.GetAddressOf())))) return;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(g.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &gGroup.bitmap))) return;
    gGroup.w = w;
    gGroup.h = h;
    if (g.compDevice) g.compDevice->Commit();
}

void PositionGroup() {
    if (!gGroup.hwnd || !g.hwnd) return;
    RECT dock{};
    if (!GetWindowRect(g.hwnd, &dock)) return;
    const RECT area = DockArea();
    const int w = GroupWidthPx();
    const int h = GroupHeightPx();
    const int gap = ScaledPx(10.0f);
    int x = dock.left + (dock.right - dock.left - w) / 2;
    int y = dock.top - h - gap;
    switch (s.edge) {
    case 1: y = dock.bottom + gap; break;
    case 2: x = dock.right + gap; y = dock.top + (dock.bottom - dock.top - h) / 2; break;
    case 3: x = dock.left - w - gap; y = dock.top + (dock.bottom - dock.top - h) / 2; break;
    default: break;
    }
    if (x < area.left + 4) x = area.left + 4;
    if (x + w > area.right - 4) x = area.right - 4 - w;
    if (y < area.top + 4) y = area.top + 4;
    if (y + h > area.bottom - 4) y = area.bottom - 4 - h;
    SetWindowPos(gGroup.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

// ------------------------------------------------------- 面板过渡动画 ---
// 文件夹二级栏 与 九宫格设置面板 共用同一套动画：
// 只动 DComp 视觉对象的透明度和缩放，内容不重绘，每帧开销接近零。
// 打开：透明度 0→1、缩放 0.96→1；关闭：反向，动画跑完才真正 SW_HIDE。
constexpr float kPanelAnimOpenMs  = 140.0f;
constexpr float kPanelAnimCloseMs = 110.0f;

// 锚点取"靠近 Dock 的那一侧"，视觉上像从 Dock 长出来
D2D1_POINT_2F PanelAnimAnchor(UINT wPx, UINT hPx) {
    const float w = static_cast<float>(wPx);
    const float h = static_cast<float>(hPx);
    switch (s.edge) {
    case 1:  return D2D1::Point2F(w * 0.5f, 0.0f);   // Dock 在顶部 → 锚上沿
    case 0:  return D2D1::Point2F(w * 0.5f, h);      // Dock 在底部 → 锚下沿
    case 2:  return D2D1::Point2F(0.0f, h * 0.5f);   // Dock 在左   → 锚左沿
    default: return D2D1::Point2F(w, h * 0.5f);      // Dock 在右   → 锚右沿
    }
}

// SDK 版本差异：IDCompositionVisual::SetTransform 在不同头文件里分别声明为
// 取指针 / 取引用。用 SFINAE 自动选能编译的那一种，避免构建往返。
template <typename TV, typename TM>
static auto SetVisualMatrix(TV* v, const TM& m, int) -> decltype(v->SetTransform(m), void()) {
    v->SetTransform(m);
}
template <typename TV, typename TM>
static auto SetVisualMatrix(TV* v, const TM& m, long) -> decltype(v->SetTransform(&m), void()) {
    v->SetTransform(&m);
}

// 通用：把「透明度 k + 缩放 0.96+0.04k」套到某个面板视觉对象上。
// DComp 的透明度必须走效果组（IDCompositionVisual 本身没有 SetOpacity）。
void ApplyPanelVisual(IDCompositionVisual* vis, ComPtr<IDCompositionEffectGroup>& fx,
                      float k, D2D1_POINT_2F anchor) {
    if (!vis) return;
    if (g.compDevice) {
        if (!fx) {
            if (SUCCEEDED(g.compDevice->CreateEffectGroup(&fx)) && fx) vis->SetEffect(fx.Get());
        }
        if (fx) fx->SetOpacity(k);
    }
    const float scale = 0.96f + 0.04f * k;
    const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(scale, scale, anchor);
    SetVisualMatrix(vis, m, 0);
    if (g.compDevice) g.compDevice->Commit();
}

// 进度 → 视觉权重（ease-out-cubic；关闭时从 1 退回 0）
float PanelEaseT(const GroupAnimState& a) {
    const float inv = 1.0f - a.t;
    const float e = 1.0f - inv * inv * inv;
    return a.closing ? (1.0f - e) : e;
}

// ---------------- 文件夹二级栏 ----------------
void ApplyGroupAnim() {
    ApplyPanelVisual(gGroup.visual.Get(), gGroup.opacityFx, PanelEaseT(gGroupAnim),
                     PanelAnimAnchor(gGroup.w, gGroup.h));
}

void ResetGroupVisual() {
    ApplyPanelVisual(gGroup.visual.Get(), gGroup.opacityFx, 1.0f,
                     PanelAnimAnchor(gGroup.w, gGroup.h));
}

void StopGroupAnimTimer() {
    if (!gGroupAnim.timerOn) return;
    gGroupAnim.timerOn = false;
    KillTimer(g.hwnd, kGroupAnimTimerId);
}

void StartGroupAnim(bool closing) {
    if (!gGroup.visual) return;                 // 没有视觉对象（窗口异常）就不做动画
    gGroupAnim.closing = closing;
    gGroupAnim.running = true;
    gGroupAnim.t = 0.0f;
    ApplyGroupAnim();
    if (!gGroupAnim.timerOn) {
        gGroupAnim.timerOn = true;
        SetTimer(g.hwnd, kGroupAnimTimerId, kAnimIntervalMs, nullptr);
    }
}

// 退场动画结束后的收尾：这时候才真正隐藏窗口
void FinishHideGroup() {
    StopGroupAnimTimer();
    gGroupAnim.running = false;
    if (gGroup.hwnd) ShowWindow(gGroup.hwnd, SW_HIDE);
    ResetGroupVisual();
}

void StepGroupAnim() {
    if (!gGroupAnim.running) return;
    const float ms = gGroupAnim.closing ? kPanelAnimCloseMs : kPanelAnimOpenMs;
    gGroupAnim.t += static_cast<float>(kAnimIntervalMs) / ms;
    if (gGroupAnim.t >= 1.0f) {
        gGroupAnim.t = 1.0f;
        ApplyGroupAnim();
        gGroupAnim.running = false;
        if (gGroupAnim.closing) {
            FinishHideGroup();
        } else {
            StopGroupAnimTimer();
        }
        return;
    }
    ApplyGroupAnim();
}

// 正在退场时又被打开（比如快速连点另一个文件夹）：撤销动画状态，别把窗口藏掉
void CancelGroupCloseAnim() {
    if (!gGroupAnim.closing) return;
    gGroupAnim.running = false;
    gGroupAnim.closing = false;
    StopGroupAnimTimer();
    ResetGroupVisual();
}

// ---------------- 九宫格设置面板 ----------------
void ApplyPopupAnim() {
    ApplyPanelVisual(gPopup.visual.Get(), gPopup.opacityFx, PanelEaseT(gPopupAnim),
                     PanelAnimAnchor(gPopup.w, gPopup.h));
}

void StopPopupAnimTimer() {
    if (!gPopupAnim.timerOn) return;
    gPopupAnim.timerOn = false;
    KillTimer(g.hwnd, kPopupAnimTimerId);
}

void StartPopupAnim(bool closing) {
    if (!gPopup.visual) return;
    gPopupAnim.closing = closing;
    gPopupAnim.running = true;
    gPopupAnim.t = 0.0f;
    ApplyPopupAnim();
    if (!gPopupAnim.timerOn) {
        gPopupAnim.timerOn = true;
        SetTimer(g.hwnd, kPopupAnimTimerId, kAnimIntervalMs, nullptr);
    }
}

void FinishHidePopup() {
    StopPopupAnimTimer();
    gPopupAnim.running = false;
    if (gPopup.hwnd) ShowWindow(gPopup.hwnd, SW_HIDE);
    ApplyPanelVisual(gPopup.visual.Get(), gPopup.opacityFx, 1.0f,
                     PanelAnimAnchor(gPopup.w, gPopup.h));
}

void StepPopupAnim() {
    if (!gPopupAnim.running) return;
    const float ms = gPopupAnim.closing ? kPanelAnimCloseMs : kPanelAnimOpenMs;
    gPopupAnim.t += static_cast<float>(kAnimIntervalMs) / ms;
    if (gPopupAnim.t >= 1.0f) {
        gPopupAnim.t = 1.0f;
        ApplyPopupAnim();
        gPopupAnim.running = false;
        if (gPopupAnim.closing) {
            FinishHidePopup();
        } else {
            StopPopupAnimTimer();
        }
        return;
    }
    ApplyPopupAnim();
}

LRESULT CALLBACK GroupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 退场动画期间面板逻辑上已关闭：屏蔽鼠标，避免点到正在淡出的窗口
    if (!gGroup.shown && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
    switch (msg) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        const int hover = GroupHitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hover != gGroup.hover) {
            gGroup.hover = hover;
            RenderGroup();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (gGroup.hover != -1) {
            gGroup.hover = -1;
            RenderGroup();
        }
        return 0;
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (!gGroup.parents.empty()) {
            const float headH = GroupPad() + GroupHeader();
            if (y < headH && x < GroupPad() + 28.0f * g.dpiScale) {
                GroupGoBack();
                return 0;
            }
        }
        const int index = GroupHitTest(x, y);
        if (index >= 0) GroupActivate(index);
        return 0;
    }
    case WM_MOUSEWHEEL:
        GroupScroll(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) HideGroup();
        return 0;
    case WM_DESTROY:
        gGroup.hwnd = nullptr;
        gGroup.shown = false;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RenderGroup() {
    if (!gGroup.hwnd || !gGroup.bitmap || !g.dc) return;
    EnsureGroupFormats();
    g.dc->SetTarget(gGroup.bitmap.Get());
    g.dc->BeginDraw();
    g.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (!g.brush) {
        if (FAILED(g.dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g.brush))) {
            g.dc->EndDraw();
            return;
        }
    }

    const float w = static_cast<float>(gGroup.w);
    const float h = static_cast<float>(gGroup.h);
    const float r = 18.0f * g.dpiScale;
    const D2D1_ROUNDED_RECT panel{ D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r, r };

    // 层次 1：背景直接复用主 Dock 的 PanelStyleFor()，不再维护第二张颜色表。
    // 透明主题（material=1）下主 Dock 不画底板，但文件夹面板若也透明就完全看不清，
    // 所以面板在这里把 1 映射成 0 —— 直接套用毛玻璃主题的那套设定（白 20% + 白 35% 描边）。
    const int panelMaterial = (s.material == 1) ? 0 : s.material;
    D2D1_COLOR_F fill{}, line{};
    PanelStyleFor(panelMaterial, &fill, &line);
    const float ink = InkFor(panelMaterial);
    if (fill.a > 0.002f) {
        g.brush->SetColor(fill);
        g.dc->FillRoundedRectangle(panel, g.brush.Get());
    }
    if (line.a > 0.0f) {
        g.brush->SetColor(line);
        g.dc->DrawRoundedRectangle(panel, g.brush.Get(), 1.0f);
    }

    // 顶栏：返回箭头 + 标题
    const float headY = GroupPad() + GroupHeader() * 0.5f;
    if (!gGroup.parents.empty()) {
        const float aw = 6.0f * g.dpiScale;
        const float ax = GroupPad() + 8.0f * g.dpiScale;
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.92f));
        g.dc->DrawLine(D2D1::Point2F(ax + aw, headY - aw), D2D1::Point2F(ax, headY),
                       g.brush.Get(), 1.6f * g.dpiScale);
        g.dc->DrawLine(D2D1::Point2F(ax, headY), D2D1::Point2F(ax + aw, headY + aw),
                       g.brush.Get(), 1.6f * g.dpiScale);
    }

    std::wstring title = gGroup.title;
    if (title.size() > 20) title = title.substr(0, 19) + L"…";
    const float titleLeft = (gGroup.parents.empty()) ? GroupPad()
                                                     : GroupPad() + 26.0f * g.dpiScale;
    const D2D1_RECT_F titleRect{ titleLeft, GroupPad() + 4.0f * g.dpiScale,
                                 w - GroupPad(), GroupPad() + GroupHeader() - 4.0f * g.dpiScale };
    if (gGroup.titleFmt) {
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.97f));
        g.dc->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()),
                        gGroup.titleFmt.Get(), titleRect, g.brush.Get());
    }

    const int count = static_cast<int>(gGroup.paths.size());
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F tile = GroupTileRect(i);
        if (tile.top >= h - kGroupFooter * g.dpiScale) break;
        const bool hot = (i == gGroup.hover);
        if (hot) {
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.14f));
            g.dc->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(tile.left - 2.0f, tile.top - 2.0f,
                            tile.right + 2.0f, tile.bottom + 2.0f),
                10.0f * g.dpiScale, 10.0f * g.dpiScale), g.brush.Get());
        }
        const float iconSize = 46.0f * g.dpiScale;
        const float iconLeft = (tile.left + tile.right) * 0.5f - iconSize * 0.5f;
        const float iconTop = tile.top + 8.0f * g.dpiScale;
        const D2D1_RECT_F iconRect{ iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize };

        if (i < static_cast<int>(gGroup.isDir.size()) && gGroup.isDir[static_cast<size_t>(i)]) {
            DrawFolderGlyph(g.dc.Get(), g.brush.Get(), iconRect, 1.0f);
        } else if (i < static_cast<int>(gGroup.icons.size()) && gGroup.icons[static_cast<size_t>(i)]) {
            g.dc->DrawBitmap(gGroup.icons[static_cast<size_t>(i)].Get(), iconRect, 1.0f,
                             D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR, nullptr, nullptr);
        } else {
            g.brush->SetColor(HashColor(gGroup.paths[static_cast<size_t>(i)]));
            g.dc->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 9.0f * g.dpiScale,
                                                         9.0f * g.dpiScale), g.brush.Get());
        }

        std::wstring name = FileNameOf(gGroup.paths[static_cast<size_t>(i)]);
        if (name.size() > 9) name = name.substr(0, 8) + L"…";
        const D2D1_RECT_F nameRect{ tile.left, iconTop + iconSize + 6.0f * g.dpiScale,
                                    tile.right, tile.bottom - 2.0f * g.dpiScale };
        if (gGroup.labelFmt) {
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, hot ? 1.0f : 0.86f));
            g.dc->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                            gGroup.labelFmt.Get(), nameRect, g.brush.Get());
        }
    }

    if (count == 0) {
        const D2D1_RECT_F empty{ GroupPad(), GroupPad() + GroupHeader(),
                                 w - GroupPad(), h - kGroupFooter * g.dpiScale };
        if (gGroup.labelFmt) {
            g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.62f));
            g.dc->DrawTextW(L"这个文件夹里没有可启动的程序", 14, gGroup.labelFmt.Get(),
                            empty, g.brush.Get());
        }
    }

    // 底栏：条目数 / 滚动提示
    wchar_t footer[96]{};
    const int rowsTotal = (count + kGroupCols - 1) / kGroupCols;
    if (rowsTotal > kGroupMaxRows) {
        swprintf_s(footer, L"%d 个项目 · 滚轮翻页 %d/%d", count,
                   gGroup.scrollRow + 1, std::max(1, rowsTotal - kGroupMaxRows + 1));
    } else {
        swprintf_s(footer, L"%d 个项目", count);
    }
    if (gGroup.captionFmt) {
        g.brush->SetColor(D2D1::ColorF(ink, ink, ink, 0.55f));
        const D2D1_RECT_F fr{ GroupPad(), h - kGroupFooter * g.dpiScale - GroupPad() * 0.6f,
                              w - GroupPad(), h - GroupPad() * 0.3f };
        g.dc->DrawTextW(footer, static_cast<UINT32>(wcslen(footer)), gGroup.captionFmt.Get(),
                        fr, g.brush.Get());
    }

    if (FAILED(g.dc->EndDraw())) {
        g.dc->SetTarget(nullptr);
        return;
    }
    gGroup.swap->Present(1, 0);
}

// 记住是哪个 Dock 条目打开了文件夹面板：
// 面板开着的期间再点同一个图标，就当成"切换用资源管理器打开"。
static int gGroupOwnerItem = -1;

void HideGroup() {
    if (!gGroup.shown && !gGroup.hwnd) return;
    if (gGroupAnim.running && gGroupAnim.closing) return;   // 已经在退场，别重启动画
    gGroup.shown = false;
    gGroupOwnerItem = -1;          // 面板已关，下次点击重新走"展开"分支
    gGroup.hover = -1;
    gGroup.prevMouseDown = false;
    gGroup.prevEscapeDown = false;
    if (gGroup.watchOn) {
        gGroup.watchOn = false;
        KillTimer(g.hwnd, kGroupWatchTimerId);
    }
    // 平滑退场：先跑动画，跑完在 FinishHideGroup() 里才真正隐藏窗口
    if (gGroup.hwnd && gGroup.visual) {
        StartGroupAnim(true);
        return;
    }
    if (gGroup.hwnd) ShowWindow(gGroup.hwnd, SW_HIDE);
}

void OpenGroup(const std::wstring& root, const std::wstring& title, bool pushParent) {
    if (!g.hwnd) return;
    if (pushParent && !gGroup.root.empty() && gGroup.root != root) {
        gGroup.parents.push_back(gGroup.root);
    }
    gGroup.root = StripTrailingSlash(root);
    gGroup.title = title.empty() ? FileNameOf(root) : title;
    LoadGroupEntries();

    EnsureGroupWindow();
    if (!gGroup.hwnd) return;
    ResizeGroupTarget();
    if (!gGroup.bitmap) return;
    const bool wasShown = gGroup.shown;
    gGroup.shown = true;
    gGroup.hover = -1;
    gGroup.prevMouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    gGroup.prevEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    PositionGroup();
    RenderGroup();

    // 初次展开才播入场动画；同一面板里切换文件夹只换内容，不重播（否则会闪一下）。
    // 注意顺序：先把透明度设成 0 再 ShowWindow，否则会有一帧完全可见的闪烁。
    CancelGroupCloseAnim();
    if (wasShown) {
        ResetGroupVisual();
    } else {
        StartGroupAnim(false);
    }
    ShowWindow(gGroup.hwnd, SW_SHOWNOACTIVATE);

    if (!gGroup.watchOn) {
        gGroup.watchOn = true;
        SetTimer(g.hwnd, kGroupWatchTimerId, kPopupWatchMs, nullptr);
    }
    Dbg(L"group open: %s (%zu items)", gGroup.root.c_str(), gGroup.paths.size());
}

void OpenGroupAtItem(int index) {
    if (index < 0 || index >= ItemCount()) return;
    std::wstring root;
    if (!ResolveGroupRoot(g.items[static_cast<size_t>(index)], &root)) return;
    HidePopup();
    gGroup.parents.clear();
    OpenGroup(root, FileNameOf(g.items[static_cast<size_t>(index)]), false);
}

// 点击"文件夹类条目"的统一入口（取代原来两处 IsDockItemGroup 分支）：
//   · 面板没开 / 开的是别的条目 → 展开这个文件夹的面板
//   · 面板开着的就是这个条目   → 关闭面板，改用资源管理器打开
// 完全按状态判断，不做双击时间判定，所以单击立刻响应、也不会手快误判。
void ActivateItem(int index) {
    if (index < 0 || index >= ItemCount()) return;
    if (!IsDockItemGroup(index)) {
        LaunchItem(index);
        return;
    }
    if (gGroup.shown && gGroupOwnerItem == index) {
        HideGroup();
        LaunchItem(index);
        return;
    }
    OpenGroupAtItem(index);
    gGroupOwnerItem = index;
}

void GroupActivate(int index) {
    if (index < 0 || index >= static_cast<int>(gGroup.paths.size())) return;
    const std::wstring path = gGroup.paths[static_cast<size_t>(index)];
    if (index < static_cast<int>(gGroup.isDir.size()) && gGroup.isDir[static_cast<size_t>(index)]) {
        OpenGroup(path, FileNameOf(path), true);
        return;
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(gGroup.hwnd, path.c_str(), L"无法启动该条目", MB_ICONWARNING | MB_OK);
        return;
    }
    HideGroup();
}

void GroupGoBack() {
    if (gGroup.parents.empty()) {
        HideGroup();
        return;
    }
    const std::wstring prev = gGroup.parents.back();
    gGroup.parents.pop_back();
    // 返回时不再压栈
    gGroup.root = StripTrailingSlash(prev);
    gGroup.title = FileNameOf(prev);
    LoadGroupEntries();
    ResizeGroupTarget();
    PositionGroup();
    RenderGroup();
}

void GroupScroll(int delta) {
    const int count = static_cast<int>(gGroup.paths.size());
    const int rowsTotal = (count + kGroupCols - 1) / kGroupCols;
    const int maxRow = std::max(0, rowsTotal - kGroupMaxRows);
    int row = gGroup.scrollRow + (delta > 0 ? -1 : 1);
    row = std::max(0, std::min(maxRow, row));
    if (row == gGroup.scrollRow) return;
    gGroup.scrollRow = row;
    gGroup.hover = -1;
    RenderGroup();
}

void OpenSystemTray() {
    // 目标是任务栏右侧「显示隐藏的图标」面板（后台运行的应用），不是通知中心。
    // 先用 UI 自动化按名字找到系统托盘按钮并 Invoke；不依赖屏幕坐标与语言之外的布局。
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // 任务栏被 Dock 隐藏时，托盘按钮不可见也无法展开；先临时借出任务栏。
    // 只看 Dock 自己的开关，避免用户本来就隐藏了任务栏（自动隐藏）时被误判。
    if (g.taskbarHidden && !IsWindowVisible(tray)) {
        ShowWindow(tray, SW_SHOW);
        g.trayPeek = true;
        g.trayPeekStart = GetTickCount();
        g.trayOverflowSeen = false;
        if (!g.trayTimerOn) {
            g.trayTimerOn = true;
            SetTimer(g.hwnd, kSystemTrayTimerId, kSystemTrayPollMs, nullptr);
        }
    }

    bool invoked = false;
    HRESULT uiaHr = E_FAIL;
    HRESULT invokeHr = E_FAIL;
    bool foundButton = false;
    if (tray && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        ComPtr<IUIAutomation> uia;
        ComPtr<IUIAutomationElement> root;
        uiaHr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&uia));
        if (SUCCEEDED(uiaHr) &&
            SUCCEEDED(uia->ElementFromHandle(tray, &root)) && root) {
            VARIANT name;
            VariantInit(&name);
            name.vt = VT_BSTR;
            name.bstrVal = SysAllocString(L"显示隐藏的图标");
            ComPtr<IUIAutomationCondition> cond;
            if (name.bstrVal && SUCCEEDED(uia->CreatePropertyCondition(UIA_NamePropertyId, name, &cond)) && cond) {
                ComPtr<IUIAutomationElement> btn;
                if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, cond.Get(), &btn)) && btn) {
                    foundButton = true;
                    ComPtr<IUIAutomationInvokePattern> invoke;
                    if (SUCCEEDED(btn->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) {
                        invokeHr = invoke->Invoke();
                        invoked = SUCCEEDED(invokeHr);
                    }
                }
            }
            VariantClear(&name);
        }
        CoUninitialize();
    }

    // 兜底：英文 / 旧版 Win11 上按 AutomationId + 可见按钮定位，再用鼠标点一次原生按钮。
    if (!invoked && tray) {
        wchar_t cls[64]{};
        HWND child = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
        if (!child) child = FindWindowExW(tray, nullptr, L"NotifyIconOverflowWindow", nullptr);
        POINT pt{};
        if (child && (pt = [&] {
                RECT r{};
                if (!GetWindowRect(child, &r)) return POINT{};
                return POINT{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            }()).x > 0) {
            SetCursorPos(pt.x, pt.y);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            invoked = true;
        }
        (void)cls;
    }

    if (invoked) {
        Dbg(L"system tray invoked (peek=%d)", g.trayPeek ? 1 : 0);
        g.trayOverflow = nullptr;   // 面板窗口在点击后才创建，轮询时再抓
        return;
    }

    Dbg(L"system tray not invoked: uia=0x%08X found=%d invoke=0x%08X",
        static_cast<unsigned>(uiaHr), foundButton ? 1 : 0, static_cast<unsigned>(invokeHr));

    // 老系统：没有该按钮时，退回通知区域图标设置页（仍比打开通知中心更接近“托盘图标”）。
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar",
                                                nullptr, nullptr, SW_SHOWNORMAL)) > 32) return;
    ShellExecuteW(nullptr, L"open", L"ms-actioncenter:", nullptr, nullptr, SW_SHOWNORMAL);
}

void CyclePopupValue(int action) {
    switch (action) {
    case kPopupMaterial: HandleCommand(kCmdMatAcrylic + ((s.material + 1) % 4)); break;
    case kPopupSize: {
        const int next = (s.iconSize == 36) ? 44 : (s.iconSize == 44 ? 52 : 36);
        HandleCommand(next == 36 ? kCmdSizeSmall : (next == 44 ? kCmdSizeMedium : kCmdSizeLarge));
        break;
    }
    case kPopupEdge: HandleCommand(kCmdEdgeBottom + ((s.edge + 1) % 4)); break;
    case kPopupTaskbar: HandleCommand(kCmdTaskbar); break;
    case kPopupSmartHide: HandleCommand(kCmdSmartHide); break;
    case kPopupFullscreen: HandleCommand(kCmdHideFullscreen); break;
    default: break;
    }
}

void ExecutePopupAction(int action) {
    switch (action) {
    case kPopupQuickSettings:
        ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kPopupTray:
        OpenSystemTray();
        break;
    case kPopupAdd:
        HidePopup();
        HandleCommand(kCmdAdd);
        break;
    default:
        CyclePopupValue(action);
        RenderPopup();
        break;
    }
    Dbg(L"popup action=%d", action);
}

void StartPopupWatch() {
    if (!g.hwnd || gPopup.watchOn) return;
    gPopup.watchOn = true;
    SetTimer(g.hwnd, kPopupWatchTimerId, kPopupWatchMs, nullptr);
}

void TogglePopup() {
    if (gPopup.shown) {
        HidePopup();
        return;
    }
    HideGroup();
    EnsurePopupWindow();
    if (!gPopup.hwnd) return;
    ResizePopupTarget();
    if (!gPopup.bitmap) return;
    PositionPopup();
    gPopup.shown = true;
    gPopup.hover = -1;
    gPopup.opacity = 1.0f;
    gPopup.opacityTarget = 1.0f;
    gPopup.prevMouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    gPopup.prevEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    RenderPopup();
    StartPopupAnim(false);          // 先把透明度设成 0 再显示，避免首帧闪烁
    ShowWindow(gPopup.hwnd, SW_SHOWNOACTIVATE);
    StartPopupWatch();
    Dbg(L"popup shown at edge=%d", s.edge);
}


struct LabelWindow {
    HWND hwnd = nullptr;
    int shownFor = -1;
};
LabelWindow gLabel;

void EnsureLabelWindow() {
    if (gLabel.hwnd) return;
    static bool once = false;
    if (!once) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DockLabelWindow";
        RegisterClassExW(&wc);
        once = true;
    }
    gLabel.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"DockLabelWindow", L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
}

void HideLabel() {
    if (gLabel.hwnd && IsWindowVisible(gLabel.hwnd)) ShowWindow(gLabel.hwnd, SW_HIDE);
    gLabel.shownFor = -1;
}

void ShowLabelFor(int index) {
    if (index < 0 || index >= ItemCount()) {
        HideLabel();
        return;
    }
    EnsureLabelWindow();
    if (!gLabel.hwnd) return;
    if (gLabel.shownFor == index && IsWindowVisible(gLabel.hwnd)) return;

    const std::wstring text = FileNameOf(g.items[static_cast<size_t>(index)]);
    if (text.empty()) return;

    static Gdiplus::FontFamily family(L"Microsoft YaHei UI");
    // 关键：字号必须落到"整数像素"。
    // 原来 10.5pt 在 125% 缩放下 = 10.5 × 96/72 × 1.25 = 17.5px，是半像素；
    // GDI+ 按浮点 em 栅格化，笔画落不到像素网格上，抗锯齿一混就成了"灰块"，
    // 中文小字尤其明显（加粗后更糊）。改成按 DPI 算出整数像素再建字体。
    const float fontPx = std::round(10.5f * (96.0f / 72.0f) * (g.dpiScale > 0.0f ? g.dpiScale : 1.0f));
    Gdiplus::Font font(&family, fontPx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    int tw = 0, th = 0;
    {
        Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics pg(&probe);
        pg.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        Gdiplus::RectF bounds;
        pg.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font, Gdiplus::PointF(0, 0),
                         &fmt, &bounds);
        tw = static_cast<int>(std::ceil(bounds.Width));
        th = static_cast<int>(std::ceil(bounds.Height));
    }
    const int padX = 13, padY = 6;
    const int w = tw + padX * 2;
    const int h = th + padY * 2;

    RECT wr{};
    GetWindowRect(g.hwnd, &wr);
    const D2D1_RECT_F cell = IconRect(index);
    POINT iconPt{ static_cast<LONG>(cell.left), static_cast<LONG>(cell.top) };
    ClientToScreen(g.hwnd, &iconPt);
    const int iconCx = iconPt.x + static_cast<int>((cell.right - cell.left) * 0.5f);
    const int iconCy = iconPt.y + static_cast<int>((cell.bottom - cell.top) * 0.5f);
    const int gap = ScaledPx(9.0f);

    int x = iconCx - w / 2;
    int y = wr.top - h - gap;                   // 底部停靠：标签在上方
    switch (s.edge) {
    case 1: y = wr.bottom + gap; break;          // 顶部停靠：标签在下方
    case 2: x = wr.right + gap; y = iconCy - h / 2; break;
    case 3: x = wr.left - w - gap; y = iconCy - h / 2; break;
    default: break;
    }
    const RECT area = DockArea();
    if (x < area.left + 4) x = area.left + 4;
    if (x + w > area.right - 4) x = area.right - 4 - w;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    if (!bmp || !bits) {
        ReleaseDC(nullptr, screen);
        return;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    {
        Gdiplus::Graphics gfx(mem);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        const float r = h * 0.5f;
        const float W = static_cast<float>(w);
        const float H = static_cast<float>(h);
        Gdiplus::GraphicsPath path;
        path.AddArc(0.0f, 0.0f, r * 2, r * 2, 180.0f, 90.0f);
        path.AddArc(W - r * 2, 0.0f, r * 2, r * 2, 270.0f, 90.0f);
        path.AddArc(W - r * 2, H - r * 2, r * 2, r * 2, 0.0f, 90.0f);
        path.AddArc(0.0f, H - r * 2, r * 2, r * 2, 90.0f, 90.0f);
        path.CloseFigure();

        // 背景/文字颜色：白色背景配深色文字，黑色背景配浅色文字（自动相反，不会配出白底白字）
        const bool lightBg = (s.labelBg == 1);
        const BYTE bgA = static_cast<BYTE>(s.labelAlphaPct * 255 / 100);

        Gdiplus::SolidBrush bg(Gdiplus::Color(bgA,
                                              lightBg ? 250 : 18,
                                              lightBg ? 250 : 20,
                                              lightBg ? 250 : 26));
        gfx.FillPath(&bg, &path);

        // 1px 描边也自动取反：白底上用深边，黑底上用浅边。
        // 背景几乎全透明时（bgA 很小）不画这圈边，否则会剩下一个"若隐若现的空胶囊框"。
        if (bgA > 8) {
            Gdiplus::Pen edge(Gdiplus::Color(40,
                                             lightBg ? 0 : 255,
                                             lightBg ? 0 : 255,
                                             lightBg ? 0 : 255), 1.0f);
            gfx.DrawPath(&edge, &path);
        }

        Gdiplus::SolidBrush fg(Gdiplus::Color(255,
                                              lightBg ? 20 : 246,
                                              lightBg ? 22 : 247,
                                              lightBg ? 26 : 250));

        // 文字描边：把文字往八个方向各偏移 1px 画一遍底色，再叠正文。
        // 这样背景不透明度拉到 0 时，文字压在浅色壁纸上也不会糊。
        if (s.labelOutline) {
            // 描边用"不透明"的背景色：否则 labelAlphaPct=0 时描边也跟着透明，等于没描
            Gdiplus::SolidBrush ol(Gdiplus::Color(255,
                                                  lightBg ? 250 : 18,
                                                  lightBg ? 250 : 20,
                                                  lightBg ? 250 : 26));
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    gfx.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                                   Gdiplus::RectF(static_cast<Gdiplus::REAL>(dx),
                                                  static_cast<Gdiplus::REAL>(dy),
                                                  W, H), &fmt, &ol);
                }
            }
        }

        gfx.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                       Gdiplus::RectF(0.0f, 0.0f, W, H), &fmt, &fg);
    }

    // UpdateLayeredWindow 要求预乘 alpha
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t c = px[i];
        const uint32_t a = (c >> 24) & 0xFFu;
        const uint32_t rr = (((c >> 16) & 0xFFu) * a + 127u) / 255u;
        const uint32_t gg = (((c >> 8) & 0xFFu) * a + 127u) / 255u;
        const uint32_t bb = ((c & 0xFFu) * a + 127u) / 255u;
        px[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }

    POINT dstPt{ x, y };
    SIZE size{ w, h };
    POINT srcPt{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    SetWindowPos(gLabel.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    UpdateLayeredWindow(gLabel.hwnd, screen, &dstPt, &size, mem, &srcPt, 0, &bf, ULW_ALPHA);
    ShowWindow(gLabel.hwnd, SW_SHOWNOACTIVATE);

    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    DeleteObject(bmp);
    ReleaseDC(nullptr, screen);

    gLabel.shownFor = index;
}

// 样式改完后立刻按新参数重画一次（标签只在下一次悬停时才会重建，否则要移开鼠标再回来）
void RefreshLabelStyle() {
    if (!gLabel.hwnd || !IsWindowVisible(gLabel.hwnd)) return;
    const int idx = gLabel.shownFor;
    if (idx < 0) return;
    gLabel.shownFor = -1;
    ShowLabelFor(idx);
}
// -------------------------------------------------------------------- anim ---
void StartAnimTimer() {
    if (!g.animating) {
        g.animating = true;
        SetTimer(g.hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
    }
}

void StopAnimTimer() {
    if (g.animating) {
        g.animating = false;
        KillTimer(g.hwnd, kAnimTimerId);
    }
}

void SetHover(int index) {
    if (index == g.hover) return;
    g.hover = index;
    if (index >= 0 && index < ItemCount()) ShowLabelFor(index);
    else HideLabel();
    StartAnimTimer();
}

bool StepAnimation() {
    const float dt = static_cast<float>(kAnimIntervalMs) / 1000.0f;
    bool moving = false;

    for (size_t i = 0; i < g.anims.size(); ++i) {
        ItemAnim& a = g.anims[i];
        const int idx = static_cast<int>(i);

        // 鱼眼：悬停项放大 1.16，左右相邻项轻微跟随，形成连续起伏
        float target = 1.0f;
        if (g.hover >= 0) {
            const int d = std::abs(idx - g.hover);
            const float falloff = (d == 0) ? 1.0f : (d == 1 ? 0.22f : (d == 2 ? 0.06f : 0.0f));
            target = 1.0f + (kHoverScale - 1.0f) * falloff;
        }

        const float acc = (target - a.scale) * kSpringK - a.vel * kSpringD;
        a.vel += acc * dt;
        a.scale += a.vel * dt;

        if (std::fabs(target - a.scale) < 0.0015f && std::fabs(a.vel) < 0.02f) {
            a.scale = target;
            a.vel = 0.0f;
        } else {
            moving = true;
        }
    }

    // 淡入 / 淡出
    if (std::fabs(g.opacity - g.opacityTarget) > 0.002f) {
        const float step = dt * 9.0f;
        if (g.opacity < g.opacityTarget) g.opacity = std::min(g.opacityTarget, g.opacity + step);
        else g.opacity = std::max(g.opacityTarget, g.opacity - step);
        moving = true;
    }

    // 收起 / 滑出（临界阻尼，约 160ms）
    const float slideTarget = g.autoHidden ? 1.0f : 0.0f;
    if (std::fabs(g.slide - slideTarget) > 0.004f) {
        g.slide += (slideTarget - g.slide) * std::min(1.0f, dt * 14.0f);
        PositionDock();
        moving = true;
    } else if (g.slide != slideTarget) {
        g.slide = slideTarget;
        PositionDock();
        moving = true;
    }

    // 完全收起：① 已滑出屏幕，或 ② 内容已淡到全透明且不是毛玻璃材质
    // ② 是针对 DWM 圆角窗口的补丁：透明/实心材质下淡完之后窗口里已无内容，但窗口还挂在
    // 屏幕上走完滑动，DWM 会继续画系统投影，表现为"收回后残影多留零点几秒"。
    // 这里提前隐藏窗口；下次显示时 SetAutoHidden(false) 会把 slide 重置为 1，位置不受影响。
    if (g.autoHidden && (g.slide >= 0.999f || (g.opacity <= 0.02f && s.material != 0)) && g.winShown) {
        g.winShown = false;
        ShowWindow(g.hwnd, SW_HIDE);
        HideLabel();
        HidePopup();
        HideGroup();
        StopAnimTimer();
        return false;
    }
    return moving;
}
// ------------------------------------------------------------------ launch ---
void LaunchItem(int index) {
    if (index < 0 || index >= ItemCount()) return;

    const std::wstring& path = g.items[static_cast<size_t>(index)];
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(g.hwnd, path.c_str(), L"无法启动该条目", MB_ICONWARNING | MB_OK);
        return;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);

    // 启动反馈：给该图标一个向下冲量，弹簧自动回弹
    if (index < static_cast<int>(g.anims.size())) {
        g.anims[static_cast<size_t>(index)].vel -= 3.2f;
        StartAnimTimer();
    }
}

// ------------------------------------------------------------------- items ---
void RefreshAfterItemChange() {
    ReloadIcons();
    UpdateWindowGeometry();
    Render();
}

void AddPath(const std::wstring& path) {
    for (const auto& existing : g.items) {
        if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return;
    }
    g.items.push_back(path);
}

// 拖动排序：把第 from 个条目移动到 to 位置，图标与动画状态同步搬移（不重新解码图标）
void MoveItem(int from, int to) {
    const int n = ItemCount();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n) return;

    std::wstring item = std::move(g.items[static_cast<size_t>(from)]);
    g.items.erase(g.items.begin() + from);
    g.items.insert(g.items.begin() + to, std::move(item));

    if (from < static_cast<int>(g.icons.size()) && to < static_cast<int>(g.icons.size())) {
        ComPtr<ID2D1Bitmap1> bmp = std::move(g.icons[static_cast<size_t>(from)]);
        g.icons.erase(g.icons.begin() + from);
        g.icons.insert(g.icons.begin() + to, std::move(bmp));
    }
    if (from < static_cast<int>(g.reflections.size()) && to < static_cast<int>(g.reflections.size())) {
        ComPtr<ID2D1Bitmap1> refl = std::move(g.reflections[static_cast<size_t>(from)]);
        g.reflections.erase(g.reflections.begin() + from);
        g.reflections.insert(g.reflections.begin() + to, std::move(refl));
    }
    if (from < static_cast<int>(g.anims.size()) && to < static_cast<int>(g.anims.size())) {
        const ItemAnim a = g.anims[static_cast<size_t>(from)];
        g.anims.erase(g.anims.begin() + from);
        g.anims.insert(g.anims.begin() + to, a);
    }
}

void RemoveItem(int index) {
    if (index < 0 || index >= ItemCount()) return;
    g.items.erase(g.items.begin() + index);
    g.hover = -1;
    SaveItems();
    RefreshAfterItemChange();
}

void AddViaDialog() {
    wchar_t file[MAX_PATH * 2]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"应用程序 (*.exe;*.lnk)\0*.exe;*.lnk\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return;
    AddPath(file);
    SaveItems();
    RefreshAfterItemChange();
}

void AddFolderViaDialog() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"选择要加入 Dock 的文件夹");
    if (FAILED(dlg->Show(g.hwnd))) return;

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
    const std::wstring folder = path;
    CoTaskMemFree(path);

    AddPath(folder);
    SaveItems();
    RefreshAfterItemChange();
}

void OpenConfigFolder() {
    const std::wstring dir = ExeDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --------------------------------------------------------------- fullscreen ---
// 全屏应用检测：前台窗口是否完整覆盖它所在显示器的整块屏幕。
// 只做 GetForegroundWindow + GetWindowRect，不注入、不读别的进程内存；
// 前景变化由 WinEvent 钩子即时触发，另有低频轮询兜底（F11 之类不换焦点的情形）。
bool IsShellOrOverlayClass(const wchar_t* cls) {
    static const wchar_t* const kSkip[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskSwitcherWnd",
        L"XamlExplorerHostIslandWindow",
        L"DV2ControlHost", L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland", L"DockWindowClass", L"DockLabelWindow",
    };
    for (const wchar_t* skip : kSkip) {
        if (wcscmp(cls, skip) == 0) return true;
    }
    return false;
}

bool IsFullscreenForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g.hwnd || (gLabel.hwnd && fg == gLabel.hwnd)) return false;
    if (!IsWindowVisible(fg) || IsIconic(fg)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;

    wchar_t cls[128]{};
    if (!GetClassNameW(fg, cls, ARRAYSIZE(cls))) return false;
    if (IsShellOrOverlayClass(cls)) return false;

    // 只认顶层窗口：被子窗口/工具窗抢到前台时不当作全屏
    if (GetAncestor(fg, GA_ROOT) != fg) return false;

    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    // 只关心 Dock 所在的那块屏：别的显示器全屏不影响本屏 Dock
    if (g.hwnd && MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST) != mon) return false;

    const RECT& mr = mi.rcMonitor;
    constexpr int kTol = 2;
    if (wr.left > mr.left + kTol || wr.top > mr.top + kTol ||
        wr.right < mr.right - kTol || wr.bottom < mr.bottom - kTol) return false;

    // 铺满整屏之后还要区分「真全屏」与「贴着整屏的普通最大化窗口」：
    // 任务栏开了自动隐藏时，Word / 浏览器最大化也会铺满 rcMonitor，不能因此藏 Dock。
    //   无标题栏（WS_POPUP）           → 全屏（游戏无边框 / F11 / 播放器 / PPT）
    //   有标题栏但置顶（WS_EX_TOPMOST）→ 也算全屏（少数游戏的实现方式）
    //   有标题栏且非置顶              → 普通最大化窗口，不隐藏
    {
        const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(fg, GWL_EXSTYLE);
        const bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
        const bool topmost = (exStyle & WS_EX_TOPMOST) != 0;
        if (hasCaption && !topmost) return false;
    }

    // 放到最后：cloaked 查询要走 DWM，只有"看起来是全屏"时才付这个代价，
    // 日常普通窗口在前台时这条路径根本不执行（600ms 轮询几乎零开销）。
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    return true;
}
// 是否是「桌面壳层」窗口（任务栏、开始菜单、托盘、桌面本身）
bool IsShellWindow(HWND h) {
    if (!h) return true;
    wchar_t cls[128]{};
    if (!GetClassNameW(h, cls, ARRAYSIZE(cls))) return true;
    return IsShellOrOverlayClass(cls);
}

void ShowDockAgain() {
    g.fsHidden = false;
    if (!g.hidden) {
        g.winShown = true;
        ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    }
    g.opacity = 0.0f;
    g.opacityTarget = 1.0f;
    StartAnimTimer();
    Render();
}

void ApplyFullscreenState(bool fs) {

    if (fs) {
        if (g.fsSuppressed) return;             // 本次全屏里用户手动唤出过，尊重用户
        if (!s.hideFullscreen || g.hidden) return;
        if (g.fsHidden) return;
        g.fsHidden = true;
        g.fsWnd = GetForegroundWindow();
        g.hover = -1;
        HideLabel();
        HidePopup();
        HideGroup();
        StopAnimTimer();
        ShowWindow(g.hwnd, SW_HIDE);
        Dbg(L"fullscreen on: dock hidden");
        return;
    }

    // 退出全屏：先恢复 Dock
    if (g.fsHidden) ShowDockAgain();

    // 解除抑制：真正切到了另一个普通应用窗口时（而不是任务栏/托盘这类壳层）。
    // 这样"在游戏里点托盘把 Dock 叫出来"不会被 600ms 轮询立刻收回去。
    if (g.fsSuppressed) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != g.fsWnd && !IsShellWindow(fg)) {
            g.fsSuppressed = false;
            g.fsWnd = nullptr;
        }
    }
}
void CheckFullscreenState() {
    ApplyFullscreenState(IsFullscreenForeground());
}

void CALLBACK FullscreenEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && g.hwnd) {
        PostMessageW(g.hwnd, kFullscreenCheckMsg, 0, 0);
    }
}

void StartFullscreenWatch() {
    if (!g.hwnd) return;
    if (!g.fsHook) {
        g.fsHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                   nullptr, FullscreenEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
    if (!g.fsTimerOn) {
        g.fsTimerOn = true;
        SetTimer(g.hwnd, kFullscreenTimerId, kFullscreenPollMs, nullptr);
    }
    CheckFullscreenState();
}

// ---------------------------------------------------------------- smart hide ---
// macOS 式"被挡住才收起"：只有当某个普通窗口和 Dock 所在区域重叠时，
// 才把 Dock 滑出屏幕（留 3px 热区）；鼠标触到该边缘时再滑回来。
// 判定只用 Z 序 + 矩形相交（GetTopWindow/GetWindowRect），不读别的进程任何数据。

bool IsWindowInDockArea(HWND h) {
    // 代价从低到高排列：矩形相交是最便宜的，先做它，
    // 只有真正压在 Dock 上的窗口才继续做类名 / DWM 查询（后者是跨进程同步调用，最贵）。
    if (!h || h == g.hwnd || (gLabel.hwnd && h == gLabel.hwnd)) return false;
    if (!IsWindowVisible(h) || IsIconic(h)) return false;

    RECT wr{};
    if (!GetWindowRect(h, &wr)) return false;
    if (wr.bottom <= wr.top || wr.right <= wr.left) return false;

    const RECT& d = g.baseRect;
    const int overlapX = std::min(wr.right, d.right) - std::max(wr.left, d.left);
    const int overlapY = std::min(wr.bottom, d.bottom) - std::max(wr.top, d.top);
    if (overlapX <= 8 || overlapY <= 8) return false;   // 只蹭到边的不算

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;

    wchar_t cls[128]{};
    if (!GetClassNameW(h, cls, ARRAYSIZE(cls))) return false;
    if (IsShellOrOverlayClass(cls)) return false;
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 || wcscmp(cls, L"SysListView32") == 0) return false;

    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;   // 输入法/通知等
    if (st & WS_CHILD) return false;
    // 必须有标题栏或是应用型窗口，过滤掉各种透明覆盖层
    if ((st & WS_CAPTION) != WS_CAPTION && !(ex & WS_EX_APPWINDOW) && !IsZoomed(h)) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
        return false;   // UWP 挂起 / 其它虚拟桌面：画面根本不可见

    return true;
}
bool AnyWindowOverDock() {
    // 扫一遍 Z 序：只要存在一个可见的普通窗口与 Dock 区域相交，就算"被挡住"。
    // Dock 自己是置顶的，所以不能扫到自己就停——那样永远找不到下面的普通窗口。
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (IsWindowInDockArea(h)) return true;
    }
    return false;
}

// 收起后的唤出热区：只有贴着屏幕边缘、且落在 Dock 长度范围内的窄条才算。
//
// 之前用的是 Dock 矩形四周 24px 的"扩边"，结果鼠标在浏览器/游戏里划到底部、
// 想点页面上的按钮时就会误唤起 Dock。现在改成：
//   · 只在 Dock 朝向的那条屏幕边缘；
//   · 厚度仅 4px（真正"撞到屏幕边"）；
//   · 还需停留 ~190ms，快速划过不会触发。
// keepAlive=true ：这是"已经弹出后别收回"的判定 —— 把屏幕边缘到 Dock 上沿那段空档也算进来，
//                  鼠标停留在空隙里时 Dock 保持展开。
// keepAlive=false：这是"从收起状态唤出"的判定 —— 只认真正贴边的 1px，
//                  必须把鼠标顶到屏幕最边缘才会弹出，不会提前触发。
bool CursorAtHotEdge(bool keepAlive = false) {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    const RECT d = g.baseRect;
    if (d.right <= d.left || d.bottom <= d.top) return false;
    if (MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST) !=
        MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST)) {
        return false;
    }

    const RECT area = DockArea();
    const bool grow = keepAlive && s.hotEdgeGrow;
    const int band = kHotEdgePx + (grow ? (kMargin + s.edgeOffset) : 0);
    switch (s.edge) {
    // 窗口内部"靠近边缘"永远不算，这样浏览器 / 游戏里点页面下方按钮时不会被抢走。
    case 0:  // 底部：贴屏幕最下方
        return p.y >= area.bottom - band &&
               p.x >= d.left && p.x < d.right;
    case 1:  // 顶部
        return p.y <= area.top + band &&
               p.x >= d.left && p.x < d.right;
    case 2:  // 左侧
        return p.x <= area.left + band &&
               p.y >= d.top && p.y < d.bottom;
    default: // 右侧
        return p.x >= area.right - band &&
               p.y >= d.top && p.y < d.bottom;
    }
}

// 光标是否顶到边缘后保持不动。"划过去"和"停在页面底部"都不会触发。
bool CursorHeldStill(const POINT& p) {
    return std::abs(p.x - g.edgeAnchorPt.x) <= kRevealStillPx &&
           std::abs(p.y - g.edgeAnchorPt.y) <= kRevealStillPx;
}

bool CursorInsideDock() {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    RECT r{};
    if (!GetWindowRect(g.hwnd, &r)) return false;
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

bool CursorInsidePanel() {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    auto inside = [&](HWND h) {
        if (!h || !IsWindowVisible(h)) return false;
        RECT r{};
        if (!GetWindowRect(h, &r)) return false;
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    };
    return inside(gPopup.hwnd) || inside(gGroup.hwnd);
}

void StartRevealWatch() {
    if (!g.hwnd || g.revealTimerOn) return;
    if (!s.edgeReveal) return;   // 关掉"贴边唤出"后完全不起定时器，零额外唤醒
    g.revealTimerOn = true;
    g.edgeArmed = false;
    g.edgeEnterTick = 0;
    g.revealLeaveTick = 0;
    g.edgeAnchorPt = {};
    // 停稳判定的采样精度跟随设定值：固定 100ms 对 300ms 的设定太粗糙
    const UINT revealPoll = (UINT)std::min<int>(100, std::max<int>(20, s.revealDwellMs / 4));
    SetTimer(g.hwnd, kRevealTimerId, revealPoll, nullptr);
}

void StopRevealWatch() {
    if (!g.revealTimerOn) return;
    g.revealTimerOn = false;
    g.edgeArmed = false;
    g.edgeEnterTick = 0;
    KillTimer(g.hwnd, kRevealTimerId);
}

void SetSmartTimerInterval(UINT intervalMs) {
    if (g.smartTimerOn) SetTimer(g.hwnd, kSmartTimerId, intervalMs, nullptr);
}

void RefreshRevealDeadline() {
    if (g.hoverRevealed) g.revealHideAt = GetTickCount() + (DWORD)s.retractMs;
}

void ClearRevealDeadline() {
    g.revealHideAt = 0;
    SetSmartTimerInterval(kSmartPollMs);
}

void SetAutoHidden(bool hide) {
    if (hide == g.autoHidden) return;
    g.autoHidden = hide;

    if (hide) {
        g.hoverRevealed = false;
        ClearRevealDeadline();
        g.hover = -1;
        HideLabel();
        HidePopup();
        HideGroup();
        StartRevealWatch();
        g.opacityTarget = 0.0f;     // 先淡出，滑出到位后由动画 SW_HIDE
    } else {
        if (!g.winShown) {          // 从完全隐藏状态回来：先显示窗口再滑入
            g.winShown = true;
            ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
        }
        g.slide = 1.0f;             // 从屏幕外开始滑入
        g.opacity = 0.0f;
        g.opacityTarget = 1.0f;
        StopRevealWatch();
        PositionDock();
        if (g.hoverRevealed) {
            RefreshRevealDeadline();
            SetSmartTimerInterval(kSmartRevealPollMs);
        }
    }
    StartAnimTimer();
    Dbg(L"smart hide=%d", hide ? 1 : 0);
}

// 是否应当自动收起：常隐模式下无条件收起；否则只在被窗口遮挡时收起。
static bool ShouldAutoHide() {
    if (s.alwaysHide) return true;
    return s.smartHide && AnyWindowOverDock();
}

// 700ms 轮询只负责"是否被遮挡 → 自动回正"。
// 边缘唤出改成独立的 80ms 热区检测（kRevealTimerId），两者互不拖累。
void UpdateSmartHide() {
    if (!g.hwnd || g.hidden || g.fsHidden) return;
    if (g.menuOpen) return;

    if (g.autoHidden) {
        // ① 自动回正：遮挡消失了就回来
        const bool blocked = ShouldAutoHide();
        if (!blocked) {
            g.hoverRevealed = false;
            SetAutoHidden(false);
            return;
        }
        // ② 鼠标停在边缘热区 → 由 UpdateReveal 负责临时浮出
        return;
    }

    // 临时浮出后：鼠标在 Dock / 面板内、或仍停在边缘热区，都算"人还在"，续期；
    // 离开满 retractMs 才重新收起。（后者是为了 edgeOffset > 0 时鼠标停在屏幕边缘、
    // 而 Dock 展开在其下方的那段空档——否则会出现"收起→唤出→收起"的死循环。）
    if (g.hoverRevealed) {
        const DWORD now = GetTickCount();
        if (CursorInsideDock() || CursorInsidePanel() || CursorAtHotEdge(true)) {
            g.revealHideAt = now + (DWORD)s.retractMs;
            return;
        }
        if (g.revealHideAt && now < g.revealHideAt) return;
        g.hoverRevealed = false;
        ClearRevealDeadline();
        if (ShouldAutoHide()) SetAutoHidden(true);
        return;
    }

    if (ShouldAutoHide()) SetAutoHidden(true);
}

// 边缘唤出：100ms 采样，鼠标必须"顶到屏幕边缘 + 基本不动"满 kRevealDwellMs 才浮出；
// 只要在停留过程中移动超过 3px 就重新计时，所以快速划向下方按钮不会误触。
void UpdateReveal() {
    if (!g.hwnd || g.hidden || g.fsHidden) return;
    if (!g.autoHidden && !g.hoverRevealed) {
        StopRevealWatch();
        return;
    }
    if (!s.edgeReveal) {
        StopRevealWatch();
        return;
    }

    POINT p{};
    GetCursorPos(&p);
    const bool onEdge = CursorAtHotEdge();
    const DWORD now = GetTickCount();
    const bool cooling = g.revealCooldownUntil && now < g.revealCooldownUntil;

    // 顶到边缘还在点击（例如最大化窗口里点最下方的按钮）：判定为"要用下面的应用"，
    // 立刻进入冷却，不抢这次点击。
    if (onEdge && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        g.revealCooldownUntil = now + kRevealCooldownMs;
        g.edgeArmed = false;
        g.edgeEnterTick = 0;
        return;
    }

    if (onEdge && !cooling) {
        g.revealLeaveTick = 0;
        if (!g.edgeArmed) {
            g.edgeArmed = true;
            g.edgeEnterTick = now;
            g.edgeAnchorPt = p;
        } else if (!CursorHeldStill(p)) {
            // 在边缘上还在滑动：重新计时，不唤出
            g.edgeAnchorPt = p;
            g.edgeEnterTick = now;
        } else if (now - g.edgeEnterTick >= (DWORD)s.revealDwellMs) {
            if (!g.hoverRevealed) {
                g.hoverRevealed = true;
                SetAutoHidden(false);
                RefreshRevealDeadline();
                SetSmartTimerInterval(kSmartRevealPollMs);
            }
        }
        return;
    }

    g.edgeArmed = false;
    g.edgeEnterTick = 0;
    if (CursorInsideDock() || CursorInsidePanel()) {
        g.revealLeaveTick = 0;
        return;
    }
    if (g.revealLeaveTick == 0) g.revealLeaveTick = now;
    if (g.revealLeaveTick && now - g.revealLeaveTick >= kRevealLeaveMs) {
        if (g.autoHidden) return;   // 已收起，等下一个边缘停留
        g.revealLeaveTick = 0;
        if (g.hoverRevealed) return;   // 交给 3 秒无操作计时器
        if (ShouldAutoHide()) SetAutoHidden(true);
    }
}
void StartSmartWatch() {
    if (!g.hwnd || g.smartTimerOn) return;
    g.smartTimerOn = true;
    SetTimer(g.hwnd, kSmartTimerId, kSmartPollMs, nullptr);
}

void StopSmartWatch() {
    if (g.smartTimerOn) {
        g.smartTimerOn = false;
        KillTimer(g.hwnd, kSmartTimerId);
    }
    g.revealHideAt = 0;
    if (g.autoHidden) SetAutoHidden(false);
    g.hoverRevealed = false;
    StopRevealWatch();
}
void StopFullscreenWatch() {
    if (g.fsHook) {
        UnhookWinEvent(g.fsHook);
        g.fsHook = nullptr;
    }
    if (g.fsTimerOn) {
        g.fsTimerOn = false;
        KillTimer(g.hwnd, kFullscreenTimerId);
    }
    g.fsSuppressed = false;
    g.fsWnd = nullptr;
    if (g.fsHidden) ShowDockAgain();
    Dbg(L"fullscreen watch stopped");
}

void ToggleVisibility() {
    if (g.autoHidden) {         // 被遮挡自动收起中：点托盘 = 唤出（并临时保持）
        g.hoverRevealed = true;
        SetAutoHidden(false);
        RefreshRevealDeadline();
        SetSmartTimerInterval(kSmartRevealPollMs);
        return;
    }
    if (g.fsHidden) {           // 全屏中被自动隐藏：手动唤出，本次全屏内不再自动隐藏
        g.fsSuppressed = true;
        ShowDockAgain();
        return;
    }
    g.hidden = !g.hidden;
    if (g.hidden) {
        HideLabel();
        HidePopup();
        HideGroup();
    }
    if (g.hidden) {
        ShowWindow(g.hwnd, SW_HIDE);
    } else {
        g.winShown = true;
        ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
        g.opacity = 0.0f;
        g.opacityTarget = 1.0f;
        StartAnimTimer();
        Render();
    }
}
// -------------------------------------------------------------------- tray ---
HICON CreateTrayIcon() {
    const int S = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return LoadIconW(nullptr, IDI_APPLICATION);

    auto* px = static_cast<uint32_t*>(bits);
    const float r = 7.0f;
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const float dx = std::max(std::max(r - x, 0.0f), static_cast<float>(x) - (S - 1 - r));
            const float dy = std::max(std::max(r - y, 0.0f), static_cast<float>(y) - (S - 1 - r));
            const bool inside = (dx * dx + dy * dy) <= r * r;
            uint32_t c = 0;
            if (inside) {
                c = 0xFFF0F3F8;
                for (int k = 0; k < 3; ++k) {
                    const float cx = 9.0f + k * 7.0f;
                    const float cy = 16.0f;
                    const float ddx = x - cx;
                    const float ddy = y - cy;
                    if (ddx * ddx + ddy * ddy <= 6.0f) c = 0xFF232A36;
                }
            }
            px[y * S + x] = c;
        }
    }

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void AddTrayIcon() {
    if (!g.trayIcon) {
        g.trayIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (!g.trayIcon) g.trayIcon = CreateTrayIcon();
    }

    g.nid = {};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.nid.uCallbackMessage = kTrayMsg;
    g.nid.hIcon = g.trayIcon;
    wcscpy_s(g.nid.szTip, L"Dock（左键显示/隐藏，右键菜单）");
    Shell_NotifyIconW(NIM_ADD, &g.nid);
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
}

void RemoveTrayIcon() {
    if (g.nid.cbSize) {
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        g.nid.cbSize = 0;
    }
}

// -------------------------------------------------------------------- menu ---
HMENU BuildMenu(int itemIndex) {
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, 0, (g.hidden || g.fsHidden || g.autoHidden) ? L"显示 Dock" : L"隐藏 Dock");

    HMENU pos = CreatePopupMenu();
    AppendMenuW(pos, MF_STRING | (s.edge == 0 ? MF_CHECKED : 0), kCmdEdgeBottom, L"底部");
    AppendMenuW(pos, MF_STRING | (s.edge == 1 ? MF_CHECKED : 0), kCmdEdgeTop, L"顶部");
    AppendMenuW(pos, MF_STRING | (s.edge == 2 ? MF_CHECKED : 0), kCmdEdgeLeft, L"左侧");
    AppendMenuW(pos, MF_STRING | (s.edge == 3 ? MF_CHECKED : 0), kCmdEdgeRight, L"右侧");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pos), L"位置");

    HMENU appearance = CreatePopupMenu();
    AppendMenuW(appearance, MF_STRING | (s.material == 0 ? MF_CHECKED : 0), kCmdMatAcrylic, L"毛玻璃");
    AppendMenuW(appearance, MF_STRING | (s.material == 1 ? MF_CHECKED : 0), kCmdMatTransparent, L"透明");
    AppendMenuW(appearance, MF_STRING | (s.material == 2 ? MF_CHECKED : 0), kCmdMatDark, L"深色实心");
    AppendMenuW(appearance, MF_STRING | (s.material == 3 ? MF_CHECKED : 0), kCmdMatLight, L"浅色实心");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), L"外观");

    HMENU sizes = CreatePopupMenu();
    AppendMenuW(sizes, MF_STRING | (s.iconSize == 36 ? MF_CHECKED : 0), kCmdSizeSmall, L"小");
    AppendMenuW(sizes, MF_STRING | (s.iconSize == 44 ? MF_CHECKED : 0), kCmdSizeMedium, L"中");
    AppendMenuW(sizes, MF_STRING | (s.iconSize == 52 ? MF_CHECKED : 0), kCmdSizeLarge, L"大");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizes), L"图标大小");
    AppendMenuW(menu, MF_STRING | (s.reflection ? MF_CHECKED : 0), kCmdReflection, L"图标倒影");
    AppendMenuW(menu, MF_STRING | (g.taskbarHidden ? MF_CHECKED : 0), kCmdTaskbar, L"隐藏系统任务栏");
    AppendMenuW(menu, MF_STRING | (s.hideFullscreen ? MF_CHECKED : 0), kCmdHideFullscreen, L"全屏应用时自动隐藏");
    AppendMenuW(menu, MF_STRING | (s.smartHide ? MF_CHECKED : 0), kCmdSmartHide, L"被窗口挡住时自动收起");
    AppendMenuW(menu, MF_STRING | (s.edgeReveal ? MF_CHECKED : 0), kCmdEdgeReveal, L"贴边唤出（需停稳）");
    AppendMenuW(menu, MF_STRING | (s.alwaysHide ? MF_CHECKED : 0), kCmdAlwaysHide, L"常隐（只在贴边时出现）");

    // 时序与边缘留白：父项标题直接显示当前生效值，这样 ini 里填的非档位数值也能看见
    {
        static const int vals[] = { 80, 150, 300, 450 };
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(vals); ++i) {
            AppendMenuW(sub, MF_STRING | (s.revealDwellMs == vals[i] ? MF_CHECKED : 0),
                        kCmdDwellBase + i, (std::to_wstring(vals[i]) + L" ms").c_str());
        }
        const std::wstring title = L"停稳唤出：" + std::to_wstring(s.revealDwellMs) + L" ms";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
    }
    {
        static const int vals[] = { 300, 500, 750, 1000, 1250, 1500 };
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(vals); ++i) {
            AppendMenuW(sub, MF_STRING | (s.retractMs == vals[i] ? MF_CHECKED : 0),
                        kCmdRetractBase + i, (std::to_wstring(vals[i]) + L" ms").c_str());
        }
        const std::wstring title = L"无操作收回：" + std::to_wstring(s.retractMs) + L" ms";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
    }
    {
        static const int vals[] = { 0, 8, 16, 20, 24, 28, 32, 36, 48, 64 };
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(vals); ++i) {
            AppendMenuW(sub, MF_STRING | (s.edgeOffset == vals[i] ? MF_CHECKED : 0),
                        kCmdOffsetBase + i, (std::to_wstring(vals[i]) + L" px").c_str());
        }
        const std::wstring title = L"距屏幕边缘：" + std::to_wstring(s.edgeOffset) + L" px";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
    }
    AppendMenuW(menu, MF_STRING | (s.hotEdgeGrow ? MF_CHECKED : 0), kCmdHotEdgeGrow, L"边缘空隙保持展开");

    // 悬停标签：背景黑白、不透明度档位（含自定义）、文字描边
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (s.labelBg == 0 ? MF_CHECKED : 0), kCmdLabelBgBase + 0, L"黑色");
        AppendMenuW(sub, MF_STRING | (s.labelBg == 1 ? MF_CHECKED : 0), kCmdLabelBgBase + 1, L"白色");
        const std::wstring title = (s.labelBg == 1) ? L"标签背景：白色" : L"标签背景：黑色";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
    }
    {
        static const int pcts[] = { 0, 20, 40, 60, 80, 100 };
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < ARRAYSIZE(pcts); ++i) {
            AppendMenuW(sub, MF_STRING | (s.labelAlphaPct == pcts[i] ? MF_CHECKED : 0),
                        kCmdLabelAlphaBase + i, (std::to_wstring(pcts[i]) + L"%").c_str());
        }
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, kCmdLabelAlphaCustom, L"自定义…");
        const std::wstring title = L"标签不透明度：" + std::to_wstring(s.labelAlphaPct) + L"%";
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
    }
    AppendMenuW(menu, MF_STRING | (s.labelOutline ? MF_CHECKED : 0), kCmdLabelOutline, L"标签文字描边");

    HMENU modules = CreatePopupMenu();
    AppendMenuW(modules, MF_STRING | (s.showClock ? MF_CHECKED : 0), kCmdShowClock, L"日期时间");
    AppendMenuW(modules, MF_STRING | (s.showCenter ? MF_CHECKED : 0), kCmdShowCenter, L"九宫格设置");
    AppendMenuW(modules, MF_STRING | (s.showAdd ? MF_CHECKED : 0), kCmdShowAdd, L"「＋」添加按钮");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modules), L"内置图标");

    AppendMenuW(menu, MF_STRING | (s.autoStart ? MF_CHECKED : 0), kCmdAutoStart, L"开机自启");
    AppendMenuW(menu, MF_STRING, kCmdOpenTray, L"打开系统托盘");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (itemIndex >= 0 && itemIndex < ItemCount()) {
        const std::wstring label = L"移除「" + FileNameOf(g.items[static_cast<size_t>(itemIndex)]) + L"」";
        AppendMenuW(menu, MF_STRING, kCmdRemoveBase + itemIndex, label.c_str());
    }
    AppendMenuW(menu, MF_STRING, kCmdAdd, L"添加应用…");
    AppendMenuW(menu, MF_STRING, kCmdAddFolder, L"添加文件夹…");
    AppendMenuW(menu, MF_STRING, kCmdRefreshIcons, L"刷新图标");
    AppendMenuW(menu, MF_STRING, kCmdOpenFolder, L"打开程序所在文件夹");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");

    return menu;
}

// ------------------------------------------------------------------ prompt ---
// 极简模态输入框：用系统 EDIT / BUTTON 类现场创建，不需要 .rc 资源模板。
struct PromptState {
    HWND edit = nullptr;
    int  value = 0, lo = 0, hi = 100;
    bool done = false, ok = false;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PromptState* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_COMMAND:
        if (!st) return 0;
        if (LOWORD(wp) == 1) {                       // 确定
            wchar_t buf[16]{};
            GetWindowTextW(st->edit, buf, 16);
            int v = 0;                               // 手写解析，避免依赖 _wtoi
            for (const wchar_t* p = buf; *p >= L'0' && *p <= L'9'; ++p) v = v * 10 + (*p - L'0');
            if (v < st->lo) v = st->lo;
            if (v > st->hi) v = st->hi;
            st->value = v;
            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == 2) {                // 取消
            st->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 返回用户输入值；取消返回 -1
int PromptInt(HWND owner, const wchar_t* caption, const wchar_t* hint, int defVal, int lo, int hi) {
    static bool once = false;
    if (!once) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DockPromptWindow";
        RegisterClassExW(&wc);
        once = true;
    }

    PromptState st;
    st.value = defVal;
    st.lo = lo;
    st.hi = hi;

    const float sc = (g.dpiScale > 0.0f) ? g.dpiScale : 1.0f;
    auto S = [sc](int v) { return static_cast<int>(v * sc); };

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    const int cw = S(320), chh = S(160);
    const int cx = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - cw) / 2;
    const int cy = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - chh) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"DockPromptWindow", caption,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, cx, cy, cw, chh,
                               owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return -1;

    CreateWindowExW(0, L"STATIC", hint, WS_CHILD | WS_VISIBLE,
                    S(18), S(16), S(280), S(20), dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(defVal).c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                              S(18), S(42), S(110), S(26), dlg, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                    S(210), S(104), S(90), S(30), dlg, reinterpret_cast<HMENU>(1),
                    GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    S(112), S(104), S(90), S(30), dlg, reinterpret_cast<HMENU>(2),
                    GetModuleHandleW(nullptr), nullptr);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    if (st.edit) SetFocus(st.edit);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    return st.ok ? st.value : -1;
}

void HandleCommand(int cmd) {
    // 时序 / 边缘留白：档位命令，改完立即生效（不需要重启）
    // 必须放在下面那条 "移除第 N 项" 的兜底判断之前——档位 ID 是 300+，会被
    // cmd >= kCmdRemoveBase(100) 误吞掉，表现为"点了菜单完全没反应"。
    {
        static const int dwellVals[] = { 80, 150, 300, 450 };
        static const int retractVals[] = { 300, 500, 750, 1000, 1250, 1500 };
        static const int offsetVals[] = { 0, 8, 16, 20, 24, 28, 32, 36, 48, 64 };

        if (cmd >= kCmdDwellBase && cmd < kCmdDwellBase + ARRAYSIZE(dwellVals)) {
            s.revealDwellMs = dwellVals[cmd - kCmdDwellBase];
            SaveSettings();
            // 热区采样间隔依赖 dwell，重开一次让它重算
            StopRevealWatch();
            if (g.autoHidden) StartRevealWatch();
            RefreshPopupIfShown();
            return;
        }
        if (cmd >= kCmdRetractBase && cmd < kCmdRetractBase + ARRAYSIZE(retractVals)) {
            s.retractMs = retractVals[cmd - kCmdRetractBase];
            SaveSettings();
            RefreshPopupIfShown();
            return;
        }
        if (cmd >= kCmdOffsetBase && cmd < kCmdOffsetBase + ARRAYSIZE(offsetVals)) {
            s.edgeOffset = offsetVals[cmd - kCmdOffsetBase];
            SaveSettings();
            PositionDock();      // 立即重排到位
            Render();
            RefreshPopupIfShown();
            return;
        }

        // 悬停标签：背景黑白 / 不透明度档位 / 自定义
        if (cmd >= kCmdLabelBgBase && cmd < kCmdLabelBgBase + 2) {
            s.labelBg = cmd - kCmdLabelBgBase;
            SaveSettings();
            RefreshLabelStyle();
            RefreshPopupIfShown();
            return;
        }
        if (cmd >= kCmdLabelAlphaBase && cmd < kCmdLabelAlphaBase + 6) {
            static const int pcts[] = { 0, 20, 40, 60, 80, 100 };
            s.labelAlphaPct = pcts[cmd - kCmdLabelAlphaBase];
            SaveSettings();
            RefreshLabelStyle();
            RefreshPopupIfShown();
            return;
        }
        if (cmd == kCmdLabelAlphaCustom) {
            const int v = PromptInt(g.hwnd, L"标签不透明度", L"请输入 0 – 100 的整数（%）：",
                                    s.labelAlphaPct, 0, 100);
            if (v >= 0) {
                s.labelAlphaPct = v;
                SaveSettings();
                RefreshLabelStyle();
                RefreshPopupIfShown();
            }
            return;
        }
    }

    // 「移除第 N 项」：编号从 100 起，上界卡在档位区之前，避免吞掉 300+ 的命令
    if (cmd >= kCmdRemoveBase && cmd < kCmdDwellBase) {
        RemoveItem(cmd - kCmdRemoveBase);
        return;
    }
    switch (cmd) {
    case kCmdToggle: ToggleVisibility(); break;
    case kCmdLabelOutline:
        s.labelOutline = !s.labelOutline;
        SaveSettings();
        RefreshLabelStyle();
        RefreshPopupIfShown();
        break;
    case kCmdEdgeBottom:
    case kCmdEdgeTop:
    case kCmdEdgeLeft:
    case kCmdEdgeRight:
        s.edge = cmd - kCmdEdgeBottom;
        Dbg(L"cmd edge=%d", s.edge);
        SaveSettings();
        UpdateWindowGeometry();
        Render();
        if (gPopup.shown) { PositionPopup(); RenderPopup(); }
        if (gGroup.shown) { PositionGroup(); RenderGroup(); }
        break;
    case kCmdMatAcrylic:
    case kCmdMatTransparent:
    case kCmdMatDark:
    case kCmdMatLight:
        s.material = cmd - kCmdMatAcrylic;
        SaveSettings();
        ApplyMaterial();
        Render();
        RefreshPopupIfShown();
        if (gGroup.shown) RenderGroup();
        break;
    case kCmdSizeSmall:
    case kCmdSizeMedium:
    case kCmdSizeLarge:
        s.iconSize = (cmd == kCmdSizeSmall) ? 36 : (cmd == kCmdSizeMedium) ? 44 : 52;
        Dbg(L"cmd size=%d", s.iconSize);
        SaveSettings();
        RefreshAfterItemChange();
        RefreshPopupIfShown();
        if (gGroup.shown) { ResizeGroupTarget(); PositionGroup(); RenderGroup(); }
        break;
    case kCmdAutoStart:
        s.autoStart = !s.autoStart;
        SaveSettings();
        ApplyAutoStart();
        RefreshPopupIfShown();
        break;
    case kCmdReflection:
        s.reflection = !s.reflection;
        SaveSettings();
        UpdateWindowGeometry();
        Render();
        RefreshPopupIfShown();
        break;
    case kCmdTaskbar:
        SetTaskbarHidden(!g.taskbarHidden);
        RefreshPopupIfShown();
        break;
    case kCmdSmartHide:
        s.smartHide = !s.smartHide;
        SaveSettings();
        g.hoverRevealed = false;
        ClearRevealDeadline();
        if (!s.smartHide) SetAutoHidden(false);
        else UpdateSmartHide();
        RefreshPopupIfShown();
        Render();
        break;
    case kCmdEdgeReveal:
        s.edgeReveal = !s.edgeReveal;
        SaveSettings();
        g.revealCooldownUntil = 0;
        if (!s.edgeReveal) StopRevealWatch();
        else if (g.autoHidden) StartRevealWatch();
        RefreshPopupIfShown();
        break;
    case kCmdAlwaysHide:
        s.alwaysHide = !s.alwaysHide;
        SaveSettings();
        g.hoverRevealed = false;
        ClearRevealDeadline();
        UpdateSmartHide();
        RefreshPopupIfShown();
        break;
    case kCmdHotEdgeGrow:
        s.hotEdgeGrow = !s.hotEdgeGrow;
        SaveSettings();
        RefreshPopupIfShown();
        break;
    case kCmdHideFullscreen:
        s.hideFullscreen = !s.hideFullscreen;
        SaveSettings();
        // 关闭时连定时器/钩子一起摘掉：不开这个功能就完全不产生周期唤醒
        if (s.hideFullscreen) {
            StartFullscreenWatch();
        } else {
            StopFullscreenWatch();
        }
        RefreshPopupIfShown();
        Render();
        break;
    case kCmdAdd: AddViaDialog(); break;
    case kCmdAddFolder: AddFolderViaDialog(); break;
    case kCmdShowClock:
        s.showClock = !s.showClock;
        SaveSettings();
        g.cacheBox = -1;
        RefreshAfterItemChange();
        if (gGroup.shown) { PositionGroup(); RenderGroup(); }
        RefreshPopupIfShown();
        break;
    case kCmdShowCenter:
        s.showCenter = !s.showCenter;
        SaveSettings();
        g.cacheBox = -1;
        RefreshAfterItemChange();
        if (gGroup.shown) { PositionGroup(); RenderGroup(); }
        RefreshPopupIfShown();
        break;
    case kCmdShowAdd:
        s.showAdd = !s.showAdd;
        SaveSettings();
        g.cacheBox = -1;
        RefreshAfterItemChange();
        if (gGroup.shown) { PositionGroup(); RenderGroup(); }
        RefreshPopupIfShown();
        break;
    case kCmdRefreshIcons: RefreshAfterItemChange(); break;
    case kCmdOpenFolder: OpenConfigFolder(); break;
    case kCmdOpenTray: OpenSystemTray(); break;
    case kCmdExit:
        RemoveTrayIcon();
        DestroyWindow(g.hwnd);
        break;
    default: break;
    }
}

void ShowMenu(int itemIndex) {
    if (itemIndex >= ItemCount()) itemIndex = -1;  // 「＋」位不提供“移除”

    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = BuildMenu(itemIndex);
    g.menuOpen = true;
    if (g.hoverRevealed) RefreshRevealDeadline();
    SetForegroundWindow(g.hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   pt.x, pt.y, 0, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(menu);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);

    if (cmd) HandleCommand(cmd);
    if (g.hoverRevealed) RefreshRevealDeadline();
}
// ------------------------------------------------------------------ wndproc ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.taskbarCreatedMsg && msg == g.taskbarCreatedMsg) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[MAX_PATH * 2]{};
            if (DragQueryFileW(drop, i, buf, ARRAYSIZE(buf))) AddPath(buf);
        }
        DragFinish(drop);
        SaveItems();
        RefreshAfterItemChange();
        return 0;
    }

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);

        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        if (g.pressIndex >= 0) {
            if (!g.dragging) {
                const int dx = x - g.pressPt.x;
                const int dy = y - g.pressPt.y;
                if (dx * dx + dy * dy >= 16) {   // 4px 阈值，避免点击被误判为拖动
                    g.dragging = true;
                    Dbg(L"drag start %d", g.pressIndex);
                    SetHover(-1);
                }
            }
            if (g.dragging) {
                const int target = HitTest(x, y);
                if (target >= 0 && target < ItemCount() && target != g.pressIndex) {
                    MoveItem(g.pressIndex, target);
                    g.pressIndex = target;
                    SetHover(target);
                    Render();
                }
                return 0;
            }
        }

        SetHover(HitTest(x, y));
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g.pressIndex < 0) SetHover(-1);
        return 0;

    case WM_LBUTTONDOWN: {
        const int idx = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        Dbg(L"lbtn down idx=%d", idx);
        g.suppressClick = false;
        if (idx >= 0 && idx < ItemCount()) {
            g.pressIndex = idx;
            g.dragging = false;
            g.pressPt.x = GET_X_LPARAM(lp);
            g.pressPt.y = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // 捕获被系统收回（例如拖出窗口后松开鼠标）：保存排序，并抑制随后的误点击启动。
        if (g.pressIndex >= 0) {
            g.suppressClick = true;
            if (g.dragging) SaveItems();
        }
        g.pressIndex = -1;
        g.dragging = false;
        return 0;

    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const int idx = HitTest(x, y);

        // 文件夹面板开着时，点任何"非文件夹类"的槽位（其他应用 / 九宫格 / 加号）
        // 先把面板收掉。必须放在抬起时而不是按下时：按下就收会让 gGroup.shown 变 false，
        // 等到抬起时 ActivateItem 会误判成"重新展开"，把双击打开的语义顶掉。
        if (gGroup.shown && idx >= 0 && !IsDockItemGroup(idx)) HideGroup();

        if (g.pressIndex < 0 && g.suppressClick) {
            g.suppressClick = false;
            Render();
            return 0;
        }

        if (g.pressIndex >= 0) {
            const int pressed = g.pressIndex;
            const bool dragged = g.dragging;
            g.pressIndex = -1;
            g.dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();

            if (dragged) {
                SaveItems();     // 排序结果落盘
                Render();
                return 0;
            }
            if (idx == pressed) {
                ActivateItem(idx);
            }
            return 0;
        }

        if (idx >= 0 && idx == CenterIndex()) {
            TogglePopup();
        } else if (idx >= 0 && idx == AddIndex()) {
            AddViaDialog();
        } else if (idx >= 0 && idx < ItemCount()) {
            ActivateItem(idx);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        ShowMenu(HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;

    case WM_TIMER:
        if (wp == kAnimTimerId) {
            const bool moving = StepAnimation();
            Render();
            if (!moving) StopAnimTimer();
            return 0;
        }
        if (wp == kFullscreenTimerId) {
            CheckFullscreenState();
            return 0;
        }
        if (wp == kSmartTimerId) {
            UpdateSmartHide();
            return 0;
        }
        if (wp == kRevealTimerId) {
            UpdateReveal();
            return 0;
        }
        if (wp == kClockTimerId) {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            // 分钟变化才重绘；跨天时也刷新日期。Dock 不可见时不白跑一次绘制。
            if (st.wMinute != g.clockMinute || st.wDay != g.clockDay) {
                g.clockMinute = st.wMinute;
                g.clockDay = st.wDay;
                if (!g.hidden && !g.fsHidden && !g.autoHidden) {
                    Render();
                    RefreshPopupIfShown();
                }
            }
            return 0;
        }
        if (wp == kPopupWatchTimerId) {
            if (!gPopup.shown) return 0;
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (esc && !gPopup.prevEscapeDown) HidePopup();
            if (down && !gPopup.prevMouseDown) {
                POINT p{};
                GetCursorPos(&p);
                RECT pr{}, dr{};
                GetWindowRect(gPopup.hwnd, &pr);
                GetWindowRect(g.hwnd, &dr);
                const bool inPopup = p.x >= pr.left && p.x < pr.right && p.y >= pr.top && p.y < pr.bottom;
                const bool inDock = p.x >= dr.left && p.x < dr.right && p.y >= dr.top && p.y < dr.bottom;
                if (!inPopup && !inDock) HidePopup();
            }
            gPopup.prevMouseDown = down;
            gPopup.prevEscapeDown = esc;
            return 0;
        }
        if (wp == kPopupAnimTimerId) {
            StepPopupAnim();
            return 0;
        }
        if (wp == kGroupAnimTimerId) {
            StepGroupAnim();
            return 0;
        }
        if (wp == kGroupWatchTimerId) {
            if (!gGroup.shown) return 0;
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (esc && !gGroup.prevEscapeDown) HideGroup();
            if (down && !gGroup.prevMouseDown) {
                POINT p{};
                GetCursorPos(&p);
                RECT gr{}, dr{};
                GetWindowRect(gGroup.hwnd, &gr);
                GetWindowRect(g.hwnd, &dr);
                const bool inGroup = p.x >= gr.left && p.x < gr.right && p.y >= gr.top && p.y < gr.bottom;
                const bool inDock = p.x >= dr.left && p.x < dr.right && p.y >= dr.top && p.y < dr.bottom;
                if (!inGroup && !inDock) HideGroup();
            }
            gGroup.prevMouseDown = down;
            gGroup.prevEscapeDown = esc;
            return 0;
        }
        if (wp == kSystemTrayTimerId) {
            if (!g.trayTimerOn) return 0;
            const HWND overflow = g.trayOverflow ? g.trayOverflow
                                                  : FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
            if (overflow) g.trayOverflow = overflow;
            const bool visible = overflow && IsWindowVisible(overflow);
            Dbg(L"tray poll overflow=0x%p visible=%d seen=%d", overflow, visible ? 1 : 0,
                g.trayOverflowSeen ? 1 : 0);
            if (visible) g.trayOverflowSeen = true;
            const bool timedOut = (GetTickCount() - g.trayPeekStart) > 15000;
            if ((g.trayOverflowSeen && !visible) || timedOut) {
                if (g.trayPeek) {
                    ShowWindow(FindWindowW(L"Shell_TrayWnd", nullptr), SW_HIDE);
                    g.trayPeek = false;
                }
                g.trayOverflowSeen = false;
                g.trayOverflow = nullptr;
                g.trayTimerOn = false;
                KillTimer(g.hwnd, kSystemTrayTimerId);
            }
            return 0;
        }
        break;

    case kFullscreenCheckMsg:
        CheckFullscreenState();
        return 0;

    case WM_SIZE: {
        const UINT w = LOWORD(lp);
        const UINT h = HIWORD(lp);
        if (!g.swap || w == 0 || h == 0) return 0;
        Dbg(L"wm_size %u x %u edge=%d vert=%d size=%d", w, h, s.edge, Vertical() ? 1 : 0, s.iconSize);
        g.width = w;
        g.height = h;

        // 关键：D2D 设备上下文仍持有上一帧 target bitmap 的引用，
        // 若不解绑，ResizeBuffers 会以 DXGI_ERROR_INVALID_CALL 失败，
        // 于是窗口尺寸变了、画面却一直停在旧帧（新旧尺寸混剪：图标溢出 / 只剩一个）。
        if (g.dc) {
            g.dc->SetTarget(nullptr);
            g.dc->Flush();
        }
        g.brush.Reset();
        g.targetBitmap.Reset();

        HRESULT hr = g.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            Dbg(L"resizebuffers fail hr=0x%08X", static_cast<unsigned>(hr));
            return 0;
        }
        hr = CreateTargetBitmap();
        if (FAILED(hr)) {
            Dbg(L"targetbitmap fail hr=0x%08X", static_cast<unsigned>(hr));
            return 0;
        }
        // 交换链尺寸变了以后重新提交合成树，让 DComp 立即采用新内容尺寸。
        if (g.compDevice) g.compDevice->Commit();
        UpdateWindowRegion();
        Render();
        return 0;
    }

    case WM_COMMAND: {
        // 允许脚本 / 快捷键发送菜单命令（例如 WM_COMMAND + kCmdEdgeRight）
        if (HIWORD(wp) == 0) {
            HandleCommand(LOWORD(wp));
            return 0;
        }
        break;
    }

    case WM_DISPLAYCHANGE:
        UpdateWindowGeometry();
        Render();
        CheckFullscreenState();
        return 0;

    case WM_DPICHANGED: {
        g.dpiScale = static_cast<float>(HIWORD(wp)) / 96.0f;
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ReloadIcons();
        UpdateWindowGeometry();
        Render();
        return 0;
    }

    case kTrayMsg:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ToggleVisibility();
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu(-1);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        StopSmartWatch();
        StopFullscreenWatch();
        HidePopup();
        if (gPopup.hwnd) DestroyWindow(gPopup.hwnd);
        HideGroup();
        if (gGroup.hwnd) DestroyWindow(gGroup.hwnd);
        if (g.clockTimerOn) {
            g.clockTimerOn = false;
            KillTimer(g.hwnd, kClockTimerId);
        }
        if (g.taskbarHidden) SetTaskbarHidden(false);
        StopAnimTimer();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--debug")) gDebug = true;

    if (auto setCtx = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
        const BOOL ok = setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Dbg(L"setdpi=%d err=%lu sysdpi=%u", ok, GetLastError(), GetDpiForSystem());
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    LoadSettings();
    LoadItems();
    Dbg(L"start items=%zu material=%d edge=%d size=%d",
        g.items.size(), s.material, s.edge, s.iconSize);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    g.dpiScale = 1.0f;
    g.width = BoxWidth();
    g.height = BoxHeight();

    g.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kClassName, L"Dock", WS_POPUP,
        0, 0, static_cast<int>(g.width), static_cast<int>(g.height),
        nullptr, nullptr, instance, nullptr);
    if (!g.hwnd) return 1;

    DragAcceptFiles(g.hwnd, TRUE);

    // 自愈：若上次异常退出时任务栏被藏掉了，启动时恢复，避免系统残留"任务栏不见了"
    if (HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        if (!IsWindowVisible(tb)) ShowWindow(tb, SW_SHOW);
    }

    g.dpiScale = static_cast<float>(GetDpiForWindow(g.hwnd)) / 96.0f;
    UpdateWindowGeometry();
    Dbg(L"geom scale=%.2f box=%u x %u slots=%d size=%d", g.dpiScale, g.width, g.height, SlotCount(), s.iconSize);

    if (FAILED(CreateDeviceResources())) {
        MessageBoxW(g.hwnd, L"图形设备初始化失败（D3D11 / D2D / DComp）。", L"Dock", MB_ICONERROR);
        DestroyWindow(g.hwnd);
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }

    ReloadIcons();
    ApplyMaterial();
    AddTrayIcon();

    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    g.opacity = 0.0f;
    StartAnimTimer();
    Render();
    if (s.hideFullscreen) StartFullscreenWatch();
    StartSmartWatch();
    // 常隐模式：启动即收起，不必等第一个 700ms 轮询
    if (s.alwaysHide) UpdateSmartHide();

    SYSTEMTIME now{};
    GetLocalTime(&now);
    g.clockMinute = now.wMinute;
    g.clockDay = now.wDay;
    g.clockTimerOn = true;
    SetTimer(g.hwnd, kClockTimerId, kClockPollMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HideLabel();
    if (gLabel.hwnd) DestroyWindow(gLabel.hwnd);
    HidePopup();
    if (gPopup.hwnd) DestroyWindow(gPopup.hwnd);
    HideGroup();
    if (gGroup.hwnd) DestroyWindow(gGroup.hwnd);
    DestroyIcon(g.trayIcon);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
