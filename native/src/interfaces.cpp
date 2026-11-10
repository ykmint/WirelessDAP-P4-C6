#include "stlink_bridge.h"
#include "core.h"
#include "ssh.h"
#include <winrt/base.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#include <map>
#include <cwctype>
static constexpr UINT IO_EVENT = WM_APP + 70;
static std::wstring txt(HWND w, int id)
{
    int n = GetWindowTextLengthW(GetDlgItem(w, id));
    std::wstring s(n + 1, 0);
    GetDlgItemTextW(w, id, s.data(), n + 1);
    s.resize(n);
    return s;
}
static void set(HWND w, int id, const std::wstring &s)
{
    SetDlgItemTextW(w, id, s.c_str());
}
static int sel(HWND w, int id)
{
    return (int)SendDlgItemMessageW(w, id, CB_GETCURSEL, 0, 0);
}
static void combo(HWND w, int id, std::initializer_list<const wchar_t *> items, int index = 0)
{
    SendDlgItemMessageW(w, id, CB_RESETCONTENT, 0, 0);
    for (auto s : items)
        SendDlgItemMessageW(w, id, CB_ADDSTRING, 0, (LPARAM)s);
    SendDlgItemMessageW(w, id, CB_SETCURSEL, index, 0);
}
static unsigned number(const std::wstring &s, unsigned max, int base = 10)
{
    if (s.empty() || s[0] == L'-')
        throw std::runtime_error("Enter a non-negative number");
    size_t n = 0;
    auto v = std::stoull(s, &n, base);
    if (n != s.size() || v > max)
        throw std::runtime_error("Number out of range");
    return (unsigned)v;
}
static std::wstring hexbytes(std::span<const uint8_t> b)
{
    std::wostringstream s;
    for (auto v : b)
        s << std::hex << std::uppercase << std::setfill(L'0') << std::setw(2) << int(v) << L' ';
    return s.str();
}
static Bytes parse_hex(const std::wstring &s)
{
    std::wstring digits;
    for (auto c : s)
    {
        if (iswspace(c) || c == L',' || c == L':' || c == L'-')
            continue;
        if (!iswxdigit(c))
            throw std::runtime_error("HEX: use byte pairs such as 01 02 A0 FF");
        digits += c;
    }
    if (digits.size() % 2)
        throw std::runtime_error("HEX requires complete byte pairs");
    if (digits.size() > 480)
        throw std::runtime_error("Maximum 240 bytes per transaction");
    Bytes out;
    for (size_t i = 0; i < digits.size(); i += 2)
        out.push_back((uint8_t)number(digits.substr(i, 2), 255, 16));
    return out;
}
struct IoEvent
{
    int kind;
    std::wstring message;
};
class Interfaces
{
    HWND window;
    HFONT font = nullptr, titleFont = nullptr, smallFont = nullptr, monoFont = nullptr, brandFont = nullptr;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH canvas = CreateSolidBrush(RGB(246, 246, 246));
    bool busy = false, connected = false;
    int mode = 1;
    std::string token;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::function<void()>> jobs;
    std::atomic<bool> stop = false, autoRx = true;
    std::atomic<int> active = 0;
    struct Saved
    {
        std::wstring hz, address, count, data;
        int a, b, c, frame;
        bool hex, crlf;
    };
    std::map<int, Saved> saved;
    std::shared_ptr<Link> link;
    std::map<int, std::wstring> histories;
    std::unique_ptr<KeilBridge> keil;
    std::unique_ptr<StlinkBridge> stlink;
    std::atomic<bool> stlinkActive = false;
    std::atomic<bool> keilActive = false;
    std::unique_ptr<SshPanel> ssh;
    std::unique_ptr<Engine> engine;
    std::vector<HWND> busWidgets;
    int page = 0;

