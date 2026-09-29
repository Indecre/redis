#pragma once

#include "keyforge/common/fd.hpp"
#include "keyforge/protocol/resp.hpp"

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace keyforge::persistence {

enum class FsyncPolicy { always, everysec, no };

class Sink {
public:
    virtual ~Sink() = default;
    [[nodiscard]] virtual bool append_batch(const std::vector<std::vector<std::string>>& commands,
                                            std::string& error) = 0;
    [[nodiscard]] virtual bool flush(std::string& error) = 0;
};

class Aof final : public Sink {
public:
    using ReplayCallback =
        std::function<bool(const std::vector<std::string>& command, std::string& error)>;

    Aof(std::filesystem::path path, FsyncPolicy policy, resp::Limits limits);
    ~Aof() override;

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    [[nodiscard]] bool open(std::string& error);
    [[nodiscard]] bool replay(const ReplayCallback& callback, std::string& error);
    void start_background_flush();

    [[nodiscard]] bool append_batch(const std::vector<std::vector<std::string>>& commands,
                                    std::string& error) override;
    [[nodiscard]] bool flush(std::string& error) override;

private:
    [[nodiscard]] bool write_all_locked(std::string_view bytes, std::string& error);
    [[nodiscard]] bool sync_locked(std::string& error);
    void worker_loop();
    void stop_worker();

    std::filesystem::path path_;
    FsyncPolicy policy_;
    resp::Limits limits_;
    Fd file_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    bool stop_{false};
    bool dirty_{false};
    std::string background_error_;
};

}  // namespace keyforge::persistence
