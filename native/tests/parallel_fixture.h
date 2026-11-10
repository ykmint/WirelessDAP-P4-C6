#pragma once
#include "../src/core.h"
#include <iostream>

// Physical UART1 fixture plus driver checks while the same Link serves Keil.
class ParallelFixture
{
    struct SerialHandle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~SerialHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    } serial;
    std::shared_ptr<Link> link;
    unsigned phase = 0, rounds = 0;
    Bytes pattern = Bytes(96), canFrame;
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();

    void configure(uint8_t channel, uint32_t rate, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0)
    {
        Bytes data{channel};
        put32(data, rate);
        data.insert(data.end(), {a, b, c});
        link->request(17, data);
    }

  public:
    explicit ParallelFixture(std::shared_ptr<Link> connection) : link(std::move(connection))
    {
        serial.value =
            CreateFileW(L"\\\\.\\COM10", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (serial.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("COM10 fixture unavailable");
        DCB settings{sizeof(settings)};
        if (!GetCommState(serial.value, &settings))
            throw std::runtime_error("Cannot read UART fixture settings");
        settings.BaudRate = 115200;
        settings.ByteSize = 8;
        settings.Parity = NOPARITY;
        settings.StopBits = ONESTOPBIT;
        settings.fBinary = TRUE;
        settings.fParity = settings.fOutxCtsFlow = settings.fOutxDsrFlow = FALSE;
        settings.fDtrControl = DTR_CONTROL_ENABLE;
        settings.fRtsControl = RTS_CONTROL_DISABLE;
        settings.fOutX = settings.fInX = settings.fAbortOnError = FALSE;
        COMMTIMEOUTS timeouts{MAXDWORD, 0, 50, 0, 1000};
        if (!SetCommState(serial.value, &settings) || !SetCommTimeouts(serial.value, &timeouts))
            throw std::runtime_error("Cannot configure UART fixture");
        PurgeComm(serial.value, PURGE_RXCLEAR | PURGE_TXCLEAR);
        for (uint8_t channel : {1, 2, 3})
            configure(channel, 115200, 0, 8, 1);
        configure(4, 100000);
        configure(5, 1000000);
        configure(6, 500000, 2);
        // Discard input captured while the powered fixture and GPIO routes settle.
        // All bytes after this point are compared exactly during each active cycle.
        Sleep(200);
        auto startup = link->request(19, Bytes{1});
        PurgeComm(serial.value, PURGE_RXCLEAR);
        std::cout << "UART_STARTUP_BYTES=" << startup.size() << std::endl;
    }
    void tick()
    {
        if (std::chrono::steady_clock::now() < next)
            return;
        next = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        switch (phase)
        {
        case 0: {
            for (size_t i = 0; i < pattern.size(); ++i)
                pattern[i] = uint8_t(i * 17 + rounds * 29);
            DWORD sent = 0;
            if (!WriteFile(serial.value, pattern.data(), DWORD(pattern.size()), &sent, nullptr) ||
                sent != pattern.size())
                throw std::runtime_error("Parallel UART fixture write failed");
            if (!FlushFileBuffers(serial.value))
                throw std::runtime_error("Parallel UART fixture drain failed");
            break;
        }
        case 1: {
            Bytes received;
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (received.size() < pattern.size() && std::chrono::steady_clock::now() < until)
            {
                auto data = link->request(19, Bytes{1});
                received.insert(received.end(), data.begin(), data.end());
            }
            if (received != pattern)
                throw std::runtime_error(
                    "Parallel physical UART1 RX mismatch received=" + std::to_string(received.size()) +
                    " expected=" + std::to_string(pattern.size()));
            break;
        }
        case 2: {
            Bytes data{1};
            data.insert(data.end(), pattern.begin(), pattern.end());
            link->request(18, data);
            break;
        }
        case 3: {
            Bytes received;
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (received.size() < pattern.size() && std::chrono::steady_clock::now() < until)
            {
                uint8_t bytes[256];
                DWORD count = 0;
                if (!ReadFile(serial.value, bytes, sizeof(bytes), &count, nullptr))
                    throw std::runtime_error("Parallel UART fixture read failed");
                received.insert(received.end(), bytes, bytes + count);
            }
            if (received != pattern)
                throw std::runtime_error(
                    "Parallel physical UART1 TX mismatch received=" + std::to_string(received.size()) +
                    " expected=" + std::to_string(pattern.size()));
            break;
        }
        case 4: {
            Bytes data(241, uint8_t(rounds));
            data[0] = 5;
            if (link->request(18, data).size() != 240)
                throw std::runtime_error("Parallel SPI driver length mismatch");
            break;
        }
        case 5:
            canFrame = Bytes{6};
            put32(canFrame, 0x123);
            canFrame.insert(canFrame.end(), {0, 2, uint8_t(rounds), 0x5a});
            link->request(18, canFrame);
            break;
        case 6: {
            auto received = link->request(19, Bytes{6});
            Bytes expected(canFrame.begin() + 1, canFrame.end());
            expected.resize(14);
            if (received != expected)
                throw std::runtime_error("Parallel CAN self-loop mismatch");
            break;
        }
        case 7: {
            uint8_t first = uint8_t(8 + 16 * (rounds % 7));
            if (!link->request(20, Bytes{4, first, uint8_t(first + 15)}).empty())
                throw std::runtime_error("Parallel empty I2C bus false ACK");
            break;
        }
        case 8:
            link->request(18, Bytes{3, 0x55, 0xaa});
            break;
        case 9:
            link->request(18, Bytes{2, 0xa5, 0x5a});
            break;
        case 10: {
            auto data = link->request(21);
            if (json_int(std::string(data.begin(), data.end()), "enabled_mask") != 126)
                throw std::runtime_error("Keil activity closed a concurrent interface");
            ++rounds;
            if (rounds % 10 == 0)
                std::cout << "PARALLEL_ROUNDS=" << rounds
                          << " UART_BYTES_EACH_DIRECTION=" << rounds * pattern.size() << std::endl;
            break;
        }
        }
        phase = (phase + 1) % 11;
    }
    void verify()
    {
        auto data = link->request(21, Bytes{1});
        if (rounds < 10 || json_int(std::string(data.begin(), data.end()), "rx_errors") != 0)
            throw std::runtime_error("Insufficient parallel cycles or UART errors");
        std::cout << "PASS KEIL_PARALLEL_ROUNDS=" << rounds
                  << " UART_BYTES_EACH_DIRECTION=" << rounds * pattern.size() << std::endl;
    }
};
