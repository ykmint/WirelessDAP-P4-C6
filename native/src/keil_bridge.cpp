#include "core.h"
#include "../third_party/elaphureLink/ipc_common.hpp"
#include <winrt/base.h>
// Wire-compatible IPC for the upstream elaphureRddi native Keil plugin.
// Transport remains our authenticated USB / BLE / Wi-Fi Link.
struct KeilBridge::Impl
{
    std::shared_ptr<Link> link;
    Log log;
    HANDLE mapping = nullptr, producer = nullptr, consumer = nullptr;
    el_memory_t *memory = nullptr;
    std::atomic<bool> stopping = false;
    std::thread thread;
    ~Impl()
    {
        stopping = true;
        if (producer)
            SetEvent(producer);
        if (thread.joinable())
            thread.join();
        if (memory)
        {
            memory->info_page.is_proxy_ready = 0;
            memory->consumer_page.command_response = 4;
            if (consumer)
                SetEvent(consumer);
            UnmapViewOfFile(memory);
        }
        if (mapping)
            CloseHandle(mapping);
        if (producer)
            CloseHandle(producer);
        if (consumer)
            CloseHandle(consumer);
    }
    Bytes dap(Bytes p)
    {
        if (p.size() > 64)
            throw std::runtime_error("CMSIS-DAP packet exceeds device capacity");
        p.resize(64);
        if (p[0] == 0x11)
        {
            p[1] = 0xa0;
            p[2] = 0x86;
            p[3] = 1;
            p[4] = 0;
        }
        if (p[0] == 4)
        {
            p[2] = p[4] = 80;
            p[3] = p[5] = 0;
        }
        if (p[0] == 0x10 && (p[2] & 128) && !(p[1] & 128))
            throw std::runtime_error("NRST is not wired; choose software reset in Keil");
        return link->request(2, p);
    }
    static size_t length(std::span<const uint8_t> p)
    {
        auto need = [&](size_t n) {
            if (p.size() < n)
                throw std::runtime_error("Truncated Keil DAP request");
            return n;
        };
        need(1);
        switch (p[0])
        {
        case 0:
        case 2:
        case 0x13:
            return need(2);
        case 1:
            return need(3);
        case 3:
        case 7:
        case 0x0a:
            return 1;
        case 4:
        case 8:
            return need(6);
        case 0x10:
            return need(7);
        case 0x11:
            return need(5);
        case 0x12:
            need(2);
            return need(2 + ((p[1] ? p[1] : 256) + 7) / 8);
        case 5: {
            need(3);
            size_t n = 3;
            for (int i = 0; i < p[2]; i++)
            {
                need(n + 1);
                auto r = p[n++];
                if (!(r & 2) || (r & 0x10))
                    n += 4;
                need(n);
            }
            return n;
        }
        case 6:
            need(5);
            return need(5 + ((p[4] & 2) ? 0 : 4 * u16(p.data() + 2)));
        default:
            throw std::runtime_error("Unsupported Keil command; select SWD without SWO/JTAG");
        }
    }
    std::atomic<uint64_t> requestCount{0};
    std::chrono::steady_clock::time_point lastProgress = std::chrono::steady_clock::now();
    void process()
    {
        auto count = memory->producer_page.command_count;
        auto size = memory->producer_page.data_len;
        if (size > sizeof(memory->producer_page.data))
            throw std::runtime_error("Keil IPC request too large");
        Bytes input(memory->producer_page.data, memory->producer_page.data + size), output;
        size_t off = 0;
        unsigned commands = 1, total = 0, status = 1;
        bool hadTransfer = false;
        if (input.size() >= 2 && input[0] == 0x7f)
        {
            commands = input[1];
            off = 2;
        }
        for (unsigned ci = 0; ci < commands; ci++)
        {
            if (off >= input.size())
                throw std::runtime_error("Truncated Keil batch");
            auto len = length(std::span<const uint8_t>(input).subspan(off));
            Bytes p(input.begin() + off, input.begin() + off + len);
            off += len;
            if (p[0] == 5)
            {
                hadTransfer = true;
                size_t at = 3;
                unsigned done = 0;
                while (done < p[2])
                {
                    Bytes q{5, p[1], 0};
                    size_t response = 3;
                    while (done + q[2] < p[2])
                    {
                        uint8_t r = p[at];
                        size_t bytes = 1 + ((!(r & 2) || (r & 0x10)) ? 4 : 0);
                        size_t reply = ((r & 2) && !(r & 0x10) ? 4 : 0) + ((r & 0x80) ? 4 : 0);
                        if (q.size() + bytes > 64 || response + reply > 64)
                            break;
                        q.insert(q.end(), p.begin() + at, p.begin() + at + bytes);
                        at += bytes;
                        response += reply;
                        q[2]++;
                    }
                    if (!q[2])
                        throw std::runtime_error("Invalid Keil transfer");
                    auto r = dap(q);
                    if (r.size() < 3)
                        throw std::runtime_error("Short DAP transfer response");
                    total += r[1];
                    status = r[2];
                    if (status != 1 || r[1] != q[2])
                    {
                        status = status == 1 ? 4 : status;
                        break;
                    }
                    output.insert(output.end(), r.begin() + 3, r.end());
                    done += q[2];
                }
            }
            else if (p[0] == 6)
            {
                hadTransfer = true;
                unsigned requested = u16(p.data() + 2), done = 0;
                bool read = p[4] & 2;
                while (done < requested)
                {
                    unsigned n = std::min<unsigned>(read ? 15 : 14, requested - done);
                    Bytes q{6, p[1], uint8_t(n), 0, p[4]};
                    if (!read)
                        q.insert(q.end(), p.begin() + 5 + 4 * done, p.begin() + 5 + 4 * (done + n));
                    auto r = dap(q);
                    if (r.size() < 4)
                        throw std::runtime_error("Short DAP block response");
                    unsigned completed = u16(r.data() + 1);
                    total += completed;
                    status = r[3];
                    if (status != 1 || completed != n)
                    {
                        status = status == 1 ? 4 : status;
                        break;
                    }
                    output.insert(output.end(), r.begin() + 4, r.end());
                    done += n;
                }
            }
            else
            {
                if (p[0] == 0x0a)
                    throw std::runtime_error("NRST is not wired; use SYSRESETREQ software reset");
                auto r = dap(p);
                if (r.size() < 2 || r[0] != p[0])
                    throw std::runtime_error("Invalid Keil DAP response");
                if (p[0] == 0x10)
                {
                    output.push_back(r[1]);
                }
                else if (p[0] == 2)
                {
                    if (r[1] != 1)
                        status = 4;
                }
                else if (p[0] != 0 && r[1] != 0)
                    status = 4;
            }
            if (status != 1)
                break;
        }
        if (status == 1 && off != input.size())
            throw std::runtime_error("Unexpected trailing Keil commands");
        if (hadTransfer && status == 1 && total != count)
            status = 4;
        if (output.size() > sizeof(memory->consumer_page.data))
            throw std::runtime_error("Keil response exceeds IPC capacity");
        memory->consumer_page.data_len = (uint32_t)output.size();
        memcpy(memory->consumer_page.data, output.data(), output.size());
        memory->consumer_page.command_response = status;
        if (status != 1)
            log(L"Keil DAP 状态=" + std::to_wstring(status) + L" 实际传输=" + std::to_wstring(total) +
                L" 请求传输=" + std::to_wstring(count));
        ++requestCount;
        auto now = std::chrono::steady_clock::now();
        if (now - lastProgress > std::chrono::seconds(2))
        {
            log(L"Keil 已处理调试请求 " + std::to_wstring(requestCount.load()));
            lastProgress = now;
        }
    }
    void start()
    {
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, EL_SHARED_MEMORY_SIZE,
                                     EL_SHARED_MEMORY_NAME);
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Another Keil bridge is already running");
        memory = (el_memory_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, EL_SHARED_MEMORY_SIZE);
        if (!memory)
            throw std::runtime_error("Cannot map Keil bridge");
        memset(memory, 0, sizeof(*memory));
        producer = CreateEventA(nullptr, FALSE, FALSE, EL_EVENT_PRODUCER_NAME);
        consumer = CreateEventA(nullptr, FALSE, FALSE, EL_EVENT_CONSUMER_NAME);
        if (!producer || !consumer)
            throw std::runtime_error("Cannot create Keil bridge events");
        memory->info_page.major_version = 1;
        memory->info_page.minor_version = 4;
        // Required IPC compatibility marker for the bundled elaphureRddi 1.4.0.0 DLL.
        strcpy_s(memory->info_page.version_string, "1.4.0.0");
        memory->info_page.capabilities = 1;
        memory->info_page.device_dap_buffer_size = 64;
        strcpy_s(memory->info_page.product_name, "WirelessDAP CMSIS-DAP");
        strcpy_s(memory->info_page.serial_number, "0AE8A294");
        strcpy_s(memory->info_page.firmware_version, "2.1.2");
        memory->info_page.enable_vendor_command = 0;
        memory->info_page.is_proxy_ready = 1;
        thread = std::thread([this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            while (!stopping)
            {
                if (WaitForSingleObject(producer, 100) != WAIT_OBJECT_0)
                    continue;
                if (stopping)
                    break;
                try
                {
                    process();
                }
                catch (const std::exception &e)
                {
                    memory->consumer_page.data_len = 0;
                    memory->consumer_page.command_response = 4;
                    memory->info_page.is_proxy_ready = 0;
                    log(L"Keil " + wide(e.what()));
                    stopping = true;
                }
                SetEvent(consumer);
            }
            winrt::uninit_apartment();
        });
    }
};
KeilBridge::KeilBridge(std::shared_ptr<Link> l, Log log) : impl(std::make_unique<Impl>())
{
    impl->link = std::move(l);
    impl->log = std::move(log);
    impl->start();
}
KeilBridge::~KeilBridge() = default;

