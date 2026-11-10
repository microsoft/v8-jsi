// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "v8-inspector.h"

#include "inspector_socket_server.h"
#include "inspector_tcp.h"
#include "inspector_utils.h"

#include "V8Windows.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace inspector {

namespace {

  void Escape(std::string* string) {
    for (char& c : *string) {
      c = (c == '\"' || c == '\\') ? '_' : c;
    }
  }

  std::string FormatHostPort(const std::string& host, int port) {
    // Host is valid (socket was bound) so colon means it's a v6 IP address
    bool v6 = host.find(':') != std::string::npos;
    std::ostringstream url;
    if (v6) {
      url << '[';
    }
    url << host;
    if (v6) {
      url << ']';
    }
    url << ':' << port;
    return url.str();
  }

  std::string FormatAddress(const std::string& host,
    const std::string& target_id,
    bool include_protocol) {
    std::ostringstream url;
    if (include_protocol)
      url << "ws://";
    url << host << '/' << target_id;
    return url.str();
  }

  std::string MapToString(const std::map<std::string, std::string>& object) {
    bool first = true;
    std::ostringstream json;
    json << "{\n";
    for (const auto& name_value : object) {
      if (!first)
        json << ",\n";
      first = false;
      json << "  \"" << name_value.first << "\": \"";
      json << name_value.second << "\"";
    }
    json << "\n} ";
    return json.str();
  }

  std::string MapsToString(
    const std::vector<std::map<std::string, std::string>>& array) {
    bool first = true;
    std::ostringstream json;
    json << "[ ";
    for (const auto& object : array) {
      if (!first)
        json << ", ";
      first = false;
      json << MapToString(object);
    }
    json << "]\n\n";
    return json.str();
  }

  const char* MatchPathSegment(const char* path, const char* expected) {
    size_t len = strlen(expected);
    if (utils::StringEqualNoCaseN(path, expected, len)) {
      if (path[len] == '/' || path[len] == '?') return path + len + 1;
      if (path[len] == '\0') return path + len;
    }
    return nullptr;
  }

  void SendHttpResponse(InspectorSocket* socket, const std::string& response) {
    const char HEADERS[] = "HTTP/1.0 200 OK\r\n"
      "Content-Type: application/json; charset=UTF-8\r\n"
      "Cache-Control: no-cache\r\n"
      "Content-Length: %zu\r\n"
      "\r\n";
    char header[sizeof(HEADERS) + 20];
    int header_len = snprintf(header, sizeof(header), HEADERS, response.size());
    socket->Write(header, header_len);
    socket->Write(response.data(), response.size());
  }

  void SendVersionResponse(InspectorSocket* socket) {
    std::map<std::string, std::string> response;
    response["Browser"] = "node.js v12.0.0";
    response["Protocol-Version"] = "1.1";
    SendHttpResponse(socket, MapToString(response));
  }

  void SendProtocolJson(InspectorSocket* socket) {
    SendHttpResponse(socket, "TBD");
  }
}  // namespace



class SocketSession {
public:
  SocketSession(InspectorSocketServer* server, int id, int server_port);
  void Close() {
    ws_socket_.reset();
  }
  void Send(const std::string& message);
  void Own(std::unique_ptr<InspectorSocket> ws_socket) {
    ws_socket_ = std::move(ws_socket);
  }
  int id() const { return id_; }
  int server_port() {
    return server_port_;
  }
  InspectorSocket* ws_socket() {
    return ws_socket_.get();
  }
  void Accept(const std::string& ws_key) {
    ws_socket_->AcceptUpgrade(ws_key);
  }
  void Decline() {
    ws_socket_->CancelHandshake();
  }

  class Delegate : public InspectorSocket::Delegate {
  public:
    Delegate(InspectorSocketServer* server, int session_id)
      : server_(server), session_id_(session_id) { }
    ~Delegate() {
      server_->SessionTerminated(session_id_);
    }
    void OnHttpGet(const std::string& host, const std::string& path) override;
    void OnSocketUpgrade(const std::string& host, const std::string& path,
      const std::string& ws_key) override;
    void OnWsFrame(const std::vector<char>& data) override;

  private:
    SocketSession* Session() {
      return server_->Session(session_id_);
    }

    InspectorSocketServer* server_;
    int session_id_;
  };

private:
  const int id_;
  std::unique_ptr<InspectorSocket> ws_socket_;
  const int server_port_;
};

