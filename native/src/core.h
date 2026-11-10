#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <memory>
#include <span>
#include <queue>
#include <condition_variable>
namespace fs=std::filesystem;
using Bytes=std::vector<uint8_t>;
using Log=std::function<void(const std::wstring&)>;
std::wstring wide(std::string_view s);
std::string utf8(std::wstring_view s);
Bytes read_file(const fs::path& path);
void write_file(const fs::path& path,std::span<const uint8_t> b);
uint16_t u16(const uint8_t* p);
uint32_t u32(const uint8_t* p);
void put16(Bytes&b,uint16_t v);
void put32(Bytes&b,uint32_t v);
std::wstring hex32(uint32_t n);
int json_int(std::string_view s,std::string_view key);
struct Profile {const wchar_t* name; const char* script;};
extern const std::vector<Profile> profiles;
struct Segment {uint32_t address;Bytes bytes;};
std::vector<Segment> parse_image(const fs::path&,uint32_t base);
void validate_segments(std::vector<Segment>&);
fs::path unpack_assets();
int run_process(const fs::path&exe,const std::wstring&args,const fs::path&cwd,Log log,unsigned timeout_seconds=900,std::function<void()> tick={});
class Link {
    uint32_t sequence_=0;
    std::mutex mutex_;
public:
    virtual ~Link()=default;
    virtual Bytes exchange(std::span<const uint8_t>)=0;
    virtual void close() noexcept=0;
    Bytes request(uint8_t op,std::span<const uint8_t> payload={});
    void authenticate(const std::string&token);
    void disconnect() noexcept;
};
std::shared_ptr<Link> connect_tcp(const std::wstring&,uint16_t,const std::string&);
struct BleDevice {uint64_t address;std::wstring name;};
std::vector<BleDevice> scan_ble();
std::shared_ptr<Link> connect_ble(uint64_t,const std::string&);
class Bridge {
    std::shared_ptr<Link> link_;
    SOCKET listener_=INVALID_SOCKET;
    std::atomic<SOCKET> client_{INVALID_SOCKET};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex error_mutex_;
    std::string error_;
    void serve();
public:
    uint16_t port=0;
    explicit Bridge(std::shared_ptr<Link>);
    ~Bridge();
    std::string error();
};
struct Detected {int profile=0;uint32_t dp=0,id=0,flash_kib=0;};
Detected detect_stm32(Link&);
class Engine {
public:
    fs::path assets;
    bool useStlink = false;
    std::function<void()> background;
    explicit Engine(fs::path p):assets(std::move(p)){}
    void check(std::shared_ptr<Link>,int profile,Log);
    fs::path backup(std::shared_ptr<Link>,int profile,uint32_t address,uint32_t size,Log);
    void flash(std::shared_ptr<Link>,int profile,const fs::path&file,uint32_t base,bool run,Log);
    void self_test(Log);
private:
    int execute(std::shared_ptr<Link>,const std::string& script,const fs::path&job,Log,unsigned timeout=900);
};
bool self_test(const fs::path& report,Log log);

void show_interfaces(HWND parent,bool ble,const std::wstring& endpoint,const std::string& token);

bool capture(HWND h,const fs::path&path);

class InterfaceError:public std::runtime_error{public:using std::runtime_error::runtime_error;};

std::shared_ptr<Link> connect_usb(const std::wstring& port,const std::string& token);

std::vector<std::wstring> scan_usb();

class KeilBridge {
 struct Impl;std::unique_ptr<Impl> impl;
public:
 KeilBridge(std::shared_ptr<Link>,Log);
 ~KeilBridge();
 uint64_t request_count() const;
};

void install_keil_driver(const fs::path& root,const fs::path& assets);
