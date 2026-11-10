#include "stlink_bridge.h"
#include <winrt/base.h>
#include <array>

namespace
{
constexpr char probeSerial[] = "57445354E83DC1D250420001";
constexpr uint32_t deviceId = 0x57440001;
constexpr uint32_t connectionId = 0x100;
constexpr uint8_t statusOk = 0x80;
constexpr uint8_t statusFault = 0x81;

struct Transfer
{
    uint8_t reg;
    uint32_t value = 0;
};

class SwdAccess
{
    std::shared_ptr<Link> link;
    std::atomic<bool> &stopping;

    Bytes command(Bytes request)
    {
        if (stopping)
            throw std::runtime_error("ST-LINK bridge stopped");
        request.resize(64);
        return link->request(2, request);
    }
    void okCommand(Bytes request)
    {
        auto response = command(request);
        if (response.size() < 2 || response[0] != request[0] || response[1] != 0)
            throw std::runtime_error("CMSIS-DAP setup failed");
    }

  public:
    explicit SwdAccess(std::shared_ptr<Link> connection, std::atomic<bool> &stop)
        : link(std::move(connection)), stopping(stop)
    {
    }

    Bytes transfers(std::initializer_list<Transfer> operations)
    {
        Bytes packet{5, 0, uint8_t(operations.size())};
        size_t reads = 0;
        for (auto operation : operations)
        {
            packet.push_back(operation.reg);
            if (operation.reg & 2)
                ++reads;
            else
                put32(packet, operation.value);
        }
        if (packet.size() > 64)
            throw std::runtime_error("DAP transfer packet exceeds capacity");
        auto response = command(packet);
        if (response.size() < 3 + reads * 4 || response[0] != 5 || response[1] != operations.size() ||
            response[2] != 1)
            throw std::runtime_error("Target SWD transfer incomplete count=" +
                                     std::to_string(response.size() > 1 ? response[1] : 0) +
                                     " ack=" + std::to_string(response.size() > 2 ? response[2] : 0));
        return Bytes(response.begin() + 3, response.begin() + 3 + reads * 4);
    }
    uint32_t initialize()
    {
        auto connected = command({2, 1});
        if (connected.size() < 2 || connected[1] != 1)
            throw std::runtime_error("Target SWD connection rejected");
        okCommand({4, 0, 80, 0, 0, 0});
        okCommand({0x11, 0xa0, 0x86, 1, 0});
        okCommand({0x13, 0});
        Bytes sequence{0x12, 136};
        sequence.insert(sequence.end(), 7, 0xff);
        sequence.insert(sequence.end(), {0x9e, 0xe7});
        sequence.insert(sequence.end(), 7, 0xff);
        sequence.push_back(0);
        okCommand(sequence);
        auto identity = transfers({{2}});
        auto state = transfers({{0, 0x1e}, {8, 0}, {4, 0x50000000}, {6}});
        for (unsigned attempt = 0; (u32(state.data()) & 0xa0000000) != 0xa0000000; ++attempt)
        {
            if (attempt == 20)
                throw std::runtime_error("Target debug power did not acknowledge");
            Sleep(1);
            state = transfers({{6}});
        }
        return u32(identity.data());
    }
    uint32_t dapRegister(uint16_t port, uint16_t address, bool write, uint32_t value = 0)
    {
        if ((address & 3) || (port != 0xffff && port > 255))
            throw std::runtime_error("Invalid DAP register");
        uint8_t reg = uint8_t(address & 12) | (write ? 0 : 2);
        Bytes result;
        if (port == 0xffff)
            result = transfers({{reg, value}});
        else
            result = transfers({{8, uint32_t(port) << 24 | (address & 0xf0)}, {uint8_t(reg | 1), value}});
        return write ? 0 : u32(result.data());
    }
    Bytes memory(uint8_t ap, uint32_t address, size_t length, unsigned width,
                 std::span<const uint8_t> input = {}, bool noIncrement = false, uint32_t csw = 0)
    {
        bool write = !input.empty();
        if (!length || length > 65535 || (width != 1 && width != 2 && width != 4) || length % width ||
            address % width || (write && input.size() != length) ||
            (!noIncrement && uint64_t(address) + length > 0x100000000ULL))
            throw std::runtime_error("Invalid ST-LINK memory transfer");
        Bytes output;
        output.reserve(length);
        unsigned sizeBits = width == 4 ? 2 : width == 2 ? 1 : 0;
        uint32_t control = (csw ? csw : 0x23000040) | sizeBits | (noIncrement ? 0 : 0x10);
        for (size_t offset = 0; offset < length;)
        {
            uint32_t current = noIncrement ? address : address + uint32_t(offset);
            size_t count = std::min<size_t>((length - offset) / width, write ? 14 : 15);
            if (!noIncrement)
                count = std::min<size_t>(count, (1024 - (current & 1023)) / width);
            if (offset == 0 || (!noIncrement && (current & 1023) == 0))
                transfers({{8, uint32_t(ap) << 24}, {1, control}, {5, current}});
            Bytes packet{6, 0, uint8_t(count), 0, uint8_t(write ? 13 : 15)};
            if (write)
                for (size_t index = 0; index < count; ++index)
                {
                    uint32_t value = 0;
                    uint32_t at = noIncrement ? current : current + uint32_t(index * width);
                    for (unsigned byte = 0; byte < width; ++byte)
                        value |= uint32_t(input[offset + index * width + byte]) << (8 * byte);
                    put32(packet, value << ((at & 3) * 8));
                }
            auto reply = command(packet);
            if (reply.size() < 4 + (write ? 0 : count * 4) || reply[0] != 6 ||
                u16(reply.data() + 1) != count || reply[3] != 1)
                throw std::runtime_error("Target memory transfer incomplete");
            if (!write)
                for (size_t index = 0; index < count; ++index)
                {
                    uint32_t at = noIncrement ? current : current + uint32_t(index * width);
                    uint32_t value = u32(reply.data() + 4 + index * 4) >> ((at & 3) * 8);
                    for (unsigned byte = 0; byte < width; ++byte)
                        output.push_back(uint8_t(value >> (8 * byte)));
                }
            offset += count * width;
        }
        return output;
    }
    uint32_t read32(uint32_t address, uint8_t ap = 0)
    {
        auto data = transfers({{8, uint32_t(ap) << 24}, {1, 0x23000052}, {5, address}, {15}});
        return u32(data.data());
    }
    void write32(uint32_t address, uint32_t value, uint8_t ap = 0)
    {
        transfers({{8, uint32_t(ap) << 24}, {1, 0x23000052}, {5, address}, {13, value}});
    }
    uint32_t coreRegister(uint8_t reg, bool write, uint32_t value = 0)
    {
        uint32_t selector = reg & 0x7f;
        Bytes data;
        if (write)
            data = transfers({{8, 0},
                              {1, 0x23000052},
                              {5, 0xe000edf8},
                              {13, value},
                              {5, 0xe000edf4},
                              {13, selector | 0x10000},
                              {5, 0xe000edf0},
                              {15}});
        else
            data = transfers({{8, 0},
                              {1, 0x23000052},
                              {5, 0xe000edf4},
                              {13, selector},
                              {5, 0xe000edf0},
                              {15},
                              {5, 0xe000edf8},
                              {15}});
        if (!(u32(data.data()) & 0x10000))
            throw std::runtime_error("Core register transfer not ready");
        return write ? 0 : u32(data.data() + 4);
    }
    void clearFault() noexcept
    {
        try
        {
            transfers({{0, 0x1e}});
        }
        catch (...)
        {
        }
    }
};

void putAt(Bytes &bytes, size_t offset, uint32_t value)
{
    if (offset + 4 > bytes.size())
        throw std::runtime_error("Invalid ST-LINK response capacity");
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = uint8_t(value >> (i * 8));
}
} // namespace

