// CloudPhoneHelper.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "CloudPhoneHelper.h"
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <commdlg.h>
#include <commctrl.h>

#define MAX_LOADSTRING 100

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
static HWND gList;
static HWND gRefresh;
static HWND gLog;
static HWND gClear;

struct DeviceInfo
{
    std::wstring serial;
    std::wstring brand;
    std::wstring model;
    std::wstring deviceIdSd;
    std::wstring deviceIdData;
    std::wstring androidVersion;
    std::wstring abi;
    std::wstring adbStatus;
    bool agentRunning;
};
static std::vector<DeviceInfo> g_devs;

#define IDC_LIST 1001
#define IDC_REFRESH 1002
#define IDC_LOG 1003
#define IDC_CLEAR 1004

// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

static std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) return p.substr(0, pos);
    return L".";
}

static bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring FindAdb()
{
    std::wstring d = GetExeDir();
    std::wstring p1 = d + L"\\adb.exe";
    if (FileExists(p1)) return p1;
    std::wstring p2 = d + L"\\adb\\adb.exe";
    if (FileExists(p2)) return p2;
    return L"adb.exe";
}

static bool RunProcess(const std::wstring& cmd, std::wstring& out)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return false;
    if (!SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) return false;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = w;
    si.hStdError = w;
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmd;
    std::vector<wchar_t> clbuf(cl.begin(), cl.end());
    clbuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, clbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(w);
        CloseHandle(r);
        return false;
    }
    CloseHandle(w);
    char buf[4096];
    std::string s;
    for (;;)
    {
        DWORD n = 0;
        BOOL ok = ReadFile(r, buf, sizeof(buf), &n, nullptr);
        if (!ok || n == 0) break;
        s.append(buf, buf + n);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(r);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0)
    {
        int wlen2 = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring ws(wlen2, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &ws[0], wlen2);
        out = ws;
        return true;
    }
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], wlen);
    out = ws;
    return true;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L"\r\n\t ");
    size_t b = s.find_last_not_of(L"\r\n\t ");
    if (a == std::wstring::npos) return L"";
    return s.substr(a, b - a + 1);
}

static void AppendLog(HWND hEdit, const std::wstring& text);

static bool AdbShell(const std::wstring& serial, const std::wstring& cmd, std::wstring& out)
{
    std::wstring adb = FindAdb();
    std::wstring wrapped = L"\"" + cmd + L"\"";
    bool ok = RunProcess(adb + L" -s " + serial + L" shell " + wrapped, out);
    AppendLog(gLog, L"> " + adb + L" -s " + serial + L" shell " + cmd + L"\r\n" + out + L"\r\n");
    return ok;
}

static bool Adb(const std::wstring& args, std::wstring& out)
{
    std::wstring adb = FindAdb();
    std::wstring cmd = adb + args;
    bool ok = RunProcess(cmd, out);
    AppendLog(gLog, L"> " + cmd + L"\r\n" + out + L"\r\n");
    return ok;
}

static std::vector<DeviceInfo> GatherDevices()
{
    std::wstring adb = FindAdb();
    std::wstring out;
    Adb(L" devices", out);
    std::vector<DeviceInfo> res;
    std::wstringstream ss(out);
    std::wstring line;
    while (std::getline(ss, line))
    {
        if (line.find(L"List of devices") != std::wstring::npos) continue;
        if (line.empty()) continue;
        size_t p = line.find(L"\t");
        if (p == std::wstring::npos) continue;
        DeviceInfo d{};
        d.serial = Trim(line.substr(0, p));
        d.adbStatus = Trim(line.substr(p + 1));
        d.agentRunning = false;
        if (d.adbStatus == L"device")
        {
            std::wstring b, m, idSd, idData, ver, abi, ps;
            AdbShell(d.serial, L"getprop ro.product.brand", b);
            AdbShell(d.serial, L"getprop ro.product.model", m);
            AdbShell(d.serial, L"cat /sdcard/cloud_agent_id.txt", idSd);
            AdbShell(d.serial, L"cat /data/local/tmp/cloud_agent_id.txt", idData);
            AdbShell(d.serial, L"getprop ro.build.version.release", ver);
            AdbShell(d.serial, L"getprop ro.product.cpu.abi", abi);
            AdbShell(d.serial, L"ps -elf | grep -w adb-agent", ps);
            d.brand = Trim(b);
            d.model = Trim(m);
            d.deviceIdSd = Trim(idSd);
            d.deviceIdData = Trim(idData);
            d.androidVersion = Trim(ver);
            d.abi = Trim(abi);
            auto isRunning = [&](const std::wstring& serial)->bool {
                std::wstring psOut;
                AdbShell(serial, L"ps -elf | grep -w adb-agent", psOut);
                std::wstringstream pss(psOut);
                std::wstring pl;
                while (std::getline(pss, pl))
                {
                    std::wstring t = Trim(pl);
                    if (t.empty()) continue;
                    if (t.find(L"adb-agent") == std::wstring::npos) continue;
                    if (t.find(L"grep") != std::wstring::npos) continue;
                    if (t.find(L"sh -c") != std::wstring::npos) continue;
                    return true;
                }
                std::wstring pid;
                AdbShell(serial, L"pidof adb-agent", pid);
                if (!Trim(pid).empty()) return true;
                std::wstring pgrep;
                AdbShell(serial, L"pgrep -f adb-agent", pgrep);
                if (!Trim(pgrep).empty()) return true;
                return false;
            };
            d.agentRunning = isRunning(d.serial);
        }
        res.push_back(d);
    }
    return res;
}

