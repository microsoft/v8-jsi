// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef SRC_PUBLIC_COMPAT_H_
#define SRC_PUBLIC_COMPAT_H_

#include <string>

namespace napijsi {

/**
 * @brief A span of values that can be used to pass arguments to function.
 *
 * For C++20 we should consider to replace it with std::span.
 */
template <typename T>
struct span {
  constexpr span(std::initializer_list<T> il) noexcept : data_{const_cast<T *>(il.begin())}, size_{il.size()} {}
  constexpr span(T *data, size_t size) noexcept : data_{data}, size_{size} {}

  [[nodiscard]] constexpr T *data() const noexcept {
    return data_;
  }

  [[nodiscard]] constexpr size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] constexpr T *begin() const noexcept {
    return data_;
  }

  [[nodiscard]] constexpr T *end() const noexcept {
    return data_ + size_;
  }

  const T &operator[](size_t index) const noexcept {
    return *(data_ + index);
  }

 private:
  T *data_;
  size_t size_;
};

/**
 * @brief A minimal subset of std::string_view.
 *
 * In C++17 we must replace it with std::string_view.
 */
struct string_view {
  constexpr string_view() noexcept = default;
  constexpr string_view(const string_view &other) noexcept = default;
  string_view(const char *data) noexcept : data_{data}, size_{std::char_traits<char>::length(data)} {}
  string_view(const char *data, size_t size) noexcept : data_{data}, size_{size} {}
  string_view(const std::string &str) noexcept : data_{str.data()}, size_{str.size()} {}

  constexpr string_view &operator=(const string_view &view) noexcept = default;

  [[nodiscard]] constexpr const char *begin() const noexcept {
    return data_;
  }

  [[nodiscard]] constexpr const char *end() const noexcept {
    return data_ + size_;
  }

  [[nodiscard]] constexpr const char &operator[](size_t pos) const noexcept {
    return *(data_ + pos);
  }

  [[nodiscard]] constexpr const char *data() const noexcept {
    return data_;
  }

  [[nodiscard]] constexpr size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    return size_ == 0;
  }

 private:
  const char *data_{nullptr};
  size_t size_{0};
};

inline string_view operator"" _sv(const char *str, std::size_t len) noexcept {
  return string_view(str, len);
}

} // namespace napijsi

#endif // SRC_PUBLIC_COMPAT_H_
