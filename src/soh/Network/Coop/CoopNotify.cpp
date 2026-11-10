// On-screen notifications, queued and stacked (Crests of Courage notify.cpp)
#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace CoopGame {

void Notify(NoticeKind, const std::string& text) {
    SPDLOG_INFO("[Coop] {}", text);
}

} // namespace CoopGame