static bool PushFile(const std::wstring& local, const std::wstring& remote, const std::wstring& serial);

static bool ActivateAgent(const std::wstring& serial, std::wstring& log)
{
    std::wstring local = GetExeDir() + L"\\res\\armV7\\adb-agent";
    if (!FileExists(local)) local = L"d:\\github\\CloudPhoneHelper\\res\\armV7\\adb-agent";
    if (!FileExists(local))
    {
        log += L"未找到adb-agent本地文件\r\n";
        return false;
    }
    std::wstring pushOut;
    bool okPush = PushFile(local, L"/data/local/tmp/adb-agent", serial);
    log += okPush ? L"推送adb-agent成功\r\n" : L"推送adb-agent失败\r\n";
    if (!okPush) return false;
    std::wstring out1, out2, out3;
    AdbShell(serial, L"chmod a+x /data/local/tmp/adb-agent", out1);
    log += L"chmod输出:\r\n" + Trim(out1) + L"\r\n";
    AdbShell(serial, L"cd /data/local/tmp && ./adb-agent server start --daemon=true", out2);
    log += L"启动输出:\r\n" + Trim(out2) + L"\r\n";
    std::wstring ps;
    AdbShell(serial, L"ps -elf | grep -w adb-agent", ps);
    bool running = false;
    {
        std::wstringstream pss(ps);
        std::wstring pl;
        while (std::getline(pss, pl))
        {
            std::wstring t = Trim(pl);
            if (t.empty()) continue;
            if (t.find(L"adb-agent") == std::wstring::npos) continue;
            if (t.find(L"grep") != std::wstring::npos) continue;
            if (t.find(L"sh -c") != std::wstring::npos) continue;
            running = true; break;
        }
    }
    if (!running)
    {
        AdbShell(serial, L"cd /data/local/tmp && ./adb-agent server -d", out3);
        log += L"二次启动输出:\r\n" + Trim(out3) + L"\r\n";
        AdbShell(serial, L"ps -elf | grep -w adb-agent", ps);
        running = false;
        std::wstringstream pss2(ps);
        std::wstring pl2;
        while (std::getline(pss2, pl2))
        {
            std::wstring t = Trim(pl2);
            if (t.empty()) continue;
            if (t.find(L"adb-agent") == std::wstring::npos) continue;
            if (t.find(L"grep") != std::wstring::npos) continue;
            if (t.find(L"sh -c") != std::wstring::npos) continue;
            running = true; break;
        }
    }
    log += running ? L"adb-agent已运行\r\n" : L"adb-agent未运行\r\n";
    return running;
}

