#include "ssh.h"
#include <cctype>

static std::wstring quote_argument(const std::wstring &value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (auto c : value)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::wstring ssh_command(const fs::path &executable, const SshOptions &options)
{
    if (options.host.empty() || options.host.front() == L'-' ||
        options.host.find_first_of(L" \t\r\n\"@") != std::wstring::npos)
        throw std::runtime_error("SSH host must be a hostname or IP address");
    if (options.user.empty() || options.user.front() == L'-' ||
        options.user.find_first_of(L" \t\r\n\"") != std::wstring::npos)
        throw std::runtime_error("Enter a valid SSH username");
    if (!options.port || options.port > 65535)
        throw std::runtime_error("SSH port range is 1..65535");
    std::wstring command = quote_argument(executable.wstring()) +
                           L" -tt -o ConnectTimeout=12 -o ServerAliveInterval=15 -o "
                           L"ServerAliveCountMax=3 -o StrictHostKeyChecking=ask";
    command += L" -p " + std::to_wstring(options.port) + L" -l " + quote_argument(options.user);
    auto key = options.key;
    if (key.size() >= 2 && key.front() == L'"' && key.back() == L'"')
        key = key.substr(1, key.size() - 2);
    if (!key.empty())
    {
        if (!fs::is_regular_file(key))
            throw std::runtime_error("SSH private key file does not exist");
        command += L" -o IdentitiesOnly=yes -i " + quote_argument(key);
    }
    if (!options.knownHosts.empty())
        command += L" -F none -o " + quote_argument(L"UserKnownHostsFile=" + options.knownHosts.wstring());
    return command + L" " + quote_argument(options.host);
}
PtySession::~PtySession()
{
    stop();
}
void PtySession::stop()
{
    stopping = true;
    if (worker.joinable())
    {
        while (WaitForSingleObject(worker.native_handle(), 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(worker.native_handle());
        worker.join();
    }
    std::lock_guard lock(mutex);
    if (!input.empty())
        SecureZeroMemory(input.data(), input.size());
    input.clear();
}
void PtySession::send(std::string_view bytes)
{
    if (!active)
        return;
    std::lock_guard lock(mutex);
    if (input.size() + bytes.size() > 1024 * 1024)
        return;
    input.append(bytes);
}
void PtySession::resize(int columns, int rows)
{
    std::lock_guard lock(mutex);
    requestedSize = {SHORT(std::clamp(columns, 20, 300)), SHORT(std::clamp(rows, 4, 150))};
}
void PtySession::start(const fs::path &executable, std::wstring command, Output output, Finished finished)
{
    stop();
    stopping = false;
    active = true;
    worker = std::thread([this, executable, command = std::move(command), output, finished]() mutable {
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
        HPCON console = nullptr;
        HANDLE job = nullptr;
        PROCESS_INFORMATION process{};
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
        std::vector<uint8_t> attributeStorage;
        unsigned exitCode = 1;
        std::string error;
        auto drain = [&] {
            DWORD available = 0;
            if (!outRead || !PeekNamedPipe(outRead, nullptr, 0, nullptr, &available, nullptr) || !available)
                return false;
            char bytes[8192];
            DWORD count = 0;
            if (ReadFile(outRead, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr) && count)
            {
                output(std::string(bytes, count));
                return true;
            }
            return false;
        };
        try
        {
            if (!CreatePipe(&inRead, &inWrite, nullptr, 65536) ||
                !CreatePipe(&outRead, &outWrite, nullptr, 65536))
                throw std::runtime_error("Cannot create SSH terminal pipes");
            COORD size;
            {
                std::lock_guard lock(mutex);
                size = requestedSize;
            }
            if (FAILED(CreatePseudoConsole(size, inRead, outWrite, 0, &console)))
                throw std::runtime_error(
                    "Cannot create Windows pseudoconsole (Windows 10 1809 or newer required)");
            CloseHandle(inRead);
            inRead = nullptr;
            CloseHandle(outWrite);
            outWrite = nullptr;
            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            attributeStorage.resize(attributeBytes);
            attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
            {
                attributes = nullptr;
                throw std::runtime_error("Cannot initialize SSH process attributes");
            }
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                                           sizeof(console), nullptr, nullptr))
                throw std::runtime_error("Cannot attach SSH pseudoconsole");
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            // Bind all standard streams to ConPTY, even when our host was launched with redirected output.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.lpAttributeList = attributes;
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr,
                                &startup.StartupInfo, &process))
                throw std::runtime_error("Cannot start embedded SSH, Windows error " +
                                         std::to_string(GetLastError()));
            job = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job ||
                !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job, process.hProcess))
                throw std::runtime_error("Cannot establish SSH process cleanup scope");
            ResumeThread(process.hThread);
            CloseHandle(process.hThread);
            process.hThread = nullptr;
            while (!stopping && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
            {
                drain();
                std::string pending;
                COORD nextSize;
                {
                    std::lock_guard lock(mutex);
                    pending.swap(input);
                    nextSize = requestedSize;
                }
                if (nextSize.X != size.X || nextSize.Y != size.Y)
                {
                    ResizePseudoConsole(console, nextSize);
                    size = nextSize;
                }
                if (!pending.empty())
                {
                    // Small bounded writes preserve responsiveness when the remote terminal stalls.
                    size_t offset = 0;
                    while (offset < pending.size() && !stopping)
                    {
                        DWORD sent = 0;
                        DWORD amount = DWORD(std::min<size_t>(512, pending.size() - offset));
                        if (!WriteFile(inWrite, pending.data() + offset, amount, &sent, nullptr) || !sent)
                            break;
                        offset += sent;
                        drain();
                    }
                    SecureZeroMemory(pending.data(), pending.size());
                }
                Sleep(10);
            }
            if (stopping && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 130);
            WaitForSingleObject(process.hProcess, 1000);
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            exitCode = code;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
        }
        if (process.hProcess && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(process.hProcess, 130);
        if (job)
        {
            CloseHandle(job);
            job = nullptr;
        }
        if (console)
        {
            std::atomic<bool> closed{false};
            std::thread closer([&] {
                ClosePseudoConsole(console);
                closed = true;
            });
            while (!closed)
            {
                drain();
                Sleep(5);
            }
            closer.join();
            while (drain())
            {
            }
        }
        if (attributes)
            DeleteProcThreadAttributeList(attributes);
        for (HANDLE handle : {inRead, inWrite, outRead, outWrite, process.hThread, process.hProcess})
            if (handle)
                CloseHandle(handle);
        active = false;
        finished(exitCode, error);
    });
}
