#pragma once
#include "core.h"
#include <deque>
struct SshOptions
{
    std::wstring host, user, key;
    unsigned port = 22;
    fs::path knownHosts; // Optional explicit host-key store for isolated tests.
};
std::wstring ssh_command(const fs::path &executable, const SshOptions &options);
class PtySession
{
    std::thread worker;
    std::atomic<bool> stopping{false}, active{false};
    std::mutex mutex;
    std::string input;
    COORD requestedSize{100, 24};

  public:
    using Output = std::function<void(const std::string &)>;
    using Finished = std::function<void(unsigned, const std::string &)>;
    ~PtySession();
    void start(const fs::path &executable, std::wstring command, Output output, Finished finished);
    void send(std::string_view bytes);
    void resize(int columns, int rows);
    void stop();
    bool running() const
    {
        return active;
    }
};
class SshPanel
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit SshPanel(HWND parent);
    ~SshPanel();
    void show(bool visible);
    void layout(RECT bounds, HFONT normalFont, HFONT labelFont);
    bool message(UINT message, WPARAM wparam, LPARAM lparam);
};
void ssh_self_test(const fs::path &assets, Log log);
