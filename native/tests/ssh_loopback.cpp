#include "../src/ssh.h"
#include "../third_party/libvterm/include/vterm.h"
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc != 3) throw std::runtime_error("Specify fixture directory and port");
        fs::path fixture = fs::absolute(argv[1]);
        unsigned port = std::stoul(argv[2]);
        fs::path executable = fs::absolute(L"native/build/payload/ssh/ssh.exe");
        for (int mode = 0; mode < 5; ++mode)
        {
            auto hosts = fixture / (mode == 2 ? L"wrong_hosts" : mode == 3 ? L"new_hosts" : L"known_hosts");
            auto key = fixture / (mode == 4 ? L"client_key_encrypted" : L"client_key");
            if (mode == 3) write_file(hosts, {});
            SshOptions options{L"127.0.0.1", L"wdtest", mode == 1 ? L"" : key.wstring(), port, hosts};
            if (!options.key.empty()) options.key = L"\"" + options.key + L"\"";
            PtySession terminal;
            VTerm *vt = vterm_new(24, 100);
            vterm_set_utf8(vt, 1);
            auto screen = vterm_obtain_screen(vt);
            vterm_screen_reset(screen, 1);
            vterm_output_set_callback(vt, [](const char *data, size_t size, void *context) {
                static_cast<PtySession *>(context)->send({data, size});
            }, &terminal);
            std::atomic<bool> finished = false;
            unsigned code = 1;
            std::string output, error;
            std::mutex mutex;
            terminal.start(executable, ssh_command(executable, options), [&](const std::string &bytes) {
                std::lock_guard lock(mutex);
                output += bytes;
                vterm_input_write(vt, bytes.data(), bytes.size());
                vterm_screen_flush_damage(screen);
            }, [&](unsigned value, const std::string &problem) {
                code = value; error = problem; finished = true;
            });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            bool secretSent = false, commandSent = false, hostAccepted = false;
            while (!finished && std::chrono::steady_clock::now() < deadline)
            {
                std::string text;
                { std::lock_guard lock(mutex); text = output; }
                if (mode == 1 && !secretSent && (text.find("password:") != text.npos || text.find("Password:") != text.npos))
                { terminal.send("fixture-only-Password7\r"); secretSent = true; }
                if (mode == 4 && !secretSent && text.find("passphrase") != text.npos)
                { terminal.send("fixture-only-keyPass9\r"); secretSent = true; }
                if (mode == 3 && !hostAccepted && text.find("Are you sure you want to continue") != text.npos)
                { terminal.send("yes\r"); hostAccepted = true; }
                if (!commandSent && text.find("SSH_READY") != text.npos)
                { Sleep(500); terminal.send("echo SSH_ACCEPTANCE\r\n"); commandSent = true; }
                Sleep(20);
            }
            terminal.stop();
            vterm_free(vt);
            if (!finished || !error.empty()) throw std::runtime_error("SSH session timeout or backend error: " + error);
            if (mode == 2)
            {
                if (code == 0 || output.find("REMOTE HOST IDENTIFICATION HAS CHANGED") == output.npos)
                    throw std::runtime_error("Changed host key was not rejected");
                std::cout << "PASS changed host key rejected" << std::endl;
            }
            else
            {
                if (code || output.find("SSH_ACCEPTANCE_OK") == output.npos)
                { std::cerr << output << std::endl; throw std::runtime_error("SSH authenticated terminal exchange failed"); }
                if (mode == 3 && (!hostAccepted || read_file(hosts).empty()))
                    throw std::runtime_error("First-use host fingerprint confirmation failed");
                const char *results[] = {"public-key authentication", "password authentication", "", "first-use fingerprint confirmation", "encrypted private-key authentication"};
                std::cout << "PASS " << results[mode] << " and shell I/O" << std::endl;
            }
        }
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << std::endl; return 1; }
}
