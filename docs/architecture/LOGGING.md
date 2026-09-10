# Logging conventions

**Tier:** architecture  
**Related:** [`pp-cpp-common` `Module` / `Logger`](https://github.com/people-post/pp-cpp-common) (hierarchy + `redirectTo`), [THREADING.md](THREADING.md) (runtime façades), [OWNERSHIP.md](OWNERSHIP.md) (who owns long-lived objects).

How code obtains a logger. Prefer **one style per ownership shape** — do not mix ad-hoc `getLogger("…")` into Module methods or static façades that already expose `logger()`.

---

## Three rules

| Kind | Rule | Example |
|------|------|---------|
| **`Module` instance** | In the constructor call `redirectLogger("…")`; use `log()` for the object’s lifetime | `WorkerPool`, `CoordinatorThread`, `BackgroundSyncScheduler`, `Application` |
| **Static façade** | Provide `InitLogging()` (idempotent); call it from `Initialize` / process bring-up; expose `logger()` (avoid naming the method `log()` — Android `#define log` clashes) | `AppRuntime`, `AppLifecycle` |
| **Free function / lambda at a boundary** | Take `logging::Logger&` (or a by-value `Logger` handle) explicitly — no hidden globals | Shutdown watchdog thread, posted worker callbacks that log |

Dotted names create hierarchy (`getLogger("Runtime.AppRuntime")` → parent `Runtime`, child `AppRuntime`). Prefer a shared parent for a subsystem so dogfood can raise `Runtime.*=DEBUG` without renaming every leaf.

---

## Module instances

```cpp
class ConversationsHub : public Module {
public:
  ConversationsHub() {
    redirectLogger("Conversations.Hub");
  }
  void StopMesh() {
    log().info << "StopMesh …";
  }
};
```

- Do **not** call `logging::getLogger` inside Module methods when `log()` is available.
- Name once in the ctor; keep names stable (they are filter keys).

---

## Static façades

```cpp
class AppRuntime {
public:
  static void InitLogging();   // idempotent; safe to call more than once
  static logging::Logger& logger();
  static void Initialize(const AppRuntimeConfig& = {});
};

void AppRuntime::Initialize(const AppRuntimeConfig& config) {
  InitLogging();
  // …
}
```

- `logger()` may call `InitLogging()` defensively (tests), but product paths must init from `Initialize`.
- Prefer binding under a parent category, e.g. `Runtime.AppRuntime`, `Runtime.Lifecycle`.
- `AppRuntime::Initialize` calls `AppRuntime::InitLogging()`; `AppRuntime::InitializeUI` calls `AppLifecycle::InitLogging()` (lifecycle lives in the GUI runtime lib).

---

## Free functions and lambdas

Pass the logger at **edges** (threads, `PostWorker` / `PostUI` callbacks, free helpers shared by multiple owners):

```cpp
void RunShutdownWatchdog(logging::Logger& log, uint64_t gen, TimePoint deadline) {
  // …
  log.error << "watchdog fired gen=" << gen;
}

std::thread([deadline, gen]() {
  AppRuntime::InitLogging();
  RunShutdownWatchdog(AppRuntime::logger(), gen, deadline);
}).detach();
```

`Logger` embeds `LogProxy` members that point at `this` — **do not copy** it. Prefer `Logger&` to a process-lifetime façade logger, or call `getLogger(category)` again inside the thread.

Do **not** thread `Logger&` through every private leaf helper of a single Module; use `log()` there.

---

## Anti-patterns

| Avoid | Prefer |
|-------|--------|
| `getLogger("Foo")` inside a `Module` method | `log()` after ctor `redirectLogger` |
| Meyers-only `static Logger` with no init hook on a static façade | `InitLogging()` + `log()` |
| Free function that silently logs via a global name | Explicit `Logger` parameter |
| Logger-on-every-private-helper | Parameter only at boundaries |

---

## Runtime categories (current)

| Name | Kind |
|------|------|
| `Runtime` | Parent (filter / level cascade) |
| `Runtime.AppRuntime` | Static façade (`AppRuntime::InitLogging`) |
| `Runtime.Lifecycle` | Static façade (`AppLifecycle::InitLogging`) |
| `Runtime.ThreadRuntime` | Module (`ThreadRuntime` ctor) |
| `Runtime.Coordinator` | Module (`CoordinatorThread` ctor) |

Changelog: 2026-09-09 — conventions + Runtime.* wiring for shutdown-latency work.
