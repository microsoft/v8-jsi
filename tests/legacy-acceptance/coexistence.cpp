// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

extern "C" bool legacy_create(unsigned short);
extern "C" bool modern_create(unsigned short);
extern "C" bool legacy_eval();
extern "C" bool modern_eval();
extern "C" void legacy_destroy();
extern "C" void modern_destroy();

static unsigned short free_port() {
  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int size = sizeof(address);
  const bool ok = socket != INVALID_SOCKET && bind(socket, reinterpret_cast<sockaddr *>(&address), size) == 0 &&
    getsockname(socket, reinterpret_cast<sockaddr *>(&address), &size) == 0;
  if (socket != INVALID_SOCKET) closesocket(socket);
  return ok ? ntohs(address.sin_port) : 0;
}
static bool listening(unsigned short port) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool ok = socket != INVALID_SOCKET && connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    if (socket != INVALID_SOCKET) closesocket(socket);
    if (ok) return true;
    Sleep(20);
  }
  return false;
}
int main(int argc, char **argv) {
  if (argc != 2) return 1;
  const bool legacyFirst = std::strcmp(argv[1], "legacy-first") == 0;
  if (!legacyFirst && std::strcmp(argv[1], "modern-first") != 0) return 1;
  if (GetModuleHandleW(L"v8jsi.dll") || GetModuleHandleW(L"v8jsi2.dll")) {
    std::fprintf(stderr, "Engine modules must be delay-loaded to test both load orders\n"); return 2;
  }
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
  const auto legacyPort = free_port();
  auto modernPort = free_port();
  for (int attempt = 0; modernPort == legacyPort && attempt < 10; ++attempt) modernPort = free_port();
  if (!legacyPort || !modernPort || legacyPort == modernPort) return 3;
  const auto first = LoadLibraryW(legacyFirst ? L"v8jsi.dll" : L"v8jsi2.dll");
  if (!first || !(legacyFirst ? legacy_create(legacyPort) : modern_create(modernPort))) return 4;
  if (GetModuleHandleW(legacyFirst ? L"v8jsi2.dll" : L"v8jsi.dll")) return 4;
  const auto second = LoadLibraryW(legacyFirst ? L"v8jsi2.dll" : L"v8jsi.dll");
  if (!second || first == second || !(legacyFirst ? modern_create(modernPort) : legacy_create(legacyPort))) return 5;
  const bool ok = GetModuleHandleW(L"v8jsi.dll") != GetModuleHandleW(L"v8jsi2.dll") &&
    legacy_eval() && modern_eval() && listening(legacyPort) && listening(modernPort);
  legacy_destroy();
  const bool survivor = modern_eval();
  modern_destroy();
  const bool retained = GetModuleHandleW(L"v8jsi.dll") && GetModuleHandleW(L"v8jsi2.dll");
  WSACleanup();
  if (!ok || !survivor || !retained) return 6;
  std::printf("PASS: %s; distinct modules/inspector ports %u/%u; both evaluate and destroy\n", argv[1], legacyPort, modernPort);
  return 0;
}
