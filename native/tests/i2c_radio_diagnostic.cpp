#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <iostream>
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
int wmain(int argc, wchar_t **argv)
{
    std::shared_ptr<Link> link;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        WSADATA w{};
        WSAStartup(MAKEWORD(2, 2), &w);
        link = argc > 1 ? connect_ble(0xE83DC1D25042ULL, BOARD_TOKEN) : connect_usb(L"COM16", BOARD_TOKEN);
        link->request(4, Bytes{1});
        Bytes cfg{4};
        put32(cfg, 100000);
        cfg.insert(cfg.end(), {0, 0, 0});
        link->request(17, cfg);
        auto scan = [&](const char *name) {
            unsigned acknowledgments = 0;
            for (int pass = 0; pass < 32; pass++)
                for (int first = 8; first < 120; first += 16)
                {
                    auto found = link->request(20, Bytes{4, uint8_t(first), uint8_t(first + 15)});
                    for (auto a : found)
                    {
                        if (a < first || a > first + 15)
                            throw std::runtime_error("ACK outside requested address range");
                        ++acknowledgments;
                    }
                }
            std::cout << name << " PROBES=3584 ACK=" << acknowledgments << std::endl;
        };
        scan(argc > 1 ? "BLE_I2C_ONLY" : "USB_WITHOUT_BLE");
        if (argc > 1) { link->disconnect(); return 0; }
        auto device = BluetoothLEDevice::FromBluetoothAddressAsync(0xE83DC1D25042ULL).get();
        if (!device)
            throw std::runtime_error("BLE device missing");
        auto session = GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()).get();
        session.MaintainConnection(true);
        auto services = device.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
        GattDeviceService service{nullptr};
        GattCharacteristic characteristic{nullptr};
        for (auto item : services.Services())
            if (item.Uuid() == winrt::guid(L"965a0001-0d7e-4e4d-9a5e-f742e87dc610"))
                service = item;
        if (!service)
            throw std::runtime_error("BLE service unavailable");
        auto chars = service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
        for (auto item : chars.Characteristics())
            if (item.Uuid() == winrt::guid(L"965a0003-0d7e-4e4d-9a5e-f742e87dc610"))
                characteristic = item;
        if (!characteristic)
            throw std::runtime_error("BLE read characteristic unavailable");
        scan("USB_WITH_IDLE_BLE");
        std::atomic<bool> stop = false;
        std::atomic<unsigned> reads = 0;
        std::jthread traffic([&] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                while (!stop)
                {
                    auto result = characteristic.ReadValueAsync(BluetoothCacheMode::Uncached).get();
                    if (result.Status() != GattCommunicationStatus::Success)
                        break;
                    ++reads;
                    Sleep(20);
                }
            }
            catch (...)
            {
            }
            winrt::uninit_apartment();
        });
        try
        {
            scan("USB_WITH_ACTIVE_BLE_READS");
        }
        catch (...)
        {
            stop = true;
            traffic.join();
            throw;
        }
        stop = true;
        traffic.join();
        std::cout << "BLE_READS=" << reads << std::endl;
        service.Close();
        session.MaintainConnection(false);
        session.Close();
        device.Close();
        link->disconnect();
        return 0;
    }
    catch (const winrt::hresult_error &e)
    {
        std::cerr << winrt::to_string(e.message()) << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
    }
    if (link)
        link->disconnect();
    return 1;
}
