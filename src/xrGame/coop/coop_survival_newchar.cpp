// Anomaly Coop — player survival: hardcore's new character. Out of the run a player asks for one; the world server moves all he
// has into a stash at his body and then starts his character over (the host player) or sends a client the world again to choose
// a new one. See coop_survival.h.
#include "stdafx.h"
#include "coop_survival_state.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_join.h"
#include "coop_levels.h"
#include "coop_world.h"
#include "coop_net.h"
#include "coop_script.h"
#include "coop_money.h"
#include "coop_wire_vec.h"
#include "xrCoop/coop_checkpoint_store.h"

#include "Level.h"
#include "xrServer_Objects_ALife_Monsters.h"

namespace coop
{
namespace survival
{
// ---------------------------------------------------------------- state (coop_survival_state.h)

xr_map<u16, Retirement> g_retire;
xr_vector<u16> g_stashing; // a map server: the retiring characters of its level whose stash is under way

namespace
{
constexpr u32 kReleaseRetryMs = 3000; // a retired character still there is asked to go again

// ---------------------------------------------------------------- hardcore: a new character

// Where the host player's new character stands up: his arrival point on this level (coop_checkpoints keeps it in the
// actor's custom data), else where a respawn would put him (his camp, the previous one, where he stands).
Fvector host_new_place(const Fvector& stands, const char*& where, checkpoint::Kind& kind)
{
	const CSE_Abstract* e = character_entity(0);
	checkpoint::Record r;
	if (e && e->m_ini_string.size() && checkpoint::arrival_from_ini(std::string(e->m_ini_string.c_str()), r) &&
	    r.valid() && r.level == Level().name().c_str())
	{
		where = "his arrival point";
		kind = checkpoint::Kind::Arrival;
		return Fvector().set(r.pos[0], r.pos[1], r.pos[2]);
	}
	Fvector pos = stands;
	kind = checkpoint::Kind::None;
	checkpoints::host_respawn_point(0, nullptr, stands, pos, kind);
	where = kind == checkpoint::Kind::None ? "where he stands (no arrival point on this level)" : checkpoint::kind_name(kind);
	return pos;
}

// Step 4 for the host player's character (0), his things in the stash now (host_retire_frame). His actor stays in the
// world (a client's new character comes by a new join instead), so the world starts it over: not out, no death decided,
// nothing owed, 0 RU (money::host_set: his balance goes to him over the internal channel), and orders his owner, over the
// internal channel like any order, to stand up at his arrival point as a fresh one (host_send_new_life). The
// world's Lua (coop_menu_char) offers him faction and kit: coop_host_new_character() (menu.char_offer to peer 0). False
// when his actor is not here (nothing changed: he stays out and may ask again).
bool host_order_new_life(u16 character)
{
	if (!object_here(character))
		return false;
	HostRec& h = g_host[character];
	h.tracker = death::Tracker(); // a new character: not out, no death decided, nothing owed
	h.ordered_seq = death::kNoSeq;
	h.has_worn = false;
	h.has_anchor = false; // the anchor is where this character is first seen alive
	h.debt = false;
	flag_write(character, kOutKey, false); // before his Alive report: it reads the world's word (learn_out)
	flag_write(character, kDebtKey, false);
	if (!money::host_set(peer_of_character(character), 0, "new character"))
		::Msg("! [coop] new character of %s: his money is not set to 0 (no balance to write)", nick_of(character).c_str());
	::Msg("[coop] %s starts a new character", nick_of(character).c_str());
	host_send_new_life(character);
	// kit 1 only with coop_hardcore_new_kit on; else he starts with nothing (the window offers a faction only)
	script::queue_callback("coop_host_new_character", {script::cb_int(death::host_new_kit() ? 1 : 0)});
	return true;
}

// Step 2 for a client's character, his stash done (host_retire_frame): the key goes from his custom data and
// `retired = 1` comes in (death::retire_custom_data), his player joins the world again - with no character for his key
// he chooses a new one (CharOffer, coop_menu_char, create_proxy) - and the old character is asked to leave
// the world (host_release_step confirms it). False: he stays (his player could not be sent the world again).
bool host_finish_client(u16 character)
{
	if (CSE_Abstract* e = character_entity(character))
	{
		std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
		size_t at = 0, len = 0;
		if (find_key_value(text, at, len))
			join::host_key_retired(text.substr(at, len)); // his new character is not a new key
		death::retire_custom_data(text);
		e->m_ini_string = text.c_str();
	}
	const PeerId peer = join::peer_of_proxy(character);
	if (peer == kInvalidPeer)
		::Msg("[coop] retired character %u: his player is away, his next join makes a new character", character);
	else if (!join::host_rejoin_peer(peer, join::kRejoinNewCharacter))
	{
		// cannot happen (a peer of a proxy is a connected player of this world); his machine still plays this
		// character, so it stays in the world, keyless: his next join makes a new one
		::Msg("! [coop] retired character %u: his player #%u could not be sent the world again, kept", character, peer);
		g_host.erase(character);
		return false;
	}
	death::host_release_character(character); // done when his entity is gone (host_release_step)
	return true;
}

// A retired client's character is gone when no player character that says `retired = 1` is under
// his id any more (the id may name his player's new character by then). Only then is the id forgotten as a player's
// character (world::host_forget_proxy) and his records go; until then the release is asked again every
// kReleaseRetryMs, logged. True when he is gone.
bool host_release_step(u16 character, Retirement& r, u64 t)
{
	const CSE_Abstract* e = character_entity(character);
	const bool still_there = e && e->m_ini_string.size() && death::is_retired(e->m_ini_string.c_str());
	if (!still_there)
	{
		world::host_forget_proxy(character);
		g_host.erase(character);
		g_players.erase(character);
		++g_stat.retired;
		::Msg("[coop] retired character %u is gone from the world (%u attempt(s))", character, r.tries);
		return true;
	}
	if (t < r.next_try)
		return false;
	r.next_try = t + kReleaseRetryMs;
	++r.tries;
	const bool asked = death::host_release_character(character);
	if (r.tries <= 3 || r.tries % 20 == 0)
		::Msg("! [coop] retired character %u is still in the world after %u attempt(s)%s", character, r.tries,
		    asked ? ", asked again" : ", cannot be released now");
	return false;
}

// Steps 2 and 4: the owner of `character` (0: the host player) asks for a new character. The tracker, with the world's
// word (learn_out), must say he is out of the hardcore run; then everything he has - worn and quest items included - goes
// into a stash at the body (DeathMarker when it is in), and the rest waits for it (host_retire_frame). He must be in the
// world: a character that is not online here has nothing moved, and his release would take his things with him.
// coop_max_new_characters is not checked: his key had a character (join::host_key_retired).
void host_new_character(u16 character)
{
	HostRec& h = g_host[character];
	death::learn_out(h.tracker, flag_read(character, kOutKey));
	const xr_string who = nick_of(character);
	const char* refused = nullptr;
	const PeerId peer = peer_of_character(character);
	switch (death::on_new_character(h.tracker, g_retire.find(character) != g_retire.end()))
	{
	case death::NewCharVerdict::Busy:
		::Msg("[coop] new character of %s: under way already", who.c_str());
		return;
	case death::NewCharVerdict::NotOut: refused = "he is not out of a hardcore run"; break;
	case death::NewCharVerdict::Start:
		if (!mode().led_elsewhere(peer) && (character == 0 ? !local_actor() : !object_here(character)))
			refused = "his character is not in the world here";
		break;
	}
	if (refused)
	{
		++g_stat.newchar_refused;
		::Msg("! [coop] new character of %s (character %u) refused: %s", who.c_str(), character, refused);
		return;
	}
	Retirement r{};
	r.since = mgr().now_ms();
	Writer w(4);
	w.u8(OpStash);
	w.u16(character);
	r.at_map_server = mode().to_level_of(peer, Msg::PlayerRevive, w, Lane::Reliable);
	if (r.at_map_server)
	{
		g_retire[character] = r;
		::Msg("[coop] %s starts a new character: his things go into a stash on his level", who.c_str());
		return;
	}
	r.stash = death::host_move_to_stash(character, true); // everything, worn and quest items included
	g_retire[character] = r;
	if (r.stash != kNoId)
		::Msg("[coop] %s starts a new character: his things go into stash %u first", who.c_str(), r.stash);
	else
		::Msg("[coop] %s starts a new character (no stash: nothing to keep, or it dropped at the body)", who.c_str());
}

} // namespace

// The order of the host player's new life - a Respawn to his owner at his arrival point (host_new_place) that
// names no fall (death::kNewLifeSeq: only a spectating owner takes it, apply_respawn / finish_new_life), over the
// internal channel like any order. Recorded for the watch in host_on_state (HostRec::new_life). False when his actor is
// not here.
bool host_send_new_life(u16 character)
{
	CGameObject* body = object_here(character);
	if (!body)
		return false;
	const char* where = "";
	checkpoint::Kind kind = checkpoint::Kind::None;
	Fvector pos = host_new_place(body->Position(), where, kind);
	pos.y += kRespawnLift;
	Writer w(24);
	w.u8(OpRespawn);
	w.u16(character);
	w.u16(death::kNewLifeSeq);
	put_vec(w, pos);
	w.u8(u8(kind));
	host_send_order(character, w);
	HostRec& h = g_host[character];
	h.new_life.sent = true;
	h.new_life.at = mgr().now_ms();
	::Msg("[coop] %s: his new character stands up at %s (%.1f, %.1f, %.1f)", nick_of(character).c_str(), where, pos.x,
	    pos.y, pos.z);
	return true;
}

// death::retire_step: a retiring character's key, his player and his release wait until his stash
// is no longer pending (death::host_stash_outcome). The stash knows its owner by the key, or by the player while the
// key is off; changing either before it is filled leaves it without an owner, the things stay on the old character and
// go with him. A client's character whose stash gave up, or who still carries world items
// (death::host_items_on, the server's word), is not retired at all - he stays out with his things and may ask again.
// Only then step 2 (a client's character, then host_release_step) or step 4 (the host player's, host_order_new_life)
// goes on.
void host_retire_frame(u64 t)
{
	for (auto it = g_retire.begin(); it != g_retire.end();)
	{
		const u16 character = it->first;
		Retirement& r = it->second;
		if (r.releasing)
		{
			it = host_release_step(character, r, t) ? g_retire.erase(it) : std::next(it);
			continue;
		}
		const bool client = character != 0;
		const bool here = client ? character_entity(character) != nullptr : local_actor() != nullptr;
		const death::StashOutcome stash =
		    r.at_map_server ? death::remote_stash_outcome(r.answered, r.things_stay, t - r.since) : death::host_stash_outcome(character);
		const u32 left = client && !r.at_map_server && stash != death::StashOutcome::Pending ? death::host_items_on(character) : 0;
		switch (death::retire_step(here, stash, left != 0, client))
		{
		case death::RetireStep::Wait: ++it; continue;
		case death::RetireStep::Void:
			::Msg("! [coop] retirement of character %u dropped: he is gone", character);
			break;
		case death::RetireStep::Cancel:
			++g_stat.retire_cancelled;
			::Msg("! [coop] retirement of %s (character %u) cancelled: %s; he keeps his character and his things, stays "
			      "out and may ask again",
			    nick_of(character).c_str(), character,
			    stash == death::StashOutcome::GaveUp ? "his stash gave up (his player stayed away)" : "things are still on him");
			if (left)
				::Msg("! [coop]   %u item(s) still on character %u", left, character);
			break;
		case death::RetireStep::Finish:
			if (!client)
			{
				if (host_order_new_life(character))
					++g_stat.retired;
				break;
			}
			if (host_finish_client(character))
			{
				r.releasing = true;
				r.tries = 1;
				r.next_try = t + kReleaseRetryMs;
				++it; // gone on a later frame (host_release_step)
				continue;
			}
			break;
		}
		it = g_retire.erase(it);
	}
}

// The world server, step 2: Msg::NewCharacter (no payload; bytes after are a newer client's) from a player out
// of a hardcore run (the host player's over the internal channel). His character is the one of his peer, never
// an id he names.
void on_new_character_msg(PeerId from, Reader&)
{
	if (!mode().decides_world())
		return; // ClientOnly: the session refuses it from the host before any handler
	const u16 character = character_of_peer(from);
	if (character == kNoId)
	{
		++g_stat.newchar_refused;
		::Msg("! [coop] NewCharacter from #%u refused: no character of his in this world", from);
		return;
	}
	host_new_character(character);
}

// Step 1: out of a hardcore run, ask for a new character (the hold of USE, console coop_new_character). A
// client asks the host (Msg::NewCharacter, no payload); the host starts its own retirement at once. The answer is the
// world sent again (a client) or standing up (the host); a refusal is only in the host's log, so after kNewCharRetryMs
// the prompt comes back. False while a request is that fresh.
bool request_new_character(u64 t)
{
	if (g_me.life != Life::Spectating || (g_me.newchar_asked && t - g_me.newchar_asked < kNewCharRetryMs))
		return false;
	g_me.newchar_asked = t;
	g_me.newchar_start = 0;
	g_me.newchar_armed = false;
	++g_stat.newchar_asked;
	::Msg("[coop] asking for a new character");
	net::client_send(Msg::NewCharacter, Writer()); // the host player's over the internal channel
	return true;
}

// Spectating: hold USE kNewCharHoldMs (as kGiveUpHoldMs), armed once USE was seen released after the order.
void newchar_frame(u64 t)
{
	if (g_me.newchar_asked)
	{
		if (t - g_me.newchar_asked < kNewCharRetryMs)
			return;
		g_me.newchar_asked = 0; // no answer: the host refused (its log says why), or it is still at it
	}
	if (!action_down(kUSE))
	{
		g_me.newchar_armed = true;
		g_me.newchar_start = 0;
		return;
	}
	if (!g_me.newchar_armed)
		return;
	if (!g_me.newchar_start)
		g_me.newchar_start = t;
	else if (t - g_me.newchar_start >= kNewCharHoldMs)
		request_new_character(t);
}

// A map server: the retirements of its level that are over go up, after the Parent of each thing that moved (the same queue). Its word
// on what stayed on him counts: the body is its object.
void level_stash_frame()
{
	for (size_t i = 0; i < g_stashing.size();)
	{
		const u16 character = g_stashing[i];
		const death::StashOutcome stash = death::host_stash_outcome(character);
		if (stash == death::StashOutcome::Pending)
		{
			++i;
			continue;
		}
		levels::up_stash(character, stash == death::StashOutcome::GaveUp || death::host_items_on(character) != 0);
		g_stashing.erase(g_stashing.begin() + i);
	}
}

void host_stash_over(u16 character, bool things_stay)
{
	const auto it = g_retire.find(character);
	if (it == g_retire.end() || !it->second.at_map_server)
		return; // it gave up waiting: he stays out and may ask again
	it->second.answered = true;
	it->second.things_stay = things_stay;
}

} // namespace survival
} // namespace coop
