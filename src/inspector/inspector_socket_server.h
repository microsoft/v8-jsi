// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include "inspector_agent.h"
#include "inspector_socket.h"

#include <map>
#include <string>
#include <vector>

namespace inspector {

class InspectorSocketServer;
class SocketSession;
class ServerSocket;

class SocketServerDelegate {
public:
  virtual void AssignServer(InspectorSocketServer* server) = 0;
  virtual void StartSession(int session_id, const std::string& target_id) = 0;
  virtual void EndSession(int session_id) = 0;
  virtual void MessageReceived(int session_id, const std::string& message) = 0;
  virtual std::vector<std::string> GetTargetIds() = 0;
  virtual std::string GetTargetTitle(const std::string& id) = 0;
  virtual std::string GetTargetUrl(const std::string& id) = 0;
  virtual ~SocketServerDelegate() {}
};

// HTTP Server, writes messages requested as TransportActions, and responds
// to HTTP requests and WS upgrades.

class InspectorSocketServer {
public:
  InspectorSocketServer(std::unique_ptr<SocketServerDelegate> delegate, int port,
    FILE* out = stderr);
  ~InspectorSocketServer();

  // Start listening on host/port
  bool Start();

  void Stop();
  void Send(int session_id, const std::string& message);
  void TerminateConnections();
  int Port() const;

  // Session connection lifecycle
  void Accept(boost::shared_ptr<tcp_connection> connection, int server_port/*, uv_stream_t* server_socket*/);
  bool HandleGetRequest(int session_id, const std::string& host, const std::string& path);
  void SessionStarted(int session_id, const std::string& target_id, const std::string& ws_id);
  void SessionTerminated(int session_id);
  void MessageReceived(int session_id, const std::string& message) {
    delegate_->MessageReceived(session_id, message);
  }
  SocketSession* Session(int session_id);
  //bool done() const {
  //  return server_sockets_.empty() && connected_sessions_.empty();
  //}

  static void InspectorSocketServer::SocketConnectedCallback(boost::shared_ptr<tcp_connection> connection, void* callbackData_);
  static void InspectorSocketServer::SocketClosedCallback(void* callbackData_);

private:
  static void CloseServerSocket(ServerSocket*);
  
  void SendListResponse(InspectorSocket* socket, const std::string& host, SocketSession* session);
  std::string GetFrontendURL(bool is_compat, const std::string &formatted_address);
  bool TargetExists(const std::string& id);

  enum class ServerState { kNew, kRunning, kStopping, kStopped };
  std::unique_ptr<SocketServerDelegate> delegate_;
  const std::string host_;
  int port_;
  
  int next_session_id_;
  FILE* out_;
  ServerState state_;

  std::map<int, std::pair<std::string, std::unique_ptr<SocketSession>>> connected_sessions_;

  };

}  // namespace inspector
