// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <asio.hpp>

#include <queue>
#include <atomic>
#include <condition_variable>
#include <memory>

namespace inspector {

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
public:
  asio::ip::tcp::socket& socket();

  typedef void(*ReadCallback)(std::vector<char>&, bool iseof, void*data);
  inline void registerReadCallback(ReadCallback callback, void*data) { readcallback_ = callback; callbackData_ = data; }

  void read_loop_async();
  void write_async(std::vector<char>&&);
  void close();

  inline tcp_connection(asio::ip::tcp::socket socket)
      : socket_(std::move(socket)) {}
private:
  friend class tcp_server;
  void do_write(bool cont);
  void eof();

private:
  asio::ip::tcp::socket socket_;
  asio::steady_timer close_timer_{socket_.get_executor()};
  std::string message_;

  /// Buffer for incoming data.
  std::array<char, 8192> buffer_;

  void* callbackData_{nullptr};
  ReadCallback readcallback_{nullptr};
  bool eof_{false};

  // Queue and write state are confined to the sole IO thread, including close.
  std::queue<std::vector<char>> outQueue;

  std::vector<char> messageToWrite_;
  bool writing_{ false };
};

class tcp_server : public std::enable_shared_from_this<tcp_server> {
public:
  typedef void(*ConnectionCallback)(std::shared_ptr<tcp_connection> connection, void* callbackData_);

  void run();
  void stop();
  void post(std::function<void()> callback);

  tcp_server(int port, ConnectionCallback callback, void* data);

private:
  void do_accept();
  void closeOnIoThread();

private:
  std::atomic<bool> stopped_{false};
  std::mutex stopped_mutex_;
  std::condition_variable stopped_cv_;
  std::vector<std::weak_ptr<tcp_connection>> connections_;
  size_t completed_posts_{0}; // IO-thread-only progress inside batched strand dispatch.
  asio::io_context io_context_;
  asio::strand<asio::io_context::executor_type> strand_{asio::make_strand(io_context_)};
  asio::ip::tcp::acceptor acceptor_;
  asio::ip::tcp::socket socket_;

  void* callbackData_;
  ConnectionCallback connectioncallback_;
};

}
