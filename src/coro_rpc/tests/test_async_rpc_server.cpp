/*
 * Copyright (c) 2022, Alibaba Group Holding Limited;
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <array>
#include <bitset>
#include <coro_rpc/coro_rpc/async_rpc_server.hpp>
#include <variant>

#include "ServerTester.hpp"
#include "coro_rpc/coro_rpc/rpc_protocol.h"
#include "doctest.h"
#include "rpc_api.hpp"
#include "struct_pack/struct_pack.hpp"

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace coro_rpc;
using namespace std::string_literals;

struct AsyncServerTester : public ServerTester {
  AsyncServerTester(TesterConfig config)
      : ServerTester(config),
        server(2, config.port, config.conn_timeout_duration) {
#ifdef ENABLE_SSL
    if (use_ssl) {
      server.init_ssl_context(
          ssl_configure{"../openssl_files", "server.crt", "server.key"});
    }
#endif
    if (async_start) {
      auto ec = server.async_start();
      REQUIRE(ec == std::errc{});
    }
    else {
      thd = std::thread([this]() {
        auto ec = server.start();
        REQUIRE(ec == std::errc{});
      });
    }
    CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
  }
  ~AsyncServerTester() {
    if (async_start) {
    }
    else {
      server.stop();
      thd.join();
    }
  }
  void test_all() override {
    ELOGV(INFO, "run %s", __func__);
    test_server_start_again();
    g_action = {};
    ServerTester::test_all();
    test_function_not_registered();
    g_action = {};
    test_start_new_server_with_same_port();
    this->test_call_with_delay_func<async_fun_with_delay_return_void>();
    this->test_call_with_delay_func<async_fun_with_delay_return_void_twice>();
    if (enable_heartbeat) {
      this->test_call_with_delay_func_server_timeout_due_to_heartbeat<
          async_fun_with_delay_return_void_cost_long_time>();
    }
    this->test_call_with_delay_func<async_fun_with_delay_return_string>();
    this->test_call_with_delay_func<async_fun_with_delay_return_string_twice>();
  }
  void register_all_function() override {
    g_action = {};
    ELOGV(INFO, "run %s", __func__);
    server.regist_handler<async_hi, large_arg_fun, client_hello>();
    server.regist_handler<long_run_func>();
    server.regist_handler<&ns_login::LoginService::login>(&login_service_);
    server.regist_handler<&HelloService::hello>(&hello_service_);
    server.regist_handler<hello>();
    server.regist_handler<async_fun_with_delay_return_void>();
    server.regist_handler<async_fun_with_delay_return_void_twice>();
    server.regist_handler<async_fun_with_delay_return_void_cost_long_time>();
    server.regist_handler<async_fun_with_delay_return_string>();
    server.regist_handler<async_fun_with_delay_return_string_twice>();
  }
  void remove_all_rpc_function() override {
    g_action = {};
    ELOGV(INFO, "run %s", __func__);
    server.remove_handler<async_hi>();
    server.remove_handler<large_arg_fun>();
    server.remove_handler<client_hello>();
    server.remove_handler<long_run_func>();
    server.remove_handler<&ns_login::LoginService::login>();
    server.remove_handler<hello>();
    server.remove_handler<&HelloService::hello>();
    server.remove_handler<async_fun_with_delay_return_void>();
    server.remove_handler<async_fun_with_delay_return_void_twice>();
    server.remove_handler<async_fun_with_delay_return_void_cost_long_time>();
    server.remove_handler<async_fun_with_delay_return_string>();
    server.remove_handler<async_fun_with_delay_return_string_twice>();
  }

  void test_function_not_registered() {
    g_action = {};
    server.remove_handler<async_hi>();
    auto client = create_client();
    ELOGV(INFO, "run %s, client_id %d", __func__, client->get_client_id());
    auto ret = call<async_hi>(client);
    REQUIRE_MESSAGE(
        ret.error().code == std::errc::function_not_supported,
        std::to_string(client->get_client_id()).append(ret.error().msg));
    REQUIRE(client->has_closed() == true);
    ret = call<async_hi>(client);
    CHECK(client->has_closed() == true);
    ret = call<async_hi>(client);
    REQUIRE_MESSAGE(ret.error().code == std::errc::io_error, ret.error().msg);
    CHECK(client->has_closed() == true);
    server.regist_handler<async_hi>();
  }

  void test_server_start_again() {
    ELOGV(INFO, "run %s", __func__);
    std::errc ec;
    if (async_start) {
      ec = server.async_start();
    }
    else {
      ec = server.start();
    }
    CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
    CHECK_MESSAGE(ec == std::errc::io_error, make_error_code(ec).message());
  }
  void test_start_new_server_with_same_port() {
    ELOGV(INFO, "run %s", __func__);
    auto new_server = async_rpc_server(2, std::stoi(this->port_));
    std::errc ec;
    if (async_start) {
      ec = new_server.async_start();
    }
    else {
      ec = new_server.start();
    }
    CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
    CHECK_MESSAGE(ec == std::errc::address_in_use,
                  make_error_code(ec).message());
  }
  async_rpc_server server;
  std::thread thd;
};

TEST_CASE("testing async rpc server") {
  ELOGV(INFO, "run testing async rpc server");
  unsigned short server_port = 8820;
  auto conn_timeout_duration = 300ms;

  // 0:async_start, 1:enable_heartbeat,
  // 2:use_outer_io_context, 3:use_ssl
  std::array<std::bitset<4U>, 8U> switch_list{0b0000, 0b0001, 0b0010, 0b0011,
                                              0b1000, 0b1001, 0b1010, 0b1011};

  for (auto& list : switch_list) {
    TesterConfig config;
    config.async_start = list[0];
    config.enable_heartbeat = list[1];
    config.use_outer_io_context = list[2];
    config.use_ssl = list[3];
    config.sync_client = false;
    config.port = server_port;
    if (config.enable_heartbeat) {
      config.conn_timeout_duration = conn_timeout_duration;
    }
    std::stringstream ss;
    ss << config;
    ELOGV(INFO, "%s, config: %s", list.to_string().data(), ss.str().data());
    AsyncServerTester(config).run();
  }
}
// #ifndef _MSC_VER
// TEST_CASE("testing signal handler") {
//   auto pid = fork();
//   if (pid == 0) {
//     async_rpc_server server(2, 8820);
//     auto ec = server.start();
//     CHECK_MESSAGE(ec == std::errc{}, make_error_code(ec).message());
//     std::exit(2);
//   }
//   else {
//     std::this_thread::sleep_for(100ms);
//     int status = 10;
//     kill(pid, SIGINT);
//     wait(&status);
//     // TODO: sometimes return 1 not 2, maybe disturbed by sanitizer, will be
//     // reverted later.
//     CHECK(WEXITSTATUS(status) != 0);
//   }
// }
// #endif

TEST_CASE("testing async rpc server stop") {
  ELOGV(INFO, "run testing async rpc server stop");
  async_rpc_server server(2, 8820);
  auto ec = server.async_start();
  REQUIRE(ec == std::errc{});
  CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
  SUBCASE("stop twice") {
    server.stop();
    server.stop();
  }
  SUBCASE("stop in different thread") {
    std::thread thd1([&server]() {
      server.stop();
    });
    std::thread thd2([&server]() {
      server.stop();
    });
    thd1.join();
    thd2.join();
  }
}

TEST_CASE("testing async rpc write error") {
  ELOGV(INFO, "run testing async rpc write error");
  g_action = inject_action::force_inject_connection_close_socket;
  async_rpc_server server(2, 8820);

  server.regist_handler<hi>();
  auto ec = server.async_start();
  REQUIRE(ec == std::errc{});
  CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
  coro_rpc_client client(g_client_id++);
  ELOGV(INFO, "client_id %d, run testing async rpc write error",
        client.get_client_id());
  ec = syncAwait(client.connect("127.0.0.1", "8820"));
  REQUIRE_MESSAGE(ec == std::errc{},
                  std::to_string(client.get_client_id())
                      .append(make_error_code(ec).message()));
  auto ret = syncAwait(client.call<hi>());
  REQUIRE_MESSAGE(
      ret.error().code == std::errc::io_error,
      std::to_string(client.get_client_id()).append(ret.error().msg));
  REQUIRE(client.has_closed() == true);
  g_action = inject_action::nothing;
  server.remove_handler<hi>();
}

TEST_CASE("test server write queue") {
  ELOGV(INFO, "run test server write queue");
  g_action = {};
  async_rpc_server server(2, 8820);
  server.remove_handler<async_fun_with_delay_return_void_cost_long_time>();
  server.regist_handler<async_fun_with_delay_return_void_cost_long_time>();
  auto ec = server.async_start();
  REQUIRE(ec == std::errc{});
  CHECK_MESSAGE(server.wait_for_start(3s), "server start timeout");
  constexpr auto id =
      func_id<async_fun_with_delay_return_void_cost_long_time>();
  std::size_t offset = RPC_HEAD_LEN + FUNCTION_ID_LEN;
  std::vector<std::byte> buffer;
  buffer.resize(offset);
  std::memcpy(buffer.data() + RPC_HEAD_LEN, &id, FUNCTION_ID_LEN);
  rpc_header header{magic_number};
  header.seq_num = g_client_id++;
  ELOGV(INFO, "client_id %d begin to connect %d", header.seq_num, 8820);
  header.length = buffer.size() - RPC_HEAD_LEN;
  struct_pack::serialize_to((char*)buffer.data(), RPC_HEAD_LEN, header);
  asio::io_context io_context;
  std::thread thd([&io_context]() {
    asio::io_context::work work(io_context);
    io_context.run();
  });
  asio::ip::tcp::socket socket(io_context);
  auto ret = asio_util::connect(io_context, socket, "127.0.0.1", "8820");
  CHECK(!ret);
  ELOGV(INFO, "%d client_id %d call %s", "sync_client", header.seq_num,
        "coro_fun_with_delay_return_void_cost_long_time");
  for (int i = 0; i < 10; ++i) {
    auto err =
        asio_util::write(socket, asio::buffer(buffer.data(), buffer.size()));
    CHECK(err.second == buffer.size());
  }
  for (int i = 0; i < 10; ++i) {
    std::byte resp_len_buf[RESPONSE_HEADER_LEN];
    std::monostate r;
    auto buf = struct_pack::serialize<std::string>(r);
    std::string buffer_read;
    buffer_read.resize(buf.size());
    asio_util::read(socket, asio::buffer(resp_len_buf, RESPONSE_HEADER_LEN));

    rpc_header header{};
    [[maybe_unused]] auto errc = struct_pack::deserialize_to(
        header, (const char*)resp_len_buf, RESPONSE_HEADER_LEN);
    uint32_t body_len = header.length;

    CHECK(body_len == buf.size());
    asio_util::read(socket, asio::buffer(buffer_read, body_len));
    std::monostate r2;
    std::size_t sz;
    auto ret =
        struct_pack::deserialize_to(r2, buffer_read.data(), body_len, sz);
    CHECK(ret == struct_pack::errc::ok);
    CHECK(sz == body_len);
    CHECK(r2 == r);
  }

  ELOGV(INFO, "client_id %d close", header.seq_num);
  asio::error_code ignored_ec;
  socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_ec);
  socket.close(ignored_ec);
  io_context.stop();
  thd.join();
  server.stop();
  server.remove_handler<async_fun_with_delay_return_void_cost_long_time>();
}
