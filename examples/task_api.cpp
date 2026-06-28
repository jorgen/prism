#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <prism/app.h>
#include <prism/json.h>
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
} // namespace

int main()
{
  std::println("prism {} task api on http://127.0.0.1:8080", prism::version());
  std::println("  GET    /health");
  std::println("  GET    /tasks");
  std::println("  POST   /tasks            {{\"title\":\"...\"}}");
  std::println("  GET    /tasks/{{id}}");
  std::println("  PUT    /tasks/{{id}}       {{\"title\":\"...\",\"done\":true}}");
  std::println("  DELETE /tasks/{{id}}");
  std::println("  GET    /slow/{{ms}}");

  return vio::run(
    [](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      auto store = std::make_shared<store_t>();
      prism::app_t app;

      app.get("/health",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::text(prism::status_t::ok, "ok");
              });

      app.get("/tasks",
              [store](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::json::respond(prism::status_t::ok, task_list_t{store->tasks});
              });

      app.post("/tasks",
               [store](prism::request_t request) -> vio::task_t<prism::response_t>
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
               });

      app.get("/tasks/{id}",
              [store](prism::request_t request) -> vio::task_t<prism::response_t>
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
              });

      app.put("/tasks/{id}",
              [store](prism::request_t request) -> vio::task_t<prism::response_t>
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
              });

      app.del("/tasks/{id}",
              [store](prism::request_t request) -> vio::task_t<prism::response_t>
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
              });

      app.get("/slow/{ms}",
              [&loop](prism::request_t request) -> vio::task_t<prism::response_t>
              {
                int ms = 0;
                if (!parse_int(request.param("ms"), ms))
                {
                  co_return error_response(prism::status_t::bad_request, "invalid duration");
                }
                co_await vio::sleep(loop, std::chrono::milliseconds{ms});
                co_return prism::response_t::text(prism::status_t::ok, "slept " + std::to_string(ms) + "ms");
              });

      prism::keepalive_options_t options;
      options.idle_timeout = std::chrono::seconds{30};
      options.max_connections = 1024;

      auto result = co_await app.listen(loop, "127.0.0.1", 8080, nullptr, options);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
