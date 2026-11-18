// The XNet and Winsock calls a LIVE session makes, answered for relayed play: every lobby seat is 10.66.0.<pid>, the
// port-1000 VDP socket's traffic goes through cc1::relay, and key exchange, connection setup and QoS are answered
// locally.

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include "../online.h"
#include "relay.h"

namespace cc1::net {
namespace {

using rex::u32;

constexpr uint32_t kXnAddrStatic = 0x04, kXnAddrGateway = 0x20, kXnAddrDns = 0x40, kXnAddrOnline = 0x80;
constexpr uint32_t kStatusPending = 0x103;  // what an outstanding overlapped reads as
constexpr uint32_t kIoPending = 997, kIoIncomplete = 996, kOperationAborted = 995;
constexpr uint32_t kWouldBlock = 10035, kNotSocket = 10038, kMsgSize = 10040, kInvalid = 10022, kInUse = 10048;
constexpr uint32_t kConnected = 2, kLost = 3;  // XNET_CONNECT_STATUS_*
constexpr uint8_t kQosComplete = 0x01, kQosContacted = 0x02;
constexpr uint32_t kLinkActive = 0x01, kLink100Mbps = 0x02, kLinkFullDuplex = 0x08;

struct WsaOverlapped {
  rex::be<uint32_t> internal;       // completion code, kStatusPending while outstanding
  rex::be<uint32_t> internal_high;  // bytes transferred
  rex::be<uint32_t> offset;
  rex::be<uint32_t> offset_high;
  rex::be<uint32_t> event_handle;
};
struct WsaBuf {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};
struct QosInfo {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min;
  rex::be<uint16_t> rtt_med;
  rex::be<uint32_t> up_bits;
  rex::be<uint32_t> down_bits;
};
static_assert(sizeof(QosInfo) == 24);

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}

// A receive the game keeps posted; its byte-count and flags pointers are on the caller's stack, so a late
// completion writes only the buffers, the sender and the overlapped.
struct PendingRecv {
  uint32_t overlapped = 0;
  uint32_t from = 0, from_len = 0;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest pointer, length
};

std::mutex g_mutex;  // guards the two maps, and every guest write the relay thread makes
std::unordered_map<uint32_t, PendingRecv> g_pending;
std::unordered_map<uint32_t, uint16_t> g_relayed;  // socket handle -> the game port it bound

void Complete(uint32_t overlapped_ptr, uint32_t code, uint32_t bytes) {
  auto* overlapped = Guest<WsaOverlapped>(overlapped_ptr);
  overlapped->internal = code;
  overlapped->internal_high = bytes;
  if (!overlapped->event_handle) return;
  if (auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(overlapped->event_handle)) {
    event->Set(0, false);
  }
}

void WriteSender(uint32_t from_ptr, uint32_t from_len_ptr, const relay::Datagram& datagram) {
  if (from_ptr) {
    auto* from = Guest<rex::system::XSOCKADDR_IN>(from_ptr);
    from->sin_family = 2;
    from->sin_port = datagram.port;
    from->sin_addr = 0x0A420000u | datagram.src_pid;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len_ptr) *Guest<rex::be<uint32_t>>(from_len_ptr) = 16;
}

// Copies a datagram into guest buffers; 'truncated' when they were too small.
uint32_t CopyOut(const std::vector<std::pair<uint32_t, uint32_t>>& buffers, const relay::Datagram& datagram,
                 bool& truncated) {
  size_t done = 0;
  for (const auto& [ptr, len] : buffers) {
    if (done >= datagram.data.size()) break;
    const size_t n = std::min<size_t>(len, datagram.data.size() - done);
    if (ptr && n) std::memcpy(Guest<uint8_t>(ptr), datagram.data.data() + done, n);
    done += n;
  }
  truncated = done < datagram.data.size();
  return uint32_t(done);
}

bool Relayed(uint32_t socket_handle, uint16_t* port = nullptr) {
  std::lock_guard lock(g_mutex);
  const auto it = g_relayed.find(socket_handle);
  if (it == g_relayed.end()) return false;
  if (port) *port = it->second;
  return true;
}

// Hands queued relay datagrams to posted receives; runs on guest threads and on the relay thread.
void Deliver() {
  std::lock_guard lock(g_mutex);
  while (true) {
    uint32_t target = 0;
    for (const auto& [handle, pending] : g_pending) {
      if (g_relayed.count(handle)) target = handle;
    }
    relay::Datagram datagram;
    if (!target || !relay::Pop(datagram)) return;
    PendingRecv pending = std::move(g_pending[target]);
    g_pending.erase(target);
    bool truncated = false;
    const uint32_t bytes = CopyOut(pending.buffers, datagram, truncated);
    WriteSender(pending.from, pending.from_len, datagram);
    Complete(pending.overlapped, truncated ? kMsgSize : 0, bytes);
  }
}

struct HookInstaller {
  HookInstaller() { relay::SetReceiveHook(&Deliver); }
} g_hook_installer;

// Sends a game datagram to the seat its destination address names; other addresses are dropped.
void Route(uint32_t to_ptr, uint16_t bound_port, const uint8_t* data, size_t size) {
  uint32_t address = 0xFFFFFFFFu;
  uint16_t port = bound_port;
  if (to_ptr) {
    const auto* to = Guest<rex::system::XSOCKADDR_IN>(to_ptr);
    address = to->sin_addr;
    port = to->sin_port;
  }
  uint8_t octet = 0;
  if (address == 0xFFFFFFFFu || (relay::IsPeerAddress(address, &octet) && octet == 255)) {
    relay::Send(0, port, data, size);
  } else if (relay::IsPeerAddress(address, &octet) && octet >= 1 && octet <= relay::kMaxPid) {
    relay::Send(octet, port, data, size);
  } else {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      REXLOG_WARN("[relay] dropped a game datagram to {}.{}.{}.{}:{} (not a lobby address)", address >> 24,
                  (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF, port);
    }
  }
}

rex::system::object_ref<rex::system::XSocket> Socket(uint32_t handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
  if (!socket) rex::system::XThread::SetLastError(kNotSocket);
  return socket;
}

// ---- XNet --------------------------------------------------------------------------------------------------------

// Session ids and keys come from here, so two machines must not draw the same bytes.
u32 XNetRandom(u32 caller, u32 buffer_ptr, u32 length) {
  if (buffer_ptr && length) online::RandomBytes(Guest<uint8_t>(buffer_ptr), length);
  return 0;
}

u32 XNetGetTitleXnAddr(u32 caller, u32 addr_ptr) {
  if (addr_ptr) relay::FillLocalXnAddr(Guest<uint8_t>(addr_ptr));
  return kXnAddrStatic | kXnAddrGateway | kXnAddrDns | (online::Live() ? kXnAddrOnline : 0);
}

u32 XNetXnAddrToInAddr(u32 caller, u32 xnaddr_ptr, u32 xnkid_ptr, u32 in_addr_ptr) {
  if (!xnaddr_ptr || !in_addr_ptr) return kInvalid;
  std::memcpy(Guest<uint8_t>(in_addr_ptr), Guest<uint8_t>(xnaddr_ptr), 4);
  return 0;
}

u32 XNetInAddrToXnAddr(u32 caller, u32 in_addr, u32 xnaddr_ptr, u32 xnkid_ptr) {
  uint8_t octet = 0;
  if (!relay::IsPeerAddress(in_addr, &octet)) return kInvalid;
  if (xnaddr_ptr) relay::FillXnAddr(octet, Guest<uint8_t>(xnaddr_ptr));
  if (xnkid_ptr) online::CurrentSessionId(Guest<uint8_t>(xnkid_ptr));
  return 0;
}

// The relay stands in for secure associations: a lobby address is connected as soon as it is asked for.
u32 XNetConnect(u32 caller, u32 in_addr) { return 0; }
u32 XNetGetConnectStatus(u32 caller, u32 in_addr) { return relay::IsPeerAddress(in_addr) ? kConnected : kLost; }
u32 XNetRegisterKey(u32 caller, u32 xnkid, u32 xnkey) { return 0; }
u32 XNetUnregisterKey(u32 caller, u32 xnkid) { return 0; }
u32 XNetReplaceKey(u32 caller, u32 old_xnkid, u32 new_xnkid) { return 0; }
u32 XNetUnregisterInAddr(u32 caller, u32 in_addr) { return 0; }

// One finished probe per target at 40 ms: the relay is the only path, so there is nothing to measure.
u32 XNetQosLookup(u32 caller, u32 xna_count, u32 xna_ptrs, u32 xnkid_ptrs, u32 xnkey_ptrs, u32 ina_count,
                  u32 ina_ptr, u32 service_ids, u32 probes, u32 bits_per_sec, u32 flags, u32 event_handle,
                  u32 qos_out_ptr) {
  if (!qos_out_ptr) return kInvalid;
  const uint32_t count = xna_count + ina_count;
  const uint32_t size = 8 + uint32_t(sizeof(QosInfo)) * std::max<uint32_t>(count, 1);
  const uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);  // XNetQosRelease frees it
  if (!qos) return 8;  // ERROR_NOT_ENOUGH_MEMORY
  std::memset(Guest<uint8_t>(qos), 0, size);
  *Guest<rex::be<uint32_t>>(qos) = count;  // cxnqos; cxnqosPending stays 0
  auto* info = Guest<QosInfo>(qos + 8);
  const uint16_t sent = uint16_t(std::clamp<uint32_t>(probes, 1, 16));
  for (uint32_t i = 0; i < count; ++i) {
    info[i].flags = kQosComplete | kQosContacted;
    info[i].probes_xmit = sent;
    info[i].probes_recv = sent;
    info[i].rtt_min = 40;
    info[i].rtt_med = 40;
    info[i].up_bits = 1000000;
    info[i].down_bits = 1000000;
  }
  *Guest<rex::be<uint32_t>>(qos_out_ptr) = qos;
  if (event_handle) {
    if (auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event_handle)) event->Set(0, false);
  }
  return 0;
}

