// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#include <spdlog/logger.h>
#include "common/logging/log_file_sink.h"

namespace {
struct TestDirectory {
    std::filesystem::path path = std::filesystem::current_path() /
        ("log-limit-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() {
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("Could not create an isolated log test directory");
        }
    }
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove(path / "session.log", error);
        std::filesystem::remove(path / "main.log", error);
        std::filesystem::remove(path, error);
    }
};

std::string Capture(const std::filesystem::path& directory, unsigned long long limit,
                    bool critical = false) {
    {
        auto sink = std::make_shared<Common::Log::LogFileSink>(
            (directory / "main.log").string(), true, limit);
        sink->session_file_helper_.open((directory / "session.log").string(), true);
        spdlog::logger logger{"log-limit", sink};
        logger.set_pattern("%v");
        logger.info("first");
        logger.info("second");
        if (critical) {
            logger.critical("critical");
        }
        logger.flush();
    }
    std::ifstream stream{directory / "session.log", std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Could not read the captured log");
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
} // namespace

int main() {
    try {
        const TestDirectory directory;
        const auto unlimited = Capture(directory.path, 0);
        if (unlimited.find("first") == std::string::npos ||
            unlimited.find("second") == std::string::npos) {
            throw std::runtime_error("Zero limit discarded ordinary log messages");
        }
        const auto bounded = Capture(directory.path, 1, true);
        if (bounded.find("first") == std::string::npos ||
            bounded.find("second") != std::string::npos ||
            bounded.find("critical") == std::string::npos) {
            throw std::runtime_error("Positive limit or critical bypass regressed");
        }
        std::cout << "Unlimited log writing and positive-limit critical bypass passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
