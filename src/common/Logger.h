#ifndef PP_BROWSER_LOGGER_H
#define PP_BROWSER_LOGGER_H

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
// windows.h / wingdi.h define ERROR/DEBUG and collide with enum members (same as
// soralog/level.hpp). c-ares/Boost often include windows.h before this header.
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif
#endif

namespace pbr {
namespace logging {

enum class Level { DEBUG = 0, INFO = 1, WARNING = 2, ERROR = 3, CRITICAL = 4 };

// Named constants for call sites: Windows headers may redefine ERROR/DEBUG later.
inline constexpr Level kLevelDebug = Level::DEBUG;
inline constexpr Level kLevelError = Level::ERROR;

class Handler {
public:
  virtual ~Handler() = default;
  virtual void emit(Level level, const std::string &loggerName,
                    const std::string &message) = 0;

  void setLevel(Level level) { level_ = level; }
  Level getLevel() const { return level_; }

protected:
  Level level_ = Level::DEBUG;
};

class ConsoleHandler : public Handler {
public:
  void emit(Level level, const std::string &loggerName,
            const std::string &message) override;
};

class FileHandler : public Handler {
public:
  explicit FileHandler(const std::string &filename);
  ~FileHandler() override;
  void emit(Level level, const std::string &loggerName,
            const std::string &message) override;

private:
  std::ofstream file_;
  std::string filename_;
};

class Logger;
class LogStream;
class LoggerNode;

class LogProxy {
public:
  LogProxy(Logger *logger, Level level);

  template <typename T> LogStream operator<<(const T &value);

private:
  Logger *logger_;
  Level level_;
};

class LogStream {
public:
  LogStream(Logger *logger, Level level);
  ~LogStream();

  LogStream(const LogStream &) = delete;
  LogStream &operator=(const LogStream &) = delete;

  LogStream(LogStream &&other) noexcept;
  LogStream &operator=(LogStream &&other) noexcept;

  template <typename T> LogStream &operator<<(const T &value) {
    stream_ << value;
    return *this;
  }

private:
  Logger *logger_;
  Level level_;
  std::ostringstream stream_;
  bool moved_;
};

class LoggerNode : public std::enable_shared_from_this<LoggerNode> {
public:
  explicit LoggerNode(const std::string &name);
  ~LoggerNode() = default;

  void setLevel(Level level) { level_ = level; }
  Level getLevel() const { return level_; }

  void addHandler(std::shared_ptr<Handler> spHandler);
  void addFileHandler(const std::string &filename, Level level);

  void setPropagate(bool propagate) { propagate_ = propagate; }
  bool getPropagate() const { return propagate_; }

  void setParent(std::weak_ptr<LoggerNode> parent) { parent_ = std::move(parent); }
  std::shared_ptr<LoggerNode> getParent() const { return parent_.lock(); }
  void addChild(std::shared_ptr<LoggerNode> child);
  void removeChild(LoggerNode* child);
  const std::vector<std::shared_ptr<LoggerNode>>& getChildren() const { return spChildren_; }

  void log(Level level, const std::string &message);

  const std::string &getName() const { return name_; }
  std::string getFullName() const;

  std::shared_ptr<LoggerNode> getOrInitChild(const std::string& fullName);

private:
  std::shared_ptr<LoggerNode> getOrInitDirectChild(const std::string& name);

  void logToHandlers(Level level, const std::string &message);
  void logWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName);
  void logToHandlersWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName);
  std::string formatMessage(Level level, const std::string &message);
  std::string formatMessage(Level level, const std::string &message, const std::string &originatingLoggerName);
  std::string levelToString(Level level);

  std::string name_;
  std::weak_ptr<LoggerNode> parent_;
  Level level_{ Level::DEBUG };
  bool propagate_{ true };
  std::vector<std::shared_ptr<LoggerNode>> spChildren_;
  std::vector<std::shared_ptr<Handler>> spHandlers_;
  mutable std::mutex mutex_;
};

class Logger {
  friend class LogStream;
  friend class LogProxy;

  // Declared before LogProxy members so ctor init order matches (-Wreorder).
  std::shared_ptr<LoggerNode> spNode_;

  std::shared_ptr<LoggerNode> getNode() const { return spNode_; }
  void log(Level level, const std::string &message) { spNode_->log(level, message); }

public:
  explicit Logger(std::shared_ptr<LoggerNode> node);
  ~Logger() = default;

  LogProxy debug;
  LogProxy info;
  LogProxy warning;
  LogProxy error;
  LogProxy critical;

  void setLevel(Level level) { spNode_->setLevel(level); }
  Level getLevel() const { return spNode_->getLevel(); }

  void addHandler(std::shared_ptr<Handler> spHandler) { spNode_->addHandler(std::move(spHandler)); }
  void addFileHandler(const std::string &filename, Level level = Level::DEBUG) {
    spNode_->addFileHandler(filename, level);
  }

  void setPropagate(bool propagate) { spNode_->setPropagate(propagate); }
  bool getPropagate() const { return spNode_->getPropagate(); }

  /** Re-parent this logger under `targetLoggerName`. No-op if already that node. */
  void redirectTo(const std::string &targetLoggerName);

  const std::string &getName() const { return spNode_->getName(); }
  std::string getFullName() const { return spNode_->getFullName(); }

  bool operator==(const Logger& other) const { return spNode_ == other.spNode_; }
  bool operator!=(const Logger& other) const { return spNode_ != other.spNode_; }
};

template <typename T> LogStream LogProxy::operator<<(const T &value) {
  LogStream stream(logger_, level_);
  stream << value;
  return stream;
}

Logger getLogger(const std::string &name);
Logger getRootLogger();

Level getLevel();
void setLevel(Level level);

/** Minimum emitted level after a message passes the logger filter.
 *  Call-site levels below the floor are raised (e.g. INFO → WARNING) for
 *  formatting and handlers. Default is DEBUG (no boost). Filter checks still
 *  use the original call-site level. */
Level getEmitFloor();
void setEmitFloor(Level floor);

} // namespace logging
} // namespace pbr

#endif // PP_BROWSER_LOGGER_H