// A host's QoS reply data has no reader, as lookups are answered locally.
u32 XNetQosListen(u32 caller, u32 xnkid, u32 data, u32 data_size, u32 bits_per_sec, u32 flags) { return 0; }

// QNet tells machines apart by this id.
u32 XNetXnAddrToMachineId(u32 caller, u32 addr_ptr, u32 id_ptr) {
  if (id_ptr) {
    *Guest<rex::be<uint32_t>>(id_ptr) = 0xFA00BABE;
    *Guest<rex::be<uint32_t>>(id_ptr + 4) = 0xCC1A0000u | (addr_ptr ? Guest<uint8_t>(addr_ptr)[3] : 0);
  }
  return 0;
}

u32 XNetGetEthernetLinkStatus(u32 caller) { return kLinkActive | kLink100Mbps | kLinkFullDuplex; }

// ---- Winsock -----------------------------------------------------------------------------------------------------

// The VDP port's socket is relayed and never bound on the host, so two instances on one PC both get port 1000.
u32 Bind(u32 caller, u32 socket_handle, u32 name_ptr, u32 name_len) {
  auto socket = Socket(socket_handle);
  if (!socket) return uint32_t(-1);
  rex::system::N_XSOCKADDR_IN native(Guest<rex::system::XSOCKADDR_IN>(name_ptr));
  const uint16_t port = native.sin_port;
  if (port == relay::kGamePort) {
    std::lock_guard lock(g_mutex);
    g_relayed[socket_handle] = port;
    REXLOG_INFO("[relay] game socket {:08X} on port {} is relayed", uint32_t(socket_handle), port);
    return 0;
  }
  if (XFAILED(socket->Bind(&native, int(name_len)))) {
    rex::system::XThread::SetLastError(kInUse);
    return uint32_t(-1);
  }
  return 0;
}

