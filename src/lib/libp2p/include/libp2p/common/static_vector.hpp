/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>

namespace libp2p::common {

  /**
   * Fixed-capacity vector that throws std::bad_alloc when capacity is exceeded
   * (matches boost::container::static_vector overflow behavior used by
   * multiselect).
   */
  template <typename T, std::size_t N>
  class StaticVector {
   public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T *;
    using const_iterator = const T *;

    StaticVector() = default;

    size_type size() const noexcept {
      return size_;
    }
    size_type capacity() const noexcept {
      return N;
    }
    bool empty() const noexcept {
      return size_ == 0;
    }

    T *data() noexcept {
      return data_.data();
    }
    const T *data() const noexcept {
      return data_.data();
    }

    iterator begin() noexcept {
      return data_.data();
    }
    iterator end() noexcept {
      return data_.data() + size_;
    }
    const_iterator begin() const noexcept {
      return data_.data();
    }
    const_iterator end() const noexcept {
      return data_.data() + size_;
    }

    void push_back(const T &value) {
      ensure_room(1);
      data_[size_++] = value;
    }
    void push_back(T &&value) {
      ensure_room(1);
      data_[size_++] = std::move(value);
    }

    template <typename InputIt>
    void insert(iterator /*pos*/, InputIt first, InputIt last) {
      // Only append is used by multiselect serializers.
      for (; first != last; ++first) {
        push_back(*first);
      }
    }

    void resize(size_type count) {
      if (count > N) {
        throw std::bad_alloc();
      }
      if (count > size_) {
        for (size_type i = size_; i < count; ++i) {
          data_[i] = T{};
        }
      }
      size_ = count;
    }

    void reserve(size_type count) {
      if (count > N) {
        throw std::bad_alloc();
      }
    }

   private:
    void ensure_room(size_type extra) {
      if (size_ + extra > N) {
        throw std::bad_alloc();
      }
    }

    std::array<T, N> data_{};
    size_type size_{0};
  };

}  // namespace libp2p::common
