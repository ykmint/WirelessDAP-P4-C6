#include "ssh.h"
#include "../third_party/libvterm/include/vterm.h"
#include <windowsx.h>
#include <commctrl.h>
#include <deque>

namespace
{
constexpr UINT sshEvent = WM_APP + 110;
struct Event
{
    unsigned generation;
    bool finished;
    unsigned code;
    std::string text;
};
std::wstring readText(HWND parent, int id)
{
    HWND child = GetDlgItem(parent, id);
    std::wstring value(GetWindowTextLengthW(child) + 1, L'\0');
    GetWindowTextW(child, value.data(), int(value.size()));
    value.resize(wcslen(value.c_str()));
    auto begin = value.find_first_not_of(L" \r\n\t");
    if (begin == value.npos)
        return {};
    value = value.substr(begin, value.find_last_not_of(L" \r\n\t") - begin + 1);
    if (value.size() > 1 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    return value;
}
void appendCodepoint(std::wstring &value, uint32_t code)
{
    if (code <= 0xffff)
        value += wchar_t(code);
    else if (code <= 0x10ffff)
    {
        code -= 0x10000;
        value += wchar_t(0xd800 + (code >> 10));
        value += wchar_t(0xdc00 + (code & 1023));
    }
}
} // namespace
struct SshPanel::Impl
{
    HWND parent, terminal = nullptr;
    std::vector<HWND> controls;
    HFONT font = nullptr, bold = nullptr, cjkFont = nullptr, cjkBold = nullptr;
    int cellWidth = 9, cellHeight = 18, columns = 100, rows = 24, scroll = 0;
    VTerm *vt = nullptr;
    VTermScreen *screen = nullptr;
    std::deque<std::vector<VTermScreenCell>> history;
    PtySession session;
    int selectionStart = -1, selectionEnd = -1;
    bool selecting = false;
    unsigned generation = 0;
    wchar_t highSurrogate = 0;
    VTermScreenCallbacks callbacks{};

