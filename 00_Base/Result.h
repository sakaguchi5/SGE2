#pragma once

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace sge::base
{
template<class T, class E>
class Result final
{
public:
    static Result Success(T value) { return Result(std::move(value)); }
    static Result Failure(E error) { return Result(std::move(error), FailureTag{}); }

    [[nodiscard]] bool HasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return HasValue(); }

    [[nodiscard]] T& Value() & { assert(HasValue()); return std::get<T>(storage_); }
    [[nodiscard]] const T& Value() const & { assert(HasValue()); return std::get<T>(storage_); }
    [[nodiscard]] T&& Value() && { assert(HasValue()); return std::move(std::get<T>(storage_)); }

    [[nodiscard]] E& Error() & { assert(!HasValue()); return std::get<E>(storage_); }
    [[nodiscard]] const E& Error() const & { assert(!HasValue()); return std::get<E>(storage_); }

private:
    struct FailureTag {};
    explicit Result(T value) : storage_(std::move(value)) {}
    Result(E error, FailureTag) : storage_(std::move(error)) {}
    std::variant<T, E> storage_;
};

template<class E>
class Result<void, E> final
{
public:
    static Result Success() { return Result(true, std::nullopt); }
    static Result Failure(E error) { return Result(false, std::move(error)); }

    [[nodiscard]] bool HasValue() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const E& Error() const { assert(!ok_ && error_); return *error_; }

private:
    Result(bool ok, std::optional<E> error) : ok_(ok), error_(std::move(error)) {}
    bool ok_ = false;
    std::optional<E> error_;
};
}
