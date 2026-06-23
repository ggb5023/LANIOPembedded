#pragma once

#include <optional>
#include <string>
#include <utility>

extern "C" {
#include "lan_chat/core/status.h"
}

namespace lan_chat {

template <typename T>
class Result {
public:
    static Result success(T value)
    {
        return Result(std::move(value));
    }

    static Result failure(lan_chat_status_t status, std::string message = {})
    {
        return Result(status, std::move(message));
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return status_ == LAN_CHAT_STATUS_OK;
    }

    [[nodiscard]] lan_chat_status_t status() const noexcept
    {
        return status_;
    }

    [[nodiscard]] const std::string &message() const noexcept
    {
        return message_;
    }

    [[nodiscard]] T &value() & noexcept
    {
        return *value_;
    }

    [[nodiscard]] const T &value() const & noexcept
    {
        return *value_;
    }

private:
    explicit Result(T value)
        : status_(LAN_CHAT_STATUS_OK), value_(std::move(value))
    {
    }

    Result(lan_chat_status_t status, std::string message)
        : status_(status), message_(std::move(message))
    {
    }

    lan_chat_status_t status_;
    std::string message_;
    std::optional<T> value_;
};

template <>
class Result<void> {
public:
    static Result success()
    {
        return Result(LAN_CHAT_STATUS_OK, {});
    }

    static Result failure(lan_chat_status_t status, std::string message = {})
    {
        return Result(status, std::move(message));
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return status_ == LAN_CHAT_STATUS_OK;
    }

    [[nodiscard]] lan_chat_status_t status() const noexcept
    {
        return status_;
    }

    [[nodiscard]] const std::string &message() const noexcept
    {
        return message_;
    }

private:
    Result(lan_chat_status_t status, std::string message)
        : status_(status), message_(std::move(message))
    {
    }

    lan_chat_status_t status_;
    std::string message_;
};

} // namespace lan_chat
