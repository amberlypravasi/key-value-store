// Helpers shared by tests that use real log files.

#pragma once

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// A log file path in the temp folder. The log, and any leftover compaction
// file, are deleted when the test ends.
class TempLog {
public:
    TempLog() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("kvstore_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++) + ".log");
        cleanup();
    }
    ~TempLog() { cleanup(); }

    const std::filesystem::path& path() const { return path_; }

private:
    void cleanup() {
        std::filesystem::remove(path_);
        std::filesystem::remove(path_.string() + ".compact");
    }

    std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}
