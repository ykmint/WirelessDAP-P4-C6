#include "../src/stlink_bridge.h"
#include "../src/board_config.h"
#include "parallel_fixture.h"
#include <winrt/base.h>
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    std::shared_ptr<Link> link;
    fs::path backup;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WSADATA sockets{};
        WSAStartup(MAKEWORD(2, 2), &sockets);
        bool wireless = argc > 1 && std::wstring(argv[1]) == L"BLE";
        std::wstring action = argc > 2 ? argv[2] : L"read";
        bool writeTest = action == L"flash" || action == L"native";
        std::mutex outputMutex;
        std::string output;
        auto log = [&](const std::wstring &line) {
            std::lock_guard lock(outputMutex);
            auto text = utf8(line);
            std::cout << text << std::endl;
            output += text + "\n";
        };
        auto connect = [&](bool bluetooth) {
            auto connection =
                bluetooth ? connect_ble(0xe83dc1d25042ULL, BOARD_TOKEN) : connect_usb(L"COM16", BOARD_TOKEN);
            connection->request(4, Bytes{1});
            return connection;
        };
        link = connect(writeTest ? false : wireless);
        Engine engine(fs::absolute(L"work/stlink_ui_20261101/recovery-engine"));
        if (action == L"full")
        {
            auto image = engine.backup(link, 9, 0x08000000, 0x200000, log);
            if (read_file(image) != read_file(L"work/h743_original_20261031/flash_08000000_2MiB.bin"))
                throw std::runtime_error("Final 2MiB Flash differs from original backup");
            std::cout << "PASS FINAL_2MIB_FLASH_MATCHES_ORIGINAL " << image.string() << std::endl;
            link->disconnect();
            return 0;
        }
        if (action == L"parallel")
        {
            ParallelFixture fixture(link);
            StlinkBridge bridge(link, log, 0);
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            auto next = std::chrono::steady_clock::now();
            unsigned reads = 0;
            while (std::chrono::steady_clock::now() < until)
            {
                fixture.tick();
                if (std::chrono::steady_clock::now() >= next)
                {
                    if (bridge.read_debug_register(0xe000ed00) != 0x411fc271)
                        throw std::runtime_error("Concurrent ST-LINK CPUID differs");
                    ++reads;
                    next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                }
                Sleep(10);
            }
            fixture.verify();
            std::cout << "PASS STLINK_PARALLEL_TARGET_READS=" << reads << std::endl;
            link->disconnect();
            return 0;
        }
        if (writeTest)
        {
            auto target = detect_stm32(*link);
            if ((target.id & 0xfff) != 0x450 || target.flash_kib != 2048)
                throw std::runtime_error("Flash acceptance requires the connected 2MiB H743");
            backup = engine.backup(link, 9, 0x081e0000, 0x20000, log);
            std::cout << "STLINK_ORIGINAL_SECTOR=" << backup.string() << std::endl;
            if (wireless || (argc > 3 && std::wstring(argv[3]) == L"cold"))
            {
                link->disconnect();
                Sleep(1000);
                link = connect(wireless);
            }
        }
        Bytes pattern(256);
        for (size_t i = 0; i < pattern.size(); ++i)
            pattern[i] = uint8_t(i * 29 + 0x37);
        auto patternFile = writeTest ? backup.parent_path() / L"stlink-acceptance.bin" : fs::path{};
        if (writeTest)
            write_file(patternFile, pattern);
        std::exception_ptr failure;
        try
        {
            if (action == L"native")
            {
                Engine programmer(fs::absolute(L"native/build/payload"));
                programmer.useStlink = true;
                programmer.flash(link, 9, patternFile, 0x081e0000, false, log);
                std::cout << "PASS NATIVE_STLINK_PROGRAM_AND_READBACK" << std::endl;
            }
            else
            {
                StlinkBridge bridge(link, log, 7185, true);
                auto executable = fs::path(
                    L"E:/STM32CubeIDE/STM32CubeIDE/plugins/"
                    L"com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.100.202412061334/"
                    L"tools/bin/STM32_Programmer_CLI.exe");
                std::wstring arguments = L"-c port=SWD shared tcpport=7185 mode=HOTPLUG freq=100 ";
                arguments +=
                    writeTest ? L"-w \"" + patternFile.wstring() + L"\" 0x081e0000 -v" : L"-r32 0xE000ED00 1";
                auto next = std::chrono::steady_clock::now();
                int result = run_process(executable, arguments, fs::current_path(), log, 1200, [&] {
                    if (std::chrono::steady_clock::now() >= next)
                    {
                        link->request(3);
                        next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    }
                });
                std::cout << "STLINK_USB_COMMANDS=" << bridge.request_count() << std::endl;
                if (result)
                    throw std::runtime_error("Official CubeProgrammer operation failed");
                if (!writeTest && output.find("411FC271") == output.npos)
                    throw std::runtime_error("Official CubeProgrammer did not verify H743 CPUID");
                std::cout << (writeTest ? "PASS OFFICIAL_STLINK_DOWNLOAD_AND_VERIFY"
                                        : "PASS OFFICIAL_CUBEPROGRAMMER_STLINK_H743_CPUID")
                          << std::endl;
            }
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        if (writeTest)
        {
            try
            {
                {
                    StlinkBridge controller(link, log, 0);
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    for (;;)
                    {
                        uint32_t bank1 = controller.read_debug_register(0x52002010);
                        uint32_t bank2 = controller.read_debug_register(0x52002110);
                        if (!((bank1 | bank2) & 5))
                            break;
                        if (std::chrono::steady_clock::now() >= deadline)
                            throw std::runtime_error(
                                "Flash controller is still busy; keeping live transport");
                        Sleep(20);
                    }
                }
                std::cout << "FLASH_CONTROLLERS_IDLE" << std::endl;
                link->disconnect();
                Sleep(1000);
                link = connect(false);
                engine.useStlink = false;
                if (!failure)
                {
                    auto readback = engine.backup(link, 9, 0x081e0000, uint32_t(pattern.size()), log);
                    if (read_file(readback) != pattern)
                        throw std::runtime_error("Independent Flash readback differs from ST-LINK pattern");
                    std::cout << "PASS INDEPENDENT_STLINK_FLASH_READBACK" << std::endl;
                }
            }
            catch (...)
            {
                if (!failure)
                    failure = std::current_exception();
            }
            engine.useStlink = false;
            std::cout << "RESTORING_ORIGINAL_SECTOR=" << backup.string() << std::endl;
            engine.flash(link, 9, backup, 0x081e0000, false, log);
            std::cout << "PASS STLINK_ORIGINAL_SECTOR_RESTORED_AND_VERIFIED" << std::endl;
        }
        link->disconnect();
        if (failure)
            std::rethrow_exception(failure);
        return 0;
    }
    catch (const std::exception &error)
    {
        if (link)
            link->disconnect();
        if (!backup.empty())
            std::cerr << "SECTOR_BACKUP_RETAINED=" << backup.string() << std::endl;
        std::cerr << error.what() << std::endl;
        return 1;
    }
}