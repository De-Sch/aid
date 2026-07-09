#pragma once

#include "aid/plumbing/Error.h"

// Result<T> is std::expected<T, Error> when <expected> is available
// (C++23 + libstdc++ ≥ 12), and a minimal header-only shim with the
// same surface otherwise. The shim deliberately omits and_then /
// transform — none of the use cases need them yet.

#if __has_include(<expected>)
#include <expected>
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#define AID_PLUMBING_HAVE_STD_EXPECTED 1
#endif

#ifdef AID_PLUMBING_HAVE_STD_EXPECTED

namespace aid::plumbing {

template <class T, class E> using expected = std::expected<T, E>;
template <class E> using unexpected = std::unexpected<E>;

} // namespace aid::plumbing

#else

#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace aid::plumbing {

template <class E> class unexpected {
public:
    constexpr explicit unexpected(const E& e) : e_(e) {}
    constexpr explicit unexpected(E&& e) : e_(std::move(e)) {}

    [[nodiscard]] constexpr const E& error() const& noexcept { return e_; }
    [[nodiscard]] constexpr E& error() & noexcept { return e_; }
    [[nodiscard]] constexpr E&& error() && noexcept { return std::move(e_); }

private:
    E e_;
};

template <class E> unexpected(E) -> unexpected<E>;

namespace detail {
struct void_value {};
} // namespace detail

template <class T, class E> class [[nodiscard]] expected {
    static_assert(!std::is_reference_v<T>, "expected<T&,E> not supported by shim");
    using StorageT = std::conditional_t<std::is_void_v<T>, detail::void_value, T>;

public:
    constexpr expected() : data_(StorageT{}) {}
    constexpr expected(const T& v) : data_(v) {}
    constexpr expected(T&& v) : data_(std::move(v)) {}
    constexpr expected(const unexpected<E>& u) : data_(u.error()) {}
    constexpr expected(unexpected<E>&& u) : data_(std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return data_.index() == 0; }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr T& value() & { return std::get<StorageT>(data_); }
    constexpr const T& value() const& { return std::get<StorageT>(data_); }
    constexpr T&& value() && { return std::move(std::get<StorageT>(data_)); }

    constexpr T& operator*() & noexcept { return *std::get_if<StorageT>(&data_); }
    constexpr const T& operator*() const& noexcept { return *std::get_if<StorageT>(&data_); }

    constexpr T* operator->() noexcept { return std::get_if<StorageT>(&data_); }
    constexpr const T* operator->() const noexcept { return std::get_if<StorageT>(&data_); }

    constexpr E& error() & noexcept { return *std::get_if<E>(&data_); }
    constexpr const E& error() const& noexcept { return *std::get_if<E>(&data_); }

private:
    std::variant<StorageT, E> data_;
};

// Specialization for void value type — no value(), no operator*, no operator->.
template <class E> class [[nodiscard]] expected<void, E> {
public:
    constexpr expected() noexcept : has_value_(true) {}
    constexpr expected(const unexpected<E>& u) : has_value_(false), error_(u.error()) {}
    constexpr expected(unexpected<E>&& u) : has_value_(false), error_(std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_value_; }
    constexpr explicit operator bool() const noexcept { return has_value_; }

    constexpr void value() const& {
        // Mirrors std::expected<void,E>::value() — present only for symmetry.
    }

    constexpr E& error() & noexcept { return error_; }
    constexpr const E& error() const& noexcept { return error_; }

private:
    bool has_value_;
    E error_{};
};

} // namespace aid::plumbing

#endif // AID_PLUMBING_HAVE_STD_EXPECTED

namespace aid::plumbing {

template <class T> using Result = expected<T, Error>;

} // namespace aid::plumbing
