// item_cache.cpp: fetches items as CC2 does, GET <avatar_marketplace_server>/live/<title id>/avataritems/<guid>.acp
// with X-Marketplace-Key, on a thread of its own; a reply is checked before it is stored as <guid>.bin.

#include "item_cache.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "closet.h"
#include "xe_compat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

REXCVAR_DEFINE_STRING(avatar_marketplace_server, "https://avatars.jm-studios.org", "Kernel",
                      "Server that other players' avatar items are downloaded from.");
REXCVAR_DEFINE_STRING(avatar_marketplace_key, "", "Kernel",
                      "Key sent to the avatar marketplace server; empty uses the one the installer seals into the exe.");
REXCVAR_DEFINE_STRING(avatar_cache_dir, "", "Kernel",
                      "Folder for other players' downloaded avatar items; empty = avatar_cache in avatar_data_dir.");

namespace rex {
namespace avatars {
namespace item_cache {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxItem = 2u << 20;                   // the largest known item is about 314 KB
constexpr auto kRetryAfter = std::chrono::seconds(30);  // after no answer or a server error

std::mutex g_mutex;
std::condition_variable g_wake;  // the worker: a job was queued
std::condition_variable g_done;  // the waiters: a job ended
std::deque<std::pair<std::string, uint32_t>> g_queue;  // guid and title id
std::set<std::string> g_busy;                          // queued or downloading
std::set<std::string> g_missing;                       // refused or unusable: not asked again this run
std::map<std::string, Clock::time_point> g_retry;      // not asked again before then
std::filesystem::path g_dir;

std::filesystem::path Dir() {
  std::lock_guard lock(g_mutex);
  if (g_dir.empty()) {
    std::string dir = REXCVAR_GET(avatar_cache_dir);
    if (!dir.empty()) {
      g_dir = dir;
    } else if (!(dir = rex::cvar::Query<std::string>("avatar_data_dir")).empty()) {
      g_dir = std::filesystem::path(dir) / "avatar_cache";
    } else {
      g_dir = REX_KERNEL_STATE()->emulator()->user_data_root() / "avatars" / "avatar_cache";
    }
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
  }
  return g_dir;
}

std::filesystem::path PathOf(const std::string& guid) { return Dir() / (guid + ".bin"); }

// jm-pack appends [key XOR pad][u32 length]["JMKEY1"] to the shipped exe; obfuscation, not secrecy.
std::string SealedKey() {
  static const char kPad[] = "JMstudios.marketplace.v1 JMstudi";
  constexpr size_t kPadSize = sizeof(kPad) - 1;
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!n || n == MAX_PATH) return {};
  std::ifstream exe(std::filesystem::path(path), std::ios::binary);
  unsigned char tail[10];
  if (!exe.seekg(-10, std::ios::end) || !exe.read(reinterpret_cast<char*>(tail), 10) ||
      std::memcmp(tail + 4, "JMKEY1", 6) != 0) {
    return {};
  }
  const uint32_t size = tail[0] | uint32_t(tail[1]) << 8 | uint32_t(tail[2]) << 16 | uint32_t(tail[3]) << 24;
  if (!size || size > 256 || !exe.seekg(-std::streamoff(10 + size), std::ios::end)) return {};
  std::string key(size, char(0));
  if (!exe.read(key.data(), size)) return {};
  for (size_t i = 0; i < key.size(); ++i) key[i] = char(key[i] ^ kPad[i % kPadSize]);
  return key;
}

std::string Key() {
  static const std::string sealed = SealedKey();
  const std::string key = REXCVAR_GET(avatar_marketplace_key);
  return key.empty() ? sealed : key;
}

// A GET with the marketplace key: the HTTP status (0 when unreachable) and at most kMaxItem + 1 bytes of body.
int Get(const std::string& url_text, std::string& body) {
  const std::wstring url(url_text.begin(), url_text.end());
  wchar_t host[256] = {}, path[1024] = {};
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return 0;
  static const HINTERNET http = WinHttpOpen(L"CrashCourse1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  const HINTERNET connection = http ? WinHttpConnect(http, host, parts.nPort, 0) : nullptr;
  const HINTERNET request =
      connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                 : nullptr;
  int status = 0;
  if (request) {
    WinHttpSetTimeouts(request, 5000, 5000, 10000, 20000);
    const std::string key = Key();
    const std::wstring headers =
        key.empty() ? std::wstring() : L"X-Marketplace-Key: " + std::wstring(key.begin(), key.end()) + L"\r\n";
    if (WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                           headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, code_size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code,
                          &code_size, nullptr);
      status = int(code);
      char buffer[16384];
      DWORD read = 0;
      while (body.size() <= kMaxItem && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) {
        body.append(buffer, read);
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  return status;
}

// An STRB, bare or behind the 0x140-byte YTGR header, whose blocks all lie inside it and which carries a model or a
// hiding shape.
bool Usable(const std::string& body) {
  const size_t start = body.compare(0, 4, "YTGR") == 0 ? 0x140 : 0;
  if (body.size() > kMaxItem || body.size() < start + 30 || body.compare(start, 4, "STRB") != 0) return false;
  const auto* s = reinterpret_cast<const uint8_t*>(body.data()) + start;
  const size_t size = body.size() - start;
  const bool aligned = s[4] != 0, little = s[5] != 0;
  const size_t id_size = s[22], size_size = s[23], align = aligned ? s[26] : 1;
  if (id_size < 1 || id_size > 8 || size_size < 1 || size_size > 8 || align < 1 || align > 64) return false;
  auto up = [&](uint64_t v) { return (v + align - 1) / align * align; };
  auto value = [&](size_t at, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v |= uint64_t(s[at + (little ? i : bytes - 1 - i)]) << (8 * i);
    return v;
  };
  const size_t header = size_t(up(id_size + 2 * size_size));
  bool drawable = false;
  int blocks = 0;
  for (size_t at = size_t(up(aligned ? 30 : 26)); at + header <= size && blocks < 256; ++blocks) {
    const uint64_t id = value(at, id_size), count = value(at + id_size, size_size),
                   entry = value(at + id_size + size_size, size_size);
    at += header;
    if (count > size || entry > size || count * entry > size - at || up(count) > size - at + align) return false;
    drawable |= id == 3 || id == 4;  // kModel, kShapeOverrides
    at += size_t(up(count));
  }
  return drawable;
}

enum class Outcome { kStored, kRefused, kFailed };

Outcome Download(const std::string& guid, uint32_t title_id) {
  std::string server = REXCVAR_GET(avatar_marketplace_server);
  while (!server.empty() && server.back() == '/') server.pop_back();
  if (server.empty()) return Outcome::kRefused;
  std::string body;
  const int status = Get(server + fmt::format("/live/{:08x}/avataritems/{}.acp", title_id, guid), body);
  if (status != 200) {
    REXKRNL_WARN("[avatar] item {} not downloaded: status {}", guid, status);
    return status >= 400 && status < 500 ? Outcome::kRefused : Outcome::kFailed;
  }
  if (!Usable(body)) {
    REXKRNL_WARN("[avatar] item {} not stored: {} bytes that are not an avatar item", guid, body.size());
    return Outcome::kRefused;
  }
  // Written under a name of its own first, so a reader or another game never sees half a file.
  const std::filesystem::path path = PathOf(guid);
  std::filesystem::path part = path;
  part += fmt::format(".{}.part", GetCurrentProcessId());
  bool stored;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(body.data(), std::streamsize(body.size()));
    stored = bool(out);
  }
  stored = stored && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!stored) {
    std::error_code ec;
    std::filesystem::remove(part, ec);
    REXKRNL_WARN("[avatar] item {} could not be written to {}", guid, path.string());
    return Outcome::kFailed;
  }
  REXKRNL_INFO("[avatar] downloaded item {} ({} bytes)", guid, body.size());
  return Outcome::kStored;
}

void Worker() {
  std::unique_lock lock(g_mutex);
  for (;;) {
    g_wake.wait(lock, [] { return !g_queue.empty(); });
    const auto [guid, title_id] = g_queue.front();
    g_queue.pop_front();
    lock.unlock();
    const Outcome outcome = Download(guid, title_id);
    lock.lock();
    g_busy.erase(guid);
    if (outcome == Outcome::kRefused) g_missing.insert(guid);
    if (outcome == Outcome::kFailed) g_retry[guid] = Clock::now() + kRetryAfter;
    g_done.notify_all();
  }
}

}  // namespace

void Fetch(const X_AVATAR_METADATA& metadata, int timeout_ms) {
  std::vector<const X_AVATAR_COMPONENT_INFO*> slots{&metadata.body_component, &metadata.head_component};
  for (const auto& component : metadata.components) slots.push_back(&component);
  static const uint8_t kZeroTail[8] = {};
  std::vector<std::string> wanted;
  for (const X_AVATAR_COMPONENT_INFO* slot : slots) {
    const AssetId& id = slot->asset_id;
    // Stock ids and zero-tail links resolve in the pack; closet items are on disk already.
    if (id.is_zero() || IsStockPackId(id) || !std::memcmp(id.d, kZeroTail, sizeof(kZeroTail)) ||
        GetCloset().Find(id)) {
      continue;
    }
    const std::string guid = id.to_string();
    std::error_code ec;
    if (std::filesystem::exists(PathOf(guid), ec)) continue;
    std::lock_guard lock(g_mutex);
    if (g_missing.count(guid)) continue;
    if (const auto retry = g_retry.find(guid); retry != g_retry.end() && Clock::now() < retry->second) continue;
    if (g_busy.insert(guid).second) {
      g_queue.emplace_back(guid, uint32_t(id.d[4]) << 24 | uint32_t(id.d[5]) << 16 | uint32_t(id.d[6]) << 8 |
                                     uint32_t(id.d[7]));
    }
    wanted.push_back(guid);
  }
  if (wanted.empty()) return;
  static std::once_flag started;
  std::call_once(started, [] { std::thread(Worker).detach(); });
  g_wake.notify_all();
  std::unique_lock lock(g_mutex);
  g_done.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    for (const std::string& guid : wanted) {
      if (g_busy.count(guid)) return false;
    }
    return true;
  });
}

bool Read(const AssetId& id, std::vector<uint8_t>& out) {
  std::ifstream in(PathOf(id.to_string()), std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamsize size = in.tellg();
  if (size <= 0 || uint64_t(size) > kMaxItem) return false;
  out.resize(size_t(size));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char*>(out.data()), size));
}

}  // namespace item_cache
}  // namespace avatars
}  // namespace rex
