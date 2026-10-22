// Anomaly Coop — the host's emergency autosaves coop_auto_N (F3 Task 12). See coop_autosave.h.
#include "stdafx.h"
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
// A timed or requested save waits for a good moment no longer than this (the max_postpone_ms of timer_due).
constexpr u32 kMaxPostponeMs = 120000;
// A request that found no way to save for this long is dropped: the world it was meant to keep moved on long ago.
constexpr u64 kRequestLifeMs = 300000;
// CALifeStorageManager::save starts the file with u32(-1), the version and the size: less is not a save.
constexpr int kMinSaveBytes = 12;
// Moving a finished file over a slot can meet a virus scanner that holds it for a moment.
constexpr u32 kMoveTries = 3;
constexpr DWORD kMoveRetryMs = 25;

constexpr size_t kReasonCount = size_t(Reason::Manual) + 1;

// ---------------------------------------------------------------- state

int g_interval_min = 10; // coop_autosave_interval: minutes, 0 = off
int g_count = 10; // coop_autosave_count: how many slots rotate

Schedule g_schedule; // the timer

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
	u64 failed = 0; // not written, or written but not moved to its slot
	u64 dropped = 0; // requests that found no moment for kRequestLifeMs
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

// ---------------------------------------------------------------- files

void save_path(string_path& out, LPCSTR stem, LPCSTR ext)
{
	string_path file;
	xr_sprintf(file, "%s%s", stem, ext);
	FS.update_path(out, "$game_saves$", file);
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

uint32_t next_free_slot()
{
	std::vector<SlotInfo> slots;
	list_slots(slots);
	return next_slot(slots, uint32_t(g_count));
}

// Removes a file of ours: the index entry (the .scop went through FS) and whatever is on disk (the .scoc did not).
void remove_file(LPCSTR path)
{
	FS.file_delete(path);
	DeleteFileA(path);
}

// Moves a finished file over `to`. One system call: an old file there is replaced in the same step, so it stays until
// the new one is in place and a failure leaves it as it was. (CLocatorAPI::file_rename unlinks the old file first, does
// not say whether the rename worked, and does not see the .scoc at all.)
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
	::Msg("! [coop] autosave: cannot move '%s' over '%s' (error %lu)", from, to, error);
	return false;
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

// What an earlier crash or a failed save left of coop_auto_tmp must not pass for the save that is about to be written.
void begin_temp()
{
	string_path tmp_scop, tmp_scoc;
	save_path(tmp_scop, kTempStem, SAVE_EXTENSION);
	save_path(tmp_scoc, kTempStem, ".scoc");
	remove_file(tmp_scop);
	remove_file(tmp_scoc);
}

// coop_auto_tmp holds a finished save: moves it over coop_auto_<slot>. true when the slot holds the new save. On false
// the slot is as it was (or, when the second file could not be moved, see below) and coop_auto_tmp is still the way to
// the new world.
bool commit_temp(uint32_t slot)
{
	const std::string name = slot_name(slot);
	string_path tmp_scop, tmp_scoc, to_scop, to_scoc;
	save_path(tmp_scop, kTempStem, SAVE_EXTENSION);
	save_path(tmp_scoc, kTempStem, ".scoc");
	save_path(to_scop, name.c_str(), SAVE_EXTENSION);
	save_path(to_scoc, name.c_str(), ".scoc");

	if (FS.file_length(tmp_scop) < kMinSaveBytes) // -1 = no file
	{
		::Msg("! [coop] autosave: the world was not written to '%s'", tmp_scop);
		return false;
	}

	// Anomaly's Lua part goes first, the .scop (the file the Restore list looks for) last. When the script storage is
	// off there is no .scoc, and an old one in the slot must not be loaded together with this world.
	const bool has_scoc = file_present(tmp_scoc);
	if (has_scoc)
	{
		if (!move_over(tmp_scoc, to_scoc))
			return false;
	}
	else
	{
		::Msg("! [coop] autosave: no script part '%s' was written, %s will load without Anomaly's script data", tmp_scoc,
		    name.c_str());
		DeleteFileA(to_scoc);
	}
	if (!move_over(tmp_scop, to_scop))
	{
		// The slot now has the new script part and the old world: loadable, but the two do not belong together. Only a
		// failure of the second move gets here, after the first one worked. The script part is copied back next to the
		// new world in coop_auto_tmp, which a level change falls back to.
		::Msg("! [coop] autosave: %s keeps its older world with the new script data", name.c_str());
		if (has_scoc)
			CopyFileA(to_scoc, tmp_scoc, FALSE);
		return false;
	}
	rescan_saves(); // the index must see the slot (load, FS.exist) and not the temporary files
	xr_strcpy(g_last_saved_game, name.c_str()); // "Continue" goes to the newest world, as after a stock autosave
	return true;
}

// The host keeps coop_autosave_count slot files: after a save the oldest beyond that go (the rules: slots_to_prune). A
// host that lowered the count drops the surplus over its next saves.
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

// Writes the world as coop_auto_<slot>: to coop_auto_tmp first, then the finished files go over the slot. true when the
// slot holds the new save.
bool write_slot(uint32_t slot)
{
	begin_temp();
	send_save(kTempStem);
	return commit_temp(slot);
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
	const u64 started = mgr().now_ms();
	const uint32_t slot = next_free_slot();
	if (!write_slot(slot))
	{
		++g_stat.failed;
		::Msg("! [coop] autosave (%s) to %s failed", reason_name(why), slot_name(slot).c_str());
		return false;
	}
	const std::string name = slot_name(slot);
	note_saved(why, name);
	::Msg("[coop] autosave %s (%s) written in %u ms", name.c_str(), reason_name(why), u32(mgr().now_ms() - started));
	prune_surplus(slot);
	return true;
}

// The last save of a host who is done with the world: the session ends or its level closes. At once, whatever the host is
// doing; only a level that cannot be saved at all skips it.
void save_at_end(const char* what)
{
	g_pending = Pending(); // the save below covers it
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
} // namespace

// ---------------------------------------------------------------- public

void install(Session&) {}

void reset()
{
	g_schedule = Schedule();
	g_schedule.last_ms = mgr().now_ms(); // the interval counts from the start of the session
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
	// change_level has the world written to level_change_temp_name() and calls level_change_commit(), which moves it
	// over this slot; the new level is loaded from the slot.
	static string64 name;
	g_level_slot = next_free_slot();
	xr_strcpy(name, slot_name(g_level_slot).c_str());
	begin_temp();
	return name;
}

const char* level_change_temp_name() { return g_level_slot ? kTempStem : nullptr; }

bool level_change_commit()
{
	const uint32_t slot = g_level_slot;
	g_level_slot = 0;
	if (!slot)
		return false;
	if (!commit_temp(slot))
	{
		++g_stat.failed;
		::Msg("! [coop] autosave (%s) to %s failed: the new level loads from %s", reason_name(Reason::LevelChange),
		    slot_name(slot).c_str(), kTempStem);
		return false;
	}
	const std::string name = slot_name(slot);
	note_saved(Reason::LevelChange, name);
	::Msg("[coop] autosave %s (%s)", name.c_str(), reason_name(Reason::LevelChange));
	prune_surplus(slot);
	return true;
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
	      "skipped %llu, failed %llu, dropped %llu%s",
	    g_interval_min ? make_string("every %d min", g_interval_min).c_str() : "timer off", g_count,
	    g_stat.saved[size_t(Reason::Timer)], g_stat.saved[size_t(Reason::Sleep)],
	    g_stat.saved[size_t(Reason::LevelChange)], g_stat.saved[size_t(Reason::SessionEnd)],
	    g_stat.saved[size_t(Reason::Manual)], g_stat.skipped, g_stat.failed, g_stat.dropped,
	    g_pending.on ? make_string(", %s save waits", reason_name(g_pending.why)).c_str() : "");
	if (g_last.at_ms)
		::Msg("[coop]   autosave: last %s (%s) %llu s ago", g_last.name.c_str(), reason_name(g_last.why),
		    (mgr().now_ms() - g_last.at_ms) / 1000);
}
} // namespace autosave
} // namespace coop
