// Game traffic between lobby members through the JMstudios UDP relay (relay.h).

#include "relay.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/logging.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace cc1::relay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char kMagic[4] = {'C', 'C', '2', 'R'};  // the relay's framing, shared with CC2
constexpr uint8_t kHello = 1, kData = 2, kHelloAck = 3;
constexpr size_t kMaxPayload = 1400;
constexpr size_t kMaxQueued = 512;
constexpr auto kHelloEvery = std::chrono::seconds(5);  // the relay drops a seat after 60 s without one

std::mutex g_control;  // serialises Start and Stop
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<int> g_pid{0};
std::atomic<void (*)()> g_hook{nullptr};
std::atomic<void (*)(uint8_t, const uint8_t*, size_t)> g_control_hooks[4]{};

std::mutex g_socket_mutex;  // guards g_socket and g_relay_addr
SOCKET g_socket = INVALID_SOCKET;
sockaddr_in g_relay_addr{};

std::mutex g_queue_mutex;
std::deque<Datagram> g_queue;

// Traffic since Start, for the log.
std::atomic<uint32_t> g_sent{0}, g_received{0};

bool Resolve(const std::string& relay, sockaddr_in& out) {
  const size_t colon = relay.rfind(':');
  if (colon == std::string::npos) return false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(relay.substr(0, colon).c_str(), relay.substr(colon + 1).c_str(), &hints, &found) != 0 || !found) {
    return false;
  }
  std::memcpy(&out, found->ai_addr, sizeof(out));
  freeaddrinfo(found);
  return true;
}

void SendHello(const std::string& token) {
  std::string packet(kMagic, sizeof(kMagic));
  packet.push_back(char(kHello));
  packet += token;
  std::lock_guard lock(g_socket_mutex);
  if (g_socket == INVALID_SOCKET) return;
  sendto(g_socket, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr*>(&g_relay_addr),
         sizeof(g_relay_addr));
}

void Queue(uint8_t src, uint16_t port, const uint8_t* data, size_t size) {
  if (port == kControlPort) {
    for (auto& slot : g_control_hooks) {
      if (auto hook = slot.load()) hook(src, data, size);
    }
    return;
  }
  if (g_received++ == 0) REXLOG_INFO("[relay] first game datagram from seat {} ({} bytes)", src, size);
  {
    std::lock_guard lock(g_queue_mutex);
    if (g_queue.size() >= kMaxQueued) g_queue.pop_front();  // the game stopped reading: keep the newest
    g_queue.push_back({src, port, std::vector<uint8_t>(data, data + size)});
  }
  if (auto hook = g_hook.load()) hook();
}

void Worker(std::string relay, std::string token, int seat) {
  sockaddr_in addr{};
  while (!g_stop && !Resolve(relay, addr)) {
    REXLOG_WARN("[relay] can't resolve {}; retrying in 5 s", relay);
    for (int i = 0; i < 20 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (g_stop) return;
  const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    REXLOG_ERROR("[relay] socket() failed ({})", WSAGetLastError());
    return;
  }
  {
    std::lock_guard lock(g_socket_mutex);
    g_socket = sock;
    g_relay_addr = addr;
  }
  REXLOG_INFO("[relay] seat {} at {}", seat, relay);

  const auto started = Clock::now();
  auto next_hello = started;
  bool acked = false, warned = false;
  uint8_t members = 0;
  auto next_report = started + std::chrono::seconds(30);
  uint8_t buffer[2048];
  while (!g_stop) {
    const auto now = Clock::now();
    if (now >= next_hello) {
      SendHello(token);
      next_hello = now + kHelloEvery;
    }
    if (now >= next_report) {
      next_report = now + std::chrono::seconds(30);
      if (g_sent || g_received) REXLOG_INFO("[relay] game datagrams: {} sent, {} received", g_sent.load(), g_received.load());
    }
    if (!acked && !warned && now - started > std::chrono::seconds(15)) {
      warned = true;
      REXLOG_WARN("[relay] no answer after 15 s: the relay is down or refused the ticket");
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock, &readable);
    timeval timeout{0, 250000};
    if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
    sockaddr_in from{};
    int from_size = sizeof(from);
    const int got = recvfrom(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_size);
    if (got < 5 || std::memcmp(buffer, kMagic, sizeof(kMagic)) != 0) continue;
    if (from.sin_addr.s_addr != addr.sin_addr.s_addr || from.sin_port != addr.sin_port) continue;
    if (buffer[4] == kHelloAck && got >= 7) {
      if (!acked) REXLOG_INFO("[relay] connected: seat {}, members {:#04x}", buffer[5], buffer[6]);
      if (acked && buffer[6] != members) REXLOG_INFO("[relay] members {:#04x} -> {:#04x}", members, buffer[6]);
      acked = true;
      members = buffer[6];
      g_pid = buffer[5];  // the relay's word on the seat wins
    } else if (buffer[4] == kData && got > 8) {
      Queue(buffer[5], uint16_t((buffer[6] << 8) | buffer[7]), buffer + 8, size_t(got - 8));
    }
  }
}

}  // namespace

