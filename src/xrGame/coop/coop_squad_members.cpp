#include "stdafx.h"
#include "coop_squad_members.h"
#include "coop_entities.h"
#include "xrCoop/coop_mode.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"

// See coop_squad_members.h.

namespace coop
{
namespace squad
{
namespace
{
thread_local int g_quiet = 0; // the MemberQuiet alive on this thread
} // namespace

MemberQuiet::MemberQuiet() { ++g_quiet; }
MemberQuiet::~MemberQuiet() { --g_quiet; }

void member_up(ALife::_OBJECT_ID squad, ALife::_OBJECT_ID member, bool joined)
{
	if (g_quiet)
		return;
	if (entities::client_applying())
		return;
	LevelSide* side = mode().level();
	if (!side || !ai().get_alife() || !ai().alife().objects().object(squad, true))
		return;
	side->level_entity_up(joined ? EntityUp::Joined : EntityUp::Left, squad, member);
}
} // namespace squad
} // namespace coop