// XRNM keeps one overlapped receive posted and gives the session up if posting fails.
u32 WSARecvFrom(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count, u32 num_bytes_ptr, u32 flags_ptr,
                u32 from_ptr, u32 from_len_ptr, u32 overlapped_ptr, u32 completion_routine_ptr) {
  if (!Socket(socket_handle)) return uint32_t(-1);
  PendingRecv pending;
  pending.overlapped = overlapped_ptr;
  pending.from = from_ptr;
  pending.from_len = from_len_ptr;
  const auto* buffers = Guest<WsaBuf>(buffers_ptr);
  for (uint32_t i = 0; buffers_ptr && i < buffer_count; ++i) {
    pending.buffers.emplace_back(uint32_t(buffers[i].buf_ptr), uint32_t(buffers[i].len));
  }
  const bool relayed = Relayed(socket_handle);
  if (!overlapped_ptr) {
    relay::Datagram datagram;
    if (!relayed || !relay::Pop(datagram)) {
      rex::system::XThread::SetLastError(kWouldBlock);
      return uint32_t(-1);
    }
    bool truncated = false;
    const uint32_t bytes = CopyOut(pending.buffers, datagram, truncated);
    WriteSender(from_ptr, from_len_ptr, datagram);
    if (num_bytes_ptr) *Guest<rex::be<uint32_t>>(num_bytes_ptr) = bytes;
    if (flags_ptr) *Guest<rex::be<uint32_t>>(flags_ptr) = 0;
    if (truncated) rex::system::XThread::SetLastError(kMsgSize);
    return truncated ? uint32_t(-1) : 0;
  }
  auto* overlapped = Guest<WsaOverlapped>(overlapped_ptr);
  overlapped->internal = kStatusPending;
  overlapped->internal_high = 0;
  {
    std::lock_guard lock(g_mutex);
    g_pending[socket_handle] = std::move(pending);
  }
  if (relayed) Deliver();  // a datagram already queued completes it at once
  rex::system::XThread::SetLastError(kIoPending);
  return uint32_t(-1);
}