void Start(const std::string& relay, const std::string& token, int pid) {
  Stop();
  std::lock_guard lock(g_control);
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  g_stop = false;
  g_pid = pid;
  g_sent = 0;
  g_received = 0;
  g_thread = std::thread(Worker, relay, token, pid);
}

void Stop() {
  std::lock_guard lock(g_control);
  if (!g_thread.joinable()) return;
  g_stop = true;
  g_thread.join();
  {
    std::lock_guard socket_lock(g_socket_mutex);
    if (g_socket != INVALID_SOCKET) closesocket(g_socket);
    g_socket = INVALID_SOCKET;
  }
  {
    std::lock_guard queue_lock(g_queue_mutex);
    g_queue.clear();
  }
  g_pid = 0;
  REXLOG_INFO("[relay] stopped");
}

int Pid() { return g_pid; }

bool Send(uint8_t dst_pid, uint16_t port, const uint8_t* data, size_t size) {
  if (!data || size == 0 || size > kMaxPayload || dst_pid > kMaxPid) return false;
  uint8_t packet[8 + kMaxPayload];
  std::memcpy(packet, kMagic, sizeof(kMagic));
  packet[4] = kData;
  packet[5] = dst_pid;
  packet[6] = uint8_t(port >> 8);
  packet[7] = uint8_t(port);
  std::memcpy(packet + 8, data, size);
  std::lock_guard lock(g_socket_mutex);
  if (g_socket == INVALID_SOCKET) return false;
  if (g_sent++ == 0) REXLOG_INFO("[relay] first game datagram to seat {} ({} bytes)", dst_pid, size);
  return sendto(g_socket, reinterpret_cast<const char*>(packet), int(8 + size), 0,
                reinterpret_cast<const sockaddr*>(&g_relay_addr), sizeof(g_relay_addr)) > 0;
}

bool Pop(Datagram& out) {
  std::lock_guard lock(g_queue_mutex);
  if (g_queue.empty()) return false;
  out = std::move(g_queue.front());
  g_queue.pop_front();
  return true;
}

void SetReceiveHook(void (*hook)()) { g_hook = hook; }

bool SendControl(const uint8_t* data, size_t size, uint8_t dst_pid) {
  if (!data || size == 0 || size > kMaxPayload) return false;
  uint8_t packet[8 + kMaxPayload];
  std::memcpy(packet, kMagic, sizeof(kMagic));
  packet[4] = kData;
  packet[5] = dst_pid;
  packet[6] = uint8_t(kControlPort >> 8);
  packet[7] = uint8_t(kControlPort);
  std::memcpy(packet + 8, data, size);
  std::lock_guard lock(g_socket_mutex);
  if (g_socket == INVALID_SOCKET) return false;
  return sendto(g_socket, reinterpret_cast<const char*>(packet), int(8 + size), 0,
                reinterpret_cast<const sockaddr*>(&g_relay_addr), sizeof(g_relay_addr)) > 0;
}

void AddControlHook(void (*hook)(uint8_t, const uint8_t*, size_t)) {
  for (auto& slot : g_control_hooks) {
    void (*none)(uint8_t, const uint8_t*, size_t) = nullptr;
    if (slot.compare_exchange_strong(none, hook)) return;
  }
}

void FillXnAddr(uint8_t pid, uint8_t* xnaddr) {
  const uint8_t octet = pid ? pid : kNoSeatOctet;
  const uint8_t ip[4] = {10, 66, 0, octet};
  std::memset(xnaddr, 0, 36);
  std::memcpy(xnaddr, ip, 4);       // ina
  std::memcpy(xnaddr + 4, ip, 4);   // inaOnline
  xnaddr[8] = uint8_t(kGamePort >> 8);  // wPortOnline, network order
  xnaddr[9] = uint8_t(kGamePort);
  const uint8_t mac[6] = {0x02, 0xCC, 0x1A, 0x00, 0x00, octet};
  std::memcpy(xnaddr + 10, mac, 6);  // abEnet
  std::memcpy(xnaddr + 16, ip, 4);   // abOnline
}

void FillLocalXnAddr(uint8_t* xnaddr) { FillXnAddr(uint8_t(Pid()), xnaddr); }

bool IsPeerAddress(uint32_t in_addr, uint8_t* octet) {
  if ((in_addr & 0xFFFFFF00u) != 0x0A420000u) return false;
  if (octet) *octet = uint8_t(in_addr);
  return true;
}

}  // namespace cc1::relay
