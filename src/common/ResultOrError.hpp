#ifndef PP_BROWSER_RESULT_OR_ERROR_H
#define PP_BROWSER_RESULT_OR_ERROR_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace pbr {

struct RoeErrorBase {
  int32_t code;
  int32_t category;
  std::string message;
  std::string user;

  RoeErrorBase() : code(-1), category(0), message(""), user("") {}

  RoeErrorBase(int32_t c, const std::string& msg) : code(c), category(0), message(msg), user("") {}

  RoeErrorBase(int32_t c, std::string&& msg) : code(c), category(0), message(std::move(msg)), user("") {}

  explicit RoeErrorBase(const std::string& msg) : code(-1), category(0), message(msg), user("") {}

  explicit RoeErrorBase(std::string&& msg) : code(-1), category(0), message(std::move(msg)), user("") {}
};

template <typename T, typename E = std::string>
class ResultOrError {
public:
  ResultOrError(const T& value) : hasValue_(true) {
    using StorageType = typename std::remove_reference<T>::type;
    new (&storage_) StorageType(value);
  }

  template <typename U = T,
            typename = typename std::enable_if<!std::is_reference<U>::value>::type>
  ResultOrError(T&& value) : hasValue_(true) {
    new (&storage_) T(std::move(value));
  }

  template <typename U = E,
            typename = typename std::enable_if<std::is_base_of<RoeErrorBase, U>::value>::type>
  ResultOrError(const E& err) : hasValue_(false) {
    new (&storage_) E(err);
  }

  template <typename U = E,
            typename = typename std::enable_if<std::is_base_of<RoeErrorBase, U>::value>::type>
  ResultOrError(E&& err) : hasValue_(false) {
    new (&storage_) E(std::move(err));
  }

  static ResultOrError error(const E& err) {
    ResultOrError result;
    result.hasValue_ = false;
    new (&result.storage_) E(err);
    return result;
  }

  static ResultOrError error(E&& err) {
    ResultOrError result;
    result.hasValue_ = false;
    new (&result.storage_) E(std::move(err));
    return result;
  }

  ResultOrError(const ResultOrError& other) : hasValue_(other.hasValue_) {
    if (hasValue_) {
      using StorageType = typename std::remove_reference<T>::type;
      new (&storage_) StorageType(*reinterpret_cast<const StorageType*>(&other.storage_));
    } else {
      new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
    }
  }

  ResultOrError(ResultOrError&& other) noexcept : hasValue_(other.hasValue_) {
    if (hasValue_) {
      using StorageType = typename std::remove_reference<T>::type;
      new (&storage_) StorageType(std::move(*reinterpret_cast<StorageType*>(&other.storage_)));
    } else {
      new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
    }
  }

  ~ResultOrError() { destroy(); }

  ResultOrError& operator=(const ResultOrError& other) {
    if (this != &other) {
      destroy();
      hasValue_ = other.hasValue_;
      if (hasValue_) {
        using StorageType = typename std::remove_reference<T>::type;
        new (&storage_) StorageType(*reinterpret_cast<const StorageType*>(&other.storage_));
      } else {
        new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
      }
    }
    return *this;
  }

  ResultOrError& operator=(ResultOrError&& other) noexcept {
    if (this != &other) {
      destroy();
      hasValue_ = other.hasValue_;
      if (hasValue_) {
        using StorageType = typename std::remove_reference<T>::type;
        new (&storage_) StorageType(std::move(*reinterpret_cast<StorageType*>(&other.storage_)));
      } else {
        new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
      }
    }
    return *this;
  }

  bool isOk() const { return hasValue_; }
  bool isError() const { return !hasValue_; }
  explicit operator bool() const { return hasValue_; }

  const T& value() const {
    if (!hasValue_) {
      throw std::runtime_error("Attempting to access value of error result");
    }
    using StorageType = typename std::remove_reference<T>::type;
    return *reinterpret_cast<const StorageType*>(&storage_);
  }

  T& value() {
    if (!hasValue_) {
      throw std::runtime_error("Attempting to access value of error result");
    }
    using StorageType = typename std::remove_reference<T>::type;
    return *reinterpret_cast<StorageType*>(&storage_);
  }

