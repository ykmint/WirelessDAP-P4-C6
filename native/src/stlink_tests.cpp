#include "stlink_bridge.h"
#include <map>

namespace
{
void verify(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
class MemoryProbe final : public Link
{
    uint32_t select = 0, csw = 0x23000052, address = 0;
    bool idRequired = false;
    std::map<uint32_t, uint8_t> bytes;
    uint32_t access(uint8_t request, bool write, uint32_t value)
    {
        uint32_t reg = request & 12;
        if (!(request & 1))
        {
            if (reg == 8)
            {
                if (write)
                    select = value;
                return select;
            }
            if (reg == 4)
                return 0xf0000000;
            return write ? 0 : 0x6ba02477;
        }
        reg |= select & 0xf0;
        if (reg == 0xfc)
            return 0x24770011;
        if (reg == 0)
        {
            if (write)
                csw = value;
            return csw;
        }
        if (reg == 4)
        {
            if (write)
                address = value;
            return address;
        }
        if (reg != 12)
            return 0;
        unsigned width = 1U << (csw & 7);
        verify(width <= 4, "Invalid fixture memory width");
        uint32_t result = 0;
        for (unsigned i = 0; i < width; ++i)
        {
            unsigned lane = (address + i) & 3;
            if (write)
                bytes[address + i] = uint8_t(value >> (8 * lane));
            else
                result |= uint32_t(bytes[address + i]) << (8 * lane);
        }
        if (write)
            ++writes;
        if (csw & 0x10)
            address = (address & ~1023U) | ((address + width) & 1023);
        return result;
    }

  public:
    std::atomic<unsigned> writes = 0;
    void close() noexcept override
    {
    }
    Bytes exchange(std::span<const uint8_t> request) override
    {
        Bytes payload;
        if (request[3] == 3)
        {
            const std::string state = "{\"vref_mv\":3300,\"target_power\":1,\"fault\":0}";
            payload.assign(state.begin(), state.end());
        }
        else
        {
            verify(request[3] == 2 && request.size() == 74, "Unexpected fixture WD command");
            auto dap = request.subspan(10);
            if (dap[0] == 2)
                payload = {2, 1};
            else if (dap[0] == 4 || dap[0] == 0x11 || dap[0] == 0x12 || dap[0] == 0x13)
            {
                if (dap[0] == 0x12)
                    idRequired = true;
                payload = {dap[0], 0};
            }
            else if (dap[0] == 5)
            {
                payload = {5, 0, 1};
                size_t offset = 3;
                for (unsigned index = 0; index < dap[2]; ++index)
                {
                    uint8_t op = dap[offset++];
                    bool write = !(op & 2);
                    if (idRequired && op != 2)
                    {
                        payload[2] = 7;
                        break;
                    }
                    idRequired = false;
                    uint32_t value = write ? u32(dap.data() + offset) : 0;
                    if (write)
                        offset += 4;
                    value = access(op, write, value);
                    if (!write)
                        put32(payload, value);
                    ++payload[1];
                }
            }
            else if (dap[0] == 6)
            {
                unsigned count = u16(dap.data() + 2);
                bool write = !(dap[4] & 2);
                verify(count <= (write ? 14U : 15U), "DAP block exceeds firmware packet size");
                payload = {6, uint8_t(count), uint8_t(count >> 8), 1};
                for (unsigned index = 0; index < count; ++index)
                {
                    uint32_t value = write ? u32(dap.data() + 5 + index * 4) : 0;
                    value = access(dap[4], write, value);
                    if (!write)
                        put32(payload, value);
                }
            }
            else
                throw std::runtime_error("Unsupported fixture DAP operation");
        }
        Bytes response{'W', 'D', 1, uint8_t(request[3] | 0x80)};
        put32(response, u32(request.data() + 4));
        put16(response, uint16_t(payload.size() + 1));
        response.push_back(0);
        response.insert(response.end(), payload.begin(), payload.end());
        return response;
    }
};

void assign32(Bytes &bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = uint8_t(value >> (8 * i));
}
class Client
{
    SOCKET socket = INVALID_SOCKET;

  public:
    uint32_t handle = 0;
    explicit Client(uint16_t port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        verify(socket != INVALID_SOCKET, "ST-LINK test socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (connect(socket, (sockaddr *)&address, sizeof(address)))
        {
            closesocket(socket);
            socket = INVALID_SOCKET;
            throw std::runtime_error("ST-LINK test connection failed");
        }
        DWORD timeout = 2000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    }
    ~Client()
    {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
    }
    void sendFrame(std::span<const uint8_t> frame, bool fragmented = false)
    {
        while (!frame.empty())
        {
            size_t count = fragmented ? std::min<size_t>(7, frame.size()) : frame.size();
            int sent = send(socket, (const char *)frame.data(), int(count), 0);
            verify(sent > 0, "ST-LINK test write failed");
            frame = frame.subspan(sent);
            if (fragmented)
                Sleep(1);
        }
    }
    Bytes read(size_t length)
    {
        Bytes result(length);
        size_t offset = 0;
        while (offset < length)
        {
            int count = recv(socket, (char *)result.data() + offset, int(length - offset), 0);
            verify(count > 0, "ST-LINK test response truncated");
            offset += count;
        }
        return result;
    }
    Bytes request(Bytes bytes, size_t length, bool fragmented = false)
    {
        sendFrame(bytes, fragmented);
        return read(length);
    }
    void open()
    {
        auto version = request({6, 1, 0, 0}, 16, true);
        verify(u32(version.data()) == 3, "ST-LINK API 3 version mismatch");
        verify(u32(request({0, 0}, 4).data()) == 1, "ST-LINK refresh failed");
        verify(u32(request({1}, 4).data()) == 1, "ST-LINK device count mismatch");
        auto info = request({2, 0, 0, 0, 41, 0, 0, 0}, 45, true);
        verify(u32(info.data()) == 1 && u16(info.data() + 40) == 0x0483 && u16(info.data() + 42) == 0x3748,
               "ST-LINK device enumeration mismatch");
        Bytes open(12);
        open[0] = 3;
        assign32(open, 4, u32(info.data() + 4));
        auto result = request(open, 8, true);
        verify(u32(result.data()) == 1, "ST-LINK open failed");
        handle = u32(result.data() + 4);
    }
    Bytes packet(Bytes command, size_t readLength, std::span<const uint8_t> input = {})
    {
        Bytes bytes(32);
        bytes[0] = 5;
        assign32(bytes, 4, handle);
        std::copy(command.begin(), command.end(), bytes.begin() + 8);
        bytes[24] = input.empty() ? 1 : 0;
        assign32(bytes, 28, uint32_t(input.empty() ? readLength : input.size()));
        bytes.insert(bytes.end(), input.begin(), input.end());
        return bytes;
    }
    Bytes usb(Bytes command, size_t readLength, std::span<const uint8_t> input = {})
    {
        auto response = request(packet(command, readLength, input), 4 + readLength, true);
        verify(u32(response.data()) == 1, "ST-LINK transport status mismatch");
        return Bytes(response.begin() + 4, response.end());
    }
    void finishSending()
    {
        shutdown(socket, SD_SEND);
    }
};
Bytes memoryCommand(uint8_t opcode, uint32_t address, uint16_t length)
{
    Bytes command{0xf2, opcode};
    put32(command, address);
    put16(command, length);
    command.resize(16);
    return command;
}
} // namespace

void stlink_self_test(Log log)
{
    auto probe = std::make_shared<MemoryProbe>();
    StlinkBridge bridge(probe, [](const std::wstring &) {}, 0);
    {
        Client client(bridge.port());
        client.open();
        auto version = client.usb({0xf1}, 6);
        verify(version.size() == 6 && version[0] >> 4 == 2, "ST-LINK firmware version framing failed");
        auto voltage = client.usb({0xf7}, 8);
        verify(u32(voltage.data()) == 2400 && u32(voltage.data() + 4) == 3300,
               "ST-LINK VREF encoding failed");
        verify(client.usb({0xf2, 0x30, 0xa3}, 2)[0] == 0x80, "ST-LINK SWD initialization failed");
        Bytes mixed{5, 1, 2, 3};
        put32(mixed, 0);
        put32(mixed, 0x20000000);
        put32(mixed, 0x12345678);
        put32(mixed, 0x20000000);
        client.usb({0xf2, 0x51, 4, 0, 0, 0}, 0, mixed);
        {
            Client second(bridge.port());
            second.open();
            verify(second.usb({0xf2, 0x52}, 32)[0] == 0x81, "Mixed result leaked between clients");
        }
        auto result = client.usb({0xf2, 0x52}, 32);
        verify(u32(result.data() + 12) == 0x12345678, "Mixed read/write data mismatch");
        for (unsigned i = 0; i < 4; ++i)
            verify(u32(result.data() + 16 + 4 * i) == 0x80, "Mixed transfer status mismatch");
        unsigned before = probe->writes;
        mixed[3] = 0xff;
        client.usb({0xf2, 0x51, 4, 0, 0, 0}, 0, mixed);
        result = client.usb({0xf2, 0x52}, 32);
        verify(probe->writes == before && u32(result.data() + 16) == 0x81,
               "Malformed mixed transfer changed target memory");
        const uint32_t addresses[]{0x200003ff, 0x200007fe, 0x20000ffc};
        const uint8_t writeCommands[]{0x0d, 0x48, 0x08};
        const uint8_t readCommands[]{0x0c, 0x47, 0x07};
        for (unsigned index = 0; index < 3; ++index)
        {
            Bytes expected(128);
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = uint8_t(i * 37 + index * 11);
            client.usb(memoryCommand(writeCommands[index], addresses[index], uint16_t(expected.size())), 0,
                       expected);
            auto status = client.usb({0xf2, 0x3e}, 12);
            verify(status[0] == 0x80, "ST-LINK write status failed");
            auto actual =
                client.usb(memoryCommand(readCommands[index], addresses[index], uint16_t(expected.size())),
                           expected.size());
            verify(actual == expected, "ST-LINK byte/halfword/word boundary transfer mismatch");
        }
        verify(client.usb({0xf2, 0x3c, 0}, 2)[0] != 0x80,
               "Unwired hardware reset incorrectly reported success");
    }
    log(L"ST-LINK API 3 fragmented framing, VREF and three memory widths passed.");
    unsigned previousWrites = probe->writes;
    {
        Client partial(bridge.port());
        partial.open();
        Bytes values{1, 2, 3, 4};
        auto packet = partial.packet(memoryCommand(0x08, 0x20002000, 4), 0, values);
        packet.resize(packet.size() - 2);
        partial.sendFrame(packet, true);
        partial.finishSending();
        bool closed = false;
        try
        {
            partial.read(1);
        }
        catch (...)
        {
            closed = true;
        }
        verify(closed, "Truncated ST-LINK write was not closed");
    }
    verify(probe->writes == previousWrites, "Truncated ST-LINK write changed target data");
    {
        Client malformed(bridge.port());
        malformed.open();
        auto packet = malformed.packet({0xf2, 0x08}, 0);
        assign32(packet, 28, 65536);
        malformed.sendFrame(packet, true);
        bool closed = false;
        try
        {
            malformed.read(1);
        }
        catch (...)
        {
            closed = true;
        }
        verify(closed, "Oversized ST-LINK payload was accepted");
    }
    {
        Client recovered(bridge.port());
        recovered.open();
        verify(recovered.usb({0xf5}, 2)[0] == 2, "ST-LINK service did not recover after bad clients");
    }
    log(L"Truncated and oversized ST-LINK requests leave target data intact; service remains usable.");
}

void stlink_engine_self_test(const fs::path &assets, Log log)
{
    auto probe = std::make_shared<MemoryProbe>();
    StlinkBridge bridge(probe, [](const std::wstring &) {}, 0);
    auto folder = assets / L"stlink-protocol-test" / std::to_wstring(GetCurrentProcessId());
    fs::create_directories(folder);
    std::ostringstream configuration;
    configuration << "adapter driver st-link\nadapter usb vid_pid 0x0483 0x3748\nst-link backend tcp "
                  << bridge.port()
                  << "\ntransport select swd\nadapter speed 100\n"
                     "gdb port disabled\ntcl port disabled\ntelnet port disabled\n"
                     "init\necho WDP_STLINK_ENGINE_OK\nshutdown\n";
    auto script = configuration.str();
    write_file(folder / L"stlink.cfg", {(const uint8_t *)script.data(), script.size()});
    bool marker = false;
    int result = run_process(
        assets / L"openocd.exe", L"-s \"" + (assets / L"scripts").wstring() + L"\" -f stlink.cfg", folder,
        [&](const std::wstring &line) {
            if (line.find(L"WDP_STLINK_ENGINE_OK") != line.npos)
                marker = true;
            log(line);
        },
        30);
    verify(result == 0 && marker && bridge.request_count() >= 5,
           "Embedded OpenOCD ST-LINK TCP integration failed");
}