u32 WSAGetOverlappedResult(u32 caller, u32 socket_handle, u32 overlapped_ptr, u32 bytes_ptr, u32 wait,
                           u32 flags_ptr) {
  const uint32_t code = overlapped_ptr ? uint32_t(Guest<WsaOverlapped>(overlapped_ptr)->internal) : kStatusPending;
  if (code == kStatusPending) {
    rex::system::XThread::SetLastError(kIoIncomplete);
    return 0;
  }
  if (bytes_ptr) *Guest<rex::be<uint32_t>>(bytes_ptr) = uint32_t(Guest<WsaOverlapped>(overlapped_ptr)->internal_high);
  if (flags_ptr) *Guest<rex::be<uint32_t>>(flags_ptr) = 0;
  if (code != 0) rex::system::XThread::SetLastError(code);
  return code == 0;
}

// A send is done when it returns: UDP reports nothing later, and an overlapped one reads as already complete.
u32 WSASendTo(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count, u32 num_bytes_ptr, u32 flags,
              u32 to_ptr, u32 to_len, u32 overlapped_ptr, u32 completion_routine_ptr) {
  auto socket = Socket(socket_handle);
  if (!socket) return uint32_t(-1);
  std::vector<uint8_t> data;
  const auto* buffers = Guest<WsaBuf>(buffers_ptr);
  for (uint32_t i = 0; buffers_ptr && i < buffer_count; ++i) {
    const uint32_t len = buffers[i].len, ptr = buffers[i].buf_ptr;
    if (len && ptr) data.insert(data.end(), Guest<uint8_t>(ptr), Guest<uint8_t>(ptr) + len);
  }
  uint16_t port = 0;
  if (Relayed(socket_handle, &port)) {
    Route(to_ptr, port, data.data(), data.size());
  } else if (to_ptr) {
    rex::system::N_XSOCKADDR_IN to(Guest<rex::system::XSOCKADDR_IN>(to_ptr));
    socket->SendTo(data.data(), uint32_t(data.size()), flags, &to, to_len);
  }
  if (num_bytes_ptr) *Guest<rex::be<uint32_t>>(num_bytes_ptr) = uint32_t(data.size());
  if (overlapped_ptr) Complete(overlapped_ptr, 0, uint32_t(data.size()));
  return 0;
}

u32 SendTo(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags, u32 to_ptr, u32 to_len) {
  auto socket = Socket(socket_handle);
  if (!socket) return uint32_t(-1);
  uint16_t port = 0;
  if (Relayed(socket_handle, &port)) {
    if (buf_ptr) Route(to_ptr, port, Guest<uint8_t>(buf_ptr), buf_len);
    return buf_len;
  }
  rex::system::N_XSOCKADDR_IN to(Guest<rex::system::XSOCKADDR_IN>(to_ptr));
  return uint32_t(socket->SendTo(Guest<uint8_t>(buf_ptr), buf_len, flags, &to, to_len));
}

