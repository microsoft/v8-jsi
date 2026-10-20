#ifndef V8HOST_COMMON_V8HOST_PAYLOAD_IDENTITY_H_
#define V8HOST_COMMON_V8HOST_PAYLOAD_IDENTITY_H_

#include "v8host_file_identity.h"
#include "../protocol/v8host_protocol.h"

#include <array>
#include <string>
#include <vector>

namespace v8host {

constexpr uint16_t kV8HostWireMajor = 2;
constexpr uint16_t kV8HostWireMinor = 0;
static_assert(kV8HostWireMajor == protocol::kWireVersionMajor);
static_assert(kV8HostWireMinor == protocol::kWireVersionMinor);

enum class BrokerMode : uint8_t { kDedicated = 0, kShared = 1 };

struct PayloadIdentity {
  HeldFile container;
  HeldFile plugin;
  uint16_t native_machine = 0;
  std::array<uint8_t, 32> plugin_set_id = {};
  std::wstring install_root;
};

bool ResolvePayloadIdentity(const std::wstring& directory,
                            const std::wstring& container_name,
                            const std::wstring& plugin_name,
                            PayloadIdentity* out,
                            DWORD* error);
bool ComputePluginSetId(uint16_t machine,
                        const std::array<uint8_t, 32>& container_hash,
                        const std::array<uint8_t, 32>& plugin_hash,
                        std::array<uint8_t, 32>* out);
// Derives the install root used for the shared co-location check. Release
// layout is `<root>\<native-arch>\sbox.exe`, so the root is the parent of the
// architecture directory whose name must match the container's PE machine. A
// non-architecture parent is accepted as the root only in developer builds
// (SBOX_DEV_ALLOW_UNSIGNED); release builds fail closed. Never trust a
// client-supplied root.
bool DeriveInstallRoot(const std::wstring& container_final_path,
                       uint16_t machine,
                       std::wstring* out);
// The machine this module was compiled for; a client DLL loaded by an x86
// process is I386, by an x64 or ARM64EC process AMD64, by an ARM64 process ARM64.
#if defined(_M_ARM64)
constexpr uint16_t kModuleMachine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64)
constexpr uint16_t kModuleMachine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
constexpr uint16_t kModuleMachine = IMAGE_FILE_MACHINE_I386;
#else
#error "unsupported client architecture"
#endif
// The OS native machine (IsWow64Process2); false if it can't be determined.
bool QueryNativeMachine(uint16_t* machine);
// Locates the directory of the native payload for a client DLL in
// `module_dir`. A client of the native architecture uses its own directory;
// any other client (x86 on x64, x64 or ARM64EC on ARM64) uses the sibling
// `<parent>\<native-arch>`. Only x64 and ARM64 payloads exist: any other native
// machine (32-bit Windows) fails, and there is no fallback.
bool ResolveNativePayloadDirectory(const std::wstring& module_dir,
                                   uint16_t module_machine,
                                   uint16_t native_machine,
                                   std::wstring* out);
// V2 hashes the NUL-terminated ASCII domain V8HOST-RENDEZVOUS-V2,
// LE16 SID byte length, SID, plugin-set ID, LE32 root byte length, UTF-16LE
// root, mode byte, and (dedicated only) nonce. The root comes from HeldFile's
// normalized volume-GUID final path on both sides (including cross-arch clients);
// separators are backslashes, trailing separators removed, and case folded by
// invariant uppercase. DOS/UNC extended prefixes are normalized too; the
// volume-GUID prefix is retained.
bool DeriveEndpoint(const std::vector<uint8_t>& sid,
                    const std::array<uint8_t, 32>& plugin_set_id,
                    const std::wstring& install_root,
                    BrokerMode mode,
                    const std::array<uint8_t, 16>* nonce,
                    std::wstring* endpoint,
                    std::array<uint8_t, 32>* key);
bool GenerateNonce(std::array<uint8_t, 16>* nonce);
std::string HexPrefix(const std::array<uint8_t, 32>& value, size_t bytes);

}  // namespace v8host

#endif  // V8HOST_COMMON_V8HOST_PAYLOAD_IDENTITY_H_
