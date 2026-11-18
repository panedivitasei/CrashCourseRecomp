// Game traffic between lobby members through the JMstudios UDP relay, where a member has a seat (pid 1-4) and the
// address 10.66.0.<pid> on the game's VDP port.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cc1::relay {

constexpr uint16_t kGamePort = 1000;   // the game's VDP port
constexpr uint8_t kNoSeatOctet = 250;  // own address outside a lobby
constexpr int kMaxPid = 4;
constexpr uint16_t kControlPort = 1009;  // datagrams between the recomps themselves; the game never sees them

struct Datagram {
  uint8_t src_pid = 0;
  uint16_t port = 0;
  std::vector<uint8_t> data;
};

// 'relay' is "host:port", 'token' the server's ticket for seat 'pid'.
void Start(const std::string& relay, const std::string& token, int pid);
void Stop();
int Pid();  // 0 outside a lobby

// 'dst_pid' 0 sends to every other member; false when the relay is down or the payload out of range.
bool Send(uint8_t dst_pid, uint16_t port, const uint8_t* data, size_t size);
// The oldest datagram received; false when there is none.
bool Pop(Datagram& out);
// Called on the relay thread after a datagram is queued.
void SetReceiveHook(void (*hook)());

// Sends a control datagram to the member at 'dst_pid' (0 for every other member), whose first byte is its kind (1
// lobby_pose.cpp, 2 puppet.cpp, 3 online.cpp); false when the relay is down.
bool SendControl(const uint8_t* data, size_t size, uint8_t dst_pid = 0);
// Adds a hook called on the relay thread with each control datagram, which ignores kinds not its own; up to four,
// added before the relay starts (static initialisation).
void AddControlHook(void (*hook)(uint8_t src_pid, const uint8_t* data, size_t size));

// The 36-byte XNADDR of a seat (0: the address outside a lobby).
void FillXnAddr(uint8_t pid, uint8_t* xnaddr);
// The local machine's XNADDR.
void FillLocalXnAddr(uint8_t* xnaddr);
// True for 10.66.0.x (host order), with x in 'octet'.
bool IsPeerAddress(uint32_t in_addr, uint8_t* octet = nullptr);

}  // namespace cc1::relay
