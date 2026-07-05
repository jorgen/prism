#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include <prism/prism.h>

#include <vio/run.h>

namespace
{
struct task_t
{
  int id = 0;
  std::string title;
  bool done = false;
  STFY_OBJ(id, title, done);
};

struct task_list_t
{
  std::vector<task_t> tasks;
  STFY_OBJ(tasks);
};

struct task_create_t
{
  std::string title;
  STFY_OBJ(title);
};

struct store_t
{
  std::vector<task_t> tasks;
  int next_id = 1;
};

vio::task_t<prism::negotiated_t<task_list_t>> list_tasks(std::shared_ptr<store_t> store)
{
  co_return prism::ok(task_list_t{store->tasks});
}

vio::task_t<prism::negotiated_t<task_t>> create_task(std::shared_ptr<store_t> store, prism::body_t<task_create_t> in)
{
  if (in.value.title.empty())
  {
    co_return prism::response_t::text(prism::status_t::unprocessable_entity, "title must not be empty");
  }
  task_t created{store->next_id++, std::move(in.value.title), false};
  store->tasks.push_back(created);
  co_return prism::created(created);
}

vio::task_t<prism::negotiated_t<task_t>> toggle_task(std::shared_ptr<store_t> store, prism::path_t<"id", int> id)
{
  for (task_t &task : store->tasks)
  {
    if (task.id == id.value)
    {
      task.done = !task.done;
      co_return prism::ok(task);
    }
  }
  co_return prism::response_t::text(prism::status_t::not_found, "task not found");
}
} // namespace

int main(int argc, char **argv)
{
  std::string dist = argc > 1 ? argv[1] : "dist";
  std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 8080;
  std::println("prism react server on http://localhost:{} — REST under /api, static SPA from '{}'", port, dist);

  return vio::run(
    [dist, port](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto store = std::make_shared<store_t>();
      store->tasks.push_back({store->next_id++, "Learn prism", true});
      store->tasks.push_back({store->next_id++, "Build a React app on prism", false});

      prism::app_t app;

      // REST API first, so it takes precedence over the static SPA mount.
      app.get("/api/tasks", list_tasks, store);
      app.post("/api/tasks", create_task, store);
      app.put("/api/tasks/{id}", toggle_task, store);

      // Serve the built Vite app; unmatched navigation paths fall back to
      // index.html so React Router (client-side routing) works on reload.
      app.static_files("/", dist, /*spa_fallback=*/true);

      auto result = co_await app.listen(loop, "", port);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
