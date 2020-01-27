// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/asio.hpp>

#include <queue>

namespace inspector {

class tcp_connection : public boost::enable_shared_from_this<tcp_connection>
{
public:
  typedef boost::shared_ptr<tcp_connection> pointer;

  static pointer create(boost::asio::ip::tcp::socket socket);
  boost::asio::ip::tcp::socket& socket();

  typedef void(*ReadCallback)(std::vector<char>&, bool iseof, void*data);
  typedef void(*CloseCallback)(void*data);

  inline void registerCloseCallback(CloseCallback callback, void*data) { closecallback_ = callback; closeCallbackData_ = data; }
  inline void registerReadCallback(ReadCallback callback, void*data) { readcallback_ = callback; callbackData_ = data; }

  void read_loop_async();
  void write_async(std::vector<char>);
  void do_write(bool cont);
  void close();

private:
  inline tcp_connection(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)) {}

  int port_;

  boost::asio::ip::tcp::socket socket_;
  std::string message_;

  /// Buffer for incoming data.
  std::array<char, 8192> buffer_;

  void* callbackData_;
  ReadCallback readcallback_;

  void* closeCallbackData_;
  CloseCallback closecallback_;

  std::mutex queueAccessMutex;
  std::queue<std::vector<char>> outQueue;

  std::vector<char> messageToWrite_;
  bool writing_{ false };
};

class tcp_server : public boost::enable_shared_from_this<tcp_server> {
public:
  typedef std::shared_ptr<tcp_server> pointer;
  typedef void(*ConnectionCallback)(boost::shared_ptr<tcp_connection> connection, void* callbackData_);

  void run();

  static pointer create(int port, ConnectionCallback callback, void* data);

private:
  tcp_server(int port, ConnectionCallback callback, void* data);
  void do_accept();

private:

  boost::asio::io_service io_service_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::ip::tcp::socket socket_;

  void* callbackData_;
  ConnectionCallback connectioncallback_;
};

}