    void post(int kind, const std::wstring &s)
    {
        auto e = new IoEvent{kind, s};
        if (!PostMessageW(window, IO_EVENT, 0, (LPARAM)e))
            delete e;
    }
    void submit(std::function<void()> f)
    {
        if (busy)
            return;
        busy = true;
        enable();
        {
            std::lock_guard lock(mutex);
            jobs.push(std::move(f));
        }
        cv.notify_one();
    }
    void log(const std::wstring &s)
    {
        post(0, s);
    }
    void append(const std::wstring &s)
    {
        int target = page;
        for (int m = 1; m <= 6; m++)
            if (s.starts_with(name(m) + L" "))
                target = m - 1;
        if (s.starts_with(L"STM32 "))
            target = 6;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t ts[32];
        swprintf_s(ts, L"%02u:%02u:%02u  ", t.wHour, t.wMinute, t.wSecond);
        auto &history = histories[target];
        if (history.size() > 160000)
            history.erase(0, 80000);
        history += ts + s + L"\r\n";
        if (target == page)
        {
            set(window, 432, history);
            SendDlgItemMessageW(window, 432, EM_SETSEL, -1, -1);
            SendDlgItemMessageW(window, 432, EM_SCROLLCARET, 0, 0);
        }
    }
    void require_target_voltage()
    {
        auto bytes = link->request(3);
        std::string status(bytes.begin(), bytes.end());
        int millivolts = json_int(status, "vref_mv");
        if (millivolts < 2700 || millivolts > 3600 || json_int(status, "fault"))
            throw InterfaceError("VREF must be 2.7-3.6V. Connect J1-3 to the target supply. For "
                                 "debugger-powered targets, enable 3.3V first.");
    }
    void enable()
    {
        for (int id = 401; id <= 431; id++)
        {
            bool on = !busy;
            if (id == 401 || id == 402 || id == 403)
                on = on && !connected;
            if (id == 410 || id == 414 || id == 419 || id == 420 || id == 421 || id == 422 || id == 423 ||
                id == 429)
                on = on && connected;
            EnableWindow(GetDlgItem(window, id), on);
        }
        for (int id : {455, 456})
            EnableWindow(GetDlgItem(window, id), connected && !busy && !keilActive && !stlinkActive);
        for (int id : {461, 465})
            EnableWindow(GetDlgItem(window, id), connected && !busy);
        EnableWindow(GetDlgItem(window, 465), connected && !busy && !stlinkActive);
        EnableWindow(GetDlgItem(window, 521), connected && !busy && !keilActive);
        EnableWindow(GetDlgItem(window, 520), !busy && !keilActive && !stlinkActive);
        EnableWindow(GetDlgItem(window, 526), !busy && !keilActive && !stlinkActive);
        EnableWindow(GetDlgItem(window, 527), !busy && !keilActive && !stlinkActive);
        for (int id : {421, 422})
            EnableWindow(GetDlgItem(window, id), connected && !busy && !keilActive && !stlinkActive);
        EnableWindow(GetDlgItem(window, 450), TRUE);
        set(window, 404, connected ? L"断开" : busy ? L"处理中…" : L"连接");
        show_active();
    }
    void save_ui()
    {
        saved[mode] = {txt(window, 406),
                       txt(window, 411),
                       txt(window, 412),
                       txt(window, 417),
                       sel(window, 407),
                       sel(window, 408),
                       sel(window, 409),
                       sel(window, 413),
                       IsDlgButtonChecked(window, 415) == BST_CHECKED,
                       IsDlgButtonChecked(window, 416) == BST_CHECKED};
    }
    static std::wstring name(int m)
    {
        static const wchar_t *names[] = {L"", L"UART1", L"UART2", L"RS485", L"I2C", L"SPI", L"CAN"};
        return names[m];
    }
    void show_active()
    {
        std::wstring text = busy ? L"任务执行中  ·  已开启  " : L"已开启  ";
        for (int m = 1; m <= 6; m++)
            if (active.load() & (1 << m))
                text += name(m) + L"  ";
        if (!active)
            text += L"无";
        set(window, 433, text);
    }
    void config_ui()
    {
        mode = sel(window, 405) + 1;
        combo(window, 407, {L"无校验", L"奇校验", L"偶校验"});
        combo(window, 408, {L"5", L"6", L"7", L"8"}, 3);
        combo(window, 409, {L"1", L"2"});
        set(window, 426, L"校验位");
        set(window, 427, L"数据位");
        set(window, 428, L"停止位");
        if (mode <= 3)
            set(window, 406, L"115200");
        if (mode == 4)
        {
            set(window, 406, L"100000");
            combo(window, 407, {L"7 位地址"});
            combo(window, 408, {L"开漏"});
            combo(window, 409, {L"—"});
            set(window, 411, L"50");
            set(window, 412, L"1");
            set(window, 426, L"寻址");
            set(window, 427, L"输出类型");
            set(window, 428, L"—");
        }
        if (mode == 5)
        {
            set(window, 406, L"1000000");
            combo(window, 407, {L"Mode 0", L"Mode 1", L"Mode 2", L"Mode 3"});
            combo(window, 408, {L"MSB 优先", L"LSB 优先"});
            combo(window, 409, {L"低有效"});
            set(window, 426, L"CPOL / CPHA");
            set(window, 427, L"位序");
            set(window, 428, L"片选");
            set(window, 412, L"0");
        }
        if (mode == 6)
        {
            set(window, 406, L"500000");
            combo(window, 407, {L"正常收发", L"只监听", L"自测回环"});
            combo(window, 408, {L"经典 CAN"});
            combo(window, 409, {L"—"});
            set(window, 426, L"工作模式");
            set(window, 427, L"帧协议");
            set(window, 428, L"—");
            set(window, 411, L"123");
            set(window, 412, L"0");
        }
        const wchar_t *pins[] = {
            L"",
            L"UART1 · J1-5 TX → 目标 RX；J1-6 RX ← 目标 TX；3.3V TTL；接 J1-3 VREF 与 GND。",
            L"UART2 · J1-7 TX → 目标 RX；J1-8 RX ← 目标 TX；3.3V TTL；接 J1-3 VREF 与 GND。",
            L"RS485 · J1-17 A、18 B；共地；半双工自动方向控制。终端电阻按总线两端配置。",
            L"I²C 主机 · J1-9 SDA、10 SCL；接 J1-3 VREF 与 GND；使用 7 位地址，读写支持重复起始。",
            L"SPI 主机 · J1-11 SCK、12 MOSI、13 MISO、14 CS；接 J1-3 VREF 与 GND；每次传输保持 CS。",
            L"CAN · J1-15 CANH、16 CANL；共地；经典 CAN 2.0，最多 8 字节；总线两端需终端电阻。"};
        set(window, 425, pins[mode]);
        if (saved.contains(mode))
        {
            auto &v = saved[mode];
            set(window, 406, v.hz);
            set(window, 411, v.address);
            set(window, 412, v.count);
            set(window, 417, v.data);
            SendDlgItemMessageW(window, 407, CB_SETCURSEL, v.a, 0);
            SendDlgItemMessageW(window, 408, CB_SETCURSEL, v.b, 0);
            SendDlgItemMessageW(window, 409, CB_SETCURSEL, v.c, 0);
            SendDlgItemMessageW(window, 413, CB_SETCURSEL, v.frame, 0);
            CheckDlgButton(window, 415, v.hex ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(window, 416, v.crlf ? BST_CHECKED : BST_UNCHECKED);
        }
        set(window, 470, name(mode));
        for (int id : {411, 471})
            ShowWindow(GetDlgItem(window, id), mode == 4 || mode == 6 ? SW_SHOW : SW_HIDE);
        for (int id : {412, 472})
            ShowWindow(GetDlgItem(window, id), mode >= 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 413), mode == 6 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 414), mode == 4 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 416), mode <= 3 ? SW_SHOW : SW_HIDE);
        set(window, 471, mode == 4 ? L"7 位地址（HEX）" : L"CAN ID（HEX）");
        set(window, 472, mode == 6 ? L"远程帧 DLC" : mode == 5 ? L"传输字节数（0=数据长度）" : L"读取字节数");
        show_active();
    }
    void receive(int m)
    {
        uint8_t channel = m;
        auto b = link->request(19, {&channel, 1});
        if (b.empty())
            return;
        if (m == 6)
        {
            if (b.size() % 14)
                throw std::runtime_error("Malformed CAN receive record");
            for (size_t i = 0; i < b.size(); i += 14)
            {
                unsigned dlc = b[i + 5];
                if (dlc > 8)
                    throw std::runtime_error("Invalid CAN DLC");
                log(L"CAN RX ID=" + hex32(u32(b.data() + i)) + ((b[i + 4] & 1) ? L" EXT" : L" STD") +
                    ((b[i + 4] & 2) ? L" RTR" : L" DATA") + L" DLC=" + std::to_wstring(dlc) + L"  " +
                    ((b[i + 4] & 2) ? L"" : hexbytes({b.data() + i + 6, dlc})));
            }
        }
        else
        {
            log(name(m) + L" RX  " + hexbytes(b));
            std::string s;
            for (auto c : b)
            {
                if (c >= 32 || c == '\r' || c == '\n' || c == '\t')
                    s += (char)c;
                else
                    s += '.';
            }
            log(name(m) + L" TXT " + wide(s));
        }
    }
    void state()
    {
        auto policy = link->request(24);
        if (policy.size() == 1)
            post(11, policy[0] ? L"1" : L"0");
        auto s = link->request(3);
        post(9, wide(std::string(s.begin(), s.end())));
        log(L"电源 / VREF  " + wide(std::string(s.begin(), s.end())));
        auto b = link->request(21);
        std::string json(b.begin(), b.end());
        active = json_int(json, "enabled_mask");
        log(L"并行接口状态  " + wide(json));
        for (int m = 1; m <= 6; m++)
            if (active.load() & (1 << m))
            {
                uint8_t ch = m;
                auto data = link->request(21, {&ch, 1});
                log(name(m) + L"  " + wide(std::string(data.begin(), data.end())));
            }
        post(8, L"");
    }

  public:
    Interfaces(HWND h, bool ble, const std::wstring &endpoint, const std::string &t) : window(h), token(t)
    {
        combo(h, 401, {L"Wi-Fi", L"蓝牙 BLE", L"USB 有线"}, ble ? 1 : 0);
        set(h, 402, endpoint);
        combo(h, 405, {L"UART1", L"UART2", L"RS485", L"I²C", L"SPI", L"CAN"});
        combo(h, 413, {L"CAN 标准数据帧", L"CAN 扩展数据帧", L"CAN 标准远程帧", L"CAN 扩展远程帧"});
        set(h, 411, L"50");
        set(h, 412, L"0");
        set(h, 417, L"01 02 03 04");
        CheckDlgButton(h, 415, BST_CHECKED);
        CheckDlgButton(h, 424, BST_CHECKED);
        SendDlgItemMessageW(h, 417, EM_SETLIMITTEXT, 4096, 0);
        SendDlgItemMessageW(h, 432, EM_SETLIMITTEXT, 180000, 0);
        config_ui();
        init_pages();
        ssh = std::make_unique<SshPanel>(h);
        ssh->show(false);
        enable();
        append(L"连接调试器、确认目标供电后，选择烧录接口和固件。\r\n"
               L"STM32 烧录与第三方工具位于左侧工作区。\r\n"
               L"各接口独立运行，切换页面保留收发。\r\n"
               L"TTL、I²C、SPI 使用 3.3V 电平，并连接 VREF。");
        worker = std::thread([this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto last = std::chrono::steady_clock::now();
            while (!stop)
            {
                std::function<void()> f;
                {
                    std::unique_lock lock(mutex);
                    cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop || !jobs.empty(); });
                    if (stop)
                        break;
                    if (!jobs.empty())
                    {
                        f = std::move(jobs.front());
                        jobs.pop();
                    }
                }
                try
                {
                    if (f)
                    {
                        f();
                        post(1, L"");
                    }
                    else if (link)
                    {
                        if (autoRx)
                        {
                            for (int m : {1, 2, 3, 6})
                                if (active.load() & (1 << m))
                                    receive(m);
                        }
                        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
                        {
                            auto data = link->request(3);
                            post(9, wide(std::string(data.begin(), data.end())));
                            last = std::chrono::steady_clock::now();
                        }
                    }
                }
                catch (const InterfaceError &e)
                {
                    try
                    {
                        auto b = link->request(21);
                        active = json_int(std::string(b.begin(), b.end()), "enabled_mask");
                        post(7, wide(e.what()));
                    }
                    catch (...)
                    {
                        stlink.reset();
                        stlinkActive = false;
                        keil.reset();
                        keilActive = false;
                        if (link)
                            link->close();
                        link.reset();
                        active = 0;
                        post(3, L"状态刷新失败，已断开。");
                    }
                }
                catch (const std::exception &e)
                {
                    stlink.reset();
                    stlinkActive = false;
                    keil.reset();
                    keilActive = false;
                    if (link)
                        link->close();
                    link.reset();
                    active = 0;
                    post(3, wide(e.what()));
                }
                catch (const winrt::hresult_error &e)
                {
                    stlink.reset();
                    stlinkActive = false;
                    keil.reset();
                    keilActive = false;
                    if (link)
                        link->close();
                    link.reset();
                    active = 0;
                    post(3, e.message().c_str());
                }
            }
            if (link)
                link->close();
            winrt::uninit_apartment();
        });
    }
    ~Interfaces()
    {
        stop = true;
        cv.notify_one();
        if (worker.joinable())
            worker.join();
        if (font)
            DeleteObject(font);
        if (titleFont)
            DeleteObject(titleFont);
        if (smallFont)
            DeleteObject(smallFont);
        if (monoFont)
            DeleteObject(monoFont);
        if (brandFont)
            DeleteObject(brandFont);
        DeleteObject(canvas);
        DeleteObject(white);
    }
    int selected_backend() const
    {
        return sel(window, 520);
    }
    int current_page() const
    {
        return page;
    }
    int enabled_channels() const
    {
        return active.load();
    }
    int pixels(int value) const
    {
        return MulDiv(value, GetDpiForWindow(window), 96);
    }
    RECT content_bounds() const
    {
        RECT client;
        GetClientRect(window, &client);
        return {208, 168, MulDiv(client.right, 96, GetDpiForWindow(window)) - 24,
                MulDiv(client.bottom, 96, GetDpiForWindow(window)) - 36};
    }
    void fit()
    {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        int width = 1120, height = 760;
        if (auto text = wcsstr(GetCommandLineW(), L"--preview-width="))
            width = std::clamp(_wtoi(text + 16), 960, 1600);
        if (auto text = wcsstr(GetCommandLineW(), L"--preview-height="))
            height = std::clamp(_wtoi(text + 17), 640, 1100);
        RECT rectangle{0, 0, pixels(width), pixels(height)};
        AdjustWindowRectExForDpi(&rectangle, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                 DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
        width = std::min(int(rectangle.right - rectangle.left),
                         int(monitor.rcWork.right - monitor.rcWork.left) - 40);
        height = std::min(int(rectangle.bottom - rectangle.top),
                          int(monitor.rcWork.bottom - monitor.rcWork.top) - 40);
        SetWindowPos(window, nullptr,
                     monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                     monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width,
                     height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void layout()
    {
        if (!font)
            return;
        RECT client;
        GetClientRect(window, &client);
        int width = MulDiv(client.right, 96, GetDpiForWindow(window));
        int height = MulDiv(client.bottom, 96, GetDpiForWindow(window));
        auto bounds = content_bounds();
        int left = bounds.left, top = bounds.top, right = bounds.right;
        auto place = [&](int id, int x, int y, int w, int h) {
            HWND control = GetDlgItem(window, id);
            MoveWindow(control, pixels(x), pixels(y), pixels(w), pixels(h), TRUE);
        };
        int contentWidth = right - left;
        int addressWidth = contentWidth - 142 - 76 - 84 - 80 - 40;
        place(401, left, 18, 142, 200);
        place(402, left + 152, 18, addressWidth, 32);
        place(403, right - 260, 18, 76, 32);
        place(404, right - 174, 18, 84, 32);
        place(418, right - 80, 18, 80, 32);
        place(460, left, 60, contentWidth, 18);
        ShowWindow(GetDlgItem(window, 450), SW_HIDE);
        ShowWindow(GetDlgItem(window, 500), SW_HIDE);
        ShowWindow(GetDlgItem(window, 470), SW_HIDE);
        for (int index = 0; index < 9; ++index)
            place(600 + index, 14, index < 6 ? 116 + index * 44 : 424 + (index - 6) * 44, 156, 36);
        place(421, right - 278, 112, 86, 32);
        place(422, right - 184, 112, 86, 32);
        place(423, right - 90, 112, 90, 32);
        place(433, left, height - 18, contentWidth, 16);

        int formWidth = page < 6 ? 340 : 430;
        int logLeft = left + formWidth + 16;
        int logWidth = right - logLeft;
        place(429, right - 190, top + 14, 58, 28);
        place(430, right - 124, top + 14, 58, 28);
        place(431, right - 58, top + 14, 58, 28);
        place(424, logLeft + 16, top + 50, 130, 22);
        place(432, logLeft + 16, top + 86, logWidth - 32, bounds.bottom - top - 102);
        set(window, 429, L"读取");
        set(window, 430, L"清空");
        set(window, 431, L"导出");
        ShowWindow(GetDlgItem(window, 429), page < 6 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 424), page < 6 ? SW_SHOW : SW_HIDE);

        if (page < 6)
        {
            place(474, left + 16, top + 18, 308, 18);
            place(406, left + 16, top + 40, 308, 32);
            place(426, left + 16, top + 86, 132, 18);
            place(427, left + 160, top + 86, 76, 18);
            place(428, left + 248, top + 86, 76, 18);
            place(407, left + 16, top + 108, 132, 200);
            place(408, left + 160, top + 108, 76, 200);
            place(409, left + 248, top + 108, 76, 200);
            bool twoColumns = page == 4 || page == 5;
            if (twoColumns)
            {
                place(407, left + 16, top + 108, 148, 200);
                place(427, left + 176, top + 86, 148, 18);
                place(page == 5 ? 413 : 408, left + 176, top + 108, 148, 200);
                ShowWindow(GetDlgItem(window, 409), SW_HIDE);
                ShowWindow(GetDlgItem(window, 428), SW_HIDE);
            }
            if (page == 5)
            {
                set(window, 427, L"帧类型");
                ShowWindow(GetDlgItem(window, 408), SW_HIDE);
            }
            int actionWidth = page == 3 ? 96 : 148;
            place(410, left + 16, top + 156, actionWidth, 32);
            place(419, left + (page == 3 ? 228 : 176), top + 156, actionWidth, 32);
            place(414, left + 122, top + 156, 96, 32);
            place(425, left + 16, top + 204, 308, 42);
            place(471, left + 16, top + 260, 148, 18);
            place(472, left + 176, top + 260, 148, 18);
            place(411, left + 16, top + 282, 148, 32);
            place(412, left + 176, top + 282, 148, 32);
            if (page == 4)
            {
                place(472, left + 16, top + 260, 308, 18);
                place(412, left + 16, top + 282, 308, 32);
            }
            int sendLabel = top + (page >= 3 ? 330 : 260);
            place(475, left + 16, sendLabel, 148, 18);
            place(415, left + 174, sendLabel - 2, 52, 22);
            place(416, left + 232, sendLabel - 2, 96, 22);
            int sendTop = sendLabel + 26;
            int actionTop = bounds.bottom - 48;
            place(417, left + 16, sendTop, 308, std::max(26, actionTop - sendTop - 12));
            place(420, left + 16, actionTop, 308, 32);
            set(window, 474, page < 3 ? L"波特率" : page == 5 ? L"CAN 速率 / bit/s" : L"总线频率 / Hz");
            set(window, 475, L"发送内容");
            set(window, 410, active.load() & (1 << (page + 1)) ? L"更新参数" : L"启用接口");
        }
        if (page == 6)
        {
            place(525, left + 16, top + 18, 180, 18);
            ShowWindow(GetDlgItem(window, 520), SW_HIDE);
            place(526, left + 16, top + 40, 112, 32);
            place(527, left + 136, top + 40, 80, 32);
            place(462, left + 228, top + 18, 186, 18);
            place(451, left + 228, top + 40, 186, 240);
            place(463, left + 16, top + 86, 398, 18);
            place(452, left + 16, top + 108, 350, 32);
            place(469, left + 374, top + 108, 40, 32);
            place(464, left + 16, top + 154, 170, 18);
            place(453, left + 16, top + 176, 170, 32);
            place(454, left + 202, top + 180, 212, 24);
            place(455, left + 16, top + 226, 190, 34);
            place(456, left + 218, top + 226, 196, 34);
            place(466, left + 16, top + 302, 398, 18);
            place(467, left + 16, top + 324, 290, 32);
            place(468, left + 318, top + 324, 96, 32);
            place(465, left + 16, top + 374, 190, 32);
            place(521, left + 218, top + 374, 196, 32);
            place(522, left + 16, top + 420, 398, std::max(26, int(bounds.bottom) - top - 434));
            ShowWindow(GetDlgItem(window, 457), SW_HIDE);
            set(window, 455, L"识别目标");
            set(window, 456, L"烧录并校验");
            set(window, 468, L"配置插件");
        }
        if (page == 7)
        {
            place(458, left + 20, top + 20, formWidth - 40, bounds.bottom - top - 98);
            place(461, left + 20, bounds.bottom - 62, formWidth - 40, 40);
        }
        if (ssh)
        {
            RECT physical{pixels(left), pixels(top), pixels(right), pixels(bounds.bottom)};
            ssh->layout(physical, font, smallFont);
        }
        InvalidateRect(window, nullptr, TRUE);
    }
    void paint_background(HDC dc)
    {
        RECT client;
        GetClientRect(window, &client);
        FillRect(dc, &client, canvas);
        int width = MulDiv(client.right, 96, GetDpiForWindow(window));
        int height = MulDiv(client.bottom, 96, GetDpiForWindow(window));
        auto rect = [&](int x, int y, int w, int h) {
            return RECT{pixels(x), pixels(y), pixels(x + w), pixels(y + h)};
        };
        RECT sidebar = rect(0, 0, 184, height), header = rect(184, 0, width - 184, 88);
        FillRect(dc, &sidebar, white);
        FillRect(dc, &header, white);
        RECT footer = rect(184, height - 24, width - 184, 24);
        FillRect(dc, &footer, white);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(231, 231, 231));
        auto previousPen = SelectObject(dc, border);
        MoveToEx(dc, pixels(184), 0, nullptr);
        LineTo(dc, pixels(184), client.bottom);
        MoveToEx(dc, pixels(184), pixels(88), nullptr);
        LineTo(dc, client.right, pixels(88));
        auto text = [&](const wchar_t *label, HFONT face, int x, int y, int w, int h, COLORREF color) {
            auto old = SelectObject(dc, face);
            RECT area = rect(x, y, w, h);
            SetTextColor(dc, color);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, label, -1, &area,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, old);
        };
        text(L"WirelessDAP", brandFont, 20, 21, 150, 26, RGB(24, 24, 24));
        text(L"无线调试工作台", smallFont, 20, 49, 150, 18, RGB(126, 126, 126));
        text(L"接口", smallFont, 24, 92, 130, 18, RGB(142, 142, 142));
        text(L"工作区", smallFont, 24, 400, 130, 18, RGB(142, 142, 142));
        text(L"ESP32 P4 + C6", smallFont, 24, height - 54, 140, 18, RGB(100, 100, 100));
        text(L"WIRELESSDAP 2.2", smallFont, 24, height - 33, 140, 16, RGB(150, 150, 150));
        static const wchar_t *titles[]{L"UART 1", L"UART 2",           L"RS485",      L"I²C",     L"SPI",
                                       L"CAN",    L"STM32 烧录与调试", L"电源与引脚", L"SSH 终端"};
        static const wchar_t *descriptions[]{L"串行通信与数据监视",
                                             L"独立串行通道",
                                             L"半双工总线通信",
                                             L"设备发现与主机读写",
                                             L"同步串行数据交换",
                                             L"经典 CAN 帧收发",
                                             L"J1-1 SWDIO · J1-2 SWCLK · J1-3 VREF · J1-4 GND",
                                             L"目标供电、参考电压和接口定义",
                                             L"连接远程主机，在工作台中直接操作"};
        auto bounds = content_bounds();
        text(titles[page], titleFont, bounds.left, 105, bounds.right - bounds.left - 294, 30,
             RGB(25, 25, 25));
        text(descriptions[page], smallFont, bounds.left, 140, bounds.right - bounds.left, 18,
             RGB(116, 116, 116));
        auto card = [&](int left, int top, int right, int bottom) {
            auto oldBrush = SelectObject(dc, white);
            RoundRect(dc, pixels(left), pixels(top), pixels(right), pixels(bottom), pixels(10), pixels(10));
            SelectObject(dc, oldBrush);
        };
        if (page == 8)
        {
            card(bounds.left, bounds.top, bounds.right, bounds.top + 156);
            card(bounds.left, bounds.top + 172, bounds.right, bounds.bottom);
            text(L"终端", font, bounds.left + 20, bounds.top + 185, 74, 28, RGB(35, 35, 35));
        }
        else
        {
            int formWidth = page < 6 ? 340 : 430;
            card(bounds.left, bounds.top, bounds.left + formWidth, bounds.bottom);
            card(bounds.left + formWidth + 16, bounds.top, bounds.right, bounds.bottom);
            text(L"数据记录", font, bounds.left + formWidth + 32, bounds.top + 14, 110, 28, RGB(35, 35, 35));
            if (page == 6)
            {
                text(L"第三方工具", font, bounds.left + 16, bounds.top + 270, 280, 24, RGB(35, 35, 35));
                MoveToEx(dc, pixels(bounds.left + 16), pixels(bounds.top + 294), nullptr);
                LineTo(dc, pixels(bounds.left + 414), pixels(bounds.top + 294));
            }
        }
        SelectObject(dc, previousPen);
        DeleteObject(border);
    }

    void init_pages()
    {
        RECT upper{0, 95, 660, 306};
        MapDialogRect(window, &upper);
        MapWindowPoints(window, HWND_DESKTOP, (POINT *)&upper, 2);
        RECT lower = upper;
        lower.top = upper.bottom;
        for (HWND c = GetWindow(window, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            RECT r;
            GetWindowRect(c, &r);
            if (r.top >= upper.top && r.bottom <= lower.top && GetDlgCtrlID(c) != 450)
                busWidgets.push_back(c);
        }
        const wchar_t *names[] = {L"UART1", L"UART2",      L"RS485",      L"I²C", L"SPI",
                                  L"CAN",   L"STM32 烧录", L"电源与引脚", L"SSH"};
        for (int i = 0; i < 9; i++)
        {
            TCITEMW t{};
            t.mask = TCIF_TEXT;
            t.pszText = const_cast<wchar_t *>(names[i]);
            TabCtrl_InsertItem(GetDlgItem(window, 450), i, &t);
        }
        auto make = [&](int id, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w,
                        int h) {
            RECT r{x, y, x + w, y + h};
            MapDialogRect(window, &r);
            HWND c =
                CreateWindowW(cls, text, WS_CHILD | style, r.left, r.top, r.right - r.left, r.bottom - r.top,
                              window, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, SendMessageW(window, WM_GETFONT, 0, 0), TRUE);
        };
        make(451, L"COMBOBOX", L"",
             CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 14, 116, 275,
             180);
        for (auto &p : profiles)
            SendDlgItemMessageW(window, 451, CB_ADDSTRING, 0, (LPARAM)p.name);
        SendDlgItemMessageW(window, 451, CB_SETCURSEL, 0, 0);
        make(452, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 156, 520, 22);
        SendDlgItemMessageW(window, 452, EM_SETCUEBANNER, TRUE,
                            (LPARAM)L"固件完整路径，支持粘贴 BIN / HEX / ELF / AXF");
        make(453, L"EDIT", L"0x08000000", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 194, 130, 22);
        make(454, L"BUTTON", L"校验后运行", BS_AUTOCHECKBOX | WS_TABSTOP, 160, 194, 120, 22);
        CheckDlgButton(window, 454, BST_CHECKED);
        make(455, L"BUTTON", L"识别并检查目标", BS_PUSHBUTTON | WS_TABSTOP, 14, 235, 150, 26);
        make(456, L"BUTTON", L"烧录并读回校验", BS_PUSHBUTTON | WS_TABSTOP, 180, 235, 150, 26);
        make(457, L"STATIC",
             L"SWD：J1-1 SWDIO、J1-2 SWCLK、J1-3 VREF、J1-4 GND。使用上方同一连接。目标 NRST "
             L"未引出。Keil 选 SYSRESETREQ，并取消 Reset after Connect。",
             0, 14, 270, 632, 30);
        make(461, L"BUTTON", L"USB DAP 连接时，目标未供电则开启 J1-21 的 3.3V",
             BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 14, 280, 620, 20);
        make(462, L"STATIC", L"目标系列", 0, 14, 100, 275, 12);
        make(463, L"STATIC", L"固件文件（完整路径）", 0, 14, 140, 520, 12);
        make(464, L"STATIC", L"BIN 起始地址", 0, 14, 178, 130, 12);
        make(465, L"BUTTON", L"开启 Keil 无线桥", BS_PUSHBUTTON | WS_TABSTOP, 350, 235, 150, 26);
        make(466, L"STATIC", L"Keil 安装目录", 0, 350, 178, 275, 12);
        make(467, L"EDIT", fs::exists(L"E:/keil5/TOOLS.INI") ? L"E:\\keil5" : L"C:\\Keil_v5",
             WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 350, 194, 180, 22);
        make(468, L"BUTTON", L"配置 Keil", BS_PUSHBUTTON | WS_TABSTOP, 540, 194, 105, 22);
        make(458, L"EDIT",
             L"J1-3 VREF：输入，接目标实际 3.3V 电源轨；2.7–3.6V 为本板接口允许范围。\r\nJ1-21：受控 3.3V "
             L"输出，默认关闭；目标已外部供电时不要连接此输出。\r\nJ1-23：硬件直出 "
             L"5V，软件没有开关或独立测量功能。\r\nJ1-4/19/20/22/24：公共地。\r\nJ1-1/2：SWDIO/SWCLK；5/"
             L"6：UART1；7/8：UART2；9/10：I²C。\r\nJ1-11/12/13/14：SPI "
             L"SCK/MOSI/MISO/CS；15/16：CANH/CANL；17/18：RS485 A/B。\r\nTTL、I²C、SPI 使用 3.3V 电平。VREF "
             L"丢失会释放相关接口。\r\n断开连接关闭全部接口和受控 3.3V；不会关闭硬件直出 5V。",
             ES_MULTILINE | ES_READONLY | WS_VSCROLL, 14, 116, 632, 160);
        make(500, L"STATIC", L"WirelessDAP", 0, 14, 7, 174, 22);
        ShowWindow(GetDlgItem(window, 500), SW_SHOW);
        make(460, L"STATIC", L"未连接 · 连接后显示目标电压与供电状态", 0, 205, 12, 441, 16);
        ShowWindow(GetDlgItem(window, 460), SW_SHOW);
        const wchar_t *navigation[]{L"UART1", L"UART2",      L"RS485",      L"I²C",     L"SPI",
                                    L"CAN",   L"STM32 烧录", L"电源与引脚", L"SSH 终端"};
        for (int i = 0; i < 9; ++i)
        {
            make(600 + i, L"BUTTON", navigation[i], BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 20);
            ShowWindow(GetDlgItem(window, 600 + i), SW_SHOW);
        }
        make(520, L"COMBOBOX", L"",
             CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 0, 100,
             100);
        combo(window, 520, {L"CMSIS-DAP", L"ST-LINK"});
        make(525, L"STATIC", L"烧录接口", 0, 0, 0, 100, 15);
        make(526, L"BUTTON", L"CMSIS-DAP", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 100, 20);
        make(527, L"BUTTON", L"ST-LINK", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 100, 20);
        make(521, L"BUTTON", L"ST-LINK 兼容桥", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 100, 20);
        make(522, L"STATIC", L"Keil 使用 SYSRESETREQ。ST-LINK 通过本机共享服务接入。", 0, 0, 0, 100, 30);
        make(469, L"BUTTON", L"…", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 40, 20);
        set(window, 461, L"USB DAP 接入时自动为未供电的目标开启 3.3V");
        set(window, 458,
            L"供电与参考\r\n\r\nJ1-3   VREF 输入，接目标实际 3.3V\r\n允许范围为 2.7–3.6V\r\nJ1-21  可控 3.3V "
            L"输出，默认关闭\r\nJ1-23  硬件直出 5V\r\n\r\n信号接口\r\n\r\nSWD     1 SWDIO / 2 SWCLK\r\nUART1 "
            L"  5 TX / 6 RX\r\nUART2   7 TX / 8 RX\r\nI²C     9 SDA / 10 SCL\r\nSPI     11 SCK / 12 MOSI / "
            L"13 MISO / 14 CS\r\nCAN     15 CANH / 16 CANL\r\nRS485   17 A / 18 B\r\n\r\n公共地   4 / 19 / "
            L"20 / 22 / 24\r\n\r\n外部已供电时，J1-21 留空。\r\n断开连接后关闭受控供电与接口。");
        TabCtrl_SetCurSel(GetDlgItem(window, 450), 6);
        select_page(6);
    }
    void select_page(int next)
    {
        if (page < 6)
            save_ui();
        page = next;
        set(window, 432, histories[page]);
        for (HWND h : busWidgets)
            ShowWindow(h, next < 6 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 405), SW_HIDE);
        for (int id : {451, 452, 453, 454, 455, 456, 457, 462, 463, 464, 465,
                       466, 467, 468, 469, 520, 521, 522, 525, 526, 527})
            ShowWindow(GetDlgItem(window, id), next == 6 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 458), next == 7 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(window, 461), next == 7 ? SW_SHOW : SW_HIDE);
        for (int id = 421; id <= 424; id++)
            ShowWindow(GetDlgItem(window, id), next == 8 ? SW_HIDE : SW_SHOW);
        for (int id = 429; id <= 433; id++)
            ShowWindow(GetDlgItem(window, id), next == 8 ? SW_HIDE : SW_SHOW);
        if (ssh)
            ssh->show(next == 8);
        if (next < 6)
        {
            SendDlgItemMessageW(window, 405, CB_SETCURSEL, next, 0);
            config_ui();
        }
        layout();
    }
    void file_drop(const std::wstring &path)
    {
        if (busy)
        {
            append(L"当前操作进行中，完成后可拖入固件。");
            return;
        }
        TabCtrl_SetCurSel(GetDlgItem(window, 450), 6);
        select_page(6);
        set(window, 452, path);
    }
    bool ssh_message(UINT message, WPARAM w, LPARAM l)
    {
        return ssh && ssh->message(message, w, l);
    }
    void style()
    {
        HFONT previous[]{font, titleFont, smallFont, monoFont, brandFont};
        auto makeFont = [&](int size, int weight, const wchar_t *face) {
            return CreateFontW(-pixels(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               face);
        };
        font = makeFont(14, FW_NORMAL, L"Microsoft YaHei UI");
        titleFont = makeFont(22, FW_SEMIBOLD, L"Microsoft YaHei UI");
        smallFont = makeFont(12, FW_NORMAL, L"Microsoft YaHei UI");
        monoFont = makeFont(13, FW_NORMAL, L"Consolas");
        brandFont = makeFont(18, FW_SEMIBOLD, L"Segoe UI");
        SendMessageW(window, WM_SETFONT, (WPARAM)font, FALSE);
        for (HWND control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        {
            wchar_t name[32];
            GetClassNameW(control, name, 32);
            int id = GetDlgCtrlID(control);
            SendMessageW(control, WM_SETFONT, (WPARAM)font, FALSE);
            if (wcscmp(name, L"Static") == 0)
                SendMessageW(control, WM_SETFONT, (WPARAM)smallFont, FALSE);
            if (wcscmp(name, L"Edit") == 0)
            {
                SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                             MAKELPARAM(pixels(8), pixels(8)));
                auto flags = GetWindowLongPtrW(control, GWL_STYLE);
                SetWindowLongPtrW(control, GWL_STYLE, id == 432 ? flags & ~WS_BORDER : flags | WS_BORDER);
                SetWindowLongPtrW(control, GWL_EXSTYLE,
                                  GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
                SetWindowPos(control, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                if (id == 417)
                    SendMessageW(control, WM_SETFONT, (WPARAM)monoFont, FALSE);
            }
            if (wcscmp(name, L"ComboBox") == 0)
            {
                SendMessageW(control, CB_SETITEMHEIGHT, -1, pixels(26));
                SendMessageW(control, CB_SETITEMHEIGHT, 0, pixels(28));
                SendMessageW(control, CB_SETMINVISIBLE, 7, 0);
            }
            if (wcscmp(name, L"Button") == 0)
            {
                auto flags = GetWindowLongPtrW(control, GWL_STYLE);
                auto type = flags & BS_TYPEMASK;
                if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_OWNERDRAW)
                {
                    SetWindowLongPtrW(control, GWL_STYLE, (flags & ~BS_TYPEMASK) | BS_OWNERDRAW);
                    SetWindowSubclass(
                        control,
                        [](HWND child, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) -> LRESULT {
                            if (message == WM_MOUSEMOVE && !GetPropW(child, L"WD.Hover"))
                            {
                                SetPropW(child, L"WD.Hover", (HANDLE)1);
                                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, child, 0};
                                TrackMouseEvent(&track);
                                InvalidateRect(child, nullptr, FALSE);
                            }
                            if (message == WM_MOUSELEAVE || message == WM_NCDESTROY)
                            {
                                RemovePropW(child, L"WD.Hover");
                                InvalidateRect(child, nullptr, FALSE);
                            }
                            return DefSubclassProc(child, message, w, l);
                        },
                        90, 0);
                }
            }
        }
        for (auto old : previous)
            if (old)
                DeleteObject(old);
        layout();
    }
    HBRUSH brush()
    {
        return white;
    }
    void event(IoEvent *raw)
    {
        std::unique_ptr<IoEvent> e(raw);
        set(window, 465, keilActive ? L"关闭 Keil 桥" : L"开启 Keil 桥");
        set(window, 521, stlinkActive ? L"关闭 ST-LINK 桥" : L"开启 ST-LINK 桥");
        if (e->kind == 11)
        {
            CheckDlgButton(window, 461, e->message == L"1" ? BST_CHECKED : BST_UNCHECKED);
            return;
        }
        if (e->kind == 10)
        {
            set(window, 402, e->message);
            return;
        }
        if (e->kind == 9)
        {
            auto j = utf8(e->message);
            set(window, 460,
                L"VREF " + std::to_wstring(json_int(j, "vref_mv")) + L" mV  |  目标 3.3V：" +
                    (json_int(j, "target_power") ? L"已开启" : L"关闭") + L"  |  " +
                    (json_int(j, "fault") ? L"电源故障" : L"电源正常"));
            return;
        }
        if (e->kind == 0)
            append(e->message);
        else if (e->kind == 2)
        {
            connected = true;
            append(e->message);
        }
        else if (e->kind == 3)
        {
            busy = false;
            connected = false;
            set(window, 460, L"连接中断 · 设备状态停止更新");
            append(L"错误：" + e->message + L" · 已断开连接，请检查后重新连接。");
        }
        else if (e->kind == 4)
        {
            set(window, 402, e->message);
            append(L"已选择蓝牙设备 " + e->message);
        }
        else if (e->kind == 5)
        {
            connected = false;
            set(window, 460, L"已断开 · 设备状态停止更新");
            append(e->message);
        }
        else if (e->kind == 6)
        {
            EndDialog(window, 0);
            return;
        }
        else if (e->kind == 7)
        {
            busy = false;
            append(L"接口操作失败：" + e->message + L"；未自动重发，其他已启用接口保留。");
        }
        else if (e->kind != 8)
            busy = false;
        show_active();
        enable();
    }
    void close()
    {
        if (busy)
        {
            append(L"请等待当前操作结束后关闭。");
            return;
        }
        submit([this] {
            stlink.reset();
            stlinkActive = false;
            keil.reset();
            keilActive = false;
            if (link)
                link->disconnect();
            link.reset();
            active = 0;
            post(6, L"");
        });
    }
    void command(int id, int notification)
    {
        if (id >= 600 && id <= 608 && notification == BN_CLICKED)
        {
            TabCtrl_SetCurSel(GetDlgItem(window, 450), id - 600);
            select_page(id - 600);
            return;
        }
        if ((id == 526 || id == 527) && notification == BN_CLICKED)
        {
            if (!busy && !keilActive && !stlinkActive)
            {
                SendDlgItemMessageW(window, 520, CB_SETCURSEL, id - 526, 0);
                InvalidateRect(GetDlgItem(window, 526), nullptr, TRUE);
                InvalidateRect(GetDlgItem(window, 527), nullptr, TRUE);
            }
            return;
        }
        if (id == 405 && notification == CBN_SELCHANGE)
        {
            save_ui();
            config_ui();
            return;
        }
        if (id == 401 && notification == CBN_SELCHANGE)
        {
            int t = sel(window, 401);
            set(window, 402, t == 0 ? L"192.168.4.1" : L"");
            return;
        }
        if (notification != BN_CLICKED)
            return;
        if (busy)
            return;
        try
        {

            if (id == 469)
            {
                wchar_t file[32768]{};
                OPENFILENAMEW dialog{sizeof(dialog)};
                dialog.hwndOwner = window;
                dialog.lpstrFile = file;
                dialog.nMaxFile = 32768;
                dialog.lpstrFilter = L"固件文件\0*.bin;*.hex;*.elf;*.axf\0所有文件\0*.*\0";
                dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&dialog))
                    set(window, 452, file);
                return;
            }
            if (id == 468)
            {
                auto root = txt(window, 467);
                submit([this, root] {
                    install_keil_driver(root, unpack_assets());
                    log(L"STM32 Keil 插件已配置。选择 elaphureLink Debugger、SWD、SYSRESETREQ，取消 Reset "
                        L"after Connect，再开启本页 Keil 桥。");
                });
                return;
            }
            if (id == 521)
            {
                if (!connected || keilActive)
                    return;
                submit([this] {
                    if (stlink)
                    {
                        auto count = stlink->request_count();
                        stlink.reset();
                        stlinkActive = false;
                        log(L"STM32 ST-LINK 桥已关闭，已处理 " + std::to_wstring(count) + L" 条命令。");
                    }
                    else
                    {
                        try
                        {
                            require_target_voltage();
                            stlink = std::make_unique<StlinkBridge>(
                                link, [this](const auto &text) { log(L"STM32 " + text); }, 7185);
                        }
                        catch (const std::exception &error)
                        {
                            throw InterfaceError(error.what());
                        }
                        stlinkActive = true;
                        log(L"STM32 ST-LINK 共享服务已启动，127.0.0.1 端口 7185。"
                            L"CubeProgrammer 选择 SWD、Shared、Hotplug 和 100kHz。");
                    }
                });
                return;
            }

            if (id == 465)
            {
                if (!connected)
                {
                    append(L"先连接调试器，再开启 Keil 桥。");
                    return;
                }
                submit([this] {
                    if (keil)
                    {
                        stlink.reset();
                        stlinkActive = false;
                        keil.reset();
                        keilActive = false;
                        log(L"STM32 Keil 桥已关闭。");
                    }
                    else
                    {
                        require_target_voltage();
                        keil = std::make_unique<KeilBridge>(link, [this](auto &s) { log(L"STM32 " + s); });
                        keilActive = true;
                        log(L"STM32 Keil 桥已开启。Keil 选择 elaphureLink "
                            L"Debugger、SWD、SYSRESETREQ，取消 Reset after "
                            L"Connect；本窗口保持连接。其他已启用接口继续运行。");
                    }
                });
                return;
            }
            if (id == 461)
            {
                if (!connected)
                {
                    CheckDlgButton(window, 461, BST_UNCHECKED);
                    append(L"连接后才能保存 USB DAP 供电策略。");
                    return;
                }
                bool on = IsDlgButtonChecked(window, 461) == BST_CHECKED;
                submit([this, on] {
                    uint8_t value = on;
                    link->request(24, {&value, 1});
                    log(L"USB DAP 自动供电策略已保存；外部已供电时保持 J1-21 关闭。");
                });
                return;
            }
            if (id == 455 || id == 456)
            {
                if (!connected || keilActive || stlinkActive)
                    return;
                bool flash = id == 456;
                bool stlinkMode = sel(window, 520) == 1;
                int profile = sel(window, 451);
                auto file = txt(window, 452);
                auto first = file.find_first_not_of(L" \t\r\n");
                auto last = file.find_last_not_of(L" \t\r\n");
                file = first == std::wstring::npos ? L"" : file.substr(first, last - first + 1);
                if (file.size() >= 2 && file.front() == L'"' && file.back() == L'"')
                    file = file.substr(1, file.size() - 2);
                unsigned base = number(txt(window, 453), UINT32_MAX, 0);
                bool run = IsDlgButtonChecked(window, 454) == BST_CHECKED;
                if (flash)
                    parse_image(file, base);
                submit([this, flash, profile, file, base, run, stlinkMode] {
                    if (!engine)
                    {
                        engine = std::make_unique<Engine>(unpack_assets());
                        engine->background = [this] {
                            if (autoRx)
                                for (int m : {1, 2, 3, 6})
                                    if (active.load() & (1 << m))
                                        receive(m);
                        };
                    }
                    require_target_voltage();
                    engine->useStlink = stlinkMode;
                    auto target = detect_stm32(*link);
                    int selected = profile ? profile : target.profile;
                    if (!selected)
                        throw InterfaceError("Select the STM32 family; automatic detection was inconclusive");
                    log(L"STM32 目标：" + std::wstring(profiles[selected].name) + L" DP=" + hex32(target.dp));
                    if (flash)
                    {
                        engine->flash(link, selected, file, base, run,
                                      [this](auto &s) { log(L"STM32 " + s); });
                        log(L"STM32 烧录及读回校验完成。");
                    }
                    else
                    {
                        engine->check(link, selected, [this](auto &s) { log(L"STM32 " + s); });
                        log(L"STM32 目标检查完成。");
                    }
                });
                return;
            }
            if (id == 424)
            {
                autoRx = IsDlgButtonChecked(window, 424) == BST_CHECKED;
                return;
            }
            if (id == 403)
            {
                if (sel(window, 401) == 2)
                {
                    submit([this] {
                        auto ports = scan_usb();
                        if (ports.empty())
                        {
                            log(L"未发现 ESP USB 端口，请检查 Type-C 连接。");
                            return;
                        }
                        for (auto &p : ports)
                            log(L"USB " + p);
                        post(10, ports.front());
                    });
                    return;
                }
                if (sel(window, 401) != 1)
                {
                    append(L"USB 请填写设备管理器中的 COM 端口；Wi-Fi 请填写调试器 IP。");
                    return;
                }
                submit([this] {
                    auto list = scan_ble();
                    if (list.empty())
                        throw std::runtime_error("No WirelessDAP BLE advertisement");
                    for (auto &d : list)
                    {
                        wchar_t a[32];
                        swprintf_s(a, L"%012llX", d.address);
                        log(d.name + L"  " + a);
                    }
                    wchar_t a[32];
                    swprintf_s(a, L"%012llX", list[0].address);
                    post(4, a);
                });
                return;
            }
            if (id == 404)
            {
                if (connected)
                {
                    submit([this] {
                        stlink.reset();
                        stlinkActive = false;
                        keil.reset();
                        keilActive = false;
                        if (link)
                            link->disconnect();
                        link.reset();
                        active = 0;
                        post(5, L"已断开，受控供电关闭。");
                    });
                    return;
                }
                int transport = sel(window, 401);
                auto endpoint = txt(window, 402);
                submit([this, transport, endpoint] {
                    if (transport == 2)
                    {
                        link = connect_usb(endpoint, token);
                    }
                    else if (transport == 1)
                    {
                        std::wstring address;
                        for (auto c : endpoint)
                            if (c != L':' && c != L'-' && c != L' ')
                                address += c;
                        size_t used = 0;
                        auto a = std::stoull(address, &used, 16);
                        if (used != address.size() || a > 0xffffffffffffULL)
                            throw std::runtime_error("Invalid BLE address");
                        link = connect_ble(a, token);
                    }
                    else
                        link = connect_tcp(endpoint, 6123, token);
                    auto caps = link->request(16);
                    std::string s(caps.begin(), caps.end());
                    if (json_int(s, "j1_api") != 2)
                        throw std::runtime_error("Update P4 J1 firmware first");
                    log(wide(s));
                    state();
                    post(2, L"接口工作台已连接，无需连接 STM32。");
                });
                return;
            }
            if (id == 410)
            {
                save_ui();
                int m = mode;
                unsigned hz = number(txt(window, 406), 10000000);
                Bytes p{(uint8_t)m};
                put32(p, hz);
                p.push_back((uint8_t)sel(window, 407));
                p.push_back(m <= 3   ? (uint8_t)(sel(window, 408) + 5)
                            : m == 5 ? (uint8_t)sel(window, 408)
                                     : 0);
                p.push_back(m <= 3 ? (uint8_t)(sel(window, 409) + 1) : 0);
                submit([this, p, m] {
                    link->request(17, p);
                    active.fetch_or(1 << m);
                    log(name(m) + L" 配置已应用，其他接口保持运行。");
                    state();
                });
                return;
            }
            if (id == 419)
            {
                int m = mode;
                submit([this, m] {
                    uint8_t ch = m;
                    link->request(22, {&ch, 1});
                    active.fetch_and(~(1 << m));
                    log(name(m) + L" 已关闭，其他接口保持运行。");
                    post(8, L"");
                });
                return;
            }
            if (id == 421 || id == 422)
            {
                bool on = id == 421;
                submit([this, on] {
                    uint8_t v = on;
                    link->request(4, {&v, 1});
                    if (!on)
                        active = 0;
                    log(on ? L"受控 3.3V 已开启；请将 J1-3 接目标实际电源轨。"
                           : L"受控 3.3V 已关闭，接口已释放。");
                    state();
                });
                return;
            }
            if (id == 423)
            {
                submit([this] { state(); });
                return;
            }
            if (id == 429)
            {
                int m = mode;
                if (!(active.load() & (1 << m)) || (m != 1 && m != 2 && m != 3 && m != 6))
                    throw std::runtime_error("Enable selected UART/RS485/CAN first");
                submit([this, m] { receive(m); });
                return;
            }
            if (id == 414)
            {
                if (mode != 4 || !(active.load() & (1 << 4)))
                    throw std::runtime_error("Apply I2C configuration first");
                submit([this] {
                    Bytes found;
                    for (unsigned start = 8; start <= 0x77; start += 16)
                    {
                        uint8_t range[] = {4, (uint8_t)start, (uint8_t)std::min(start + 15, 0x77u)};
                        auto b = link->request(20, range);
                        found.insert(found.end(), b.begin(), b.end());
                        if (autoRx)
                        {
                            for (int m : {1, 2, 3, 6})
                                if (active.load() & (1 << m))
                                    receive(m);
                        }
                    }
                    log(found.empty() ? L"I²C 扫描完成：未收到设备 ACK。"
                                      : L"I²C 7 位地址：" + hexbytes(found));
                });
                return;
            }
            if (id == 420)
            {
                int m = mode;
                if (!(active.load() & (1 << m)))
                    throw std::runtime_error("Apply selected interface configuration first");
                Bytes b;
                if (IsDlgButtonChecked(window, 415) == BST_CHECKED)
                    b = parse_hex(txt(window, 417));
                else
                {
                    auto s = utf8(txt(window, 417));
                    b.assign(s.begin(), s.end());
                }
                if (IsDlgButtonChecked(window, 416) == BST_CHECKED && m <= 3)
                {
                    b.push_back(13);
                    b.push_back(10);
                }
                if (b.size() > 240)
                    throw std::runtime_error("Maximum 240 bytes per transaction");
                Bytes p;
                if (m == 4)
                {
                    unsigned addr = number(txt(window, 411), 0x77, 16), rx = number(txt(window, 412), 240);
                    if (addr < 8)
                        throw std::runtime_error("I2C address range 08..77");
                    p = {(uint8_t)addr, (uint8_t)rx};
                    p.insert(p.end(), b.begin(), b.end());
                }
                else if (m == 5)
                {
                    unsigned count = number(txt(window, 412), 240);
                    p = b;
                    if (count > p.size())
                        p.resize(count, 0xff);
                }
                else if (m == 6)
                {
                    unsigned flags = sel(window, 413);
                    auto id = number(txt(window, 411), (flags & 1) ? 0x1fffffff : 0x7ff, 16);
                    unsigned dlc = (flags & 2) ? number(txt(window, 412), 8) : (unsigned)b.size();
                    if (dlc > 8)
                        throw std::runtime_error("Classic CAN allows at most 8 bytes");
                    put32(p, id);
                    p.push_back(flags);
                    p.push_back(dlc);
                    if (!(flags & 2))
                        p.insert(p.end(), b.begin(), b.end());
                }
                else
                    p = b;
                if (p.empty())
                    throw std::runtime_error("Enter data or a read length");
                p.insert(p.begin(), (uint8_t)m);
                submit([this, p, m] {
                    log(name(m) + L" TX  " + hexbytes(std::span<const uint8_t>(p).subspan(1)));
                    auto b = link->request(18, p);
                    if (!b.empty())
                        log(name(m) + L" RX  " + hexbytes(b));
                    else
                        log(m == 6 ? L"CAN 发送已完成。" : L"发送已完成。");
                });
                return;
            }
            if (id == 430)
            {
                histories[page].clear();
                set(window, 432, L"");
                return;
            }
            if (id == 431)
            {
                wchar_t path[32768] = L"J1接口记录.txt";
                OPENFILENAMEW o{sizeof(o)};
                o.hwndOwner = window;
                o.lpstrFilter = L"文本文件\0*.txt\0";
                o.lpstrFile = path;
                o.nMaxFile = 32768;
                o.lpstrDefExt = L"txt";
                o.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                if (GetSaveFileNameW(&o))
                {
                    auto s = utf8(txt(window, 432));
                    write_file(path, {(const uint8_t *)s.data(), s.size()});
                }
                return;
            }
            if (id == 418)
            {
                TabCtrl_SetCurSel(GetDlgItem(window, 450), 7);
                select_page(7);
                return;
            }
        }
        catch (const std::exception &e)
        {
            append(L"参数检查：" + wide(e.what()));
        }
    }
};
struct IoInit
{
    bool ble;
    std::wstring endpoint;
    std::string token;
};
static bool draw_control(DRAWITEMSTRUCT *item)
{
    if (item->CtlType != ODT_BUTTON)
        return false;
    HDC dc = item->hDC;
    RECT area = item->rcItem;
    int dpi = GetDpiForWindow(item->hwndItem);
    auto px = [&](int value) { return MulDiv(value, dpi, 96); };
    bool disabled = item->itemState & ODS_DISABLED;
    bool navigation = item->CtlID >= 600 && item->CtlID <= 608;
    auto owner = (Interfaces *)GetWindowLongPtrW(GetParent(item->hwndItem), DWLP_USER);
    bool selected = navigation && owner && owner->current_page() == int(item->CtlID) - 600;
    bool hover = GetPropW(item->hwndItem, L"WD.Hover") && !disabled;
    bool backendSelected = (item->CtlID == 526 || item->CtlID == 527) && owner &&
                           owner->selected_backend() == int(item->CtlID) - 526;
    bool primary =
        backendSelected || (!navigation && (item->CtlID == 404 || item->CtlID == 410 || item->CtlID == 420 ||
                                            item->CtlID == 456 || item->CtlID == 484));
    COLORREF fill = primary && !disabled ? RGB(25, 25, 25) : RGB(255, 255, 255);
    if (selected)
        fill = RGB(235, 235, 235);
    else if (hover || (item->itemState & ODS_SELECTED))
        fill = primary ? RGB(51, 51, 51) : RGB(246, 246, 246);
    if (disabled)
        fill = backendSelected ? RGB(80, 80, 80) : RGB(246, 246, 246);
    HBRUSH background = CreateSolidBrush(fill);
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(dc, &area, white);
    HPEN border = CreatePen(PS_SOLID, 1,
                            navigation             ? fill
                            : primary && !disabled ? fill
                                                   : RGB(220, 220, 220));
    auto oldPen = SelectObject(dc, border);
    auto oldBrush = SelectObject(dc, background);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, px(7), px(7));
    wchar_t label[256]{};
    GetWindowTextW(item->hwndItem, label, 256);
    auto oldFont = SelectObject(dc, (HFONT)SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(165, 165, 165) : primary ? RGB(255, 255, 255) : RGB(38, 38, 38));
    RECT text = area;
    UINT alignment = DT_CENTER;
    if (navigation)
    {
        RECT icon{area.left + px(12), area.top + px(10), area.left + px(28), area.top + px(26)};
        SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        HPEN iconPen = CreatePen(PS_SOLID, px(1), RGB(98, 98, 98));
        SelectObject(dc, iconPen);
        RoundRect(dc, icon.left, icon.top, icon.right, icon.bottom, px(3), px(3));
        MoveToEx(dc, icon.left + px(4), icon.top + px(5), nullptr);
        LineTo(dc, icon.right - px(4), icon.top + px(5));
        MoveToEx(dc, icon.left + px(4), icon.top + px(10), nullptr);
        LineTo(dc, icon.right - px(4), icon.top + px(10));
        SelectObject(dc, border);
        DeleteObject(iconPen);
        if (owner && item->CtlID < 606 && (owner->enabled_channels() & (1 << (item->CtlID - 599))))
        {
            SelectObject(dc, GetStockObject(BLACK_BRUSH));
            SelectObject(dc, GetStockObject(BLACK_PEN));
            Ellipse(dc, area.right - px(18), area.top + px(15), area.right - px(12), area.top + px(21));
            SelectObject(dc, border);
        }
        text.left += px(40);
        text.right -= px(20);
        alignment = DT_LEFT;
    }
    DrawTextW(dc, label, -1, &text, alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (item->itemState & ODS_FOCUS)
    {
        RECT focus = area;
        InflateRect(&focus, -px(3), -px(3));
        DrawFocusRect(dc, &focus);
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);
    DeleteObject(background);
    return true;
}
static INT_PTR CALLBACK io_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto s = (Interfaces *)GetWindowLongPtrW(h, DWLP_USER);
    if (m == WM_MEASUREITEM)
    {
        ((MEASUREITEMSTRUCT *)l)->itemHeight = MulDiv(28, GetDpiForWindow(h), 96);
        return TRUE;
    }
    if (m == WM_DRAWITEM)
    {
        auto d = (DRAWITEMSTRUCT *)l;
        if (draw_control(d))
            return TRUE;
        if (d->CtlType == ODT_COMBOBOX)
        {
            auto comboFont = SelectObject(d->hDC, (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0));
            FillRect(d->hDC, &d->rcItem, (HBRUSH)GetStockObject(WHITE_BRUSH));
            if (d->itemID != (UINT)-1)
            {
                wchar_t text[256]{};
                SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)text);
                SetTextColor(d->hDC, RGB(25, 25, 25));
                SetBkMode(d->hDC, TRANSPARENT);
                RECT r = d->rcItem;
                r.left += 6;
                DrawTextW(d->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            }
            SelectObject(d->hDC, comboFont);
            if (d->itemState & ODS_FOCUS)
                DrawFocusRect(d->hDC, &d->rcItem);
            return TRUE;
        }
    }
    if ((m == WM_PRINTCLIENT || m == WM_ERASEBKGND) && s)
    {
        RECT r;
        GetClientRect(h, &r);
        s->paint_background((HDC)w);
        return TRUE;
    }
    if (m == WM_INITDIALOG)
    {
        auto a = (IoInit *)l;
        s = new Interfaces(h, a->ble, a->endpoint, a->token);
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)s);
        s->fit();
        s->style();
        DragAcceptFiles(h, TRUE);
        for (int id : {415, 416, 424, 454, 461})
            SetWindowTheme(GetDlgItem(h, id), L"", L"");
        if (wcsstr(GetCommandLineW(), L"--interfaces-preview"))
        {
            if (auto p = wcsstr(GetCommandLineW(), L"--page="))
            {
                int page = _wtoi(p + 7);
                if (page >= 0 && page < 9)
                {
                    TabCtrl_SetCurSel(GetDlgItem(h, 450), page);
                    s->select_page(page);
                }
            }
            SetTimer(h, 77, 1500, nullptr);
        }
        return TRUE;
    }
    if (!s)
        return FALSE;
    if (m == WM_SIZE)
    {
        s->layout();
        return TRUE;
    }
    if (m == WM_DPICHANGED)
    {
        RECT *area = reinterpret_cast<RECT *>(l);
        SetWindowPos(h, nullptr, area->left, area->top, area->right - area->left, area->bottom - area->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        s->style();
        return TRUE;
    }
    if (m == WM_GETMINMAXINFO)
    {
        auto limits = reinterpret_cast<MINMAXINFO *>(l);
        limits->ptMinTrackSize = {s->pixels(960), s->pixels(660)};
        return TRUE;
    }
    if (s->ssh_message(m, w, l))
        return TRUE;
    if (m == WM_DROPFILES)
    {
        wchar_t path[32768]{};
        if (DragQueryFileW((HDROP)w, 0, path, 32768))
            s->file_drop(path);
        DragFinish((HDROP)w);
        return TRUE;
    }
    if (m == WM_TIMER && w == 77)
    {
        KillTimer(h, 77);
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, 32768);
        {
            std::ofstream report(fs::path(exe).parent_path() / L"interface_controls.txt");
            HDC dc = GetDC(h);
            auto previous = SelectObject(dc, (HFONT)SendDlgItemMessageW(h, 432, WM_GETFONT, 0, 0));
            wchar_t face[256]{};
            GetTextFaceW(dc, 256, face);
            WORD glyph = 0;
            GetGlyphIndicesW(dc, L"中", 1, &glyph, GGI_MARK_NONEXISTING_GLYPHS);
            report << "font=" << utf8(face) << " cjk-glyph=" << glyph << "\n";
            SelectObject(dc, previous);
            ReleaseDC(h, dc);
            for (int id : {401, 405, 407, 408, 409, 413})
            {
                int selected = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0);
                wchar_t value[256]{};
                if (selected >= 0)
                    SendDlgItemMessageW(h, id, CB_GETLBTEXT, selected, (LPARAM)value);
                report << id << " count=" << SendDlgItemMessageW(h, id, CB_GETCOUNT, 0, 0)
                       << " selected=" << selected << " text=" << utf8(value) << "\n";
            }
        }
        capture(h, fs::path(exe).parent_path() /
                       (L"J1接口工作台-" + std::to_wstring(TabCtrl_GetCurSel(GetDlgItem(h, 450))) + L".png"));
        s->close();
        return TRUE;
    }
    if (m == WM_NOTIFY && ((NMHDR *)l)->idFrom == 450 && ((NMHDR *)l)->code == TCN_SELCHANGE)
    {
        s->select_page(TabCtrl_GetCurSel(GetDlgItem(h, 450)));
        return TRUE;
    }
    if (m == WM_COMMAND)
    {
        if (LOWORD(w) == IDCANCEL)
        {
            return TRUE;
        }
        s->command(LOWORD(w), HIWORD(w));
        return TRUE;
    }
    if (m == IO_EVENT)
    {
        s->event((IoEvent *)l);
        return TRUE;
    }
    if (m == WM_CLOSE)
    {
        s->close();
        return TRUE;
    }
    if (m == WM_CTLCOLORDLG || m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT)
    {
        SetTextColor((HDC)w, RGB(25, 25, 25));
        SetBkColor((HDC)w, RGB(255, 255, 255));
        return (INT_PTR)s->brush();
    }
    if (m == WM_DESTROY)
    {
        SetWindowLongPtrW(h, DWLP_USER, 0);
        delete s;
        MSG msg;
        while (PeekMessageW(&msg, h, IO_EVENT, IO_EVENT, PM_REMOVE))
            delete (IoEvent *)msg.lParam;
        return TRUE;
    }
    return FALSE;
}
void show_interfaces(HWND parent, bool ble, const std::wstring &endpoint, const std::string &token)
{
    IoInit a{ble, endpoint, token};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(202), parent, io_proc, (LPARAM)&a);
}
