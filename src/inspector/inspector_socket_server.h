// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#pragma once

#include "inspector_agent.h"
#include "inspector_socket.h"

#include <map>
#include <string>
#include <vector>
#include <unordered_map>

namespace inspector {

class InspectorSocketServer;
class SocketSession;
class ServerSocket;

class InspectorAgentDelegate {
 public:
  InspectorAgentDelegate();
  void StartSession(int session_id, const std::string &target_id);
  void MessageReceived(int session_id, const std::string &message);
  void EndSession(int session_id);
  std::vector<std::string> GetTargetIds();
  std::string GetTargetTitle(const std::string &id);
  std::string GetTargetUrl(const std::string &id);
  void AddTarget(std::shared_ptr<AgentImpl> agent);

 private:
  std::unordered_map<std::string, std::shared_ptr<AgentImpl>> targets_map_;
  std::unordered_map<int, std::shared_ptr<AgentImpl>> session_targets_map_;
};

// HTTP Server, writes messages requested as TransportActions, and responds
// to HTTP requests and WS upgrades.

class InspectorSocketServer {
public:
  InspectorSocketServer(std::unique_ptr<InspectorAgentDelegate>&& delegate, int port,
    FILE* out = stderr);
  ~InspectorSocketServer();

  // Start listening on host/port
  bool Start();

  void Stop();
  void Send(int session_id, const std::string& message);
  void TerminateConnections();
  int Port() const;

  void AddTarget(std::shared_ptr<AgentImpl> agent);

  // Session connection lifecycle
  void Accept(std::shared_ptr<tcp_connection> connection, int server_port/*, uv_stream_t* server_socket*/);
  bool HandleGetRequest(int session_id, const std::string& host, const std::string& path);
  void SessionStarted(int session_id, const std::string& target_id, const std::string& ws_id);
  void SessionTerminated(int session_id);
  void MessageReceived(int session_id, const std::string& message) {
    delegate_->MessageReceived(session_id, message);
  }
  SocketSession* Session(int session_id);

  static void InspectorSocketServer::SocketConnectedCallback(std::shared_ptr<tcp_connection> connection, void* callbackData_);
  static void InspectorSocketServer::SocketClosedCallback(void* callbackData_);

private:
  static void CloseServerSocket(ServerSocket*);
  
  void SendListResponse(InspectorSocket* socket, const std::string& host, SocketSession* session);
  std::string GetFrontendURL(bool is_compat, const std::string &formatted_address);
  bool TargetExists(const std::string& id);

  enum class ServerState { kNew, kRunning, kStopping, kStopped };
  std::unique_ptr<InspectorAgentDelegate> delegate_;
  const std::string host_;
  int port_;

  std::shared_ptr<tcp_server> tcp_server_;
  
  int next_session_id_;
  FILE* out_;
  ServerState state_;

  std::map<int, std::pair<std::string, std::unique_ptr<SocketSession>>> connected_sessions_;

  };

}  // namespace inspector