InspectorSocketServer::InspectorSocketServer(
  std::unique_ptr<InspectorAgentDelegate>&& delegate, int port, FILE* out)
  : delegate_(std::move(delegate)), port_(port),
  next_session_id_(0), out_(out) {
  state_ = ServerState::kNew;
}

InspectorSocketServer::~InspectorSocketServer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = ServerState::kStopped;
    delegate_.reset();
  }
  // run() has returned; callbacks see an empty map during destruction.
  auto sessions = std::move(connected_sessions_);
  sessions.clear();
}

SocketSession* InspectorSocketServer::Session(int session_id) {
  auto it = connected_sessions_.find(session_id);
  return it == connected_sessions_.end() ? nullptr : it->second.second.get();

}

void InspectorSocketServer::SessionStarted(int session_id,
  const std::string& id,
  const std::string& ws_key) {
  std::shared_ptr<AgentImpl> agent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServerState::kStopped || !delegate_) return;
    SocketSession* session = Session(session_id);
    if (!session) return;
    if (!TargetExists(id)) {
      session->Decline();
      return;
    }
    connected_sessions_[session_id].first = id;
    session->Accept(ws_key);
    agent = delegate_->StartSession(session_id, id);
  }
  InspectorAgentDelegate::PostIncomingMessage(agent, session_id, TAG_CONNECT);
}

void InspectorSocketServer::SessionTerminated(int session_id) {
  auto session = connected_sessions_.extract(session_id);
  std::shared_ptr<AgentImpl> agent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ServerState::kStopped && delegate_ && !session.empty())
      agent = delegate_->EndSession(session_id);
  }
  InspectorAgentDelegate::PostIncomingMessage(agent, session_id, TAG_DISCONNECT);
}

void InspectorSocketServer::MessageReceived(int session_id, const std::string &message) {
  std::shared_ptr<AgentImpl> agent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServerState::kStopped || !delegate_) return;
    agent = delegate_->GetSession(session_id);
  }
  InspectorAgentDelegate::PostIncomingMessage(agent, session_id, message);
}

bool InspectorSocketServer::HandleGetRequest(int session_id,
  const std::string& host,
  const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == ServerState::kStopped || !delegate_) return false;
  SocketSession* session = Session(session_id);
  if (!session) return false;
  InspectorSocket* socket = session->ws_socket();
  const char* command = MatchPathSegment(path.c_str(), "/json");
  if (command == nullptr)
    return false;

  if (MatchPathSegment(command, "list") || command[0] == '\0') {
    SendListResponse(socket, host, session);
    return true;
  }
  else if (MatchPathSegment(command, "protocol")) {
    SendProtocolJson(socket);
    return true;
  }
  else if (MatchPathSegment(command, "version")) {
    SendVersionResponse(socket);
    return true;
  }
  return false;
}

void InspectorSocketServer::SendListResponse(InspectorSocket* socket,
  const std::string& host,
  SocketSession* session) {
  std::vector<std::map<std::string, std::string>> response;
  for (const std::string& id : delegate_->GetTargetIds()) {
    response.push_back(std::map<std::string, std::string>());
    std::map<std::string, std::string>& target_map = response.back();
    target_map["description"] = "node.js instance";
    target_map["faviconUrl"] = "https://nodejs.org/static/favicon.ico";
    target_map["id"] = id;
    target_map["title"] = delegate_->GetTargetTitle(id);
    Escape(&target_map["title"]);
    target_map["type"] = "node";
    // This attribute value is a "best effort" URL that is passed as a JSON
    // string. It is not guaranteed to resolve to a valid resource.
    target_map["url"] = delegate_->GetTargetUrl(id);
    Escape(&target_map["url"]);

    std::string detected_host = host;
    if (detected_host.empty()) {
      detected_host = FormatHostPort(socket->GetHost(),
        session->server_port());
    }
    std::string formatted_address = FormatAddress(detected_host, id, false);
    target_map["devtoolsFrontendUrl"] = GetFrontendURL(false,
      formatted_address);
    // The compat URL is for Chrome browsers older than 66.0.3345.0
    target_map["devtoolsFrontendUrlCompat"] = GetFrontendURL(true,
      formatted_address);
    target_map["webSocketDebuggerUrl"] = FormatAddress(detected_host, id, true);
  }
  SendHttpResponse(socket, MapsToString(response));
}

