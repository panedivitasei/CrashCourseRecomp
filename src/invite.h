// invite.h: Invite a Friend in the online lobby, shown as a link to the game's page to copy and send.

#pragma once

#include <string>

namespace cc1::invite {

// Sets the link the window shows and copies; called from the app's setup.
void Initialize(std::string link);

}  // namespace cc1::invite
