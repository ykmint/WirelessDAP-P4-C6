#include "../src/board_config.h"
#include "../src/core.h"
#include <iostream>
#include <winrt/base.h>
#include "parallel_fixture.h"

int wmain(int argc, wchar_t **argv)
{
    std::shared_ptr<Link> link;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WSADATA sockets{};
        WSAStartup(MAKEWORD(2, 2), &sockets);
        std::wstring endpoint = argc > 1 ? argv[1] : L"BLE";
        auto connect = [&]() {
            if (endpoint == L"BLE")
                return connect_ble(0xE83DC1D25042ULL, BOARD_TOKEN);
            if (endpoint.starts_with(L"COM"))
                return connect_usb(endpoint, BOARD_TOKEN);
            return connect_tcp(endpoint, 6123, BOARD_TOKEN);
        };
        link = connect();
        if (argc > 2 && std::wstring(argv[2]) == L"power")
        {
            uint8_t on = 1;
            link->request(4, {&on, 1});
        }
        auto state = link->request(3);
        std::cout << std::string(state.begin(), state.end()) << std::endl;
        auto device = detect_stm32(*link);
        std::cout << "DP=" << std::hex << device.dp << " ID=" << device.id << " profile=" << std::dec
                  << device.profile << std::endl;
        Engine engine(fs::absolute(L"native/build/payload"));
        auto log = [](const std::wstring &line) { std::cout << utf8(line) << std::endl; };
        std::wstring action = argc > 3 ? argv[3] : L"";
        if (action == L"backup" && device.profile == 9)
        {
            auto path = engine.backup(link, 9, 0x08000000, 0x200000, log);
            std::cout << "BACKUP=" << path.string() << std::endl;
        }
        if (action == L"flash-test" || action == L"ble-flash-test")
        {
            if (device.profile != 9 || (device.id & 0xfff) != 0x450)
                throw std::runtime_error("This acceptance test requires the connected H743");
            constexpr uint32_t address = 0x081e0000;
            auto backup = engine.backup(link, 9, address, 0x20000, log);
            std::cout << "SECTOR_BACKUP=" << backup.string() << std::endl;
            Bytes pattern(256);
            for (size_t i = 0; i < pattern.size(); ++i)
                pattern[i] = uint8_t(i * 37 + 19);
            auto testFile = backup.parent_path() / L"acceptance-pattern.bin";
            write_file(testFile, pattern);
            bool wireless = action == L"ble-flash-test";
            if (wireless)
            {
                link->disconnect();
                link = connect_ble(0xE83DC1D25042ULL, BOARD_TOKEN);
                uint8_t on = 1;
                link->request(4, {&on, 1});
                auto verifiedTarget = detect_stm32(*link);
                if (verifiedTarget.id != device.id)
                    throw std::runtime_error("Target changed before BLE flash test");
            }
            std::exception_ptr failure;
            try
            {
                engine.flash(link, 9, testFile, address, false, log);
                std::cout << "PASS TEST_IMAGE_PROGRAM_AND_READBACK" << std::endl;
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            if (wireless)
            {
                link->disconnect();
                link = connect();
                auto statusBytes = link->request(3);
                std::string status(statusBytes.begin(), statusBytes.end());
                if (json_int(status, "vref_mv") < 400)
                {
                    uint8_t on = 1;
                    link->request(4, {&on, 1});
                }
            }
            std::cout << "RESTORING_ORIGINAL_SECTOR" << std::endl;
            engine.flash(link, 9, backup, address, false, log);
            std::cout << "PASS ORIGINAL_SECTOR_RESTORED_AND_VERIFIED" << std::endl;
            if (failure)
                std::rethrow_exception(failure);
        }
        if (action == L"verify-original")
        {
            if (argc < 5 || (device.id & 0xfff) != 0x450)
                throw std::runtime_error("Expected original H743 sector file");
            auto expected = read_file(argv[4]);
            if (expected.size() != 0x20000)
                throw std::runtime_error("Expected 128KiB original sector");
            auto current = engine.backup(link, 9, 0x081e0000, 0x20000, log);
            if (read_file(current) != expected)
                engine.flash(link, 9, argv[4], 0x081e0000, false, log);
            std::cout << "PASS ORIGINAL_SECTOR_CONTENT_VERIFIED" << std::endl;
        }
        if (action == L"keil-cli" || action == L"keil-ble")
        {
            if ((device.id & 0xfff) != 0x450)
                throw std::runtime_error("Expected H743 target");
            auto backup = engine.backup(link, 9, 0x081e0000, 0x20000, log);
            std::cout << "KEIL_SECTOR_BACKUP=" << backup.string() << std::endl;
            const bool wirelessKeil = action == L"keil-ble";
            if (wirelessKeil)
            {
                link->disconnect();
                link = connect_ble(0xE83DC1D25042ULL, BOARD_TOKEN);
                uint8_t on = 1;
                link->request(4, {&on, 1});
            }
            std::exception_ptr failure;
            try
            {
                KeilBridge bridge(link, log);
                auto folder = fs::absolute(L"work/keil_acceptance");
                uint64_t previousRequests = 0;
                auto progressAt = std::chrono::steady_clock::now();
                int code = run_process(
                    L"E:/keil5/UV4/UV4.exe", L"-f acceptance.uvprojx -o keil-download.log", folder, log,
                    wirelessKeil ? 900 : 180, [&] {
                        static auto last = std::chrono::steady_clock::now();
                        auto requests = bridge.request_count();
                        if (requests != previousRequests)
                        {
                            previousRequests = requests;
                            progressAt = std::chrono::steady_clock::now();
                        }
                        if (std::chrono::steady_clock::now() - progressAt > std::chrono::seconds(45))
                            throw std::runtime_error(
                                "Keil application issued no new DAP commands for 45 seconds");
                        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
                        {
                            link->request(3);
                            last = std::chrono::steady_clock::now();
                        }
                    });
                std::cout << "KEIL_EXIT=" << code << std::endl;
                if (code)
                    throw std::runtime_error("Keil application download failed");
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            if (wirelessKeil)
            {
                link->disconnect();
                link = connect();
                auto bytes = link->request(3);
                std::string status(bytes.begin(), bytes.end());
                if (json_int(status, "vref_mv") < 400)
                {
                    uint8_t on = 1;
                    link->request(4, {&on, 1});
                }
            }
            engine.flash(link, 9, backup, 0x081e0000, false, log);
            std::cout << "PASS KEIL_TEST_ORIGINAL_SECTOR_RESTORED" << std::endl;
            if (failure)
                std::rethrow_exception(failure);
        }
        if (action == L"keil-test")
        {
            KeilBridge bridge(link, log);
            int code = run_process(
                fs::absolute(L"native/build/rddi_smoke.exe"),
                L"\"" + fs::absolute(L"native/build/payload/keil/elaphureRddi.dll").wstring() + L"\"",
                fs::current_path(), log, 60);
            if (code)
                throw std::runtime_error("Native Keil RDDI acceptance failed");
            std::cout << "PASS NATIVE_KEIL_RDDI_H743_READS" << std::endl;
        }
        if (action == L"i2c-scan")
        {
            for (uint8_t channel : {1, 2, 3})
            {
                Bytes port{channel};
                put32(port, 115200);
                port.insert(port.end(), {0, 8, 1});
                link->request(17, port);
            }
            Bytes canConfig{6};
            put32(canConfig, 500000);
            canConfig.insert(canConfig.end(), {2, 0, 0});
            link->request(17, canConfig);

            Bytes config{4};
            put32(config, argc > 4 ? std::stoul(argv[4]) : 100000);
            config.insert(config.end(), {0, 0, 0});
            link->request(17, config);
            for (int round = 0; round < 32; round++)
            {
                Bytes spiConfig{5};
                put32(spiConfig, 1000000);
                spiConfig.insert(spiConfig.end(), {uint8_t((round / 2) % 4), uint8_t(round % 2), 0});
                link->request(17, spiConfig);
                Bytes spiData(241, 0xa5);
                spiData[0] = 5;
                link->request(18, spiData);
                link->request(18, Bytes{3, 0x55, 0xaa});
                link->request(17, config);
                std::cout << "SCAN " << round << std::endl;
                for (int start = 8; start <= 0x77; start += 16)
                {
                    Bytes range{4, uint8_t(start), uint8_t(std::min(start + 15, 0x77))};
                    auto found = link->request(20, range);
                    for (auto address : found)
                        std::cout << "ACK " << std::hex << unsigned(address) << std::dec << std::endl;
                }
            }
        }
        if (action == L"keil-debug")
        {
            if ((device.id & 0xfff) != 0x450)
                throw std::runtime_error("Expected H743 target for debug acceptance");
            std::unique_ptr<ParallelFixture> parallel;
            if (argc > 4 && std::wstring(argv[4]) == L"parallel")
                parallel = std::make_unique<ParallelFixture>(link);
            write_file(L"work/keil_debug_acceptance/keil-debug-session.log", {});
            KeilBridge bridge(link, log);
            auto last = std::chrono::steady_clock::now();
            int result =
                run_process(L"E:/keil5/UV4/UV4.exe", L"-d debug.uvprojx -j0 -sg -o debug-launch.log",
                            fs::absolute(L"work/keil_debug_acceptance"), log, parallel ? 1800 : 180, [&] {
                                if (parallel)
                                    parallel->tick();
                                if (std::chrono::steady_clock::now() - last > std::chrono::seconds(1))
                                {
                                    link->request(3);
                                    last = std::chrono::steady_clock::now();
                                }
                            });
            if (result)
                throw std::runtime_error("Keil debug application failed");
            auto output = read_file(L"work/keil_debug_acceptance/keil-debug-session.log");
            std::string record(output.begin(), output.end());
            if (record.find("KEIL_BREAKPOINT_RETURN PC=0800711C DFSR=00000002") == std::string::npos ||
                record.find("error") != std::string::npos || record.find("Error") != std::string::npos)
                throw std::runtime_error("Keil debug script did not complete cleanly");
            if (parallel)
                parallel->verify();
            std::cout << record << std::endl << "PASS KEIL_APPLICATION_DEBUG_SESSION" << std::endl;
        }
        if (action == L"can-frames")
        {
            Bytes configuration{6};
            put32(configuration, 500000);
            configuration.insert(configuration.end(), {2, 0, 0});
            link->request(17, configuration);
            unsigned frames = 0;
            for (uint8_t flags = 0; flags < 4; ++flags)
            {
                for (uint8_t dlc = 0; dlc <= 8; ++dlc)
                {
                    Bytes frame{6};
                    put32(frame, (flags & 1) ? 0x1abcde : 0x123);
                    frame.insert(frame.end(), {flags, dlc});
                    if (!(flags & 2))
                        for (uint8_t i = 0; i < dlc; ++i)
                            frame.push_back(uint8_t(i * 17 + dlc));
                    link->request(18, frame);
                    auto received = link->request(19, Bytes{6});
                    Bytes expected(frame.begin() + 1, frame.end());
                    expected.resize(14);
                    if (received != expected)
                        throw std::runtime_error("CAN frame mismatch flags=" + std::to_string(flags) +
                                                 " dlc=" + std::to_string(dlc));
                    ++frames;
                    std::cout << "PASS CAN flags=" << unsigned(flags) << " dlc=" << unsigned(dlc)
                              << std::endl;
                }
            }
            link->request(22, Bytes{6});
            std::cout << "PASS CAN_ALL_36_CLASSIC_FRAME_VARIANTS=" << frames << std::endl;
        }
        if (action == L"hid-power")
        {
            uint8_t enabled = 1;
            link->request(24, {&enabled, 1});
            std::cout << "USB DAP target-power policy enabled" << std::endl;
        }
        link->disconnect();
        return device.profile == 9 ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << std::endl;
        if (link)
            link->disconnect();
        return 1;
    }
}