    explicit Impl(HWND owner) : parent(owner)
    {
        vt = vterm_new(rows, columns);
        if (!vt)
            throw std::runtime_error("Cannot allocate SSH terminal");
        vterm_set_utf8(vt, 1);
        screen = vterm_obtain_screen(vt);
        vterm_screen_enable_altscreen(screen, 1);
        callbacks.sb_pushline = [](int count, const VTermScreenCell *cells, void *context) {
            auto self = static_cast<Impl *>(context);
            self->history.emplace_back(cells, cells + count);
            if (self->history.size() > 2000)
            {
                self->history.pop_front();
                self->selectionStart = self->selectionEnd = -1;
            }
            if (self->scroll)
                ++self->scroll;
            return 1;
        };
        callbacks.sb_popline = [](int count, VTermScreenCell *cells, void *context) {
            auto self = static_cast<Impl *>(context);
            if (self->history.empty())
                return 0;
            std::fill(cells, cells + count, VTermScreenCell{});
            auto &line = self->history.back();
            std::copy_n(line.begin(), std::min<size_t>(line.size(), count), cells);
            self->history.pop_back();
            return 1;
        };
        vterm_screen_set_callbacks(screen, &callbacks, this);
        vterm_output_set_callback(
            vt,
            [](const char *bytes, size_t count, void *context) {
                static_cast<Impl *>(context)->session.send({bytes, count});
            },
            this);
        vterm_screen_reset(screen, 1);
        WNDCLASSW windowClass{};
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpfnWndProc = terminalProc;
        windowClass.lpszClassName = L"WirelessDAP.SshTerminal";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        RegisterClassW(&windowClass);
        make(750, L"STATIC", L"SSH 远程终端", 0, 14, 100, 300, 14);
        make(751, L"STATIC", L"主机 / IP", 0, 14, 125, 230, 12);
        make(752, L"STATIC", L"端口", 0, 254, 125, 55, 12);
        make(753, L"STATIC", L"用户名", 0, 321, 125, 155, 12);
        make(480, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 141, 230, 22);
        make(481, L"EDIT", L"22", WS_BORDER | ES_NUMBER | WS_TABSTOP, 254, 141, 55, 22);
        make(482, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 321, 141, 155, 22);
        make(484, L"BUTTON", L"连接 SSH", BS_PUSHBUTTON | WS_TABSTOP, 490, 141, 156, 22);
        make(754, L"STATIC", L"私钥路径（可选，留空时在终端中完成认证）", 0, 14, 174, 610, 12);
        make(483, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 190, 632, 22);
        make(485, L"BUTTON", L"复制", BS_PUSHBUTTON | WS_TABSTOP, 14, 224, 68, 22);
        make(486, L"BUTTON", L"粘贴", BS_PUSHBUTTON | WS_TABSTOP, 90, 224, 68, 22);
        make(487, L"BUTTON", L"中断 Ctrl+C", BS_PUSHBUTTON | WS_TABSTOP, 166, 224, 110, 22);
        make(489, L"STATIC", L"独立网络连接 · 支持密码、私钥和主机指纹确认", 0, 290, 227, 356, 17);
        terminal = make(488, L"WirelessDAP.SshTerminal", L"", WS_VSCROLL | WS_TABSTOP, 14, 255, 632, 202);
        make(755, L"STATIC", L"点击终端输入 · 鼠标选择复制 · Ctrl+V 粘贴 · 密码输入不回显", 0, 14, 468, 632,
             14);
        feed(utf8(L"SSH 远程终端\r\n填写主机和用户名后连接，认证提示会显示在这里。\r\n"));
    }
    ~Impl()
    {
        session.stop();
        MSG message;
        while (PeekMessageW(&message, parent, sshEvent, sshEvent, PM_REMOVE))
            delete reinterpret_cast<Event *>(message.lParam);
        if (terminal && IsWindow(terminal))
            DestroyWindow(terminal);
        if (font)
            DeleteObject(font);
        if (bold)
            DeleteObject(bold);
        if (cjkFont)
            DeleteObject(cjkFont);
        if (cjkBold)
            DeleteObject(cjkBold);
        vterm_free(vt);
    }
    HWND make(int id, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int width,
              int height)
    {
        RECT rectangle{x, y, x + width, y + height};
        MapDialogRect(parent, &rectangle);
        HWND control =
            CreateWindowW(cls, text, WS_CHILD | style, rectangle.left, rectangle.top,
                          rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, parent,
                          reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), this);
        SendMessageW(control, WM_SETFONT, SendMessageW(parent, WM_GETFONT, 0, 0), TRUE);
        controls.push_back(control);
        return control;
    }
    void post(unsigned epoch, bool done, unsigned code, const std::string &text)
    {
        auto event = new Event{epoch, done, code, text};
        if (!PostMessageW(parent, sshEvent, 0, reinterpret_cast<LPARAM>(event)))
            delete event;
    }
    void resizeTerminal()
    {
        if (!terminal)
            return;
        int dpi = GetDpiForWindow(terminal);
        if (font)
            DeleteObject(font);
        if (bold)
            DeleteObject(bold);
        if (cjkFont)
            DeleteObject(cjkFont);
        if (cjkBold)
            DeleteObject(cjkBold);
        int height = -MulDiv(10, dpi, 72);
        font =
            CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        bold = CreateFontW(height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        cjkFont =
            CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        cjkBold =
            CreateFontW(height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HDC dc = GetDC(terminal);
        auto old = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(terminal, dc);
        cellWidth = std::max(1, int(metrics.tmAveCharWidth));
        cellHeight = std::max(1, int(metrics.tmHeight));
        RECT area;
        GetClientRect(terminal, &area);
        columns = std::clamp(int(area.right - 16) / cellWidth, 20, 300);
        rows = std::clamp(int(area.bottom - 16) / cellHeight, 4, 150);
        vterm_set_size(vt, rows, columns);
        session.resize(columns, rows);
        selectionStart = selectionEnd = -1;
        updateScroll();
        InvalidateRect(terminal, nullptr, TRUE);
    }
    void updateScroll()
    {
        scroll = std::clamp(scroll, 0, int(history.size()));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = int(history.size()) + rows - 1;
        info.nPage = rows;
        info.nPos = int(history.size()) - scroll;
        SetScrollInfo(terminal, SB_VERT, &info, TRUE);
    }
    void feed(std::string_view bytes)
    {
        vterm_input_write(vt, bytes.data(), bytes.size());
        vterm_screen_flush_damage(screen);
        if (terminal)
        {
            updateScroll();
            InvalidateRect(terminal, nullptr, FALSE);
        }
    }
    VTermScreenCell cell(int line, int column)
    {
        VTermScreenCell value{};
        if (line < 0 || column < 0 || column >= columns)
            return value;
        if (line < int(history.size()))
        {
            auto &row = history[line];
            if (column < int(row.size()))
                value = row[column];
        }
        else if (line < int(history.size()) + rows)
            vterm_screen_get_cell(screen, {line - int(history.size()), column}, &value);
        return value;
    }
    void paint(HDC dc)
    {
        RECT area;
        GetClientRect(terminal, &area);
        FillRect(dc, &area, (HBRUSH)GetStockObject(WHITE_BRUSH));
        auto old = SelectObject(dc, font);
        SetBkMode(dc, OPAQUE);
        int top = int(history.size()) - scroll;
        int first = std::min(selectionStart, selectionEnd), last = std::max(selectionStart, selectionEnd);
        for (int row = 0; row < rows; row++)
            for (int column = 0; column < columns; column++)
            {
                auto value = cell(top + row, column);
                if (value.width == 0 || value.chars[0] == UINT32_MAX)
                    continue;
                int position = (top + row) * columns + column;
                bool selected = first >= 0 && position >= first && position < last;
                bool reversed = selected || value.attrs.reverse;
                RECT rectangle{8 + column * cellWidth, 8 + row * cellHeight,
                               8 + (column + std::max(1, int(value.width))) * cellWidth,
                               8 + (row + 1) * cellHeight};
                SetTextColor(dc, reversed ? RGB(255, 255, 255) : RGB(24, 24, 24));
                SetBkColor(dc, reversed ? RGB(30, 30, 30) : RGB(255, 255, 255));
                std::wstring text;
                if (!value.attrs.conceal)
                    for (auto code : value.chars)
                    {
                        if (!code || code == UINT32_MAX)
                            break;
                        appendCodepoint(text, code);
                    }
                if (text.empty())
                    text = L" ";
                SelectObject(dc, value.chars[0] >= 0x2e80 ? (value.attrs.bold ? cjkBold : cjkFont)
                                                          : (value.attrs.bold ? bold : font));
                ExtTextOutW(dc, rectangle.left, rectangle.top, ETO_OPAQUE | ETO_CLIPPED, &rectangle,
                            text.c_str(), UINT(text.size()), nullptr);
            }
        if (GetFocus() == terminal && scroll == 0)
        {
            VTermPos cursor{};
            vterm_state_get_cursorpos(vterm_obtain_state(vt), &cursor);
            RECT marker{8 + cursor.col * cellWidth, 8 + (cursor.row + 1) * cellHeight - 2,
                        8 + (cursor.col + 1) * cellWidth, 8 + (cursor.row + 1) * cellHeight};
            FillRect(dc, &marker, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        SelectObject(dc, old);
    }
    int position(LPARAM point)
    {
        int column = std::clamp((GET_X_LPARAM(point) - 8) / cellWidth, 0, columns - 1);
        int row = std::clamp((GET_Y_LPARAM(point) - 8) / cellHeight, 0, rows - 1);
        return (int(history.size()) - scroll + row) * columns + column;
    }
    void copy()
    {
        int begin = std::min(selectionStart, selectionEnd), end = std::max(selectionStart, selectionEnd);
        if (begin < 0 || begin == end)
        {
            begin = (int(history.size()) - scroll) * columns;
            end = begin + rows * columns;
        }
        std::wstring text;
        for (int i = begin; i < end; i++)
        {
            if (i > begin && i % columns == 0)
            {
                while (!text.empty() && text.back() == L' ')
                    text.pop_back();
                text += L"\r\n";
            }
            auto value = cell(i / columns, i % columns);
            if (value.width == 0 || value.chars[0] == UINT32_MAX)
                continue;
            if (!value.chars[0] || value.attrs.conceal)
                text += L' ';
            else
                for (auto code : value.chars)
                {
                    if (!code)
                        break;
                    appendCodepoint(text, code);
                }
        }
        if (!OpenClipboard(terminal))
            return;
        HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (storage)
        {
            auto data = GlobalLock(storage);
            memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(storage);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, storage))
                GlobalFree(storage);
        }
        CloseClipboard();
    }
    void paste()
    {
        if (!session.running() || !OpenClipboard(terminal))
            return;
        HANDLE storage = GetClipboardData(CF_UNICODETEXT);
        std::wstring text;
        if (storage)
        {
            auto value = static_cast<const wchar_t *>(GlobalLock(storage));
            if (value)
            {
                size_t size = GlobalSize(storage) / sizeof(wchar_t);
                size_t count = 0;
                while (count < size && value[count])
                    ++count;
                if (count <= 512 * 1024)
                    text.assign(value, count);
                GlobalUnlock(storage);
            }
        }
        CloseClipboard();
        vterm_keyboard_start_paste(vt);
        auto bytes = utf8(text);
        session.send(bytes);
        vterm_keyboard_end_paste(vt);
        if (!text.empty())
            SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        if (!bytes.empty())
            SecureZeroMemory(bytes.data(), bytes.size());
    }
    void connect()
    {
        if (session.running())
        {
            session.stop();
            return;
        }
        try
        {
            SshOptions options;
            options.host = readText(parent, 480);
            options.user = readText(parent, 482);
            options.key = readText(parent, 483);
            auto port = readText(parent, 481);
            size_t used = 0;
            options.port = unsigned(std::stoul(port, &used));
            if (used != port.size())
                throw std::runtime_error("Invalid SSH port");
            auto executable = unpack_assets() / L"ssh/ssh.exe";
            auto command = ssh_command(executable, options);
            history.clear();
            scroll = 0;
            selectionStart = selectionEnd = -1;
            vterm_screen_reset(screen, 1);
            SetDlgItemTextW(parent, 489, L"正在连接 · 首次连接请核对终端中的主机指纹");
            SetDlgItemTextW(parent, 484, L"断开 SSH");
            for (int id : {480, 481, 482, 483})
                EnableWindow(GetDlgItem(parent, id), FALSE);
            unsigned epoch = ++generation;
            session.start(
                executable, command, [this, epoch](auto &bytes) { post(epoch, false, 0, bytes); },
                [this, epoch](unsigned code, auto &error) { post(epoch, true, code, error); });
            SetFocus(terminal);
        }
        catch (const std::exception &error)
        {
            SetDlgItemTextW(parent, 489, wide(error.what()).c_str());
        }
    }
    static LRESULT CALLBACK terminalProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl *>(((CREATESTRUCTW *)l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)self);
            self->terminal = window;
        }
        if (!self)
            return DefWindowProcW(window, message, w, l);
        if (message == WM_GETDLGCODE)
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
        if (message == WM_SIZE)
        {
            self->resizeTerminal();
            return 0;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            self->paint(dc);
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT)
        {
            self->paint((HDC)w);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        {
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_LBUTTONDOWN)
        {
            SetFocus(window);
            SetCapture(window);
            self->selecting = true;
            self->selectionStart = self->selectionEnd = self->position(l);
            return 0;
        }
        if (message == WM_MOUSEMOVE && self->selecting)
        {
            self->selectionEnd = self->position(l);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_LBUTTONUP)
        {
            self->selecting = false;
            ReleaseCapture();
            return 0;
        }
        if (message == WM_MOUSEWHEEL)
        {
            self->scroll += GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 3;
            self->updateScroll();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_VSCROLL)
        {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(window, SB_VERT, &info);
            switch (LOWORD(w))
            {
            case SB_LINEUP:
                self->scroll++;
                break;
            case SB_LINEDOWN:
                self->scroll--;
                break;
            case SB_PAGEUP:
                self->scroll += self->rows;
                break;
            case SB_PAGEDOWN:
                self->scroll -= self->rows;
                break;
            case SB_THUMBTRACK:
                self->scroll = int(self->history.size()) - info.nTrackPos;
                break;
            }
            self->updateScroll();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_CHAR)
        {
            if (w == 3 && self->selectionStart != self->selectionEnd)
            {
                self->copy();
                return 0;
            }
            if (w == 22)
            {
                self->paste();
                return 0;
            }
            self->selectionStart = self->selectionEnd = -1;
            self->scroll = 0;
            self->updateScroll();
            if (w == '\r')
                vterm_keyboard_key(self->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
            else if (w == '\t')
                vterm_keyboard_key(self->vt, VTERM_KEY_TAB, VTERM_MOD_NONE);
            else if (w == 8)
                vterm_keyboard_key(self->vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
            else if (w == 27)
                vterm_keyboard_key(self->vt, VTERM_KEY_ESCAPE, VTERM_MOD_NONE);
            else if (w >= 0xd800 && w <= 0xdbff)
                self->highSurrogate = wchar_t(w);
            else
            {
                uint32_t code = uint32_t(w);
                if (self->highSurrogate && w >= 0xdc00 && w <= 0xdfff)
                    code = 0x10000 + ((self->highSurrogate - 0xd800) << 10) + (w - 0xdc00);
                self->highSurrogate = 0;
                vterm_keyboard_unichar(self->vt, code, VTERM_MOD_NONE);
            }
            return 0;
        }
        if (message == WM_KEYDOWN)
        {
            VTermKey key = VTERM_KEY_NONE;
            switch (w)
            {
            case VK_UP:
                key = VTERM_KEY_UP;
                break;
            case VK_DOWN:
                key = VTERM_KEY_DOWN;
                break;
            case VK_LEFT:
                key = VTERM_KEY_LEFT;
                break;
            case VK_RIGHT:
                key = VTERM_KEY_RIGHT;
                break;
            case VK_HOME:
                key = VTERM_KEY_HOME;
                break;
            case VK_END:
                key = VTERM_KEY_END;
                break;
            case VK_DELETE:
                key = VTERM_KEY_DEL;
                break;
            case VK_INSERT:
                key = VTERM_KEY_INS;
                break;
            case VK_PRIOR:
                key = VTERM_KEY_PAGEUP;
                break;
            case VK_NEXT:
                key = VTERM_KEY_PAGEDOWN;
                break;
            }
            if (w >= VK_F1 && w <= VK_F12)
                key = VTermKey(VTERM_KEY_FUNCTION(1 + w - VK_F1));
            if (key != VTERM_KEY_NONE)
            {
                int modifiers = 0;
                if (GetKeyState(VK_SHIFT) < 0)
                    modifiers |= VTERM_MOD_SHIFT;
                if (GetKeyState(VK_CONTROL) < 0)
                    modifiers |= VTERM_MOD_CTRL;
                vterm_keyboard_key(self->vt, key, VTermModifier(modifiers));
                return 0;
            }
        }
        return DefWindowProcW(window, message, w, l);
    }
};
SshPanel::SshPanel(HWND parent) : impl(std::make_unique<Impl>(parent))
{
}
SshPanel::~SshPanel() = default;
void SshPanel::show(bool visible)
{
    for (auto control : impl->controls)
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    if (visible)
        impl->resizeTerminal();
}
void SshPanel::layout(RECT bounds, HFONT normalFont, HFONT labelFont)
{
    int dpi = GetDpiForWindow(impl->parent);
    auto px = [&](int value) { return MulDiv(value, dpi, 96); };
    auto place = [&](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(impl->parent, id), x, y, width, height, TRUE);
    };
    int left = bounds.left + px(20), right = bounds.right - px(20);
    int hostWidth = right - left - px(70 + 170 + 120 + 36);
    int portX = left + hostWidth + px(12);
    int userX = portX + px(82);
    int actionX = right - px(120);
    ShowWindow(GetDlgItem(impl->parent, 750), SW_HIDE);
    place(751, left, bounds.top + px(16), hostWidth, px(18));
    place(752, portX, bounds.top + px(16), px(70), px(18));
    place(753, userX, bounds.top + px(16), px(170), px(18));
    place(480, left, bounds.top + px(38), hostWidth, px(32));
    place(481, portX, bounds.top + px(38), px(70), px(32));
    place(482, userX, bounds.top + px(38), px(170), px(32));
    place(484, actionX, bounds.top + px(38), px(120), px(32));
    place(754, left, bounds.top + px(86), right - left, px(18));
    place(483, left, bounds.top + px(108), right - left, px(32));
    int terminalTop = bounds.top + px(172);
    place(485, right - px(232), terminalTop + px(12), px(72), px(28));
    place(486, right - px(152), terminalTop + px(12), px(72), px(28));
    place(487, right - px(72), terminalTop + px(12), px(72), px(28));
    place(489, left, terminalTop + px(46), right - left, px(18));
    place(488, left, terminalTop + px(74), right - left,
          std::max(px(80), int(bounds.bottom) - terminalTop - px(90)));
    RECT parentBounds;
    GetClientRect(impl->parent, &parentBounds);
    place(755, bounds.left, parentBounds.bottom - px(18), bounds.right - bounds.left, px(16));
    SetDlgItemTextW(impl->parent, 487, L"Ctrl+C");
    for (auto child : impl->controls)
    {
        int id = GetDlgCtrlID(child);
        if (id != 488)
            SendMessageW(child, WM_SETFONT, (WPARAM)((id >= 750 || id == 489) ? labelFont : normalFont),
                         TRUE);
        if (id >= 480 && id <= 483)
            SendMessageW(child, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(8), px(8)));
    }
}

bool SshPanel::message(UINT message, WPARAM w, LPARAM l)
{
    if (message == sshEvent)
    {
        std::unique_ptr<Event> event(reinterpret_cast<Event *>(l));
        if (event->generation != impl->generation)
            return true;
        if (event->finished)
        {
            for (int id : {480, 481, 482, 483})
                EnableWindow(GetDlgItem(impl->parent, id), TRUE);
            SetDlgItemTextW(impl->parent, 484, L"连接 SSH");
            auto status = event->text.empty() ? L"会话已结束 · 退出码 " + std::to_wstring(event->code)
                                              : wide(event->text);
            SetDlgItemTextW(impl->parent, 489, status.c_str());
            if (!event->text.empty())
                impl->feed("\r\n" + utf8(status) + "\r\n");
        }
        else
            impl->feed(event->text);
        return true;
    }
    if (message == WM_COMMAND && HIWORD(w) == BN_CLICKED)
    {
        switch (LOWORD(w))
        {
        case 484:
            impl->connect();
            return true;
        case 485:
            impl->copy();
            return true;
        case 486:
            impl->paste();
            return true;
        case 487:
            impl->session.send("\x03");
            return true;
        }
    }
    return false;
}
