#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace vio
{
class event_loop_t;
}

namespace prism::detail
{
// RTTI-free type identity: the address of a per-type inline variable is a unique,
// stable key (prism builds with -fno-rtti, so typeid is unavailable).
template <typename T>
inline constexpr char type_tag_v{};

template <typename T>
constexpr const void *type_key()
{
  return &type_tag_v<T>;
}

// One factory-produced instance per event loop (== per thread, since a loop is
// run by a single thread and connection coroutines never migrate). The instance
// is created lazily on first use for a loop. The returned reference is stable for
// the storage's lifetime (unique_ptr keeps T at a fixed address), so it remains
// valid after the lock is released even if another loop's insert rehashes the map.
template <typename T>
class per_thread_storage_t
{
public:
  explicit per_thread_storage_t(std::function<T()> factory)
    : _factory(std::move(factory))
  {
  }

  T &get(vio::event_loop_t *loop)
  {
    {
      std::shared_lock read(_mutex);
      auto it = _instances.find(loop);
      if (it != _instances.end())
      {
        return *it->second;
      }
    }
    std::unique_lock write(_mutex);
    auto it = _instances.find(loop);
    if (it == _instances.end())
    {
      it = _instances.emplace(loop, std::make_unique<T>(_factory())).first;
    }
    return *it->second;
  }

private:
  std::function<T()> _factory;
  std::shared_mutex _mutex;
  std::unordered_map<vio::event_loop_t *, std::unique_ptr<T>> _instances;
};

// A type-keyed set of per-thread factories owned by the router/app. Populated by
// provide<T>() at configure time (before serving, single-threaded); read by
// find<T>() during dispatch. The map itself is therefore not locked; each stored
// storage self-synchronizes its per-loop instances.
class per_thread_registry_t
{
public:
  template <typename T, typename F>
  void provide(F factory)
  {
    _storages[type_key<T>()] = std::make_shared<per_thread_storage_t<T>>(std::function<T()>(std::move(factory)));
  }

  template <typename T>
  [[nodiscard]] T *find(vio::event_loop_t *loop) const
  {
    auto it = _storages.find(type_key<T>());
    if (it == _storages.end())
    {
      return nullptr;
    }
    return &static_cast<per_thread_storage_t<T> *>(it->second.get())->get(loop);
  }

private:
  std::unordered_map<const void *, std::shared_ptr<void>> _storages;
};
} // namespace prism::detail
