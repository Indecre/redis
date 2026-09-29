#include "keyforge/persistence/aof.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace keyforge::persistence {
namespace {

[[nodiscard]] std::string system_error(std::string_view operation) {
    return std::string{operation} + ": " + std::strerror(errno);
}

[[nodiscard]] bool to_command(const resp::Value& value, std::vector<std::string>& command) {
    if (value.type != resp::Type::array || value.elements.empty()) {
        return false;
    }
    command.clear();
    command.reserve(value.elements.size());
    for (const auto& element : value.elements) {
        if (element.type != resp::Type::bulk_string) {
            return false;
        }
        command.push_back(element.text);
    }
    return true;
}

[[nodiscard]] bool equals_ascii_case_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char character = left[index];
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        if (character != right[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

Aof::Aof(std::filesystem::path path, FsyncPolicy policy, resp::Limits limits)
    : path_(std::move(path)), policy_(policy), limits_(limits) {}

Aof::~Aof() {
    stop_worker();
    std::string ignored;
    (void)flush(ignored);
}

bool Aof::open(std::string& error) {
    std::error_code filesystem_error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "cannot create AOF directory '" + parent.string() + "': " +
                    filesystem_error.message();
            return false;
        }
    }
    file_.reset(::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!file_) {
        error = system_error("cannot open AOF '" + path_.string() + "'");
        return false;
    }
    return true;
}

bool Aof::replay(const ReplayCallback& callback, std::string& error) {
    if (!file_) {
        error = "AOF is not open";
        return false;
    }
    if (::lseek(file_.get(), 0, SEEK_SET) < 0) {
        error = system_error("cannot seek AOF for replay");
        return false;
    }

    resp::Parser parser{limits_};
    std::vector<std::string> command;
    std::uint64_t safe_offset = 0;
    std::size_t transaction_depth = 0;
    char buffer[64U * 1024U];
    for (;;) {
        const ssize_t count = ::read(file_.get(), buffer, sizeof(buffer));
        if (count > 0) {
            if (!parser.feed(std::string_view{buffer, static_cast<std::size_t>(count)})) {
                error = "AOF replay input limit exceeded: " + std::string{parser.error()};
                return false;
            }
            for (;;) {
                auto parsed = parser.next();
                if (parsed.status == resp::ParseStatus::incomplete) {
                    break;
                }
                if (parsed.status == resp::ParseStatus::invalid) {
                    error = "AOF corruption at byte " + std::to_string(parser.consumed_bytes()) +
                            ": " + parsed.error;
                    return false;
                }
                if (!parsed.value.has_value() || !to_command(*parsed.value, command)) {
                    error = "AOF corruption at byte " + std::to_string(parser.consumed_bytes()) +
                            ": expected an array of bulk strings";
                    return false;
                }
                if (!command.empty() &&
                    equals_ascii_case_insensitive(command.front(), "MULTI")) {
                    ++transaction_depth;
                }
                if (!callback(command, error)) {
                    error = "AOF replay command failed at byte " +
                            std::to_string(parser.consumed_bytes()) + ": " + error;
                    return false;
                }
                if (!command.empty() && equals_ascii_case_insensitive(command.front(), "EXEC")) {
                    if (transaction_depth == 0U) {
                        error = "AOF corruption: EXEC without a matching MULTI";
                        return false;
                    }
                    --transaction_depth;
                }
                if (transaction_depth == 0U) {
                    safe_offset = parser.consumed_bytes();
                }
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        error = system_error("cannot read AOF");
        return false;
    }

    if (parser.buffered_bytes() != 0U || transaction_depth != 0U) {
        if (safe_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            error = "AOF offset is not representable on this platform";
            return false;
        }
        if (::ftruncate(file_.get(), static_cast<off_t>(safe_offset)) < 0) {
            error = system_error("cannot trim truncated final AOF record");
            return false;
        }
    }
    if (::lseek(file_.get(), 0, SEEK_END) < 0) {
        error = system_error("cannot seek to AOF end");
        return false;
    }
    return true;
}

void Aof::start_background_flush() {
    if (policy_ == FsyncPolicy::everysec && !worker_.joinable()) {
        worker_ = std::thread([this] { worker_loop(); });
    }
}

bool Aof::append_batch(const std::vector<std::vector<std::string>>& commands,
                       std::string& error) {
    if (commands.empty()) {
        return true;
    }
    std::string record = resp::encode_command({"MULTI"});
    for (const auto& command : commands) {
        record.append(resp::encode_command(command));
    }
    record.append(resp::encode_command({"EXEC"}));

    std::lock_guard lock{mutex_};
    if (!background_error_.empty()) {
        error = background_error_;
        return false;
    }
    if (!write_all_locked(record, error)) {
        return false;
    }
    dirty_ = true;
    if (policy_ == FsyncPolicy::always && !sync_locked(error)) {
        return false;
    }
    condition_.notify_one();
    return true;
}

bool Aof::flush(std::string& error) {
    std::lock_guard lock{mutex_};
    if (!background_error_.empty()) {
        error = background_error_;
        return false;
    }
    if (policy_ == FsyncPolicy::no || !dirty_) {
        return true;
    }
    return sync_locked(error);
}

bool Aof::write_all_locked(std::string_view bytes, std::string& error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(file_.get(), bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        error = system_error("cannot append AOF");
        return false;
    }
    return true;
}

bool Aof::sync_locked(std::string& error) {
    if (::fdatasync(file_.get()) < 0) {
        error = system_error("cannot synchronize AOF");
        return false;
    }
    dirty_ = false;
    return true;
}

void Aof::worker_loop() {
    std::unique_lock lock{mutex_};
    while (!stop_) {
        condition_.wait_for(lock, std::chrono::seconds{1}, [this] { return stop_; });
        if (stop_ || !dirty_) {
            continue;
        }
        std::string error;
        if (!sync_locked(error)) {
            background_error_ = std::move(error);
        }
    }
}

void Aof::stop_worker() {
    {
        std::lock_guard lock{mutex_};
        stop_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

}  // namespace keyforge::persistence
