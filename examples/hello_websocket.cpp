#include <memory>
#include <print>
#include <string>

#include <prism/app.h>
#include <prism/prism.h>
#include <prism/websocket.h>

#include <vio/run.h>

namespace
{
vio::task_t<void> echo(std::shared_ptr<prism::ws_connection_t> connection)
{
  co_await connection->send_text("welcome to the prism websocket echo");
  for (;;)
  {
    prism::ws_message_t message = co_await connection->receive();
    if (!message.ok)
    {
      break;
    }
    co_await connection->send_text("echo: " + message.data);
  }
}

constexpr std::string_view page = R"(<!doctype html><meta charset=utf-8><title>prism ws</title>
<input id=m placeholder="say something"><button onclick=send()>send</button><pre id=log></pre>
<script>
const ws = new WebSocket("ws://" + location.host + "/ws");
const log = t => document.getElementById('log').textContent += t + "\n";
ws.onopen = () => log("[open]");
ws.onmessage = e => log("< " + e.data);
ws.onclose = () => log("[close]");
function send(){ const m = document.getElementById('m'); log("> " + m.value); ws.send(m.value); m.value=""; }
</script>)";
} // namespace

int main(int argc, char **argv)
{
  std::uint16_t port = 8080;
  if (argc > 1)
  {
    port = static_cast<std::uint16_t>(std::atoi(argv[1]));
  }
  std::println("prism {} — websocket echo on http://127.0.0.1:{} (open in a browser, or: websocat ws://127.0.0.1:{}/ws)", prism::version(), port, port);

  return vio::run(
    [port](vio::event_loop_t &loop) -> vio::task_t<int>
    {
      prism::app_t app;
      app.ws("/ws", echo);
      app.get("/",
              [](prism::request_t) -> vio::task_t<prism::response_t>
              {
                co_return prism::response_t::finished(prism::status_t::ok, "text/html", std::string(page));
              });

      auto result = co_await app.listen(loop, "127.0.0.1", port);
      if (!result.has_value())
      {
        std::println(stderr, "listen failed: {}", result.error().msg);
        co_return 1;
      }
      co_return 0;
    });
}
