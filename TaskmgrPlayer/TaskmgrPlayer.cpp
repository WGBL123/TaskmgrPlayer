// TaskmgrPlayer
// 在任务管理器的 CPU 性能图上播放视频。
//
// 本项目参考并修改自 svr2kos2/TaskmgrPlayer（GPL-3.0）：
// https://github.com/svr2kos2/TaskmgrPlayer
//
// 与旧版的区别：
//  * 不再使用跨进程 SetParent。视频窗口是一个独立置顶、鼠标穿透的覆盖窗口，
//    每帧跟随任务管理器中的图表窗口。这样即使任务管理器是管理员权限
//    （Windows 11 默认自动提权），也能正常覆盖显示，不会再跑到桌面上。
//  * 自动检测当前可见的图表窗口，支持“逻辑处理器”（多核）视图：
//    多核时把所有小图合成到一张画布、只创建一个覆盖窗口，小图之间的空隙透明。
//  * 修复了 DPI 缩放、英文系统找不到窗口、视频文件枚举、rm 删除、计时等问题。

// MSVC 会把 _wfopen 等传统 CRT 函数报成 C4996；本工程只需要读文件，
// 统一关闭这类“不安全函数”告警，避免 /WX 或 VS 默认 SDL 检查把编译打断。
#define _CRT_SECURE_NO_WARNINGS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

using namespace std;
using namespace cv;

// 不直接依赖 SDK 是否声明 DPI_AWARENESS_CONTEXT（老 SDK 在默认
// NTDDI_VERSION 下可能没有这个类型）；该 API 是动态加载的，参数按 HANDLE 传递即可。
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((HANDLE)-3)
#endif

// ---------------------------------------------------------------------------
// 控制台输出（统一走 WriteConsoleW，避免代码页 / locale 问题）
// ---------------------------------------------------------------------------
static bool HasConsole()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode) != 0;
}

static void ConWrite(const wstring& text)
{
    if (text.empty())
        return;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(h, text.data(), (DWORD)text.size(), &written, nullptr);
        return;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return;
    string utf8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &utf8[0], n, nullptr, nullptr);
    fwrite(utf8.data(), 1, utf8.size(), stdout);
    fflush(stdout);
}

static void ConWriteLine(const wstring& text)
{
    ConWrite(text + L"\n");
}

