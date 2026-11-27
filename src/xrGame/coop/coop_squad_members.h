// Anomaly Coop: what the co-op hears of the members of an ALife squad (CSE_ALifeOnlineOfflineGroup), the hook of its stock file. A
// process that leads the squad's level apart from its world has a level side (mode().level(): a map server, whose journal carries
// Joined / Left up); the others have none (the world server, the co-op host and client, a player of a server world, single player) and
// say nothing. What the world server does with a map server's Joined / Left is srv/coop_squad_journal.cpp.
#pragma once

#include "alife_space.h"

namespace coop
{
namespace squad
{
// register_member (joined) and unregister_member (left) after the change. Quiet while a MemberQuiet lives on this thread, while a copy
// applies its world's word (entities::client_applying), and for a squad that is no object of this world any more (its release is under
// way: its Destroy says it).
void member_up(ALife::_OBJECT_ID squad, ALife::_OBJECT_ID member, bool joined);

// While one lives, member_up says nothing on this thread: not a change this process makes. The world's own apply of a journal entry
// (coop_join_member, coop_leave_member, coop_bind_members) and a load that binds the members of a save again (on_after_game_load). Per
// thread: the ALife's thread and the main thread do not quiet each other.
class MemberQuiet
{
public:
	MemberQuiet();
	~MemberQuiet();
	MemberQuiet(const MemberQuiet&) = delete;
	MemberQuiet& operator=(const MemberQuiet&) = delete;
};
} // namespace squad
} // namespace coop
