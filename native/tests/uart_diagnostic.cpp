#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/base.h>
#include <iostream>
int wmain(int argc, wchar_t **argv)
{
    HANDLE port = INVALID_HANDLE_VALUE;
    std::shared_ptr<Link> link;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WSADATA sockets{};
        WSAStartup(MAKEWORD(2, 2), &sockets);
        port =
            CreateFileW(L"\\\\.\\COM10", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (port == INVALID_HANDLE_VALUE)
            throw std::runtime_error("COM10 unavailable");
        DCB d{sizeof(d)};
        GetCommState(port, &d);
        d.BaudRate = 115200;
        d.ByteSize = 8;
        d.Parity = NOPARITY;
        d.StopBits = ONESTOPBIT;
        d.fBinary = TRUE;
        d.fParity = FALSE;
        d.fOutxCtsFlow = FALSE;
        d.fOutxDsrFlow = FALSE;
        d.fDtrControl = DTR_CONTROL_ENABLE;
        d.fRtsControl = RTS_CONTROL_DISABLE;
        d.fOutX = FALSE;
        d.fInX = FALSE;
        d.fAbortOnError = FALSE;
        COMMTIMEOUTS timeouts{MAXDWORD, 0, 50, 0, 1000};
        if (!SetCommState(port, &d) || !SetCommTimeouts(port, &timeouts))
            throw std::runtime_error("COM10 setup failed");
        PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR);
        link = argc > 1 ? connect_ble(0xE83DC1D25042ULL, BOARD_TOKEN) : connect_usb(L"COM16", BOARD_TOKEN);
        link->request(4, Bytes{1});
        Bytes config{1};
        put32(config, 115200);
        config.insert(config.end(), {0, 8, 1});
        link->request(17, config);
        auto collect = [&](unsigned duration) {
            Bytes result;
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
            while (std::chrono::steady_clock::now() < until)
            {
                auto data = link->request(19, Bytes{1});
                result.insert(result.end(), data.begin(), data.end());
                Sleep(50);
            }
            return result;
        };
        auto idle = collect(1000);
        std::cout << "IDLE_BYTES=" << idle.size() << std::endl;
        for (size_t length : {16, 64, 96, 128, 240})
        {
            Bytes tx(length);
            for (size_t i = 0; i < length; i++)
                tx[i] = uint8_t(i * 17 + length);
            DWORD written = 0;
            if (!WriteFile(port, tx.data(), DWORD(tx.size()), &written, nullptr) || written != tx.size())
                throw std::runtime_error("Write incomplete");
            FlushFileBuffers(port);
            auto received = collect(1500);
            std::cout << "SENT=" << length << " GOT=" << received.size() << " MATCH=" << (received == tx)
                      << " PREFIX="
                      << (received.size() >= tx.size() && std::equal(tx.begin(), tx.end(), received.begin()))
                      << std::endl;
            std::cout << "RX=";
            for (auto b : received)
                std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(b) << ' ';
            std::cout << std::dec << std::endl;
            auto after = collect(300);
            std::cout << "AFTER_IDLE_BYTES=" << after.size() << std::endl;
        }
        auto status = link->request(21, Bytes{1});
        std::cout << std::string(status.begin(), status.end()) << std::endl;
        link->disconnect();
        CloseHandle(port);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        if (link)
            link->disconnect();
        if (port != INVALID_HANDLE_VALUE)
            CloseHandle(port);
        return 1;
    }
}