static void AppendLog(HWND hEdit, const std::wstring& text)
{
    if (!hEdit) return;
    int len = GetWindowTextLengthW(hEdit);
    SendMessageW(hEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

static std::vector<std::wstring> ListDevices()
{
    std::wstring out;
    bool ok = Adb(L" devices", out);
    std::vector<std::wstring> res;
    if (!ok) return res;
    std::wstringstream ss(out);
    std::wstring line;
    while (std::getline(ss, line))
    {
        if (line.find(L"List of devices") != std::wstring::npos) continue;
        if (line.empty()) continue;
        size_t p = line.find(L"\t");
        if (p != std::wstring::npos)
        {
            std::wstring status = Trim(line.substr(p + 1));
            if (status == L"device") res.push_back(Trim(line.substr(0, p)));
        }
    }
    return res;
}

static std::wstring BaseName(const std::wstring& p)
{
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p;
    return p.substr(pos + 1);
}

static bool PushFile(const std::wstring& local, const std::wstring& remote, const std::wstring& serial)
{
    std::wstring out;
    bool ok = Adb(L" -s " + serial + L" push \"" + local + L"\" \"" + remote + L"\"", out);
    std::wstring low = out; for (auto& c : low) c = towlower(c);
    if (!ok) return false;
    if (low.find(L"error") != std::wstring::npos) return false;
    if (low.find(L"failed") != std::wstring::npos) return false;
    if (low.find(L"cannot stat") != std::wstring::npos) return false;
    return true;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: 在此处放置代码。

    // 初始化全局字符串
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_CLOUDPHONEHELPER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // 执行应用程序初始化:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_CLOUDPHONEHELPER));

    MSG msg;

    // 主消息循环:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  函数: MyRegisterClass()
//
//  目标: 注册窗口类。
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CLOUDPHONEHELPER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = nullptr;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // 将实例句柄存储在全局变量中

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  目标: 处理主窗口的消息。
//
//  WM_COMMAND  - 处理应用程序菜单
//  WM_PAINT    - 绘制主窗口
//  WM_DESTROY  - 发送退出消息并返回
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        {
            INITCOMMONCONTROLSEX icc{};
            icc.dwSize = sizeof(icc);
            icc.dwICC = ICC_LISTVIEW_CLASSES;
            InitCommonControlsEx(&icc);
            RECT rc; GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            int topH = h * 45 / 100;
            gList = CreateWindowW(WC_LISTVIEW, L"", WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,
                                  8, 8, w - 120, topH - 16, hWnd, (HMENU)IDC_LIST, hInst, nullptr);
            gRefresh = CreateWindowW(L"BUTTON", L"刷新", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                     w - 100, 8, 92, 28, hWnd, (HMENU)IDC_REFRESH, hInst, nullptr);
            gLog = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,
                                 8, topH + 8, w - 120, h - topH - 24, hWnd, (HMENU)IDC_LOG, hInst, nullptr);
            gClear = CreateWindowW(L"BUTTON", L"清空", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                   w - 100, topH + 8, 92, 28, hWnd, (HMENU)IDC_CLEAR, hInst, nullptr);
            ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);
            LVCOLUMNW col{}; col.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            const wchar_t* headers[] = { L"设备号", L"品牌机型", L"DeviceID(sdcard)", L"DeviceID(data)", L"ARM架构", L"Android版本", L"激活状态" };
            int widths[] = { 120, 200, 240, 240, 140, 100, 100 };
            for (int i = 0; i < 7; ++i) { col.pszText = (LPWSTR)headers[i]; col.cx = widths[i]; col.iSubItem = i; ListView_InsertColumn(gList, i, &col); }
            PostMessageW(hWnd, WM_COMMAND, (WPARAM)IDC_REFRESH, 0);
        }
        break;
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // 分析菜单选择:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_LIST_DEVICES:
                {
                    auto devs = ListDevices();
                    if (devs.empty())
                        MessageBoxW(hWnd, L"未检测到设备，请连接并启用USB调试", L"ADB", MB_ICONWARNING);
                    else
                    {
                        std::wstring m = L"检测到设备:\n";
                        for (auto& d : devs) m += d + L"\n";
                        MessageBoxW(hWnd, m.c_str(), L"ADB", MB_OK);
                    }
                }
                break;
            case IDM_PUSH_FILE:
                {
                    auto devs = ListDevices();
                    if (devs.empty())
                    {
                        MessageBoxW(hWnd, L"未检测到设备", L"ADB", MB_ICONWARNING);
                        break;
                    }
                    OPENFILENAMEW ofn{};
                    wchar_t file[MAX_PATH] = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"所有文件\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                    if (!GetOpenFileNameW(&ofn)) break;
                    std::wstring local = file;
                    std::wstring remote = L"/sdcard/" + BaseName(local);
                    bool ok = PushFile(local, remote, devs[0]);
                    MessageBoxW(hWnd, ok ? L"推送成功" : L"推送失败", L"ADB", ok ? MB_ICONINFORMATION : MB_ICONERROR);
                }
                break;
            case IDC_REFRESH:
                {
                    g_devs = GatherDevices();
                    ListView_DeleteAllItems(gList);
                    for (int i = 0; i < (int)g_devs.size(); ++i)
                    {
                        const auto& d = g_devs[i];
                        std::wstring brandModel = Trim(d.brand) + (d.brand.empty() && d.model.empty() ? L"" : L" ") + Trim(d.model);
                        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.iSubItem = 0; it.pszText = (LPWSTR)d.serial.c_str(); ListView_InsertItem(gList, &it);
                        ListView_SetItemText(gList, i, 1, (LPWSTR)brandModel.c_str());
                        ListView_SetItemText(gList, i, 2, (LPWSTR)d.deviceIdSd.c_str());
                        ListView_SetItemText(gList, i, 3, (LPWSTR)d.deviceIdData.c_str());
                        ListView_SetItemText(gList, i, 4, (LPWSTR)d.abi.c_str());
                        ListView_SetItemText(gList, i, 5, (LPWSTR)d.androidVersion.c_str());
                        const wchar_t* stateText = (d.adbStatus == L"device") ? (d.agentRunning ? L"运行中" : L"待激活") : L"不可用";
                        ListView_SetItemText(gList, i, 6, (LPWSTR)stateText);
                    }
                    AppendLog(gLog, L"已刷新设备列表\r\n");
                }
                break;
            case IDC_CLEAR:
                SetWindowTextW(gLog, L"");
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR hdr = (LPNMHDR)lParam;
            if (hdr->hwndFrom == gList && hdr->code == NM_CLICK)
            {
                LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
                if (pia->iItem >= 0 && pia->iSubItem == 6 && pia->iItem < (int)g_devs.size())
                {
                    auto& d = g_devs[pia->iItem];
                    if (d.adbStatus == L"device" && !d.agentRunning)
                    {
                        std::wstring log;
                        bool ok = ActivateAgent(d.serial, log);
                        AppendLog(gLog, log);
                        auto isRunning = [&](const std::wstring& serial)->bool {
                            std::wstring psOut;
                            AdbShell(serial, L"ps -elf | grep -w adb-agent", psOut);
                            std::wstringstream pss(psOut);
                            std::wstring pl;
                            while (std::getline(pss, pl))
                            {
                                std::wstring t = Trim(pl);
                                if (t.empty()) continue;
                                if (t.find(L"adb-agent") == std::wstring::npos) continue;
                                if (t.find(L"grep") != std::wstring::npos) continue;
                                if (t.find(L"sh -c") != std::wstring::npos) continue;
                                return true;
                            }
                            std::wstring pid;
                            AdbShell(serial, L"pidof adb-agent", pid);
                            if (!Trim(pid).empty()) return true;
                            std::wstring pgrep;
                            AdbShell(serial, L"pgrep -f adb-agent", pgrep);
                            if (!Trim(pgrep).empty()) return true;
                            return false;
                        };
                        d.agentRunning = isRunning(d.serial);
                        const wchar_t* stateText = d.agentRunning ? L"运行中" : L"待激活";
                        ListView_SetItemText(gList, pia->iItem, 6, (LPWSTR)stateText);
                    }
                }
            }
            else if (hdr->hwndFrom == gList && hdr->code == NM_CUSTOMDRAW)
            {
                LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
                switch (cd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                    if ((int)cd->iSubItem == 6)
                    {
                        int idx = (int)cd->nmcd.dwItemSpec;
                        if (idx >= 0 && idx < (int)g_devs.size())
                        {
                            const auto& d = g_devs[idx];
                            if (d.adbStatus == L"device") {
                                cd->clrText = d.agentRunning ? RGB(0,128,0) : RGB(200,0,0);
                            } else {
                                cd->clrText = RGB(128,128,128);
                            }
                        }
                    }
                    return CDRF_DODEFAULT;
                }
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: 在此处添加使用 hdc 的任何绘图代码...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_SIZE:
        {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int topH = h * 45 / 100;
            SetWindowPos(gList, nullptr, 8, 8, w - 120, topH - 16, SWP_NOZORDER);
            SetWindowPos(gRefresh, nullptr, w - 100, 8, 92, 28, SWP_NOZORDER);
            SetWindowPos(gLog, nullptr, 8, topH + 8, w - 120, h - topH - 24, SWP_NOZORDER);
            SetWindowPos(gClear, nullptr, w - 100, topH + 8, 92, 28, SWP_NOZORDER);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// “关于”框的消息处理程序。
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
