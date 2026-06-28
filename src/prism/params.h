#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vio/task.h>

#include "error.h"
#include "http.h"
#include "json.h"
#include "router.h"
#include "status.h"

namespace prism
{
template <std::size_t N>
struct fixed_string_t
{
  char data[N]{};
  constexpr fixed_string_t(const char (&text)[N])
  {
    std::copy_n(text, N, data);
  }
  [[nodiscard]] constexpr std::string_view view() const
  {
    return std::string_view(data, N - 1);
  }
};

template <fixed_string_t Name, typename T>
struct path_t
{
  T value{};
};

template <fixed_string_t Name, typename T>
struct query_t
{
  T value{};
};

template <typename T>
struct body_t
{
  T value{};
};

template <typename T, typename = void>
struct param_codec_t
{
  static_assert(sizeof(T) == 0, "no prism::param_codec_t<T> for this parameter type; specialize prism::param_codec_t");
};

template <>
struct param_codec_t<std::string>
{
  static result_t<std::string> parse(std::string_view in)
  {
    return std::string(in);
  }
};

template <>
struct param_codec_t<bool>
{
  static result_t<bool> parse(std::string_view in)
  {
    if (in == "true" || in == "1")
    {
      return true;
    }
    if (in == "false" || in == "0")
    {
      return false;
    }
    return fail(status_t::bad_request, "expected a boolean");
  }
};

template <typename T>
struct param_codec_t<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
{
  static result_t<T> parse(std::string_view in)
  {
    T value{};
    const char *end = in.data() + in.size();
    auto [ptr, ec] = std::from_chars(in.data(), end, value);
    if (ec != std::errc{} || ptr != end)
    {
      return fail(status_t::bad_request, "expected an integer");
    }
    return value;
  }
};

namespace detail
{
template <typename T>
struct is_optional_t : std::false_type
{
};
template <typename U>
struct is_optional_t<std::optional<U>> : std::true_type
{
};

template <typename F>
struct function_traits_t : function_traits_t<decltype(&F::operator())>
{
};
template <typename R, typename... A>
struct function_traits_t<R(A...)>
{
  using args = std::tuple<A...>;
};
template <typename R, typename... A>
struct function_traits_t<R (*)(A...)> : function_traits_t<R(A...)>
{
};
template <typename C, typename R, typename... A>
struct function_traits_t<R (C::*)(A...) const> : function_traits_t<R(A...)>
{
};
template <typename C, typename R, typename... A>
struct function_traits_t<R (C::*)(A...)> : function_traits_t<R(A...)>
{
};

template <typename P>
struct arg_builder_t
{
  static_assert(sizeof(P) == 0, "unsupported handler parameter type; use prism::path_t, prism::query_t, prism::body_t, or request_t");
};

template <fixed_string_t Name, typename T>
struct arg_builder_t<path_t<Name, T>>
{
  static result_t<path_t<Name, T>> build(const request_t &request)
  {
    auto parsed = param_codec_t<T>::parse(request.param(Name.view()));
    if (!parsed)
    {
      return std::unexpected(parsed.error());
    }
    return path_t<Name, T>{std::move(*parsed)};
  }
};

template <fixed_string_t Name, typename T>
struct arg_builder_t<query_t<Name, T>>
{
  static result_t<query_t<Name, T>> build(const request_t &request)
  {
    if constexpr (is_optional_t<T>::value)
    {
      using inner_t = typename T::value_type;
      if (!request.has_query(Name.view()))
      {
        return query_t<Name, T>{std::nullopt};
      }
      auto parsed = param_codec_t<inner_t>::parse(request.query(Name.view()));
      if (!parsed)
      {
        return std::unexpected(parsed.error());
      }
      return query_t<Name, T>{std::move(*parsed)};
    }
    else
    {
      if (!request.has_query(Name.view()))
      {
        return fail(status_t::bad_request, "missing required query parameter");
      }
      auto parsed = param_codec_t<T>::parse(request.query(Name.view()));
      if (!parsed)
      {
        return std::unexpected(parsed.error());
      }
      return query_t<Name, T>{std::move(*parsed)};
    }
  }
};

template <typename T>
struct arg_builder_t<body_t<T>>
{
  static result_t<body_t<T>> build(const request_t &request)
  {
    auto parsed = json::parse<T>(request.body);
    if (!parsed)
    {
      return std::unexpected(parsed.error());
    }
    return body_t<T>{std::move(*parsed)};
  }
};

template <>
struct arg_builder_t<request_t>
{
  static result_t<request_t> build(const request_t &request)
  {
    return request;
  }
};

template <typename P>
P build_or_record(const request_t &request, std::optional<error_t> &error)
{
  static_assert(std::is_default_constructible_v<P>, "prism handler parameter types must be default-constructible");
  auto built = arg_builder_t<P>::build(request);
  if (built)
  {
    return std::move(*built);
  }
  if (!error)
  {
    error = built.error();
  }
  return P{};
}

inline vio::task_t<response_t> ready_response(response_t response)
{
  co_return response;
}

template <typename Handler, typename ArgsTuple, typename BoundTuple, std::size_t... BoundIndex, std::size_t... ExtractIndex>
handler_t make_typed_impl(Handler handler, BoundTuple bound, std::index_sequence<BoundIndex...>, std::index_sequence<ExtractIndex...>)
{
  constexpr std::size_t base = sizeof...(BoundIndex);
  static_assert((std::is_same_v<std::tuple_element_t<base + ExtractIndex, ArgsTuple>, std::remove_cvref_t<std::tuple_element_t<base + ExtractIndex, ArgsTuple>>> && ...),
                "prism typed handler parameters must be taken by value (drop the &/&&/const& on path_t, query_t, body_t, or request_t parameters)");
  return [handler = std::move(handler), bound = std::move(bound)](request_t request) -> vio::task_t<response_t>
  {
    std::optional<error_t> error;
    std::tuple<std::tuple_element_t<base + ExtractIndex, ArgsTuple>...> extracted{build_or_record<std::tuple_element_t<base + ExtractIndex, ArgsTuple>>(request, error)...};
    if (error)
    {
      return ready_response(response_t::text(error->code, error->msg));
    }
    return std::apply([&](auto &&...extracted_args) { return handler(std::get<BoundIndex>(bound)..., std::move(extracted_args)...); }, std::move(extracted));
  };
}

template <typename Handler, typename... Bound>
handler_t make_typed_handler(Handler handler, Bound... bound)
{
  using traits_t = function_traits_t<std::decay_t<Handler>>;
  using args_t = typename traits_t::args;
  constexpr std::size_t total = std::tuple_size_v<args_t>;
  constexpr std::size_t bound_count = sizeof...(Bound);
  static_assert(bound_count <= total, "more bound arguments than handler parameters");
  return make_typed_impl<Handler, args_t>(std::move(handler), std::tuple<std::decay_t<Bound>...>(std::move(bound)...), std::make_index_sequence<bound_count>{}, std::make_index_sequence<total - bound_count>{});
}

template <typename P>
struct path_param_name_t
{
  static constexpr bool present = false;
};
template <fixed_string_t Name, typename T>
struct path_param_name_t<path_t<Name, T>>
{
  static constexpr bool present = true;
  static constexpr std::string_view value = Name.view();
  static constexpr auto name = Name;
};

constexpr bool pattern_declares_param(std::string_view pattern, std::string_view name)
{
  std::size_t i = 0;
  while (i < pattern.size())
  {
    if (pattern[i] == '/')
    {
      ++i;
      continue;
    }
    std::size_t start = i;
    while (i < pattern.size() && pattern[i] != '/')
    {
      ++i;
    }
    std::string_view segment = pattern.substr(start, i - start);
    if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}' && segment.substr(1, segment.size() - 2) == name)
    {
      return true;
    }
  }
  return false;
}

template <typename ArgsTuple, std::size_t Base, std::size_t... ExtractIndex>
std::optional<std::string> verify_path_params(std::string_view pattern, std::index_sequence<ExtractIndex...>)
{
  std::optional<std::string> error;
  (
    [&]
    {
      using param_t = std::tuple_element_t<Base + ExtractIndex, ArgsTuple>;
      if constexpr (path_param_name_t<param_t>::present)
      {
        if (!error && !pattern_declares_param(pattern, path_param_name_t<param_t>::value))
        {
          error = std::string("route '") + std::string(pattern) + "': handler binds path parameter '" + std::string(path_param_name_t<param_t>::value) + "' that the route pattern does not declare";
        }
      }
    }(),
    ...);
  return error;
}

template <typename Handler, std::size_t BoundCount>
std::optional<std::string> verify_routes(std::string_view pattern)
{
  using args_t = typename function_traits_t<std::decay_t<Handler>>::args;
  constexpr std::size_t total = std::tuple_size_v<args_t>;
  return verify_path_params<args_t, BoundCount>(pattern, std::make_index_sequence<total - BoundCount>{});
}

template <fixed_string_t Pattern, fixed_string_t Name>
struct require_declared_param_t
{
  static_assert(pattern_declares_param(Pattern.view(), Name.view()), "static route verification: this handler binds a path_t<\"name\"> parameter that the route pattern does not declare");
};

template <fixed_string_t Pattern, typename P>
constexpr void verify_one_static()
{
  if constexpr (path_param_name_t<P>::present)
  {
    (void)require_declared_param_t<Pattern, path_param_name_t<P>::name>{};
  }
}

template <fixed_string_t Pattern, typename ArgsTuple, std::size_t Base, std::size_t... ExtractIndex>
constexpr void verify_path_params_static(std::index_sequence<ExtractIndex...>)
{
  (verify_one_static<Pattern, std::tuple_element_t<Base + ExtractIndex, ArgsTuple>>(), ...);
}

template <fixed_string_t Pattern, typename Handler, std::size_t BoundCount>
constexpr void verify_routes_static()
{
  using args_t = typename function_traits_t<std::decay_t<Handler>>::args;
  constexpr std::size_t total = std::tuple_size_v<args_t>;
  verify_path_params_static<Pattern, args_t, BoundCount>(std::make_index_sequence<total - BoundCount>{});
}
} // namespace detail
} // namespace prism
