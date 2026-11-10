#include "stlink_bridge.h"
#include "core.h"
#include <map>
const std::vector<Profile> profiles = {
    {L"自动识别常见 STM32", ""},      {L"STM32F0", "stm32f0x.cfg"},       {L"STM32F1", "stm32f1x.cfg"},
    {L"STM32F2", "stm32f2x.cfg"},     {L"STM32F3", "stm32f3x.cfg"},       {L"STM32F4", "stm32f4x.cfg"},
    {L"STM32F7", "stm32f7x.cfg"},     {L"STM32G0", "stm32g0x.cfg"},       {L"STM32G4", "stm32g4x.cfg"},
    {L"STM32H7", "stm32h7x.cfg"},     {L"STM32H7RS", "stm32h7rsx.cfg"},   {L"STM32L0", "stm32l0.cfg"},
    {L"STM32L1", "stm32l1.cfg"},      {L"STM32L4 / L4+", "stm32l4x.cfg"}, {L"STM32L5", "stm32l5x.cfg"},
    {L"STM32U5", "stm32u5x.cfg"},     {L"STM32C0", "stm32c0x.cfg"},       {L"STM32U0", "stm32u0x.cfg"},
    {L"STM32U3", "stm32u3x.cfg"},     {L"STM32WB", "stm32wbx.cfg"},       {L"STM32WL", "stm32wlx.cfg"},
    {L"STM32WBA5", "stm32wba5x.cfg"}, {L"STM32WBA6", "stm32wba6x.cfg"}};