// ---------------------------------------------------------------------------
// 路径 / 文件工具
// ---------------------------------------------------------------------------
static wstring Trim(const wstring& s)
{
    const wchar_t* ws = L" \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == wstring::npos)
        return L"";
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static wstring ToLower(wstring s)
{
    for (wchar_t& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

static bool FileExists(const wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static wstring JoinPath(const wstring& dir, const wstring& name)
{
    if (dir.empty())
        return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/')
        return dir + name;
    return dir + L"\\" + name;
}

static wstring GetExeDir()
{
    vector<wchar_t> buf(MAX_PATH);
    DWORD n = 0;
    for (;;)
    {
        n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0)
            return L".";
        if (n < buf.size())
            break;
        if (buf.size() > 32768)
            return L".";
        buf.resize(buf.size() * 2);
    }
    wstring p(buf.data(), n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == wstring::npos ? L"." : p.substr(0, slash);
}

static wstring GetCurrentDir()
{
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    if (n == 0)
        return L".";
    vector<wchar_t> buf(n + 1);
    DWORD got = GetCurrentDirectoryW((DWORD)buf.size(), buf.data());
    if (got == 0 || got >= buf.size())
        return L".";
    return wstring(buf.data(), got);
}

static bool ReadFileBytes(const wstring& path, string& out)
{
    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp)
        return false;
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return false;
    }
    long size = ftell(fp);
    if (size < 0)
    {
        fclose(fp);
        return false;
    }
    fseek(fp, 0, SEEK_SET);
    out.assign((size_t)size, '\0');
    size_t got = size > 0 ? fread(&out[0], 1, (size_t)size, fp) : 0;
    fclose(fp);
    out.resize(got);
    return true;
}

static wstring Utf8ToWide(const string& s, bool* wasUtf8 = nullptr)
{
    if (wasUtf8)
        *wasUtf8 = true;
    if (s.empty())
        return L"";

    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
    {
        if (wasUtf8)
            *wasUtf8 = false;
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0)
            return L"";
        wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }

    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static string WideToUtf8(const wstring& w)
{
    if (w.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return "";
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static string WideToAnsi(const wstring& w)
{
    if (w.empty())
        return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return "";
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 配置、视频、音频所在目录。优先当前目录，其次 exe 目录。
static wstring g_workDir;

static void InitWorkDir()
{
    wstring cwd = GetCurrentDir();
    if (FileExists(JoinPath(cwd, L"config.cfg")))
    {
        g_workDir = cwd;
        return;
    }
    wstring exeDir = GetExeDir();
    if (FileExists(JoinPath(exeDir, L"config.cfg")))
    {
        g_workDir = exeDir;
        return;
    }
    g_workDir = cwd;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
namespace config
{
    wstring WindowClassName = L"TaskManagerWindow";
    wstring WindowTitle; // 默认留空：任务管理器标题随系统语言变化，不能写死
    wstring ChildClassName = L"CvChartWindow";
    vector<wstring> ChildClassNames;

    Vec3b colorEdge(187, 125, 12);
    Vec3b colorDark(250, 246, 241);
    Vec3b colorBright(255, 255, 255);
    Vec3b colorGrid(244, 234, 217);
    Vec3b colorFrame(187, 125, 12);
    bool DrawGrid = true;

    static vector<wstring> Split(const wstring& s, wchar_t delimiter)
    {
        vector<wstring> res;
        size_t start = 0;
        for (;;)
        {
            size_t pos = s.find(delimiter, start);
            if (pos == wstring::npos)
            {
                res.push_back(s.substr(start));
                break;
            }
            res.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return res;
    }

    static unsigned char ToColorComponent(const wstring& text, unsigned char fallback)
    {
        wstring t = Trim(text);
        if (t.empty())
            return fallback;
        long v = wcstol(t.c_str(), nullptr, 10);
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        return (unsigned char)v;
    }

    static bool ParseColor(const wstring& value, Vec3b& color)
    {
        vector<wstring> parts = Split(value, L',');
        if (parts.size() != 3)
            return false;
        color[0] = ToColorComponent(parts[0], color[0]);
        color[1] = ToColorComponent(parts[1], color[1]);
        color[2] = ToColorComponent(parts[2], color[2]);
        return true;
    }

    static void RebuildChildClassNames()
    {
        ChildClassNames.clear();
        if (ChildClassName.empty())
            return;
        vector<wstring> parts = Split(ChildClassName, L'|');
        for (const wstring& p : parts)
        {
            wstring t = Trim(p);
            if (!t.empty())
                ChildClassNames.push_back(t);
        }
    }

    static void ParseLine(const wstring& rawLine)
    {
        wstring line = Trim(rawLine);
        if (line.empty() || line[0] == L'#' || line[0] == L';')
            return;

        size_t eq = line.find(L'=');
        if (eq == wstring::npos)
            return;
        wstring key = Trim(line.substr(0, eq));
        wstring value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
            value = value.substr(1, value.size() - 2);

        if (key == L"WindowClassName")
            WindowClassName = value;
        else if (key == L"WindowTitle")
            WindowTitle = value;
        else if (key == L"ChildClassName")
        {
            ChildClassName = value;
        }
        else if (key == L"DrawGrid")
            DrawGrid = (ToLower(value) == L"true" || value == L"1");
        else if (key == L"ColorEdge")
            ParseColor(value, colorEdge);
        else if (key == L"ColorDark")
            ParseColor(value, colorDark);
        else if (key == L"ColorBright")
            ParseColor(value, colorBright);
        else if (key == L"ColorGrid")
            ParseColor(value, colorGrid);
        else if (key == L"ColorFrame")
            ParseColor(value, colorFrame);
        else
            ConWriteLine(L"未知配置项: " + key);
    }

    static void ReadConfig()
    {
        wstring path = JoinPath(g_workDir, L"config.cfg");
        string bytes;
        if (!ReadFileBytes(path, bytes))
        {
            ConWriteLine(L"未找到 config.cfg，使用默认配置。");
            RebuildChildClassNames();
            return;
        }

        // UTF-8 BOM
        if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
            (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
            bytes.erase(0, 3);

        bool utf8 = true;
        wstring text = Utf8ToWide(bytes, &utf8);
        if (!utf8)
            ConWriteLine(L"警告: config.cfg 不是 UTF-8 编码，已按系统编码读取。");

        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t nl = text.find(L'\n', pos);
            if (nl == wstring::npos)
            {
                ParseLine(text.substr(pos));
                break;
            }
            ParseLine(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
        RebuildChildClassNames();
    }
}

// ---------------------------------------------------------------------------
// 目标窗口与图表窗口
// ---------------------------------------------------------------------------
struct ChartLayout
{
    vector<HWND> windows;
    bool multi = false;
};

static wstring GetWindowTitle(HWND hwnd)
{
    wchar_t buf[512] = {};
    GetWindowTextW(hwnd, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

static wstring GetWindowClass(HWND hwnd)
{
    wchar_t buf[256] = {};
    GetClassNameW(hwnd, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

static HWND FindTaskManagerWindow()
{
    HWND hwnd = nullptr;

    if (!config::WindowClassName.empty())
    {
        // 先按“类 + 标题”查找（兼容用户想精确指定窗口的情况）
        if (!config::WindowTitle.empty())
            hwnd = FindWindowW(config::WindowClassName.c_str(), config::WindowTitle.c_str());
        // 再只按类查找：任务管理器标题随系统语言不同（任务管理器 / Task Manager / ...）
        if (!hwnd)
            hwnd = FindWindowW(config::WindowClassName.c_str(), nullptr);
    }

    if (!hwnd && !config::WindowTitle.empty())
        hwnd = FindWindowW(nullptr, config::WindowTitle.c_str());

    return hwnd;
}

struct EnumChartContext
{
    vector<HWND> found;
};

static bool ClassNameMatches(HWND hwnd, const vector<wstring>& classes)
{
    wstring cls = GetWindowClass(hwnd);
    for (const wstring& c : classes)
    {
        if (_wcsicmp(cls.c_str(), c.c_str()) == 0)
            return true;
    }
    return false;
}

static BOOL CALLBACK EnumChartProc(HWND hwnd, LPARAM lParam)
{
    EnumChartContext* ctx = reinterpret_cast<EnumChartContext*>(lParam);
    if (!ctx)
        return TRUE;

    // 只统计当前真正显示的图表窗口。
    // 旧版没有这个判断，会选到其他性能页里被隐藏的图表，导致视频绑定到错误的页面。
    if (!IsWindowVisible(hwnd))
        return TRUE;

    if (!ClassNameMatches(hwnd, config::ChildClassNames))
        return TRUE;

    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc))
        return TRUE;
    if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8)
        return TRUE;

    if (find(ctx->found.begin(), ctx->found.end(), hwnd) == ctx->found.end())
        ctx->found.push_back(hwnd);
    return TRUE;
}

// Win11 某些版本的性能页可能不在主窗口的子树里，也可能是单独的顶层窗口。
// 这里收集同一进程的所有顶层窗口，再统一枚举它们的子窗口。
struct EnumTopContext
{
    DWORD pid = 0;
    vector<HWND> roots;
};

static BOOL CALLBACK EnumTopProc(HWND hwnd, LPARAM lParam)
{
    EnumTopContext* ctx = reinterpret_cast<EnumTopContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid)
        ctx->roots.push_back(hwnd);
    return TRUE;
}

static void CollectChartCandidates(HWND root, EnumChartContext& ctx)
{
    if (!root || !IsWindow(root))
        return;
    EnumChildWindows(root, EnumChartProc, reinterpret_cast<LPARAM>(&ctx));
}

// 判断两个图表矩形之间的水平/垂直间隙是否不超过 gap 像素。
static bool RectsNear(const RECT& a, const RECT& b, int gap)
{
    int dx = max(0, (int)max(a.left, b.left) - (int)min(a.right, b.right));
    int dy = max(0, (int)max(a.top, b.top) - (int)min(a.bottom, b.bottom));
    return dx <= gap && dy <= gap;
}

// 把可见图表按“相邻”聚成若干簇，选总面积最大的那一簇。
// 这样即使 Win11 把 48 个逻辑处理器图表分别放进不同的父容器，
// 也能拿到完整网格，而不会只选中左侧栏的 CPU 小图。
static vector<HWND> SelectChartCluster(const vector<HWND>& found)
{
    vector<RECT> rects(found.size());
    for (size_t i = 0; i < found.size(); ++i)
    {
        if (!GetWindowRect(found[i], &rects[i]))
            rects[i] = RECT{};
    }

    vector<int> owner(found.size(), -1);
    vector<vector<int>> clusters;
    // 只合并真正相邻的图表：任务管理器内部小图间距通常小于 10px，
    // 而左侧栏与右侧图表区之间有明显间隔，用 30px 避免把侧栏图表并进来。
    const int kGap = 30;

    for (size_t i = 0; i < found.size(); ++i)
    {
        if (owner[i] >= 0)
            continue;

        int id = (int)clusters.size();
        clusters.push_back(vector<int>());
        vector<int> stack;
        stack.push_back((int)i);
        owner[i] = id;

        while (!stack.empty())
        {
            int cur = stack.back();
            stack.pop_back();
            clusters[id].push_back(cur);

            for (size_t j = 0; j < found.size(); ++j)
            {
                if (owner[j] >= 0)
                    continue;
                if (RectsNear(rects[cur], rects[j], kGap))
                {
                    owner[j] = id;
                    stack.push_back((int)j);
                }
            }
        }
    }

    int best = -1;
    unsigned long long bestArea = 0;
    for (size_t c = 0; c < clusters.size(); ++c)
    {
        unsigned long long area = 0;
        for (int idx : clusters[c])
        {
            area += (unsigned long long)(rects[idx].right - rects[idx].left) *
                    (unsigned long long)(rects[idx].bottom - rects[idx].top);
        }
        if (best < 0 || area > bestArea ||
            (area == bestArea && clusters[c].size() > clusters[best].size()))
        {
            best = (int)c;
            bestArea = area;
        }
    }

    vector<HWND> result;
    if (best >= 0)
        for (int idx : clusters[best])
            result.push_back(found[idx]);

    // 固定顺序，避免每次枚举顺序变化导致误判“布局变了”而重建窗口。
    sort(result.begin(), result.end(), [](HWND a, HWND b) {
        RECT ra = {}, rb = {};
        GetWindowRect(a, &ra);
        GetWindowRect(b, &rb);
        if (ra.top != rb.top)
            return ra.top < rb.top;
        if (ra.left != rb.left)
            return ra.left < rb.left;
        return reinterpret_cast<INT_PTR>(a) < reinterpret_cast<INT_PTR>(b);
    });
    return result;
}

// 图表可能共享父窗口，也可能各自有独立父容器；统一按屏幕位置聚类。
static ChartLayout DiscoverCharts(HWND taskmgr)
{
    ChartLayout layout;

    if (config::ChildClassNames.empty())
    {
        // 未配置子窗口类：整块任务管理器窗口作为目标（与旧版行为一致）
        layout.windows.push_back(taskmgr);
        layout.multi = false;
        return layout;
    }

    EnumChartContext ctx;
    CollectChartCandidates(taskmgr, ctx);

    if (taskmgr)
    {
        EnumTopContext top;
        GetWindowThreadProcessId(taskmgr, &top.pid);
        EnumWindows(EnumTopProc, reinterpret_cast<LPARAM>(&top));
        for (HWND root : top.roots)
        {
            if (root == taskmgr)
                continue; // 已经枚举过
            CollectChartCandidates(root, ctx);
        }
    }

    // 去掉重复的窗口句柄（顶层窗口枚举可能和主窗口子树重叠）。
    // 去重已在 EnumChartProc 中完成。

    if (ctx.found.empty())
        return layout;

    layout.windows = SelectChartCluster(ctx.found);
    layout.multi = layout.windows.size() > 1;
    return layout;
}

// ---------------------------------------------------------------------------
// 视频画面后期处理
// ---------------------------------------------------------------------------
static void PostProcess(Mat& src, int phase)
{
    Mat gray, bin, edge;
    cvtColor(src, gray, COLOR_BGR2GRAY);
    inRange(gray, Scalar(128), Scalar(255), bin);
    Canny(bin, edge, 80, 130);

    const int stepX = max(8, src.cols / 10);
    const int stepY = max(8, src.rows / 8);
    int offset = stepX > 0 ? (phase % stepX) : 0;
    if (offset < 0)
        offset += stepX;

    for (int r = 0; r < src.rows; ++r)
    {
        Vec3b* dst = src.ptr<Vec3b>(r);
        const unsigned char* b = bin.ptr<unsigned char>(r);
        const unsigned char* e = edge.ptr<unsigned char>(r);
        const bool gridRow = config::DrawGrid && (r % stepY == 0);

        for (int c = 0; c < src.cols; ++c)
        {
            Vec3b color = (b[c] == 255) ? config::colorBright : config::colorDark;
            if (config::DrawGrid && (gridRow || ((c + offset) % stepX == 0)))
                color = config::colorGrid;
            if (e[c] == 255)
                color = config::colorEdge;
            dst[c] = color;
        }
    }

    rectangle(src, Rect(0, 0, src.cols, src.rows),
              Scalar(config::colorFrame[0], config::colorFrame[1], config::colorFrame[2]), 1);
}

// ---------------------------------------------------------------------------
// 视频窗口（独立的置顶鼠标穿透窗口）
// ---------------------------------------------------------------------------
struct FindOwnWindowContext
{
    wstring title;
    HWND result = nullptr;
};

static BOOL CALLBACK FindOwnWindowProc(HWND hwnd, LPARAM lParam)
{
    FindOwnWindowContext* ctx = reinterpret_cast<FindOwnWindowContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;

    wchar_t buf[512] = {};
    GetWindowTextW(hwnd, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    if (ctx->title == buf)
    {
        ctx->result = hwnd;
        return FALSE;
    }

    return TRUE;
}

static bool BelongsToCurrentProcess(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// 多窗口模式下必须按完整标题精确查找，不能使用“本进程第一个 HighGUI 窗口”的兜底，
// 否则第二个及之后的覆盖窗口会拿到同一个 HWND。
static HWND FindOwnWindowExact(const wstring& title)
{
    FindOwnWindowContext ctx;
    ctx.title = title;
    EnumWindows(FindOwnWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.result)
        return ctx.result;

    HWND hwnd = FindWindowW(L"Main HighGUI class", title.c_str());
    if (hwnd && BelongsToCurrentProcess(hwnd))
        return hwnd;
    return nullptr;
}

static void ApplyOverlayStyle(HWND hwnd)
{
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CHILD);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
               WS_EX_LAYERED | WS_EX_TOPMOST;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // 用颜色键把合成画布上的空隙抠成透明：多核视图只开一个覆盖窗口，
    // 画布上小图之间的空隙填成 RGB(255,0,255)，该颜色不会被显示。
    // WS_EX_LAYERED | WS_EX_TRANSPARENT 让鼠标事件穿透到下面的任务管理器。
    SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 0, LWA_COLORKEY);

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 1, 1,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_HIDEWINDOW);
}

static bool IsTaskManagerForeground(HWND taskmgr)
{
    if (!IsWindow(taskmgr) || !IsWindowVisible(taskmgr) || IsIconic(taskmgr))
        return false;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return false;
    if (fg == taskmgr)
        return true;
    // 任务管理器的子窗口获得焦点时，GetForegroundWindow 仍返回顶层窗口，
    // 这里再兼容一下少数情况。
    return IsChild(taskmgr, fg) != FALSE;
}

// 任务管理器打开右键菜单时，菜单是独立的弹出窗口，会被置顶的覆盖窗口挡住。
// 检测到前台线程正处于菜单模式时临时隐藏覆盖窗口，菜单关闭后会自动恢复。
static bool IsMenuActive(HWND taskmgr)
{
    // GetGUIThreadInfo(0, ...) 只看调用线程，任务管理器的菜单在它自己的 UI 线程里，
    // 所以必须传任务管理器窗口所属的线程 ID。
    DWORD threadId = taskmgr ? GetWindowThreadProcessId(taskmgr, nullptr) : 0;
    if (!threadId)
        return false;

    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(threadId, &info))
        return false;
    return (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0;
}

static void HideOverlay(HWND overlay, bool& visible)
{
    if (visible)
    {
        ShowWindow(overlay, SW_HIDE);
        visible = false;
    }
}

static void ShowOverlayAt(HWND overlay, const RECT& rect, bool& visible,
                          RECT& lastRect, bool& hasRect, bool force)
{
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w < 1 || h < 1)
    {
        HideOverlay(overlay, visible);
        return;
    }

    if (!visible)
    {
        ShowWindow(overlay, SW_SHOWNOACTIVATE);
        visible = true;
    }

    // 位置和大小没变时不必每帧调用 SetWindowPos；
    // 但每隔一段时间仍强制置顶一次，防止被其他置顶窗口压住。
    if (!force && hasRect && EqualRect(&lastRect, &rect))
        return;

    SetWindowPos(overlay, HWND_TOPMOST, rect.left, rect.top, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    lastRect = rect;
    hasRect = true;
}

struct OverlayWindow
{
    wstring title;
    string titleUtf8;
    HWND hwnd = nullptr;
    bool visible = false;
    RECT lastRect = {};
    bool hasRect = false;
};

static bool CreateOverlay(int index, OverlayWindow& ow)
{
    wstringstream name;
    name << L"TaskmgrPlayerOverlay_" << index;
    ow.title = name.str();
    ow.titleUtf8 = WideToUtf8(ow.title);

    namedWindow(ow.titleUtf8, WINDOW_NORMAL);
    // HighGUI 在部分版本里会异步创建窗口，稍等片刻再按标题查找。
    for (int tries = 0; tries < 20 && !ow.hwnd; ++tries)
    {
        ow.hwnd = FindOwnWindowExact(ow.title);
        if (!ow.hwnd)
            Sleep(10);
    }
    if (!ow.hwnd)
        return false;

    ApplyOverlayStyle(ow.hwnd);
    return true;
}

static void DestroyOverlay(OverlayWindow& ow)
{
    if (!ow.titleUtf8.empty())
        destroyWindow(ow.titleUtf8);
    ow.hwnd = nullptr;
    ow.visible = false;
    ow.hasRect = false;
    waitKey(1);
}

// 计算所有可见图表的屏幕外接矩形；任一图表不可用时跳过它。
static bool GetChartsBounds(const vector<HWND>& windows, RECT& bounds)
{
    bool any = false;
    for (HWND hwnd : windows)
    {
        RECT rc = {};
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &rc))
            continue;
        if (rc.right <= rc.left || rc.bottom <= rc.top)
            continue;

        if (!any)
        {
            bounds = rc;
            any = true;
        }
        else
        {
            bounds.left = min(bounds.left, rc.left);
            bounds.top = min(bounds.top, rc.top);
            bounds.right = max(bounds.right, rc.right);
            bounds.bottom = max(bounds.bottom, rc.bottom);
        }
    }
    return any;
}

// ---------------------------------------------------------------------------
// 视频文件与播放
// ---------------------------------------------------------------------------
static bool IsVideoFile(const wstring& fileName)
{
    size_t dot = fileName.find_last_of(L'.');
    if (dot == wstring::npos || dot + 1 >= fileName.size())
        return false;
    wstring ext = ToLower(fileName.substr(dot + 1));
    static const wchar_t* exts[] = { L"flv", L"mp4", L"avi", L"mkv", L"mov", L"webm", L"wmv", L"m4v" };
    for (const wchar_t* e : exts)
    {
        if (ext == e)
            return true;
    }
    return false;
}

static wstring FindVideo()
{
    wstring pattern = JoinPath(g_workDir, L"*");
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return L"";

    wstring result;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wstring name = fd.cFileName;
        if (IsVideoFile(name))
        {
            result = JoinPath(g_workDir, name);
            break;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return result;
}

static wstring GetTempAudioPath()
{
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW((DWORD)(sizeof(tmp) / sizeof(tmp[0])), tmp);
    wstring dir = (n > 0 && n < (DWORD)(sizeof(tmp) / sizeof(tmp[0]))) ? wstring(tmp, n) : L".";
    wstringstream name;
    name << L"TaskmgrPlayer_" << GetCurrentProcessId() << L".wav";
    return JoinPath(dir, name.str());
}

static bool OpenCapture(VideoCapture& cap, const wstring& path)
{
    // OpenCV 在 Windows 上一般接受 UTF-8 路径；失败时再尝试 8.3 短路径。
    if (cap.open(WideToUtf8(path)))
        return true;

    wchar_t shortPath[2048] = {};
    DWORD n = GetShortPathNameW(path.c_str(), shortPath, (DWORD)(sizeof(shortPath) / sizeof(shortPath[0])));
    if (n > 0 && n < (DWORD)(sizeof(shortPath) / sizeof(shortPath[0])))
    {
        if (cap.open(WideToAnsi(shortPath)))
            return true;
    }
    return false;
}

static void SplitAudio(const wstring& videoPath, const wstring& audioPath)
{
    DeleteFileW(audioPath.c_str());

    wstring ffmpeg = FileExists(JoinPath(g_workDir, L"ffmpeg.exe"))
                         ? L"\"" + JoinPath(g_workDir, L"ffmpeg.exe") + L"\""
                         : wstring(L"ffmpeg");
    wstring cmd = ffmpeg + L" -loglevel error -y -i \"" + videoPath +
                  L"\" -vn -acodec pcm_s16le \"" + audioPath + L"\"";
    int rc = _wsystem(cmd.c_str());
    if (rc != 0 || !FileExists(audioPath))
    {
        ConWriteLine(L"音频提取失败（未安装 ffmpeg？），将静音播放。");
        DeleteFileW(audioPath.c_str());
    }
}

static wstring FindAudioFile()
{
    // 优先使用随程序分发的 audio.wav：原版 BadApple.flv 往往没有音轨，
    // 而用户机器上不一定安装了 ffmpeg。
    wstring path = JoinPath(g_workDir, L"audio.wav");
    if (FileExists(path))
        return path;

    wchar_t cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0)
    {
        path = JoinPath(cwd, L"audio.wav");
        if (FileExists(path))
            return path;
    }
    return wstring();
}

static int Play()
{
    HWND taskmgr = nullptr;
    ConWriteLine(L"正在查找任务管理器窗口...");
    for (int i = 0; i < 60 && !taskmgr; ++i)
    {
        taskmgr = FindTaskManagerWindow();
        if (!taskmgr)
            Sleep(250);
    }
    if (!taskmgr)
    {
        ConWriteLine(L"找不到任务管理器窗口。");
        ConWriteLine(L"请确认任务管理器已打开；如果修改过 config.cfg，请检查 WindowClassName / WindowTitle。");
        return 2;
    }

    ConWriteLine(L"目标窗口: \"" + GetWindowTitle(taskmgr) + L"\" [" + GetWindowClass(taskmgr) + L"]");

    ChartLayout layout;
    ConWriteLine(L"正在查找 CPU 图表窗口...");
    ConWriteLine(L"如果还没显示图表，请切换到 任务管理器 -> 性能 -> CPU。");
    for (int i = 0; i < 60; ++i)
    {
        layout = DiscoverCharts(taskmgr);
        if (!layout.windows.empty())
            break;
        Sleep(250);
    }
    if (layout.windows.empty())
    {
        ConWriteLine(L"没有找到图表窗口。");
        ConWriteLine(L"请先在任务管理器中打开 性能 -> CPU，并确认 CPU 图表可见。");
        return 3;
    }

    {
        wstringstream ss;
        ss << L"检测到 " << layout.windows.size() << L" 个图表窗口"
           << (layout.multi ? L"（多核视图：每个核心单独播放同一视频）" : L"（单核视图）");
        ConWriteLine(ss.str());
    }

    wstring videoPath = FindVideo();
    if (videoPath.empty())
    {
        ConWriteLine(L"当前目录下没有找到视频文件（支持 flv/mp4/avi/mkv/mov/webm/wmv）。");
        ConWriteLine(L"当前目录: " + g_workDir);
        return 4;
    }

    ConWriteLine(L"视频: " + videoPath);

    // 音频优先用 audio.wav；找不到才从视频里提取到临时文件。
    wstring audioPath = FindAudioFile();
    bool deleteAudio = false;
    if (!audioPath.empty())
    {
        ConWriteLine(L"音频: " + audioPath);
    }
    else
    {
        audioPath = GetTempAudioPath();
        deleteAudio = true;
        ConWriteLine(L"未找到 audio.wav，正在从视频提取音频...");
        SplitAudio(videoPath, audioPath);
    }

    VideoCapture video;
    if (!OpenCapture(video, videoPath) || !video.isOpened())
    {
        ConWriteLine(L"无法打开视频文件。");
        if (deleteAudio)
            DeleteFileW(audioPath.c_str());
        return 5;
    }

    double fps = video.get(CAP_PROP_FPS);
    if (!(fps > 1.0) || fps > 240.0)
        fps = 30.0;
    int srcW = (int)video.get(CAP_PROP_FRAME_WIDTH);
    int srcH = (int)video.get(CAP_PROP_FRAME_HEIGHT);
    {
        wstringstream ss;
        ss << L"输入 " << srcW << L"x" << srcH << L" @" << fps << L"fps";
        ConWriteLine(ss.str());
    }

    Mat frame;
    if (!video.read(frame) || frame.empty())
    {
        ConWriteLine(L"读取视频帧失败。");
        if (deleteAudio)
            DeleteFileW(audioPath.c_str());
        return 7;
    }

    // 等任务管理器成为前台窗口后再开始：避免程序一启动就只听到声音、
    // 视频却因为控制台在前台而被隐藏。
    ConWriteLine(L"请点击任务管理器窗口开始播放（点击前不会播放声音）。");
    {
        int waitedMs = 0;
        while (!IsTaskManagerForeground(taskmgr) && waitedMs < 60000)
        {
            Sleep(100);
            waitedMs += 100;
            waitKey(1);
            if (!IsWindow(taskmgr))
            {
                taskmgr = FindTaskManagerWindow();
                if (!taskmgr)
                    break;
            }
        }
        if (waitedMs >= 60000 && !IsTaskManagerForeground(taskmgr))
            ConWriteLine(L"等待超时：仍开始播放，视频只会在任务管理器前台时显示。");
    }
    if (!taskmgr)
    {
        ConWriteLine(L"任务管理器已关闭，停止播放。");
        if (deleteAudio)
            DeleteFileW(audioPath.c_str());
        return 8;
    }

    // 等待期间用户可能切换了视图，重新发现一次，保证单核/多核布局正确。
    layout = DiscoverCharts(taskmgr);
    if (layout.windows.empty())
    {
        ConWriteLine(L"播放前图表窗口消失了，请确认任务管理器仍停留在 性能 -> CPU 页面。");
        if (deleteAudio)
            DeleteFileW(audioPath.c_str());
        return 3;
    }

    ConWriteLine(L"正在创建视频覆盖窗口...");
    OverlayWindow overlay;
    if (!CreateOverlay(0, overlay))
    {
        ConWriteLine(L"创建视频覆盖窗口失败。");
        if (deleteAudio)
            DeleteFileW(audioPath.c_str());
        return 6;
    }

    // 第一帧已经读入；此时再启动音频，尽量对齐画面与声音。
    if (!audioPath.empty() && FileExists(audioPath))
        PlaySoundW(audioPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);

    bool quit = false;
    auto startTime = chrono::steady_clock::now();
    auto lastDiscovery = startTime;
    int frameIndex = 0;

    for (;;)
    {
        auto now = chrono::steady_clock::now();
        if (now - lastDiscovery > chrono::milliseconds(500))
        {
            lastDiscovery = now;
            if (!IsWindow(taskmgr))
            {
                taskmgr = FindTaskManagerWindow();
                if (!taskmgr)
                {
                    ConWriteLine(L"\n任务管理器已关闭，停止播放。");
                    quit = true;
                    break;
                }
            }
            layout = DiscoverCharts(taskmgr);
        }

        bool shouldShow = taskmgr && !layout.windows.empty() && overlay.hwnd &&
                          IsTaskManagerForeground(taskmgr) && !IsMenuActive(taskmgr);

        RECT unionRect = {};
        if (shouldShow && !GetChartsBounds(layout.windows, unionRect))
            shouldShow = false;

        if (shouldShow)
        {
            int uw = unionRect.right - unionRect.left;
            int uh = unionRect.bottom - unionRect.top;
            if (uw < 1 || uh < 1)
            {
                HideOverlay(overlay.hwnd, overlay.visible);
            }
            else
            {
                int phase = (int)(chrono::duration_cast<chrono::milliseconds>(now - startTime).count() / 40);

                // 只用一张大画布：小图之间的空隙填成颜色键（透明），
                // 每个小图的视频按自己的位置画进去，最后只 imshow 一个大窗口。
                Mat canvas(uh, uw, CV_8UC3, Scalar(255, 0, 255));

                // 同尺寸小图只 resize/着色一次，避免 48 份重复处理。
                map<pair<int, int>, Mat> renderCache;
                for (HWND chart : layout.windows)
                {
                    RECT rc = {};
                    if (!IsWindow(chart) || !IsWindowVisible(chart) ||
                        !GetWindowRect(chart, &rc) ||
                        rc.right <= rc.left || rc.bottom <= rc.top)
                        continue;

                    int w = rc.right - rc.left;
                    int h = rc.bottom - rc.top;
                    pair<int, int> key(w, h);

                    auto it = renderCache.find(key);
                    if (it == renderCache.end())
                    {
                        Mat processed;
                        resize(frame, processed, Size(w, h), 0, 0, INTER_NEAREST);
                        if (processed.channels() == 1)
                        {
                            Mat bgr;
                            cvtColor(processed, bgr, COLOR_GRAY2BGR);
                            processed = bgr;
                        }
                        else if (processed.channels() == 4)
                        {
                            Mat bgr;
                            cvtColor(processed, bgr, COLOR_BGRA2BGR);
                            processed = bgr;
                        }
                        PostProcess(processed, phase);
                        it = renderCache.emplace(key, processed).first;
                    }

                    int x = rc.left - unionRect.left;
                    int y = rc.top - unionRect.top;
                    int x0 = max(0, x);
                    int y0 = max(0, y);
                    int x1 = min(canvas.cols, x + w);
                    int y1 = min(canvas.rows, y + h);
                    if (x1 <= x0 || y1 <= y0)
                        continue;

                    Mat src = it->second(Rect(x0 - x, y0 - y, x1 - x0, y1 - y0));
                    src.copyTo(canvas(Rect(x0, y0, x1 - x0, y1 - y0)));
                }

                ShowOverlayAt(overlay.hwnd, unionRect, overlay.visible,
                              overlay.lastRect, overlay.hasRect,
                              (frameIndex % 30) == 0);
                imshow(overlay.titleUtf8, canvas);

                if (frameIndex % 30 == 0)
                {
                    wstringstream ss;
                    ss << L"\r帧 " << frameIndex << L"  " << layout.windows.size() << L" 个图表"
                       << (layout.multi ? L"  多核" : L"  单核") << L"      ";
                    ConWrite(ss.str());
                }
            }
        }
        else
        {
            HideOverlay(overlay.hwnd, overlay.visible);
        }

        // 按帧率节拍播放；用 steady_clock，避免旧版 clock() 的计时问题。
        ++frameIndex;
        auto deadline = startTime + chrono::duration_cast<chrono::steady_clock::duration>(
                                        chrono::duration<double>(frameIndex / fps));
        while (chrono::steady_clock::now() < deadline)
        {
            int key = waitKey(1);
            if (key == 27)
            {
                quit = true;
                break;
            }
        }
        if (quit)
            break;

        // 即使窗口隐藏也继续读帧，保证音频与画面进度一致。
        if (!video.read(frame) || frame.empty())
            break;
    }

    PlaySoundW(nullptr, nullptr, 0);
    DestroyOverlay(overlay);
    if (deleteAudio)
        DeleteFileW(audioPath.c_str());
    ConWriteLine(L"\n播放结束。");
    return 0;
}

// ---------------------------------------------------------------------------
int main()
{
    // 必须在创建任何窗口之前设置 DPI 感知，否则高缩放比例下覆盖位置会偏移。
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        bool done = false;
        if (user32)
        {
            typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFn)(HANDLE);
            SetProcessDpiAwarenessContextFn fn =
                reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                    GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn)
            {
                if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                    done = true;
                else if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                    done = true;
            }
        }
        if (!done)
            SetProcessDPIAware();
    }

    SetConsoleTitleW(L"TaskmgrPlayer");
    InitWorkDir();
    config::ReadConfig();

    int code = Play();
    if (code != 0 && HasConsole())
        system("pause");
    return code;
}
