#include "ssh.h"
#include "../third_party/libvterm/include/vterm.h"

void ssh_self_test(const fs::path &assets, Log log)
{
    auto executable = assets / L"ssh/ssh.exe";
    if (!fs::is_regular_file(executable) || !fs::is_regular_file(assets / L"ssh/libcrypto.dll"))
        throw std::runtime_error("Embedded SSH files are missing");
    SshOptions bad{L"-oProxyCommand=calc", L"test", L"", 22, {}};
    bool rejected = false;
    try
    {
        ssh_command(executable, bad);
    }
    catch (...)
    {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("SSH host option injection was accepted");
    SshOptions copiedPath{L"localhost", L"test", executable.wstring(), 22, {}};
    auto plainPathCommand = ssh_command(executable, copiedPath);
    copiedPath.key = L"\"" + copiedPath.key + L"\"";
    if (ssh_command(executable, copiedPath) != plainPathCommand)
        throw std::runtime_error("Quoted copied SSH key path was not normalized");
    PtySession session;
    std::mutex mutex;
    std::string text, error;
    std::atomic<bool> finished = false;
    unsigned result = 1;
    session.start(
        executable, L"\"" + executable.wstring() + L"\" -V",
        [&](const std::string &data) {
            std::lock_guard lock(mutex);
            text += data;
        },
        [&](unsigned code, const std::string &problem) {
            result = code;
            error = problem;
            finished = true;
        });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!finished && std::chrono::steady_clock::now() < deadline)
        Sleep(20);
    session.stop();
    if (!finished || result || !error.empty() || text.find("OpenSSH_for_Windows") == text.npos)
        throw std::runtime_error("Embedded SSH / ConPTY launch failed: " + error);
    log(L"Embedded OpenSSH starts in ConPTY without a separate console.");
    wchar_t system[32768];
    GetSystemDirectoryW(system, 32768);
    fs::path command = fs::path(system) / L"cmd.exe";
    finished = false;
    session.start(
        command, L"\"" + command.wstring() + L"\" /d /q /c ping -n 30 127.0.0.1 >nul",
        [](const std::string &) {}, [&](unsigned, const std::string &) { finished = true; });
    Sleep(120);
    session.send(std::string(256 * 1024, 'x'));
    auto before = std::chrono::steady_clock::now();
    session.stop();
    if (!finished || std::chrono::steady_clock::now() - before > std::chrono::seconds(5))
        throw std::runtime_error("SSH cancellation or process cleanup stalled");
    log(L"Terminal disconnect cancels pending input and closes child processes.");
    VTerm *vt = vterm_new(4, 30);
    vterm_set_utf8(vt, 1);
    auto screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);
    std::string sample = "abc\r\n\x1b[2;1H\x1b[2KSSH_OK";
    for (char byte : sample)
        vterm_input_write(vt, &byte, 1);
    vterm_screen_flush_damage(screen);
    VTermScreenCell cell{};
    vterm_screen_get_cell(screen, {1, 0}, &cell);
    bool valid = cell.chars[0] == 'S';
    vterm_free(vt);
    if (!valid)
        throw std::runtime_error("Terminal cursor / erase sequence parsing failed");
    log(L"Terminal parser handles fragmented cursor and erase sequences.");
}
