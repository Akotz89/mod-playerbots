#ifndef PLAYERBOTS_COURTESY_BUFFS_H
#define PLAYERBOTS_COURTESY_BUFFS_H
#include "Define.h"
class Player;
class PlayerbotAI;
namespace PlayerbotsCourtesy
{
void Tick(uint32 diff);
void Reload();
void Cancel();
void TryBuff(Player* player, PlayerbotAI* ai);
}
#endif
