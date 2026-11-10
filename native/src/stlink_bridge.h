#pragma once
#include "core.h"

// ST-LINK Server-compatible endpoint. It shares the selected ESP transport.
class StlinkBridge
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    StlinkBridge(std::shared_ptr<Link> link, Log log, uint16_t port = 7185, bool trace = false);
    ~StlinkBridge();
    uint16_t port() const;
    uint64_t request_count() const;
    uint32_t read_debug_register(uint32_t address);
};

void stlink_self_test(Log log);

void stlink_engine_self_test(const fs::path& assets, Log log);
