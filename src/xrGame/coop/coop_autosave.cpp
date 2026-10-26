// Anomaly Coop — the host's emergency autosaves coop_auto_N (F3 Task 12). See coop_autosave.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_autosave.h"
#include "coop_manager.h"
#include "coop_survival.h"
#include "coop_rest.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "ai_space.h"
#include "Level.h"
#include "Actor.h"

#include <string>
#include <vector>

extern string_path g_last_saved_game;

namespace coop
{
namespace autosave
{
namespace
{
// ---------------------------------------------------------------- tuning

// The name a save is written under before it becomes a slot. parse_slot rejects it (test autosave_names), so a pair that
// a crash left half written is never listed in the Restore window and never restored.
constexpr LPCSTR kTempStem = "coop_auto_tmp";
// The old pair of a slot waits under this name while the new pair is moved in (swap_in). Not a slot either.
constexpr LPCSTR kBackupStem = "coop_auto_bak";
// A timed or requested save waits for a good moment no longer than this (the max_postpone_ms of timer_due).
constexpr u32 kMaxPostponeMs = 120000;
// A request that found no way to save for this long is dropped: the world it was meant to keep moved on long ago.
constexpr u64 kRequestLifeMs = 300000;
// CALifeStorageManager::save starts the file with u32(-1), the version and the size: less is not a save.
constexpr int kMinSaveBytes = 12;
// Moving a finished file can meet a virus scanner or a sync client that holds it for a moment (review M1).
constexpr u32 kMoveTries = 8;
constexpr DWORD kMoveRetryMs = 50;
// A save whose slot cannot be replaced goes to the next one, this many slots at most (review M2).
constexpr u32 kSlotTries = 3;
// The Restore window renews its pause every frame; a pause nobody renewed for this long is over (review I1).
constexpr u32 kPauseLeaseMs = 3000;

constexpr size_t kReasonCount = size_t(Reason::Manual) + 1;

// ---------------------------------------------------------------- state

int g_interval_min = 10; // coop_autosave_interval: minutes, 0 = off
int g_count = 10; // coop_autosave_count: how many slots rotate

Schedule g_schedule; // the timer
Pause g_pause; // the Restore window is open (suspend)
std::vector<uint32_t> g_bad_slots; // slots whose files could not be replaced: not tried again until the session ends

struct Pending
{
	bool on = false;
	Reason why = Reason::Manual;
	u64 since = 0; // when it was asked for
	u64 busy_since = 0; // when it first met a bad moment (0 = not postponed)
} g_pending;

bool g_saving = false; // inside a save: nothing starts another
uint32_t g_level_slot = 0; // the slot level_change_name took for the save of a level change in progress (0 = none)

struct Stats
{
	u64 saved[kReasonCount] = {};
	u64 skipped = 0; // the host's level could not be saved when the time came
	u64 failed = 0; // not written, or written but not moved to any slot
	u64 dropped = 0; // requests that found no moment for kRequestLifeMs
	u64 slots_failed = 0; // moves to a slot that failed (the next slot was tried)
} g_stat;

struct Last
{
	std::string name; // "coop_auto_N"
	Reason why = Reason::Manual;
	u64 at_ms = 0; // 0 = nothing yet
} g_last;

const char* reason_name(Reason r)
{
	switch (r)
	{
	case Reason::Timer: return "timer";
	case Reason::Sleep: return "after sleep";
	case Reason::LevelChange: return "level change";
	case Reason::SessionEnd: return "end of the session";
	case Reason::Manual: return "manual";
	}
	return "?";
}

// ---------------------------------------------------------------- when

// The host's level can be saved at all: a running level of the host with a living actor, not coming up (precache).
bool can_write()
{
	return mgr().is_host() && g_pGameLevel && g_actor && g_actor->g_Alive() && ai().get_alife() &&
		Device.dwPrecacheFrame == 0;
}

// Not a good moment for a timed or requested save: the world would keep a host who is down or asleep.
bool bad_moment()
{
	return !can_write() || survival::life_of(0) != survival::Life::Alive || rest::state_of(0) == rest::State::Sleeping;
}

// The pause ended (let go, or the window that held it closed): the interval starts afresh and what was asked for during
// the pause is dropped. The host may have loaded a slot: the first frames after that load must not write over one.
void resume(u64 now, const char* why)
{
	g_schedule.last_ms = now;
	g_schedule.busy_since = 0;
	g_pending = Pending();
	::Msg("[coop] autosave resumed (%s): the interval starts afresh", why);
}

// ---------------------------------------------------------------- files

void save_path(string_path& out, LPCSTR stem, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", stem, ext);
	FS.update_path(out, "$game_saves$", file);
}

// The two files of a save: the engine's .scop and Anomaly's Lua .scoc.
struct Pair
{
	string_path scop;
	string_path scoc;
};

void pair_paths(Pair& p, LPCSTR stem)
{
	save_path(p.scop, stem, SAVE_EXTENSION);
	save_path(p.scoc, stem, ".scoc");
}

bool file_present(LPCSTR path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

// The file index of the saves folder knows only what went through FS: Anomaly's Lua writes the .scoc with io.open, and
// a file moved by the system is not in it. The same rescan as the Lua file_list_open_ex and the console's save list.
void rescan_saves()
{
	FS_Path* p = FS.get_path("$game_saves$");
	p->m_Flags.set(FS_Path::flNeedRescan, TRUE);
	FS.rescan_pathes();
}

// The slots on disk: coop_auto_N.scop in the saves folder and when each was written.
void list_slots(std::vector<SlotInfo>& out)
{
	rescan_saves();
	FS_FileSet files;
	FS.file_list(files, "$game_saves$", FS_ListFiles | FS_RootOnly, "coop_auto_*" SAVE_EXTENSION);
	for (const FS_File& f : files)
	{
		string_path stem;
		xr_strcpy(stem, f.name.c_str());
		if (char* dot = strext(stem))
			*dot = 0;
		uint32_t index = 0;
		if (parse_slot(stem, index))
			out.push_back({index, uint64_t(f.time_write)});
	}
}

// The slot a save goes to: a free one, else the oldest, except those that failed before (review M2). 0 when every place
// has failed.
uint32_t pick_untried_slot()
{
	std::vector<SlotInfo> slots;
	list_slots(slots);
	return next_slot_skipping(slots, uint32_t(g_count), g_bad_slots);
}

// The first slot to try for a new save. When every place has failed before, all are given another chance.
uint32_t pick_slot()
{
	uint32_t slot = pick_untried_slot();
	if (!slot)
	{
		g_bad_slots.clear();
		slot = pick_untried_slot();
	}
	return slot;
}

// Removes a file of ours: the index entry (the .scop went through FS) and whatever is on disk (the .scoc did not). A
// read-only file (an antivirus, a sync client or the player set it) is made writable first, as CFileWriter does.
void remove_file(LPCSTR path)
{
	SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
	FS.file_delete(path);
	DeleteFileA(path);
}

// Moves a finished file to `to` (replacing a file there in the same call). A failure is retried for a while, a virus
// scanner holds a file that was just closed for a moment. (CLocatorAPI::file_rename unlinks the old file first, does not
// say whether the rename worked, and does not see the .scoc at all.)
bool move_over(LPCSTR from, LPCSTR to)
{
	DWORD error = 0;
	for (u32 attempt = 1; attempt <= kMoveTries; ++attempt)
	{
		if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		error = GetLastError();
		if (attempt < kMoveTries)
			Sleep(kMoveRetryMs);
	}
	::Msg("! [coop] autosave: cannot move '%s' to '%s' (error %lu)", from, to, error);
	return false;
}

// Writes the data of a closed file to the disk (review M3). The M_SAVE_GAME path gives no handle: CFileWriter closes the
// .scop with fclose and Lua closes the .scoc, neither flushes the system cache. Without this a power cut after the
// rename can leave a slot of the right size with the body not written.
void flush_to_disk(LPCSTR path)
{
	HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return; // held by someone else: the move below still works, only the power-cut guarantee is missing
	FlushFileBuffers(h);
	CloseHandle(h);
}

// ---------------------------------------------------------------- writing

// The M_SAVE_GAME of the join flow (coop_join.cpp host_send_save), update_name 0. CALifeStorageManager::save runs on
// this stack and is done when Send returns: Lua writes the .scoc first (CALifeStorageManager_before_save), then the
// engine the .scop (FS.w_open .. FS.w_close), both closed. There is no thread and no deferred write. (The `save` console
// command would add a .dds screenshot that is taken a frame later, a "Game saved" line on the HUD and the name of the
// save as "last save".)
void send_save(LPCSTR stem)
{
	string_path last_save;
	xr_strcpy(last_save, g_last_saved_game); // save() records every name it writes as the last save
	NET_Packet packet;
	packet.w_begin(M_SAVE_GAME);
	packet.w_stringZ(stem);
	packet.w_u8(0); // keep the host's own current save name
	g_saving = true;
	Level().Send(packet, net_flags(TRUE));
	g_saving = false;
	xr_strcpy(g_last_saved_game, last_save);
}

// What an earlier crash or a failed save left of coop_auto_tmp must not pass for the save that is about to be written
// (review M4): a leftover .scoc that Lua could not overwrite would be moved to the new world. false = it cannot be cleared.
bool begin_temp()
{
	Pair tmp;
	pair_paths(tmp, kTempStem);
	remove_file(tmp.scop);
	remove_file(tmp.scoc);
	if (file_present(tmp.scop) || file_present(tmp.scoc))
	{
		::Msg("! [coop] autosave: the leftovers of '%s' cannot be removed, no save now", kTempStem);
		return false;
	}
	return true;
}

// Puts the finished pair `from` in the place of the pair `to` as one change (review M1). The old pair is put aside under
// kBackupStem first (the slot is then out of the Restore list), the new one moved in, the old one deleted. Any failure
// puts it all back: `to` holds the old pair or the new one, never one file of each, and `from` is whole again.
bool swap_in(const Pair& from, bool has_scoc, const Pair& to)
{
	Pair aside;
	pair_paths(aside, kBackupStem);
	remove_file(aside.scop);
	remove_file(aside.scoc);

	bool aside_scop = false;
	bool aside_scoc = false;
	auto put_old_back = [&]()
	{
		if (aside_scoc)
			move_over(aside.scoc, to.scoc);
		if (aside_scop)
			move_over(aside.scop, to.scop); // the .scop last: the slot comes back whole
	};

	if (file_present(to.scop))
	{
		if (!move_over(to.scop, aside.scop))
			return false;
		aside_scop = true;
	}
	if (file_present(to.scoc))
	{
		if (!move_over(to.scoc, aside.scoc))
		{
			put_old_back();
			return false;
		}
		aside_scoc = true;
	}
	bool scoc_in = false;
	if (has_scoc)
	{
		if (!move_over(from.scoc, to.scoc))
		{
			put_old_back();
			return false;
		}
		scoc_in = true;
	}
	if (!move_over(from.scop, to.scop))
	{
		if (scoc_in)
			move_over(to.scoc, from.scoc); // the new pair together again, a level change loads from it
		put_old_back();
		return false;
	}
	remove_file(aside.scop);
	remove_file(aside.scoc);
	return true;
}

enum class Commit
{
	Done,
	NotWritten, // coop_auto_tmp is not there: nothing to move
	NotMoved // the finished pair is in coop_auto_tmp, the slot could not take it
};

// coop_auto_tmp holds a finished save: moves it to coop_auto_<slot>.
Commit commit_to(uint32_t slot)
{
	const std::string name = slot_name(slot);
	Pair from, to;
	pair_paths(from, kTempStem);
	pair_paths(to, name.c_str());

	if (FS.file_length(from.scop) < kMinSaveBytes) // -1 = no file
	{
		::Msg("! [coop] autosave: the world was not written to '%s'", from.scop);
		return Commit::NotWritten;
	}

	// When the script storage is off there is no .scoc, and an old one in the slot must not be loaded with this world:
	// swap_in leaves the slot without one.
	const bool has_scoc = file_present(from.scoc);
	if (!has_scoc)
		::Msg("! [coop] autosave: no script part '%s' was written, %s will load without Anomaly's script data", from.scoc,
		    name.c_str());
	flush_to_disk(from.scop);
	if (has_scoc)
		flush_to_disk(from.scoc);

	if (!swap_in(from, has_scoc, to))
		return Commit::NotMoved;
	rescan_saves(); // the index must see the slot (load, FS.exist) and not the temporary files
	xr_strcpy(g_last_saved_game, name.c_str()); // "Continue" goes to the newest world, as after a stock autosave
	return Commit::Done;
}

// The finished save goes to `first`; if its files cannot be replaced (read-only, held by another process), to the next
// slot, a few times (review M2). Slots that failed are remembered for the session. Returns the slot, 0 when none took
// it, with `result` saying whether the save is still in coop_auto_tmp.
uint32_t commit_somewhere(uint32_t first, Commit& result)
{
	uint32_t slot = first;
	result = Commit::NotMoved;
	for (u32 attempt = 0; attempt < kSlotTries && slot; ++attempt)
	{
		result = commit_to(slot);
		if (result == Commit::Done)
			return slot;
		if (result == Commit::NotWritten)
			return 0;
		++g_stat.slots_failed;
		::Msg("! [coop] autosave: %s could not be replaced, %s", slot_name(slot).c_str(),
		    attempt + 1 < kSlotTries ? "the next slot is tried" : "giving up");
		if (std::find(g_bad_slots.begin(), g_bad_slots.end(), slot) == g_bad_slots.end())
			g_bad_slots.push_back(slot);
		slot = pick_untried_slot();
	}
	return 0;
}

// The host keeps coop_autosave_count slot files: after a save the oldest beyond that go (the rules: slots_to_prune). A
// host that lowered the count drops the surplus with its next save.
void prune_surplus(uint32_t written)
{
	std::vector<SlotInfo> slots;
	list_slots(slots); // with the new slot at its real time
	for (const uint32_t index : slots_to_prune(slots, uint32_t(g_count), written))
	{
		const std::string name = slot_name(index);
		for (LPCSTR ext : {SAVE_EXTENSION, ".scoc", ".dds"})
		{
			string_path file;
			save_path(file, name.c_str(), ext);
			remove_file(file);
		}
		::Msg("[coop] autosave: %s removed, the host keeps %d", name.c_str(), g_count);
	}
}

// One more save counted: the interval of the timer starts from here, and a request waiting for this save is served.
void note_saved(Reason why, const std::string& name)
{
	const u64 now = mgr().now_ms();
	++g_stat.saved[size_t(why)];
	g_last.name = name;
	g_last.why = why;
	g_last.at_ms = now;
	g_schedule.last_ms = now;
	g_schedule.busy_since = 0;
	g_pending = Pending();
}

// Writes a save now. The caller has chosen the moment; the level is checked here.
bool save(Reason why)
{
	if (g_saving)
		return false;
	if (!can_write())
	{
		++g_stat.skipped;
		::Msg("! [coop] autosave (%s) skipped: the host's level cannot be saved now", reason_name(why));
		return false;
	}
	// Live 2026-10-25 (owner: "loaded the game from the moment I quit dead: a black screen"). A down host is alive for the
	// engine with 1% health; a save then (the end of the session when he quits dead, or a timed one past its postpone)
	// loads him dying: no health, no limbs, the screen black. No save of any reason while the host's character is down:
	// the last good one stays.
	if (survival::life_of(0) != survival::Life::Alive)
	{
		++g_stat.skipped;
		::Msg("! [coop] autosave (%s) skipped: the host's character is down (%s), the last save stays", reason_name(why),
		    survival::life_name(survival::life_of(0)));
		return false;
	}
	const u64 started = mgr().now_ms();
	if (!begin_temp())
	{
		++g_stat.failed;
		return false;
	}
	const uint32_t first = pick_slot();
	send_save(kTempStem);
	Commit result = Commit::NotMoved;
	const uint32_t slot = commit_somewhere(first, result);
	if (!slot)
	{
		++g_stat.failed;
		::Msg("! [coop] autosave (%s) failed: %s", reason_name(why),
		    result == Commit::NotWritten ? "the world was not written" : "no slot took it");
		return false;
	}
	const std::string name = slot_name(slot);
	note_saved(why, name);
	::Msg("[coop] autosave %s (%s) written in %u ms", name.c_str(), reason_name(why), u32(mgr().now_ms() - started));
	prune_surplus(slot);
	return true;
}

// The last save of a host who is done with the world: the session ends or its level closes. At once, whatever the host is
// doing; only a level that cannot be saved at all skips it, and a Restore window that is open (the host may be going back
// to a slot this would overwrite).
void save_at_end(const char* what)
{
	g_pending = Pending(); // the save below covers it
	if (pause_holds(g_pause, mgr().now_ms()))
	{
		::Msg("[coop] autosave at %s skipped: the Restore window is open", what);
		return;
	}
	if (!can_write())
	{
		::Msg("[coop] autosave at %s skipped: the host has no running level to save", what);
		return;
	}
	save(Reason::SessionEnd);
}

// ---------------------------------------------------------------- console

class CCC_CoopAutosaveNow : public IConsole_Command
{
public:
	CCC_CoopAutosaveNow(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override
	{
		if (!mgr().is_host())
		{
			::Msg("! [coop] coop_autosave_now: only the host of a co-op session writes autosaves");
			return;
		}
		request(Reason::Manual);
		::Msg("[coop] autosave requested: it is written at the next safe frame");
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "host: write an autosave coop_auto_N at the next safe frame (the host's character up and awake)");
	}
};

// ---------------------------------------------------------------- Lua

// coop.autosave.suspend(on) -> bool: the Restore window holds / lets go the pause of the autosaves (see suspend).
// A plain C function: a wrong argument from an addon returns false instead of a luabind overload error, fatal here.
int l_suspend(lua_State* L)
{
	if (lua_type(L, 1) != LUA_TBOOLEAN)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	suspend(lua_toboolean(L, 1) != 0);
	lua_pushboolean(L, 1);
	return 1;
}
} // namespace

// ---------------------------------------------------------------- public

void install_server(Session&) {}

void install_client(Endpoint&) {}

void reset()
{
	g_schedule = Schedule();
	g_schedule.last_ms = mgr().now_ms(); // the interval counts from the start of the session
	g_pause = Pause(); // a pause is never carried over to another session
	g_bad_slots.clear();
	g_pending = Pending();
	g_saving = false;
	g_level_slot = 0;
}

void register_console()
{
	CMD4(CCC_Integer, "coop_autosave_interval", &g_interval_min, 0, 120); // host setting: minutes, 0 = off
	CMD4(CCC_Integer, "coop_autosave_count", &g_count, 1, 50); // host setting: slots that rotate
	CMD1(CCC_CoopAutosaveNow, "coop_autosave_now");
}

// Fills coop.autosave (made here: the table is ours).
void register_lua(lua_State* L)
{
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // the registrator logs it
	}
	lua_newtable(L);
	lua_pushcfunction(L, &l_suspend);
	lua_setfield(L, -2, "suspend");
	lua_setfield(L, -2, "autosave"); // coop.autosave = { suspend = ... }
	lua_pop(L, 1); // coop
}

void suspend(bool on)
{
	if (!mgr().is_host())
		return;
	const u64 now = mgr().now_ms();
	if (on)
	{
		const bool was = pause_holds(g_pause, now);
		pause_hold(g_pause, now, kPauseLeaseMs);
		if (!was)
			::Msg("[coop] autosave paused: the Restore window is open");
	}
	else if (pause_release(g_pause))
	{
		resume(now, "the Restore window let go");
	}
}

void request(Reason r)
{
	if (!mgr().is_host() || g_pending.on) // one save serves every request made before it
		return;
	g_pending.on = true;
	g_pending.why = r;
	g_pending.since = mgr().now_ms();
	g_pending.busy_since = 0;
}

void frame()
{
	if (!mgr().is_host())
	{
		g_pending = Pending(); // a client, or no session: nothing to write, nothing to wait for
		return;
	}
	if (g_saving)
		return;
	const u64 now = mgr().now_ms();
	if (pause_lapsed(g_pause, now))
		resume(now, "the Restore window closed"); // nobody renewed the pause: the window was closed
	if (pause_holds(g_pause, now))
		return; // the host may be picking a slot: nothing is written, no slot is overwritten
	const bool bad = bad_moment();

	if (g_pending.on)
	{
		bool go = !bad;
		if (bad)
		{
			if (g_pending.busy_since == 0)
				g_pending.busy_since = now;
			go = now - g_pending.busy_since >= kMaxPostponeMs && can_write(); // postponed long enough: now or never
		}
		else
		{
			g_pending.busy_since = 0;
		}
		if (go)
		{
			const Reason why = g_pending.why;
			g_pending = Pending();
			save(why);
		}
		else if (now - g_pending.since >= kRequestLifeMs)
		{
			++g_stat.dropped;
			::Msg("! [coop] autosave (%s) dropped: no way to save it for %u s", reason_name(g_pending.why),
			    u32(kRequestLifeMs / 1000));
			g_pending = Pending();
		}
		return;
	}

	if (g_interval_min == 0)
	{
		g_schedule.last_ms = now; // switched on again later: a full interval from then, not at once
		g_schedule.busy_since = 0;
		return;
	}
	if (timer_due(g_schedule, now, uint32_t(g_interval_min), bad, kMaxPostponeMs))
		save(Reason::Timer);
}

const char* level_change_name()
{
	if (!mgr().is_host() || g_saving)
		return nullptr;
	// While the Restore window is open every slot may be the one the host is going back to: the stock name is used then,
	// the level change is saved outside the ring.
	if (pause_holds(g_pause, mgr().now_ms()))
	{
		::Msg("[coop] autosave (%s) goes under the stock name: the Restore window is open", reason_name(Reason::LevelChange));
		return nullptr;
	}
	// change_level has the world written to level_change_temp_name() and calls level_change_commit(), which moves it to
	// this slot (or the next one if its files cannot be replaced); the new level is loaded from the slot.
	if (!begin_temp())
		return nullptr; // the stock name: the level change must go on
	static string64 name;
	g_level_slot = pick_slot();
	xr_strcpy(name, slot_name(g_level_slot).c_str());
	return name;
}

const char* level_change_temp_name() { return g_level_slot ? kTempStem : nullptr; }

const char* level_change_commit()
{
	const uint32_t first = g_level_slot;
	g_level_slot = 0;
	if (!first)
		return nullptr;
	Commit result = Commit::NotMoved;
	const uint32_t slot = commit_somewhere(first, result);
	if (!slot)
	{
		++g_stat.failed;
		if (result == Commit::NotWritten)
		{
			::Msg("! [coop] autosave (%s) failed: the world was not written", reason_name(Reason::LevelChange));
			return nullptr;
		}
		::Msg("! [coop] autosave (%s) failed: no slot took it, the new level loads from %s",
		    reason_name(Reason::LevelChange), kTempStem);
		return kTempStem;
	}
	static string64 name;
	const std::string slot_text = slot_name(slot);
	xr_strcpy(name, slot_text.c_str());
	note_saved(Reason::LevelChange, slot_text);
	::Msg("[coop] autosave %s (%s)", name, reason_name(Reason::LevelChange));
	prune_surplus(slot);
	return name;
}

void host_on_session_end()
{
	if (!mgr().is_host())
		return;
	save_at_end("the end of the session");
}

void host_on_level_exit()
{
	// A level that closes to open another (a level change, a load, a new game) queues KERNEL:start behind this
	// disconnect (CApplication::OnEvent, the same test as for the main menu): its world is saved by its own path.
	if (!mgr().is_host() || Engine.Event.Peek("KERNEL:start"))
		return;
	save_at_end("the close of the host's level");
}

void print_status()
{
	::Msg("[coop]   autosave: %s, keeps %d, saved timer %llu, after sleep %llu, level change %llu, end %llu, manual %llu; "
	      "skipped %llu, failed %llu, slot moves failed %llu, dropped %llu%s%s",
	    g_interval_min ? make_string("every %d min", g_interval_min).c_str() : "timer off", g_count,
	    g_stat.saved[size_t(Reason::Timer)], g_stat.saved[size_t(Reason::Sleep)],
	    g_stat.saved[size_t(Reason::LevelChange)], g_stat.saved[size_t(Reason::SessionEnd)],
	    g_stat.saved[size_t(Reason::Manual)], g_stat.skipped, g_stat.failed, g_stat.slots_failed, g_stat.dropped,
	    g_pending.on ? make_string(", %s save waits", reason_name(g_pending.why)).c_str() : "",
	    pause_holds(g_pause, mgr().now_ms()) ? ", PAUSED (Restore window)" : "");
	if (g_last.at_ms)
		::Msg("[coop]   autosave: last %s (%s) %llu s ago", g_last.name.c_str(), reason_name(g_last.why),
		    (mgr().now_ms() - g_last.at_ms) / 1000);
	if (!g_bad_slots.empty())
	{
		std::string bad;
		for (const uint32_t s : g_bad_slots)
			bad += (bad.empty() ? "" : ", ") + slot_name(s);
		::Msg("[coop]   autosave: slots that could not be replaced and are skipped: %s", bad.c_str());
	}
}
} // namespace autosave
} // namespace coop
