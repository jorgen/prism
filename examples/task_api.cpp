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

vio::task_t<prism::response_t> list_tasks(std::shared_ptr<store_t> store, prism::request_t request)
{
  std::string done = request.query("done");
  task_list_t result;
  for (const auto &task : store->tasks)
  {
    if (done.empty() || (done == "true" && task.done) || (done == "false" && !task.done))
    {
      result.tasks.push_back(task);
    }
  }
  co_return prism::json::respond(prism::status_t::ok, result);
}

vio::task_t<prism::response_t> create_task(std::shared_ptr<store_t> store, prism::request_t request)
{
  auto body = prism::json::parse<task_create_t>(request.body);
  if (!body.has_value())
  {
    co_return error_response(body.error().code, body.error().msg);
  }
  if (body->title.empty())
  {
    co_return error_response(prism::status_t::unprocessable_entity, "title must not be empty");
  }
  task_item_t created{store->next_id++, std::move(body->title), false};
  store->tasks.push_back(created);
  co_return prism::json::respond(prism::status_t::created, created);
}

vio::task_t<prism::response_t> get_task(std::shared_ptr<store_t> store, prism::request_t request)
{
  int id = 0;
  if (!parse_int(request.param("id"), id))
  {
    co_return error_response(prism::status_t::bad_request, "invalid task id");
  }
  if (task_item_t *task = find_task(*store, id))
  {
    co_return prism::json::respond(prism::status_t::ok, *task);
  }
  co_return error_response(prism::status_t::not_found, "task not found");
}

vio::task_t<prism::response_t> update_task(std::shared_ptr<store_t> store, prism::request_t request)
{
  int id = 0;
  if (!parse_int(request.param("id"), id))
  {
    co_return error_response(prism::status_t::bad_request, "invalid task id");
  }
  auto body = prism::json::parse<task_update_t>(request.body);
  if (!body.has_value())
  {
    co_return error_response(body.error().code, body.error().msg);
  }
  task_item_t *task = find_task(*store, id);
  if (task == nullptr)
  {
    co_return error_response(prism::status_t::not_found, "task not found");
  }
  task->title = std::move(body->title);
  task->done = body->done;
  co_return prism::json::respond(prism::status_t::ok, *task);
}

vio::task_t<prism::response_t> remove_task(std::shared_ptr<store_t> store, prism::request_t request)
{
  int id = 0;
  if (!parse_int(request.param("id"), id))
  {
    co_return error_response(prism::status_t::bad_request, "invalid task id");
  }
  for (auto it = store->tasks.begin(); it != store->tasks.end(); ++it)
  {
    if (it->id == id)
    {
      store->tasks.erase(it);
      co_return prism::response_t::text(prism::status_t::no_content, "");
    }
  }
  co_return error_response(prism::status_t::not_found, "task not found");
}

vio::task_t<prism::response_t> slow(prism::request_t request)
{
  int ms = 0;
  if (!parse_int(request.param("ms"), ms))
  {
    co_return error_response(prism::status_t::bad_request, "invalid duration");
  }
  co_await vio::sleep(*request.loop, std::chrono::milliseconds{ms});
  co_return prism::response_t::text(prism::status_t::ok, "slept " + std::to_string(ms) + "ms");
}

void configure(prism::app_t &app)
{
  auto store = std::make_shared<store_t>();

  app.logger().set_level(prism::log_level_t::debug);
  app.logger().set_sink(log_line);

  app.get("/health", health);
  app.get("/tasks", std::bind_front(list_tasks, store));
  app.post("/tasks", std::bind_front(create_task, store));
  app.get("/tasks/{id}", std::bind_front(get_task, store));
  app.put("/tasks/{id}", std::bind_front(update_task, store));
  app.del("/tasks/{id}", std::bind_front(remove_task, store));
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
