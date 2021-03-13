// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <boost/asio.hpp>

#include <queue>
#include <memory>

namespace inspector {

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
public:
  boost::asio::ip::tcp::socket& socket();

  typedef void(*ReadCallback)(std::vector<char>&, bool iseof, void*data);
  inline void registerReadCallback(ReadCallback callback, void*data) { readcallback_ = callback; callbackData_ = data; }

  void read_loop_async();
  void write_async(std::vector<char>&&);
  void close();

  inline tcp_connection(boost::asio::ip::tcp::socket socket)
      : socket_(std::move(socket)) {}
private:
  void do_write(bool cont);

private:
  int port_;

  boost::asio::ip::tcp::socket socket_;
  std::string message_;

  /// Buffer for incoming data.
  std::array<char, 8192> buffer_;

  void* callbackData_;
  ReadCallback readcallback_;

  std::mutex queueAccessMutex;
  std::queue<std::vector<char>> outQueue;

  std::vector<char> messageToWrite_;
  bool writing_{ false };
};

class tcp_server : public std::enable_shared_from_this<tcp_server> {
public:
  typedef void(*ConnectionCallback)(std::shared_ptr<tcp_connection> connection, void* callbackData_);

  void run();
  void stop();

  tcp_server(int port, ConnectionCallback callback, void* data);

private:
  void do_accept();

private:
  boost::asio::io_service io_service_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::ip::tcp::socket socket_;

  void* callbackData_;
  ConnectionCallback connectioncallback_;
};

}