void validate_segments(std::vector<Segment> &segs)
{
    if (segs.empty())
        throw std::runtime_error("Firmware contains no data");
    std::sort(segs.begin(), segs.end(), [](auto &a, auto &b) { return a.address < b.address; });
    uint64_t end = 0, total = 0;
    for (auto &s : segs)
    {
        uint64_t stop = uint64_t(s.address) + s.bytes.size();
        if (s.bytes.empty() || s.address < end || stop > 0x100000000ULL)
            throw std::runtime_error("Firmware ranges overlap or overflow");
        bool internal = (s.address >= 0x08000000 && stop <= 0x0A000000) ||
                        (s.address >= 0x0C000000 && stop <= 0x0E000000);
        if (!internal)
            throw std::runtime_error(
                "Only STM32 internal Flash images are accepted; RAM/external/option-byte address rejected");
        total += s.bytes.size();
        end = stop;
    }
    if (total > 32 * 1024 * 1024)
        throw std::runtime_error("Firmware exceeds 32 MiB");
}
std::vector<Segment> parse_image(const fs::path &p, uint32_t base)
{
    auto data = read_file(p);
    if (data.empty() || data.size() > 64 * 1024 * 1024)
        throw std::runtime_error("Empty or oversized firmware file");
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
    std::vector<Segment> segs;
    if (ext == L".bin")
        segs.push_back({base, std::move(data)});
    else if (ext == L".hex" || ext == L".ihex")
    {
        std::istringstream text(std::string(data.begin(), data.end()));
        std::string line;
        uint32_t upper = 0;
        bool ended = false;
        int line_no = 0;
        auto nib = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::runtime_error("Invalid HEX digit");
        };
        while (std::getline(text, line))
        {
            line_no++;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (ended)
                throw std::runtime_error("Data after HEX EOF");
            if (line[0] != ':' || (line.size() - 1) % 2)
                throw std::runtime_error("Malformed HEX record");
            Bytes b;
            for (size_t i = 1; i < line.size(); i += 2)
                b.push_back((uint8_t)((nib(line[i]) << 4) | nib(line[i + 1])));
            if (b.size() < 5 || b.size() != size_t(b[0]) + 5)
                throw std::runtime_error("HEX record length mismatch");
            unsigned sum = 0;
            for (auto c : b)
                sum += c;
            if (sum & 255)
                throw std::runtime_error("HEX checksum mismatch at line " + std::to_string(line_no));
            uint32_t addr = (uint32_t(b[1]) << 8) | b[2];
            switch (b[3])
            {
            case 0:
                if (b[0])
                {
                    uint64_t absolute = uint64_t(upper) + addr;
                    if (absolute > UINT32_MAX)
                        throw std::runtime_error("HEX address overflow");
                    segs.push_back({uint32_t(absolute), Bytes(b.begin() + 4, b.end() - 1)});
                }
                break;
            case 1:
                if (b[0] || addr)
                    throw std::runtime_error("Invalid HEX EOF");
                ended = true;
                break;
            case 2:
            case 4:
                if (b[0] != 2 || addr)
                    throw std::runtime_error("Invalid HEX address record");
                upper = ((uint32_t(b[4]) << 8) | b[5]) << (b[3] == 4 ? 16 : 4);
                break;
            case 3:
            case 5:
                if (b[0] != 4)
                    throw std::runtime_error("Invalid HEX entry record");
                break;
            default:
                throw std::runtime_error("Unsupported HEX record");
            }
        }
        if (!ended)
            throw std::runtime_error("HEX EOF record missing");
    }
    else if (ext == L".elf" || ext == L".axf")
    {
        if (data.size() < 52 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' ||
            data[4] != 1 || data[5] != 1 || u16(data.data() + 18) != 40)
            throw std::runtime_error("Expected little-endian ARM ELF32 firmware");
        uint32_t off = u32(data.data() + 28);
        uint16_t size = u16(data.data() + 42), count = u16(data.data() + 44);
        if (size < 32 || uint64_t(off) + uint64_t(size) * count > data.size())
            throw std::runtime_error("ELF program headers invalid");
        for (unsigned i = 0; i < count; i++)
        {
            auto h = data.data() + off + size * i;
            if (u32(h) != 1)
                continue;
            uint32_t fileoff = u32(h + 4), addr = u32(h + 12), len = u32(h + 16);
            if (!len)
                continue;
            if (uint64_t(fileoff) + len > data.size())
                throw std::runtime_error("Truncated ELF segment");
            segs.push_back({addr, Bytes(data.begin() + fileoff, data.begin() + fileoff + len)});
        }
    }
    else
        throw std::runtime_error("Choose BIN, HEX, ELF or AXF firmware");
    validate_segments(segs);
    std::vector<Segment> merged;
    for (auto &s : segs)
    {
        if (!merged.empty() && uint64_t(merged.back().address) + merged.back().bytes.size() == s.address)
            merged.back().bytes.insert(merged.back().bytes.end(), s.bytes.begin(), s.bytes.end());
        else
            merged.push_back(std::move(s));
    }
    return merged;
}
static Bytes dap(Link &l, Bytes p)
{
    p.resize(64, 0);
    return l.request(2, p);
}
static uint32_t transfer(Link &l, uint8_t reg, bool read, uint32_t value = 0)
{
    Bytes p{5, 0, 1, uint8_t(reg | (read ? 2 : 0))};
    if (!read)
        put32(p, value);
    auto r = dap(l, p);
    if (r.size() < 3 || r[0] != 5 || r[1] != 1 || r[2] != 1 || (read && r.size() < 7))
        throw std::runtime_error("SWD transfer failed: check target power, wiring or protection");
    return read ? u32(r.data() + 3) : 0;
}
Detected detect_stm32(Link &l)
{
    auto c = dap(l, {2, 1});
    if (c.size() < 2 || c[1] != 1)
        throw std::runtime_error("Cannot select SWD");
    dap(l, {4, 0, 64, 0, 0, 0});
    dap(l, {0x11, 0xa0, 0x86, 1, 0});
    dap(l, {0x13, 0});
    dap(l, {0x12, 56, 255, 255, 255, 255, 255, 255, 255});
    dap(l, {0x12, 16, 0x9e, 0xe7});
    dap(l, {0x12, 56, 255, 255, 255, 255, 255, 255, 255});
    dap(l, {0x12, 8, 0});
    Detected result;
    result.dp = transfer(l, 0, true);
    transfer(l, 0, false, 0x1e);
    transfer(l, 8, false, 0);
    transfer(l, 4, false, 0x50000000);
    bool ready = false;
    for (int i = 0; i < 30; i++)
    {
        if ((transfer(l, 4, true) & 0xa0000000) == 0xa0000000)
        {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ready)
        throw std::runtime_error("Target debug power did not acknowledge");
    auto read32 = [&](uint32_t addr) {
        transfer(l, 0, false, 0x1e);
        transfer(l, 8, false, 0);
        transfer(l, 1, false, 0x23000052);
        transfer(l, 5, false, addr);
        return transfer(l, 13, true);
    };
    // Conservative mapping from OpenOCD STM32 flash drivers. Unknown IDs require explicit family selection.
    const std::map<uint16_t, int> ids = {
        {0x410, 2}, {0x412, 2}, {0x414, 2}, {0x418, 2}, {0x420, 2}, {0x428, 2}, {0x430, 2}, {0x411, 3},
        {0x413, 5}, {0x419, 5}, {0x434, 5}, {0x423, 5}, {0x421, 5}, {0x431, 5}, {0x433, 5}, {0x441, 5},
        {0x458, 5}, {0x449, 6}, {0x451, 6}, {0x452, 6}, {0x450, 9}, {0x480, 9}, {0x483, 9}, {0x485, 10}};
    for (uint32_t addr : {0xE0042000u, 0x40015800u, 0x5C001000u, 0xE0044000u})
        try
        {
            auto id = read32(addr);
            if (auto it = ids.find(id & 0xfff); it != ids.end())
            {
                result.id = id;
                result.profile = it->second;
                break;
            }
        }
        catch (const std::runtime_error &)
        {
        }
    if ((result.id & 0xfff) == 0x450)
    {
        result.flash_kib = read32(0x1ff1e880) & 0xffff;
    }
    transfer(l, 0, false, 0x1e);
    return result;
}
static bool requires_h7_dual_bank(Link &link, int profile)
{
    if (profile != 9)
        return false;
    auto target = detect_stm32(link);
    return (target.id & 0xfff) == 0x450 && target.flash_kib > 128 && target.flash_kib <= 2048;
}
static fs::path new_job(const fs::path &assets)
{
    static std::atomic<unsigned> count = 0;
    auto p = assets.parent_path() / L"WirelessDAP-jobs" /
             (L"job-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++count));
    fs::create_directories(p);
    return p;
}
static void text_file(const fs::path &p, const std::string &s)
{
    write_file(p, {(const uint8_t *)s.data(), s.size()});
}
static std::string prefix(int profile, bool dualBank = false)
{
    if (profile <= 0 || profile >= (int)profiles.size())
        throw std::runtime_error("Choose the STM32 family when automatic identification is inconclusive");
    return std::string(dualBank ? "set DUAL_BANK 1\n" : "") + "source [find target/" +
           std::string(profiles[profile].script) +
           "]\nadapter speed 100\nreset_config none\ncortex_m reset_config sysresetreq\ngdb port "
           "disabled\ntcl port disabled\ntelnet port disabled\n";
}
int Engine::execute(std::shared_ptr<Link> link, const std::string &script, const fs::path &job, Log log,
                    unsigned timeout)
{
    std::unique_ptr<Bridge> dap;
    std::unique_ptr<StlinkBridge> stlink;
    std::ostringstream configuration;
    if (useStlink)
    {
        stlink = std::make_unique<StlinkBridge>(link, log, 0);
        configuration << "adapter driver st-link\nadapter usb vid_pid 0x0483 0x3748\nst-link backend tcp " << stlink->port()
                      << "\ntransport select swd\nadapter speed 100\n";
    }
    else
    {
        dap = std::make_unique<Bridge>(link);
        configuration << "adapter driver cmsis-dap\ncmsis-dap backend tcp\n"
                      << "cmsis-dap tcp host 127.0.0.1\ncmsis-dap tcp port " << dap->port
                      << "\ncmsis-dap tcp min_timeout 1000\ncmsis-dap quirk enable\n"
                      << "transport select swd\nadapter speed 100\n";
    }
    configuration << script;
    text_file(job / L"job.cfg", configuration.str());
    int code = run_process(
        assets / L"openocd.exe", L"-s \"" + (assets / L"scripts").wstring() + L"\" -f job.cfg", job,
        [&](const std::wstring &line) {
            std::ofstream file(job / L"engine.log", std::ios::app);
            file << utf8(line) << std::endl;
            log(line);
        },
        timeout, background);
    if (dap && !dap->error().empty())
        throw std::runtime_error(dap->error());
    return code;
}
void Engine::check(std::shared_ptr<Link> l, int profile, Log log)
{
    auto job = new_job(assets);
    auto script = prefix(profile, requires_h7_dual_bank(*l, profile)) +
                  "init\nhalt\nfor {set i 0} {$i < [llength [flash list]]} {incr i} { flash probe $i "
                  "}\nflash banks\necho WDP_TARGET_READY\nshutdown\n";
    bool ready = false;
    int code = execute(
        l, script, job,
        [&](const std::wstring &s) {
            if (s.find(L"WDP_TARGET_READY") != s.npos)
                ready = true;
            log(s);
        },
        60);
    if (code || !ready)
        throw std::runtime_error(
            "Target check failed. Confirm family, SWD wiring and target protection; no flash erased.");
}
void Engine::flash(std::shared_ptr<Link> l, int profile, const fs::path &file, uint32_t base, bool run,
                   Log log)
{
    auto segments = parse_image(file, base);
    auto job = new_job(assets);
    std::ostringstream script;
    script << prefix(profile, requires_h7_dual_bank(*l, profile))
           << "init\nreset halt\nfor {set i 0} {$i < [llength [flash list]]} {incr i} { flash probe $i }\n";
    // Validate every range against actual probed flash banks before any erase.
    for (auto &s : segments)
    {
        script << "set cursor " << s.address << "\nset finish " << uint64_t(s.address) + s.bytes.size()
               << "\nwhile {$cursor < $finish} {\n set next $cursor\n foreach bank [flash list] {\n  set b "
                  "[dict get $bank base]\n  set e [expr {$b + [dict get $bank size]}]\n  if {$cursor >= $b "
                  "&& $cursor < $e} { set next [expr {$finish < $e ? $finish : $e}]; break }\n }\n if {$next "
                  "== $cursor} { error {Firmware address outside detected Flash; nothing erased} }\n set "
                  "cursor $next\n}\n";
    }
    // Canonical HEX combines sparse segments into a single erase/write operation.
    std::ostringstream hex;
    auto record = [&](uint8_t type, uint16_t address, std::span<const uint8_t> b) {
        unsigned sum = (unsigned)b.size() + (address >> 8) + (address & 255) + type;
        hex << ":" << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << b.size()
            << std::setw(4) << address << std::setw(2) << unsigned(type);
        for (auto c : b)
        {
            hex << std::setw(2) << unsigned(c);
            sum += c;
        }
        hex << std::setw(2) << ((0u - sum) & 255) << "\n";
    };
    uint32_t upper = UINT32_MAX;
    for (auto &s : segments)
        for (size_t off = 0; off < s.bytes.size();)
        {
            uint32_t address = s.address + (uint32_t)off;
            if ((address >> 16) != upper)
            {
                upper = address >> 16;
                uint8_t b[]{uint8_t(upper >> 8), uint8_t(upper)};
                record(4, 0, b);
            }
            size_t n = std::min<size_t>({16, s.bytes.size() - off, 0x10000 - (address & 0xffff)});
            record(0, uint16_t(address), {s.bytes.data() + off, n});
            off += n;
        }
    record(1, 0, {});
    text_file(job / L"image.hex", hex.str());
    script << "echo WDP_WRITING\nflash write_image erase image.hex 0 ihex\necho WDP_READING_BACK\n";
    for (size_t i = 0; i < segments.size(); i++)
        script << "dump_image verify_" << i << ".bin " << segments[i].address << " "
               << segments[i].bytes.size() << "\n";
    script << "echo WDP_DUMP_OK\nshutdown\n";
    size_t imageBytes = 0;
    for (const auto &segment : segments)
        imageBytes += segment.bytes.size();
    unsigned programTimeout = (unsigned)std::min<size_t>(21600, 900 + imageBytes / 128);
    bool dumped = false;
    int code = execute(
        l, script.str(), job,
        [&](const std::wstring &s) {
            if (s.find(L"WDP_DUMP_OK") != s.npos)
                dumped = true;
            log(s);
        },
        programTimeout);
    if (code || !dumped)
        throw std::runtime_error("Programming/read-back failed; target image must not be treated as valid");
    size_t total = 0;
    for (size_t i = 0; i < segments.size(); i++)
    {
        auto data = read_file(job / (L"verify_" + std::to_wstring(i) + L".bin"));
        if (data != segments[i].bytes)
            throw std::runtime_error("Full read-back differs at segment " + std::to_string(i));
        total += data.size();
    }
    log(L"逐字节读回校验通过 · " + std::to_wstring(total) + L" 字节");
    if (run)
    {
        auto runjob = new_job(assets);
        if (execute(l,
                    prefix(profile, requires_h7_dual_bank(*l, profile)) +
                        "init\nreset run\necho WDP_RUN_OK\nshutdown\n",
                    runjob, log))
            throw std::runtime_error("Read-back passed, but software reset failed");
    }
}

fs::path Engine::backup(std::shared_ptr<Link> l, int profile, uint32_t address, uint32_t size, Log log)
{
    if (!size || size > 32 * 1024 * 1024 || address < 0x08000000 || uint64_t(address) + size > 0x0a000000)
        throw std::runtime_error("Invalid internal Flash backup range");
    auto job = new_job(assets);
    std::ostringstream script;
    script << prefix(profile, requires_h7_dual_bank(*l, profile))
           << "init\nhalt\nfor {set i 0} {$i < [llength [flash list]]} {incr i} { flash probe $i }\nflash "
              "banks\ndump_image original.bin "
           << address << " " << size << "\necho WDP_BACKUP_READ\nverify_image original.bin " << address
           << " bin\necho WDP_BACKUP_VERIFIED\nshutdown\n";
    bool verified = false;
    int code = execute(
        l, script.str(), job,
        [&](const std::wstring &s) {
            if (s.find(L"WDP_BACKUP_VERIFIED") != s.npos)
                verified = true;
            log(s);
        },
        7200);
    auto output = job / L"original.bin";
    if (code || !verified || fs::file_size(output) != size)
        throw std::runtime_error("Flash backup incomplete; no erase or programming was performed");
    return output;
}
