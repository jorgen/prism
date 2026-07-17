#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <vio/crypto.h>
#include <vio/event_loop.h>
#include <vio/task.h>
#include <vio/unique_buf.h>

#include "../../http.h"
#include "../../server_options.h"
#include "../../websocket.h"
#include "../io_timeout.h"
#include "frame.h"

namespace prism::detail::websocket
{
template <typename Transport>
struct ws_state_t
{
  ws_state_t(Transport transport_arg, vio::event_loop_t &el, server_options_t o, request_t req, bool client_mode = false)
    : transport(std::move(transport_arg))
    , loop(el)
    , options(o)
    , request(std::move(req))
    , client(client_mode)
  {
    const std::size_t cap = options.max_body_bytes != 0 ? options.max_body_bytes : std::size_t{16} * 1024 * 1024;
    max_message = cap;
    reader.set_max_frame_size(cap);
    reader.set_expect_masked(!client);
  }

  // A client masks every frame it sends (RFC 6455 §5.3); a server sends unmasked.
  std::string make_frame(opcode_t opcode, std::string_view payload, bool fin = true)
  {
    if (client)
    {
      std::uint8_t key[4] = {0, 0, 0, 0};
      auto filled = vio::crypto::random_bytes(std::span<std::uint8_t>(key, 4));
      (void)filled;
      return serialize_masked_frame(opcode, payload, key, fin);
    }
    return serialize_frame(opcode, payload, fin);
  }

  std::string make_close(std::uint16_t code, std::string_view reason)
  {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xff));
    payload.push_back(static_cast<char>(code & 0xff));
    payload.append(reason);
    return make_frame(opcode_t::close, payload, true);
  }

  Transport transport;
  vio::event_loop_t &loop;
  server_options_t options;
  request_t request;
  bool client = false;

  frame_reader_t reader;
  std::size_t max_message = std::size_t{16} * 1024 * 1024;

  std::deque<std::string> out_queue;
  bool writing = false;
  bool write_dead = false;
  bool sent_close = false;

  std::deque<ws_message_t> in_queue;
  std::coroutine_handle<> recv_waiter{};
  bool closed = false;

  bool in_fragment = false;
  opcode_t fragment_opcode = opcode_t::text;
  std::string fragment_buf;
};

template <typename Transport>
void wake_receiver(ws_state_t<Transport> &state)
{
  if (state.recv_waiter)
  {
    std::coroutine_handle<> handle = state.recv_waiter;
    state.recv_waiter = {};
    handle.resume();
  }
}

template <typename Transport>
void request_flush(std::shared_ptr<ws_state_t<Transport>> state)
{
  if (state->writing || state->write_dead)
  {
    return;
  }
  [](std::shared_ptr<ws_state_t<Transport>> st) -> vio::detached_task_t
  {
    st->writing = true;
    while (!st->out_queue.empty())
    {
      std::string out = std::move(st->out_queue.front());
      st->out_queue.pop_front();
      auto outcome = co_await st->transport.write(std::move(out), st->options.write_timeout);
      if (outcome != write_outcome_t::ok)
      {
        st->write_dead = true;
        st->out_queue.clear();
        break;
      }
    }
    st->writing = false;
  }(std::move(state));
}

template <typename Transport>
void enqueue_out(const std::shared_ptr<ws_state_t<Transport>> &state, std::string frame)
{
  if (state->write_dead)
  {
    return;
  }
  state->out_queue.push_back(std::move(frame));
  request_flush(state);
}

template <typename Transport>
void queue_close_once(const std::shared_ptr<ws_state_t<Transport>> &state, std::uint16_t code)
{
  if (state->sent_close || state->write_dead)
  {
    return;
  }
  state->sent_close = true;
  enqueue_out(state, state->make_close(code, ""));
}

template <typename Transport>
void deliver_message(const std::shared_ptr<ws_state_t<Transport>> &state, opcode_t opcode, std::string data)
{
  ws_message_t message;
  message.data = std::move(data);
  message.is_text = opcode == opcode_t::text;
  message.ok = true;
  state->in_queue.push_back(std::move(message));
  wake_receiver(*state);
}

