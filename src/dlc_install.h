// wwe2k16 - DLC packages added in the launcher are installed before the game starts (dlc_install.cpp).
#pragma once

namespace rex {
class Runtime;
}

namespace wwe2k16::dlc {
// Installs every package waiting in <user data>/dlc_import/ (call before the game's main thread is resumed).
void InstallPending(rex::Runtime* runtime);
}  // namespace wwe2k16::dlc
