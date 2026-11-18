// input_assignment.cpp: device-to-player assignment (see input_assignment.h).

#include "input_assignment.h"

#include <array>
#include <memory>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rexglue_guide/guide.h>

namespace cc1 {
namespace {

// The SDK's slot assignment minus the keyboard while the Guide is up, so a key typed into the Guide does not also
// arrive as its bound pad button.
class Assignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    for (auto& user : users_) user.clear();
    keyboard_ = rex::input::DeviceId::kInvalid;
    for (const auto& device : devices) {
      if (device.synthetic) {
        if (device.name == "Keyboard and Mouse") keyboard_ = device.id;
        users_[0].push_back(device.id);
      } else if (device.ordinal < rex::input::kMaxGuestUsers) {
        users_[device.ordinal].push_back(device.id);
      }
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index >= rex::input::kMaxGuestUsers) return;
    out = users_[user_index];
    // Player 1 keeps the SDK's stand-in pad, so the game sees no disconnect.
    if (rexglue_guide::IsOpen()) std::erase(out, keyboard_);
  }

 private:
  std::array<std::vector<rex::input::DeviceId>, rex::input::kMaxGuestUsers> users_;
  rex::input::DeviceId keyboard_ = rex::input::DeviceId::kInvalid;
};

}  // namespace

void InstallInputAssignment(rex::input::InputSystem* input) {
  if (input) input->SetDeviceAssignment(std::make_unique<Assignment>());
}

}  // namespace cc1