u32 RecvFrom(u32 caller, u32 socket_handle, u32 buf_ptr, u32 buf_len, u32 flags, u32 from_ptr, u32 from_len_ptr) {
  auto socket = Socket(socket_handle);
  if (!socket) return uint32_t(-1);
  if (Relayed(socket_handle)) {
    relay::Datagram datagram;
    if (!relay::Pop(datagram)) {
      rex::system::XThread::SetLastError(kWouldBlock);
      return uint32_t(-1);
    }
    bool truncated = false;
    const uint32_t bytes = CopyOut({{uint32_t(buf_ptr), uint32_t(buf_len)}}, datagram, truncated);
    WriteSender(from_ptr, from_len_ptr, datagram);
    if (truncated) rex::system::XThread::SetLastError(kMsgSize);
    return truncated ? uint32_t(-1) : bytes;
  }
  rex::system::N_XSOCKADDR_IN from;
  if (from_ptr) from = *Guest<rex::system::XSOCKADDR_IN>(from_ptr);
  uint32_t from_len = from_len_ptr ? uint32_t(*Guest<rex::be<uint32_t>>(from_len_ptr)) : 0;
  const int got = socket->RecvFrom(Guest<uint8_t>(buf_ptr), buf_len, flags, &from, from_len_ptr ? &from_len : nullptr);
  if (from_ptr) {
    auto* out = Guest<rex::system::XSOCKADDR_IN>(from_ptr);
    out->sin_family = from.sin_family;
    out->sin_port = from.sin_port;
    out->sin_addr = from.sin_addr;
    std::memset(out->x_sin_zero, 0, sizeof(out->x_sin_zero));
  }
  if (from_len_ptr) *Guest<rex::be<uint32_t>>(from_len_ptr) = from_len;
  if (got == -1) rex::system::XThread::SetLastError(kWouldBlock);
  return uint32_t(got);
}

// Closing a socket aborts its posted receive, which is what lets XRNM release the endpoint.
u32 CloseSocket(u32 caller, u32 socket_handle) {
  uint32_t aborted = 0;
  {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_pending.find(socket_handle); it != g_pending.end()) {
      aborted = it->second.overlapped;
      g_pending.erase(it);
    }
    g_relayed.erase(socket_handle);
  }
  auto socket = Socket(socket_handle);
  if (!socket) return uint32_t(-1);
  socket->Close();
  socket->ReleaseHandle();
  if (aborted) Complete(aborted, kOperationAborted, 0);
  return 0;
}

}  // namespace
}  // namespace cc1::net

REX_HOOK(__imp__NetDll_XNetRandom, cc1::net::XNetRandom)
REX_HOOK(__imp__NetDll_XNetGetTitleXnAddr, cc1::net::XNetGetTitleXnAddr)
REX_HOOK(__imp__NetDll_XNetXnAddrToInAddr, cc1::net::XNetXnAddrToInAddr)
REX_HOOK(__imp__NetDll_XNetInAddrToXnAddr, cc1::net::XNetInAddrToXnAddr)
REX_HOOK(__imp__NetDll_XNetConnect, cc1::net::XNetConnect)
REX_HOOK(__imp__NetDll_XNetGetConnectStatus, cc1::net::XNetGetConnectStatus)
REX_HOOK(__imp__NetDll_XNetRegisterKey, cc1::net::XNetRegisterKey)
REX_HOOK(__imp__NetDll_XNetUnregisterKey, cc1::net::XNetUnregisterKey)
REX_HOOK(__imp__NetDll_XNetReplaceKey, cc1::net::XNetReplaceKey)
REX_HOOK(__imp__NetDll_XNetUnregisterInAddr, cc1::net::XNetUnregisterInAddr)
REX_HOOK(__imp__NetDll_XNetQosLookup, cc1::net::XNetQosLookup)
REX_HOOK(__imp__NetDll_XNetQosListen, cc1::net::XNetQosListen)
REX_HOOK(__imp__NetDll_XNetXnAddrToMachineId, cc1::net::XNetXnAddrToMachineId)
REX_HOOK(__imp__NetDll_XNetGetEthernetLinkStatus, cc1::net::XNetGetEthernetLinkStatus)
REX_HOOK(__imp__NetDll_bind, cc1::net::Bind)
REX_HOOK(__imp__NetDll_WSARecvFrom, cc1::net::WSARecvFrom)
REX_HOOK(__imp__NetDll_WSAGetOverlappedResult, cc1::net::WSAGetOverlappedResult)
REX_HOOK(__imp__NetDll_WSASendTo, cc1::net::WSASendTo)
REX_HOOK(__imp__NetDll_sendto, cc1::net::SendTo)
REX_HOOK(__imp__NetDll_recvfrom, cc1::net::RecvFrom)
REX_HOOK(__imp__NetDll_closesocket, cc1::net::CloseSocket)