void install_keil_driver(const fs::path &root, const fs::path &assets)
{
    const auto settings = root / L"TOOLS.INI";
    if (!fs::is_regular_file(root / L"UV4/UV4.exe") || !fs::is_regular_file(settings))
        throw std::runtime_error("Select the Keil installation directory containing TOOLS.INI and UV4");
    auto original = read_file(settings);
    std::string text(original.begin(), original.end());
    auto begin = text.find("[ARM]");
    if (begin == std::string::npos)
        throw std::runtime_error("Keil ARM configuration section is missing");
    auto end = text.find("\n[", begin + 5);
    if (end == std::string::npos)
        end = text.size();
    std::string section = text.substr(begin, end - begin);
    struct ProfileEntry
    {
        const char *cpu;
        const char *title;
    };
    const ProfileEntry entries[] = {{"SARMCM3.DLL", "elaphureLink Debugger"},
                                    {"SARMV8M.DLL", "elaphureLink ARMv8-M Debugger"}};
    for (const auto &entry : entries)
    {
        if (section.find(entry.title) != std::string::npos)
            continue;
        auto cpu = section.find(entry.cpu);
        if (cpu == std::string::npos)
            continue;
        auto closing = section.find(')', cpu);
        if (closing == std::string::npos)
            throw std::runtime_error("Invalid Keil CPU driver registration");
        unsigned number = 900;
        for (; number < 1000; ++number)
            if (section.find("TDRV" + std::to_string(number) + "=") == std::string::npos)
                break;
        if (number == 1000)
            throw std::runtime_error("No free Keil driver registration slot");
        std::string key = "TDRV" + std::to_string(number);
        section.insert(closing, "," + key);
        section += "\r\n" + key + "=BIN\\elaphureLink.dll(\"" + entry.title + "\")\r\n";
    }
    text.replace(begin, end - begin, section);
    const auto backup = root / L"WirelessDAP-backups" / std::to_wstring(GetTickCount64());
    for (const wchar_t *name : {L"elaphureLink.dll", L"elaphureRddi.dll"})
    {
        auto source = read_file(assets / L"keil" / name);
        auto destination = root / L"ARM/BIN" / name;
        if (fs::exists(destination))
        {
            auto current = read_file(destination);
            if (current == source)
                continue;
            write_file(backup / name, current);
        }
        auto temporary = destination;
        temporary += L".WirelessDAP.tmp";
        write_file(temporary, source);
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(
                "Close Keil before updating its plugin, or choose a writable installation directory");
        if (read_file(destination) != source)
            throw std::runtime_error("Keil plugin copy verification failed");
    }
    Bytes updated(text.begin(), text.end());
    if (updated != original)
    {
        write_file(backup / L"TOOLS.INI", original);
        auto temporary = settings;
        temporary += L".WirelessDAP.tmp";
        write_file(temporary, updated);
        if (!MoveFileExW(temporary.c_str(), settings.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Keil settings could not be updated; original settings are preserved");
    }
}

uint64_t KeilBridge::request_count() const
{
    return impl->requestCount.load();
}