template <typename Transport>
bool process_frames(const std::shared_ptr<ws_state_t<Transport>> &state)
{
  while (state->reader.has_frame())
  {
    frame_t frame = state->reader.take_frame();
    if (frame.opcode == opcode_t::ping)
    {
      enqueue_out(state, state->make_frame(opcode_t::pong, frame.payload));
      continue;
    }
    if (frame.opcode == opcode_t::pong)
    {
      continue;
    }
    if (frame.opcode == opcode_t::close)
    {
      std::uint16_t code = close_code::normal;
      if (frame.payload.size() >= 2)
      {
        code = static_cast<std::uint16_t>((static_cast<std::uint8_t>(frame.payload[0]) << 8) | static_cast<std::uint8_t>(frame.payload[1]));
      }
      queue_close_once(state, code);
      return false;
    }
    if (frame.opcode == opcode_t::continuation)
    {
      if (!state->in_fragment)
      {
        queue_close_once(state, close_code::protocol_error);
        return false;
      }
      state->fragment_buf.append(frame.payload);
      if (frame.fin)
      {
        deliver_message(state, state->fragment_opcode, std::move(state->fragment_buf));
        state->in_fragment = false;
        state->fragment_buf.clear();
      }
    }
    else
    {
      if (state->in_fragment)
      {
        queue_close_once(state, close_code::protocol_error);
        return false;
      }
      if (frame.fin)
      {
        deliver_message(state, frame.opcode, std::move(frame.payload));
      }
      else
      {
        state->in_fragment = true;
        state->fragment_opcode = frame.opcode;
        state->fragment_buf = std::move(frame.payload);
      }
    }
    if (state->fragment_buf.size() > state->max_message)
    {
      queue_close_once(state, close_code::message_too_big);
      return false;
    }
  }
  return true;
}

template <typename Transport>
vio::task_t<void> read_pump(std::shared_ptr<ws_state_t<Transport>> state)
{
  bool running = process_frames(state);
  while (running && !state->closed)
  {
    vio::unique_buf_t buffer;
    auto outcome = co_await state->transport.read(std::chrono::milliseconds::zero(), buffer);
    if (outcome != read_outcome_t::data)
    {
      break;
    }
    if (state->reader.feed(buffer->base, buffer->len) == frame_status_t::error)
    {
      queue_close_once(state, state->reader.error_close_code());
      break;
    }
    running = process_frames(state);
  }
  state->closed = true;
  wake_receiver(*state);
  co_return;
}

template <typename Transport>
struct receive_awaiter_t
{
  std::shared_ptr<ws_state_t<Transport>> state;
  [[nodiscard]] bool await_ready() const noexcept
  {
    return !state->in_queue.empty() || state->closed;
  }
  void await_suspend(std::coroutine_handle<> handle) const noexcept
  {
    state->recv_waiter = handle;
  }
  ws_message_t await_resume() const
  {
    if (!state->in_queue.empty())
    {
      ws_message_t message = std::move(state->in_queue.front());
      state->in_queue.pop_front();
      return message;
    }
    return ws_message_t{std::string{}, true, false};
  }
};

template <typename Transport>
class ws_connection_impl_t final : public ws_connection_t
{
public:
  explicit ws_connection_impl_t(std::shared_ptr<ws_state_t<Transport>> state)
    : _state(std::move(state))
  {
  }

  vio::task_t<bool> send_text(std::string_view text) override
  {
    if (_state->write_dead || _state->sent_close)
    {
      co_return false;
    }
    enqueue_out(_state, _state->make_frame(opcode_t::text, text));
    co_return !_state->write_dead;
  }

  vio::task_t<bool> send_binary(std::span<const std::byte> data) override
  {
    if (_state->write_dead || _state->sent_close)
    {
      co_return false;
    }
    std::string_view payload(reinterpret_cast<const char *>(data.data()), data.size());
    enqueue_out(_state, _state->make_frame(opcode_t::binary, payload));
    co_return !_state->write_dead;
  }

  vio::task_t<ws_message_t> receive() override
  {
    co_return co_await receive_awaiter_t<Transport>{_state};
  }

  void close(std::uint16_t code, std::string_view reason) override
  {
    if (!_state->sent_close && !_state->write_dead)
    {
      _state->sent_close = true;
      enqueue_out(_state, _state->make_close(code, reason));
    }
    if (_state->transport.reader)
    {
      _state->transport.reader->cancel();
    }
    _state->closed = true;
    wake_receiver(*_state);
  }

  const request_t &request() const override
  {
    return _state->request;
  }

private:
  std::shared_ptr<ws_state_t<Transport>> _state;
};

template <typename Transport>
vio::task_t<void> run_websocket(Transport transport, request_t request, ws_handler_t handler, vio::event_loop_t &loop, server_options_t options)
{
  auto state = std::make_shared<ws_state_t<Transport>>(std::move(transport), loop, options, std::move(request));
  auto connection = std::make_shared<ws_connection_impl_t<Transport>>(state);
  auto pump = read_pump(state);
  co_await handler(connection);
  connection->close(close_code::normal, "");
  co_await std::move(pump);
  co_return;
}
} // namespace prism::detail::websocket
