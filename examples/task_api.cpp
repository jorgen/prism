#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <prism/prism.h>

#include <vio/operation/sleep.h>
#include <vio/run.h>

namespace
{
struct task_item_t
{
  int id = 0;
  std::string title;
  bool done = false;
  STFY_OBJ(id, title, done);
};

struct task_list_t
{
  std::vector<task_item_t> tasks;
  STFY_OBJ(tasks);
};

struct task_create_t
{
  std::string title;
  STFY_OBJ(title);
};

struct task_update_t
{
  std::string title;
  bool done = false;
  STFY_OBJ(title, done);
};

struct error_body_t
{
  std::string error;
  STFY_OBJ(error);
};

struct store_t
{
  std::vector<task_item_t> tasks;
  int next_id = 1;
};

prism::response_t error_response(prism::status_t status, std::string message)
{
  return prism::json::respond(status, error_body_t{std::move(message)});
}

bool parse_int(std::string_view text, int &out)
{
  const char *end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, out);
  return ec == std::errc{} && ptr == end;
}

task_item_t *find_task(store_t &store, int id)
{
  for (auto &task : store.tasks)
  {
    if (task.id == id)
    {
      return &task;
    }
  }
  return nullptr;
}

void log_line(prism::log_level_t level, std::string_view message)
{
  std::println("[task-api] {} {}", prism::log_level_name(level), message);
  std::fflush(stdout);
}

vio::task_t<prism::response_t> health(prism::request_t)
{
  co_return prism::response_t::text(prism::status_t::ok, "ok");
}

vio::task_t<prism::negotiated_t<task_list_t>> list_tasks(std::shared_ptr<store_t> store, prism::query_t<"done", std::optional<bool>> done)
{
  task_list_t result;
  for (const auto &task : store->tasks)
  {
    if (!done.value.has_value() || task.done == *done.value)
    {
      result.tasks.push_back(task);
    }
  }
  co_return prism::ok(result);
}

vio::task_t<prism::negotiated_t<task_item_t>> create_task(std::shared_ptr<store_t> store, prism::body_t<task_create_t> in)
{
  if (in.value.title.empty())
  {
    co_return error_response(prism::status_t::unprocessable_entity, "title must not be empty");
  }
  task_item_t created{store->next_id++, std::move(in.value.title), false};
  store->tasks.push_back(created);
  co_return prism::created(created);
}

vio::task_t<prism::negotiated_t<task_item_t>> get_task(std::shared_ptr<store_t> store, prism::path_t<"id", int> id)
{
  if (task_item_t *task = find_task(*store, id.value))
  {
    co_return prism::ok(*task);
  }
  co_return error_response(prism::status_t::not_found, "task not found");
}

vio::task_t<prism::negotiated_t<task_item_t>> update_task(std::shared_ptr<store_t> store, prism::path_t<"id", int> id, prism::body_t<task_update_t> in)
{
  task_item_t *task = find_task(*store, id.value);
  if (task == nullptr)
  {
    co_return error_response(prism::status_t::not_found, "task not found");
  }
  task->title = std::move(in.value.title);
  task->done = in.value.done;
  co_return prism::ok(*task);
}

vio::task_t<prism::response_t> remove_task(std::shared_ptr<store_t> store, prism::path_t<"id", int> id)
{
  for (auto it = store->tasks.begin(); it != store->tasks.end(); ++it)
  {
    if (it->id == id.value)
    {
      store->tasks.erase(it);
      co_return prism::response_t::text(prism::status_t::no_content, "");
    }
  }
  co_return error_response(prism::status_t::not_found, "task not found");
}

vio::task_t<prism::response_t> export_csv(std::shared_ptr<store_t> store)
{
  auto rows = std::make_shared<std::vector<task_item_t>>(store->tasks);
  auto index = std::make_shared<std::size_t>(0);
  auto header_sent = std::make_shared<bool>(false);
  co_return prism::response_t::streaming(prism::status_t::ok, "text/csv",
                                         [rows, index, header_sent]() -> vio::task_t<prism::body_chunk_t>
                                         {
                                           if (!*header_sent)
                                           {
                                             *header_sent = true;
                                             co_return prism::body_chunk_t{"id,title,done\n", false};
                                           }
                                           if (*index >= rows->size())
                                           {
                                             co_return prism::body_chunk_t{"", true};
                                           }
                                           const task_item_t &task = (*rows)[(*index)++];
                                           std::string row = std::to_string(task.id) + ',' + task.title + ',' + (task.done ? "true" : "false") + '\n';
                                           co_return prism::body_chunk_t{std::move(row), false};
                                         });
}

vio::task_t<prism::response_t> slow(prism::path_t<"ms", int> ms, prism::request_t request)
{
  co_await vio::sleep(*request.loop, std::chrono::milliseconds{ms.value});
  co_return prism::response_t::text(prism::status_t::ok, "slept " + std::to_string(ms.value) + "ms");
}

void configure(prism::app_t &app)
{
  auto store = std::make_shared<store_t>();

  app.logger().set_level(prism::log_level_t::debug);
  app.logger().set_sink(log_line);

  app.get("/health", health);
  app.get("/tasks", list_tasks, store);
  app.post("/tasks", create_task, store);
  app.get("/tasks/{id}", get_task, store);
  app.put("/tasks/{id}", update_task, store);
  app.del("/tasks/{id}", remove_task, store);
  app.get("/tasks.csv", export_csv, store);
  app.get("/slow/{ms}", slow);
}
} // namespace

VIO_MAIN(loop, argc, argv)
{
  std::uint16_t port = 8080;
  int parsed = 0;
  if (argc > 1 && parse_int(argv[1], parsed) && parsed > 0 && parsed <= 65535)
  {
    port = static_cast<std::uint16_t>(parsed);
  }

  std::println("prism {} task api on http://127.0.0.1:{}", prism::version(), port);

  prism::keepalive_options_t options;
  options.idle_timeout = std::chrono::seconds{30};
  options.max_connections = 1024;

  co_return co_await prism::run(loop, "127.0.0.1", port, configure, options);
}