struct StlinkBridge::Impl
{
    struct Session
    {
        SOCKET socket;
        std::thread thread;
        std::atomic<bool> done{false};
        bool opened = false;
        Bytes miscReply;
        explicit Session(SOCKET value) : socket(value)
        {
        }
    };
    std::shared_ptr<Link> link;
    Log log;
    bool trace;
    SOCKET listener = INVALID_SOCKET;
    uint16_t boundPort = 0;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> commands{0};
    std::thread acceptThread;
    std::vector<std::unique_ptr<Session>> sessions;
    std::mutex targetMutex;
    SwdAccess swd;
    uint8_t mode = 1, lastStatus = statusOk;
    uint32_t dpId = 0, lastAddress = 0;

    Impl(std::shared_ptr<Link> connection, Log logger, uint16_t port, bool diagnostics)
        : link(std::move(connection)), log(std::move(logger)), trace(diagnostics), swd(link, stopping)
    {
        sessions.reserve(8);
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
            throw std::runtime_error("Cannot create ST-LINK server socket");
        BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&exclusive, sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (bind(listener, (sockaddr *)&address, sizeof(address)) || listen(listener, 8))
        {
            closesocket(listener);
            listener = INVALID_SOCKET;
            throw std::runtime_error("ST-LINK bridge port is occupied");
        }
        int length = sizeof(address);
        getsockname(listener, (sockaddr *)&address, &length);
        boundPort = ntohs(address.sin_port);
        try
        {
            acceptThread = std::thread([this] { acceptClients(); });
        }
        catch (...)
        {
            closesocket(listener);
            throw;
        }
    }
    ~Impl()
    {
        stopping = true;
        if (acceptThread.joinable())
            acceptThread.join();
        for (auto &session : sessions)
            shutdown(session->socket, SD_BOTH);
        for (auto &session : sessions)
        {
            if (session->thread.joinable())
                session->thread.join();
            closesocket(session->socket);
        }
        if (listener != INVALID_SOCKET)
            closesocket(listener);
    }
    void receive(SOCKET socket, uint8_t *data, size_t length, bool idle = false)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (length && !stopping)
        {
            int count = recv(socket, (char *)data, int(length), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
            {
                if (idle || std::chrono::steady_clock::now() < deadline)
                    continue;
            }
            if (count <= 0)
                throw std::runtime_error("ST-LINK client disconnected or timed out");
            data += count;
            length -= count;
        }
        if (length)
            throw std::runtime_error("ST-LINK bridge stopped");
    }
    void sendBytes(SOCKET socket, std::span<const uint8_t> bytes)
    {
        while (!bytes.empty() && !stopping)
        {
            int sent = send(socket, (const char *)bytes.data(), int(bytes.size()), 0);
            if (sent <= 0)
                throw std::runtime_error("ST-LINK client response failed");
            bytes = bytes.subspan(sent);
        }
    }
    void ensureSwd()
    {
        if (mode != 2)
        {
            dpId = swd.initialize();
            mode = 2;
        }
    }
    Bytes usbCommand(Session &session, std::span<const uint8_t> command, std::span<const uint8_t> payload,
                     size_t readLength)
    {
        std::lock_guard lock(targetMutex);
        auto ordinal = ++commands;
        if (trace)
        {
            std::wostringstream line;
            line << L"ST-LINK #" << ordinal << L" command";
            for (auto byte : command)
                line << L" " << std::hex << std::setw(2) << std::setfill(L'0') << unsigned(byte);
            line << std::dec << L" rx=" << readLength << L" tx=" << payload.size();
            if (command[0] == 0xf2 && payload.size() == 4)
            {
                uint32_t address = u32(command.data() + 2);
                if (address >= 0x52002000 && address < 0x52002200)
                    line << L" flash-register-value=" << hex32(u32(payload.data()));
            }
            log(line.str());
        }
        Bytes answer(readLength);
        uint8_t operation = command[0], subcommand = command[1];
        auto status = [&](uint8_t value = statusOk) {
            if (!answer.empty())
                answer[0] = value;
        };
        try
        {
            if (operation == 0xf1)
            {
                const uint8_t version[]{0x29, 0x47, 0x83, 0x04, 0x48, 0x37};
                std::copy_n(version, std::min(answer.size(), sizeof(version)), answer.begin());
                return answer;
            }
            if (operation == 0xfb)
            {
                const uint8_t version[]{2, 7, 37, 0, 0, 0, 0x83, 0x04, 0x48, 0x37, 0, 0};
                std::copy_n(version, std::min(answer.size(), sizeof(version)), answer.begin());
                return answer;
            }
            if (operation == 0xf5)
            {
                if (!answer.empty())
                    answer[0] = mode;
                return answer;
            }
            if (operation == 0xf7)
            {
                auto bytes = link->request(3);
                int voltage = json_int(std::string(bytes.begin(), bytes.end()), "vref_mv");
                if (voltage < 0)
                    throw std::runtime_error("Target voltage measurement failed");
                putAt(answer, 0, 2400);
                putAt(answer, 4, uint32_t(voltage));
                return answer;
            }
            if (operation == 0xf3 && subcommand == 7)
            {
                mode = 1;
                return answer;
            }
            if (operation != 0xf2)
                throw std::runtime_error("Unsupported ST-LINK device command");
            if (subcommand == 0x56)
            {
                status();
                constexpr char boardName[] = "WIRELESSDAP";
                if (answer.size() >= 24 + sizeof(boardName))
                    std::copy(std::begin(boardName), std::end(boardName), answer.begin() + 24);
                return answer;
            }
            if (subcommand == 0x21)
            {
                mode = 1;
                status();
                return answer;
            }
            if (subcommand == 0x3b || subcommand == 0x3e)
            {
                status(lastStatus);
                if (answer.size() >= 8)
                    putAt(answer, 4, lastAddress);
                return answer;
            }
            if (subcommand == 0x43)
            {
                // The physical board is qualified at 100 kHz.
                Bytes clock{0x11};
                put32(clock, 100000);
                clock.resize(64);
                auto response = link->request(2, clock);
                if (response.size() < 2 || response[1])
                    throw std::runtime_error("SWD clock configuration failed");
                status();
                return answer;
            }
            ensureSwd();
            lastStatus = statusOk;
            status();
            switch (subcommand)
            {
            case 0x51: {
                uint32_t count = u32(command.data() + 2);
                session.miscReply.clear();
                if (!count || count > 64)
                    throw std::runtime_error("Invalid ST-LINK mixed-transfer count");
                session.miscReply.resize(count * 8);
                for (uint32_t i = 0; i < count; ++i)
                    putAt(session.miscReply, 4 * (count + i), statusFault);
                size_t values = (count + 3) & ~size_t(3);
                if (payload.size() != values + 4 * count)
                    throw std::runtime_error("Invalid ST-LINK mixed-transfer payload");
                bool addressSet = false, apSet = false;
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t value = u32(payload.data() + values + 4 * i);
                    switch (payload[i])
                    {
                    case 5:
                        if (value > 255)
                            throw std::runtime_error("Invalid mixed-transfer AP");
                        apSet = true;
                        break;
                    case 1:
                    case 3:
                        if (!apSet || (value & 3))
                            throw std::runtime_error("Invalid mixed-transfer address");
                        addressSet = true;
                        break;
                    case 2:
                        if (!apSet || !addressSet)
                            throw std::runtime_error("Mixed write has no AP or address");
                        break;
                    default:
                        throw std::runtime_error("Unsupported mixed-transfer item");
                    }
                }
                uint8_t ap = 0;
                uint32_t address = 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t value = u32(payload.data() + values + 4 * i);
                    switch (payload[i])
                    {
                    case 5:
                        ap = uint8_t(value);
                        break;
                    case 1:
                        address = value;
                        break;
                    case 2:
                        swd.write32(address, value, ap);
                        break;
                    case 3:
                        address = value;
                        putAt(session.miscReply, 4 * i, swd.read32(address, ap));
                        break;
                    }
                    putAt(session.miscReply, 4 * (count + i), statusOk);
                }
                break;
            }
            case 0x52:
                if (session.miscReply.empty() || readLength != session.miscReply.size())
                    throw std::runtime_error("No matching ST-LINK mixed-transfer result");
                answer = std::move(session.miscReply);
                session.miscReply.clear();
                break;
            case 0x20:
            case 0x30:
                if (command[2] != 0xa3)
                    throw std::runtime_error("Only SWD without hardware reset is available");
                break;
            case 0x22:
                putAt(answer, 0, dpId);
                break;
            case 0x31:
                putAt(answer, 4, dpId);
                if (answer.size() >= 12)
                    putAt(answer, 8, swd.dapRegister(0, 0xfc, false));
                break;
            case 0x36:
                putAt(answer, 4, swd.read32(u32(command.data() + 2)));
                break;
            case 0x0f:
            case 0x35:
                swd.write32(u32(command.data() + 2), u32(command.data() + 6));
                break;
            case 0x45:
                putAt(answer, 4, swd.dapRegister(u16(command.data() + 2), u16(command.data() + 4), false));
                break;
            case 0x46:
                swd.dapRegister(u16(command.data() + 2), u16(command.data() + 4), true,
                                u32(command.data() + 6));
                break;
            case 0x4b:
                if (!swd.dapRegister(command[2], 0xfc, false))
                    throw std::runtime_error("Requested access port is absent");
                break;
            case 0x4c:
                break;
            case 0x33:
                putAt(answer, 4, swd.coreRegister(command[2], false));
                break;
            case 0x34:
                swd.coreRegister(command[2], true, u32(command.data() + 3));
                break;
            case 0x3a:
                for (unsigned reg = 0; reg < 21; ++reg)
                    putAt(answer, 4 + reg * 4, reg == 19 ? 0 : swd.coreRegister(uint8_t(reg), false));
                break;
            case 0x02:
                swd.write32(0xe000edf0, 0xa05f0003);
                break;
            case 0x09:
                swd.write32(0xe000edf0, 0xa05f0001);
                break;
            case 0x0a:
                swd.write32(0xe000edf0, 0xa05f0005);
                break;
            case 0x32:
            case 0x03:
                swd.write32(0xe000ed0c, 0x05fa0004);
                break;
            case 0x3c:
                if (command[2] != 1)
                    throw std::runtime_error("NRST is not routed; select software reset / hotplug");
                break;
            case 0x07:
            case 0x08:
            case 0x0c:
            case 0x0d:
            case 0x47:
            case 0x48:
            case 0x50:
            case 0x54: {
                bool write =
                    subcommand == 0x08 || subcommand == 0x0d || subcommand == 0x48 || subcommand == 0x50;
                unsigned width = (subcommand == 0x0c || subcommand == 0x0d)   ? 1
                                 : (subcommand == 0x47 || subcommand == 0x48) ? 2
                                                                              : 4;
                size_t length = u16(command.data() + 6);
                uint32_t address = u32(command.data() + 2);
                uint32_t csw =
                    uint32_t(command[9]) << 8 | uint32_t(command[10]) << 16 | uint32_t(command[11]) << 24;
                lastAddress = address;
                if ((write && payload.size() != length) || (!write && length > answer.size()))
                    throw std::runtime_error("ST-LINK memory payload length mismatch");
                auto bytes = swd.memory(command[8], address, length, width,
                                        write ? payload : std::span<const uint8_t>{},
                                        subcommand == 0x50 || subcommand == 0x54, csw);
                if (!write)
                    std::copy(bytes.begin(), bytes.end(), answer.begin());
                break;
            }
            default:
                throw std::runtime_error("Unsupported ST-LINK debug command");
            }
        }
        catch (const std::exception &error)
        {
            lastStatus = statusFault;
            status(statusFault);
            if (mode == 2)
                swd.clearFault();
            std::wostringstream text;
            text << L"ST-LINK 命令 " << std::hex << unsigned(operation) << L"/" << unsigned(subcommand)
                 << L"  " << wide(error.what());
            log(text.str());
        }
        return answer;
    }
    void serve(Session &session)
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try
        {
            while (!stopping)
            {
                std::array<uint8_t, 32> header{};
                receive(session.socket, header.data(), 1, true);
                if (trace)
                    log(L"ST-LINK SERVER cmd=" + std::to_wstring(header[0]));
                Bytes response;
                switch (header[0])
                {
                case 0:
                    receive(session.socket, header.data() + 1, 1);
                    put32(response, 1);
                    break;
                case 1:
                    put32(response, 1);
                    break;
                case 2: {
                    receive(session.socket, header.data() + 1, 7);
                    uint32_t size = u32(header.data() + 4);
                    if (trace)
                        log(L"ST-LINK DEVICE_INFO size=" + std::to_wstring(size));
                    if (header[1] != 0 || size < 41 || size > 4096)
                        throw std::runtime_error("Invalid ST-LINK device-info request");
                    response.resize(4 + size);
                    putAt(response, 0, 1);
                    putAt(response, 4, deviceId);
                    std::copy(std::begin(probeSerial), std::end(probeSerial), response.begin() + 8);
                    response[40] = 0x83;
                    response[41] = 0x04;
                    response[42] = 0x48;
                    response[43] = 0x37;
                    break;
                }
                case 3:
                    receive(session.socket, header.data() + 1, 11);
                    if (u32(header.data() + 4) != deviceId)
                        throw std::runtime_error("Unknown ST-LINK device ID");
                    session.opened = true;
                    put32(response, 1);
                    put32(response, connectionId);
                    break;
                case 4:
                    receive(session.socket, header.data() + 1, 7);
                    session.opened = false;
                    put32(response, 1);
                    break;
                case 5: {
                    receive(session.socket, header.data() + 1, 31);
                    uint32_t length = u32(header.data() + 28);
                    uint8_t direction = header[24];
                    if (!session.opened || u32(header.data() + 4) != connectionId || length > 65535 ||
                        (direction != 0 && direction != 1))
                        throw std::runtime_error("Invalid ST-LINK USB transaction");
                    Bytes payload(direction == 0 ? length : 0);
                    if (!payload.empty())
                        receive(session.socket, payload.data(), payload.size());
                    auto started = std::chrono::steady_clock::now();
                    auto body = usbCommand(session, std::span(header).subspan(8, 16), payload,
                                           direction == 1 ? length : 0);
                    if (trace)
                    {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count();
                        std::wostringstream result;
                        result << L"ST-LINK RESPONSE ms=" << elapsed << L" bytes=";
                        for (size_t i = 0; i < std::min<size_t>(12, body.size()); ++i)
                            result << L" " << std::hex << std::setw(2) << std::setfill(L'0')
                                   << unsigned(body[i]);
                        log(result.str());
                    }
                    put32(response, 1);
                    response.insert(response.end(), body.begin(), body.end());
                    break;
                }
                case 6:
                    receive(session.socket, header.data() + 1, 3);
                    put32(response, 3);
                    put32(response, 2);
                    put32(response, 1);
                    put32(response, 0);
                    break;
                case 7:
                    receive(session.socket, header.data() + 1, 7);
                    put32(response, session.opened ? 1 : 0);
                    break;
                default:
                    log(L"ST-LINK 未识别的服务器命令 " + std::to_wstring(header[0]));
                    throw std::runtime_error("Unsupported ST-LINK server command");
                }
                sendBytes(session.socket, response);
            }
        }
        catch (const std::exception &error)
        {
            if (!stopping && std::string(error.what()).find("disconnected") == std::string::npos)
                log(L"ST-LINK " + wide(error.what()));
        }
        catch (const winrt::hresult_error &error)
        {
            if (!stopping)
                log(L"ST-LINK " + std::wstring(error.message()));
        }
        shutdown(session.socket, SD_BOTH);
        session.done = true;
        winrt::uninit_apartment();
    }
    void acceptClients()
    {
        while (!stopping)
        {
            for (auto i = sessions.begin(); i != sessions.end();)
            {
                if ((*i)->done)
                {
                    (*i)->thread.join();
                    closesocket((*i)->socket);
                    i = sessions.erase(i);
                }
                else
                    ++i;
            }
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener, &ready);
            timeval timeout{0, 100000};
            if (select(0, &ready, nullptr, nullptr, &timeout) <= 0)
                continue;
            SOCKET accepted = accept(listener, nullptr, nullptr);
            if (accepted == INVALID_SOCKET)
                continue;
            if (sessions.size() >= 8)
            {
                closesocket(accepted);
                continue;
            }
            DWORD timeoutMs = 1000;
            BOOL noDelay = TRUE;
            setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeoutMs, sizeof(timeoutMs));
            setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeoutMs, sizeof(timeoutMs));
            setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, (char *)&noDelay, sizeof(noDelay));
            auto session = std::make_unique<Session>(accepted);
            auto ptr = session.get();
            try
            {
                session->thread = std::thread([this, ptr] { serve(*ptr); });
                sessions.push_back(std::move(session));
            }
            catch (...)
            {
                closesocket(accepted);
                log(L"ST-LINK 无法创建客户端工作线程");
            }
        }
    }
};

StlinkBridge::StlinkBridge(std::shared_ptr<Link> link, Log log, uint16_t port, bool trace)
    : impl(std::make_unique<Impl>(std::move(link), std::move(log), port, trace))
{
}
StlinkBridge::~StlinkBridge() = default;
uint16_t StlinkBridge::port() const
{
    return impl->boundPort;
}
uint64_t StlinkBridge::request_count() const
{
    return impl->commands;
}

uint32_t StlinkBridge::read_debug_register(uint32_t address)
{
    std::lock_guard lock(impl->targetMutex);
    impl->ensureSwd();
    return impl->swd.read32(address);
}