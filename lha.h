#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lha {

typedef std::function<void(uint64_t done, uint64_t total, void* userdata)> ProgressFn;

struct Member {
    std::string name;
    uint32_t mode;
    std::vector<uint8_t> data;
    std::string pkg_name;
    std::string pkg_version;
};

bool archive_write(const std::string& out_path, const std::vector<Member>& members,
                   std::string& err, ProgressFn progress = nullptr,
                   void* userdata = nullptr);
bool archive_read(const std::string& in_path, std::vector<Member>& members,
                  std::string& err, ProgressFn progress = nullptr,
                  void* userdata = nullptr);
bool archive_read_first(const std::string& in_path, Member& m, std::string& err);

}  // namespace lha