std::string InspectorSocketServer::GetFrontendURL(bool is_compat,
  const std::string &formatted_address) {
  std::ostringstream frontend_url;
  frontend_url << "devtools://devtools/bundled/";
  frontend_url << (is_compat ? "inspector" : "node_app");
  frontend_url << ".html?experiments=true&v8only=true&ws=";
  frontend_url << formatted_address;
  return frontend_url.str();
}

/*static */void InspectorSocketServer::SocketConnectedCallback(std::shared_ptr<tcp_connection> connection, void* callbackData_) {
  InspectorSocketServer* server = reinterpret_cast<InspectorSocketServer*>(callbackData_);
  server->Accept(connection, server->port_);
}

void InspectorSocketServer::AddTarget(std::shared_ptr<AgentImpl> agent) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == ServerState::kStopped || !delegate_) return;
  delegate_->AddTarget(agent);
}

void InspectorSocketServer::RemoveTarget(std::shared_ptr<AgentImpl> agent) {
  std::vector<int> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServerState::kStopped || !delegate_) return;
    sessions = delegate_->RemoveTarget(agent);
  }
  // Close on the IO strand even if the runtime has not dispatched CONNECT.
  for (int session_id : sessions) Send(session_id, "");
}

bool InspectorSocketServer::HasTargets() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == ServerState::kStopped || !delegate_) return false;
  return delegate_->HasTargets();
}

bool InspectorSocketServer::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK_NE(state_, ServerState::kStopped);
  state_ = ServerState::kRunning;

  std::thread([self = shared_from_this()]() {
    {
      std::unique_lock<std::mutex> tcp_server_lock(self->mutex_tcp_server_);
      if (self->tcp_server_stopped_) {
        return;
      }

      self->tcp_server_ = std::make_shared<tcp_server>(self->port_, InspectorSocketServer::SocketConnectedCallback, self.get());
    }

    self->tcp_server_->run();
  }).detach();
  return true;
}

void InspectorSocketServer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServerState::kStopped || !delegate_) return;
    CHECK_EQ(state_, ServerState::kRunning);
    state_ = ServerState::kStopped;
    delegate_.reset();
  }
  std::shared_ptr<tcp_server> server;
  {
    std::lock_guard<std::mutex> lock(mutex_tcp_server_);
    tcp_server_stopped_ = true;
    server = tcp_server_;
  }
  if (server) server->stop();
}

bool InspectorSocketServer::TargetExists(const std::string& id) {
  const std::vector<std::string>& target_ids = delegate_->GetTargetIds();
  const auto& found = std::find(target_ids.begin(), target_ids.end(), id);
  return found != target_ids.end();
}

int InspectorSocketServer::Port() const {
  // return socket_->port();
  return port_;
}

void InspectorSocketServer::Accept(std::shared_ptr<tcp_connection> connection, int server_port) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == ServerState::kStopped || !delegate_) return;
  std::unique_ptr<SocketSession> session(
    new SocketSession(this, next_session_id_++, server_port));

  auto delegate = std::make_unique<SocketSession::Delegate>(this, session->id());

  std::unique_ptr<InspectorSocket> inspector =
    InspectorSocket::Accept(connection, std::move(delegate));
  if (inspector) {
    session->Own(std::move(inspector));
    connected_sessions_[session->id()].second = std::move(session);
  }
}

void InspectorSocketServer::Send(int session_id, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_tcp_server_);
  if (!tcp_server_) return;
  // The io thread owns sessions, including empty-message close and destruction.
  tcp_server_->post([this, session_id, message] {
    if (auto *session = Session(session_id)) session->Send(message);
  });
}

// InspectorSession tracking
SocketSession::SocketSession(InspectorSocketServer* server, int id,
  int server_port)
  : id_(id), server_port_(server_port) {}

void SocketSession::Send(const std::string& message) {
  ws_socket_->Write(message.data(), message.length());
}

void SocketSession::Delegate::OnHttpGet(const std::string& host,
  const std::string& path) {
  if (!server_->HandleGetRequest(session_id_, host, path))
    Session()->ws_socket()->CancelHandshake();
}

void SocketSession::Delegate::OnSocketUpgrade(const std::string& host,
  const std::string& path,
  const std::string& ws_key) {
  std::string id = path.empty() ? path : path.substr(1);
  server_->SessionStarted(session_id_, id, ws_key);
}

void SocketSession::Delegate::OnWsFrame(const std::vector<char>& data) {
  server_->MessageReceived(session_id_,
    std::string(data.data(), data.size()));
}

}  // namespace inspector