  T valueOr(const T& defaultValue) const { return hasValue_ ? value() : defaultValue; }

  const E& error() const {
    if (hasValue_) {
      throw std::runtime_error("Attempting to access error of success result");
    }
    return *reinterpret_cast<const E*>(&storage_);
  }

  E& error() {
    if (hasValue_) {
      throw std::runtime_error("Attempting to access error of success result");
    }
    return *reinterpret_cast<E*>(&storage_);
  }

  const T& operator*() const { return value(); }
  T& operator*() { return value(); }

  template <typename U = T>
  typename std::enable_if<!std::is_reference<U>::value, typename std::remove_reference<T>::type*>::type
  operator->() const {
    using StorageType = typename std::remove_reference<T>::type;
    return reinterpret_cast<const StorageType*>(&storage_);
  }

  template <typename U = T>
  typename std::enable_if<!std::is_reference<U>::value, typename std::remove_reference<T>::type*>::type
  operator->() {
    using StorageType = typename std::remove_reference<T>::type;
    return reinterpret_cast<StorageType*>(&storage_);
  }

private:
  ResultOrError() : hasValue_(false) {}

  void destroy() {
    if (hasValue_) {
      using StorageType = typename std::remove_reference<T>::type;
      reinterpret_cast<StorageType*>(&storage_)->~StorageType();
    } else {
      reinterpret_cast<E*>(&storage_)->~E();
    }
  }

  bool hasValue_;
  using StorageType = typename std::remove_reference<T>::type;
  typename std::aligned_union<0, StorageType, E>::type storage_;
};

template <typename E>
class ResultOrError<void, E> {
public:
  ResultOrError() : hasValue_(true) {}

  template <typename U = E,
            typename = typename std::enable_if<std::is_base_of<RoeErrorBase, U>::value>::type>
  ResultOrError(const E& err) : hasValue_(false) {
    new (&storage_) E(err);
  }

  template <typename U = E,
            typename = typename std::enable_if<std::is_base_of<RoeErrorBase, U>::value>::type>
  ResultOrError(E&& err) : hasValue_(false) {
    new (&storage_) E(std::move(err));
  }

  static ResultOrError error(const E& err) {
    ResultOrError result;
    result.hasValue_ = false;
    new (&result.storage_) E(err);
    return result;
  }

  static ResultOrError error(E&& err) {
    ResultOrError result;
    result.hasValue_ = false;
    new (&result.storage_) E(std::move(err));
    return result;
  }

  ResultOrError(const ResultOrError& other) : hasValue_(other.hasValue_) {
    if (!hasValue_) {
      new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
    }
  }

  ResultOrError(ResultOrError&& other) noexcept : hasValue_(other.hasValue_) {
    if (!hasValue_) {
      new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
      other.hasValue_ = true;
    }
  }

  ~ResultOrError() {
    if (!hasValue_) {
      reinterpret_cast<E*>(&storage_)->~E();
    }
  }

  ResultOrError& operator=(const ResultOrError& other) {
    if (this != &other) {
      if (!hasValue_) {
        reinterpret_cast<E*>(&storage_)->~E();
      }
      hasValue_ = other.hasValue_;
      if (!hasValue_) {
        new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
      }
    }
    return *this;
  }

  ResultOrError& operator=(ResultOrError&& other) noexcept {
    if (this != &other) {
      if (!hasValue_) {
        reinterpret_cast<E*>(&storage_)->~E();
      }
      hasValue_ = other.hasValue_;
      if (!hasValue_) {
        new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
        other.hasValue_ = true;
      }
    }
    return *this;
  }

  bool isOk() const { return hasValue_; }
  bool isError() const { return !hasValue_; }
  explicit operator bool() const { return hasValue_; }

  const E& error() const {
    if (hasValue_) {
      throw std::runtime_error("Attempting to access error of success result");
    }
    return *reinterpret_cast<const E*>(&storage_);
  }

  E& error() {
    if (hasValue_) {
      throw std::runtime_error("Attempting to access error of success result");
    }
    return *reinterpret_cast<E*>(&storage_);
  }

private:
  bool hasValue_;
  typename std::aligned_union<0, E>::type storage_;
};

} // namespace pbr

#endif // PP_BROWSER_RESULT_OR_ERROR_H
