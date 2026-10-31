// Anomaly Coop — engine-side session manager.
// Owns the network Session, drives the join flow and dispatches game messages to the
// coop subsystems. Everything runs on the game main thread (frame() from CGamePersistent).
#pragma once

#include "xrCoop/coop_session.h"

namespace coop
{
// Lifecycle of this machine in a co-op session.
enum class Phase : u8
{
	Off, // no session
	Hosting, // host: listening, world is ours
	Connecting, // client: transport + handshake in progress
	Handshaked, // client: accepted, asked for the world
	ReceivingWorld, // client: world save is downloading
	LoadingWorld, // client: loading the received save
	Live, // client: in the host's world, live sync running
};
const char* phase_name(Phase p);

// F3a split levels, the roles of this process (docs/plans/2026-10-24-f3a-split-levels.md §1.1) as flags. They replace
// the pair host / client: every check "am I the host" is one of the roles below, whichever the code needs.
//   WorldServer   the host's process: the world, offline, time, story, money, the save, the registry of levels;
//   MapAuthority  the host's process for its own level, a map server for the level it serves: the objects, the AI, the
//                 physics, the requests of the players on that level;
//   Player        the host's process (the host player, a client of its own world server over the LocalLink) and a
//                 remote client;
//   Mirror        a remote client: its objects are copies of an authority's;
//   MapServer     a background map server of the host's PC (the key -coop_mapserver, F3a T5/T9).
enum class ProcessRole : u8
{
	WorldServer = 1 << 0,
	MapAuthority = 1 << 1,
	Player = 1 << 2,
	Mirror = 1 << 3,
	MapServer = 1 << 4,
};
// The roles of a phase: hosting = WorldServer | MapAuthority | Player; a client in any phase after its transport start =
// Player | Mirror; off = none. A map server's (MapAuthority | MapServer) comes with T9.
u8 roles_of(Phase p);

class Manager
{
public:
	static Manager& get();

	// Called once per frame from CGamePersistent::OnFrame (menu and in game).
	void frame();

	bool start_host(u16 port);
	bool start_client(const char* address, u16 port);
	void stop(const char* reason);

	void print_status() const;
	void send_chat(const char* text);
	void set_netsim(const NetSim& sim);

	Phase phase() const { return m_phase; }
	void set_phase(Phase p); // also sets the roles (roles_of)
	bool active() const { return m_phase != Phase::Off; }
	// F3a T4: the roles of this process (ProcessRole flags), set with the phase.
	bool has_role(ProcessRole r) const { return (m_roles & u8(r)) != 0; }
	u8 roles() const { return m_roles; }
	// Until a module is moved over, the old names keep their meaning (§1.1): is_host() is the world server (the phase
	// Hosting), is_client() a mirror (a client in any phase after transport start).
	bool is_host() const { return has_role(ProcessRole::WorldServer); }
	bool is_client() const { return has_role(ProcessRole::Mirror); }
	bool is_live_client() const { return m_phase == Phase::Live; }

	Session& session() { return m_session; }
	const Session& session() const { return m_session; }
	PeerId self() const { return m_session.self(); }
	// F3a T4: the internal channel of the host's process (xrCoop/coop_local_link.h): its world server's session sees the
	// host player as peer 0 on it, and it is the host player's Endpoint (the client-side handlers). Attached for the
	// life of the process; it passes only the messages moved onto it (link_enabled). Main thread.
	LocalLink& local_link() { return m_link; }
	const LocalLink& local_link() const { return m_link; }

	// Persistent identity of this game folder (appdata\coop_player.key), created on first use.
	const PlayerKey& player_key();
	const char* nick() const;
	static const char* build_id();
	const Manifest& manifest();

	u64 now_ms() const;

	// What the session's world gate asks whenever a game message is due (xrCoop/coop_world_gate.h): the host's world is Up
	// or in a Hole; a client's is Up only while it is live in the host's world, else Absent. Main thread.
	wgate::World world_state() const;

private:
	Manager();
	void install_session_callbacks();
	// F3a T4: the handler tables of the role this process starts in (every module's install_server / install_client):
	// hosting, the server's on the session and the client's on the host player's LocalLink; connecting, the client's on
	// the session. What a table had before goes first.
	void install_handlers(bool hosting);
	void autostart();
	SessionConfig make_config(u16 port);

	LocalLink m_link; // declared before m_session: it outlives the session it is attached to
	Session m_session;
	Phase m_phase = Phase::Off;
	u8 m_roles = 0; // ProcessRole flags, set with the phase
	PlayerKey m_key{};
	bool m_key_loaded = false;
	Manifest m_manifest;
	bool m_manifest_ready = false;
};

inline Manager& mgr() { return Manager::get(); }

// F3a split levels, the roles of this process (ProcessRole above, §1.1). Until a module is moved over, the old names keep
// their meaning: is_host() = is_world_server() (and the map authority and a player), is_client() = is_mirror() (and a
// player). A process that is no map server (everything today) is every role of the old host or of the old client. Main
// thread.
inline bool is_world_server() { return mgr().has_role(ProcessRole::WorldServer); }
inline bool is_map_authority() { return mgr().has_role(ProcessRole::MapAuthority); } // + a map server, T9
inline bool is_player() { return mgr().has_role(ProcessRole::Player); }
inline bool is_mirror() { return mgr().has_role(ProcessRole::Mirror); }
inline bool is_map_server() { return mgr().has_role(ProcessRole::MapServer); } // the map-server key comes with T5/T9

// Level-change review (owner 2026-10-23, xrCoop/coop_world_gate.h): the level this machine plays is UP: constructed and
// loaded (g_pGameLevel->bReady: set at the end of IGame_Level::Load, cleared first thing in CLevel::net_Stop), the ALife
// simulator and the local actor exist. g_pGameLevel alone says nothing: IGame_Level::IGame_Level() sets it at the START of
// the construction and ai().get_alife() is set at the start of the simulator's constructor (live 2026-10-23: a handler met
// ai().alife() null inside CLevel::CLevel()). Actor() may be called in an up world only: it asserts on GameID(). Main thread.
bool world_up();
// A level exists but is not up: it is being built or torn down. A pass that cleans up "no level at all" itself still runs
// when there is none (!g_pGameLevel); in a gap it must not run.
bool world_gap();

// Registers coop_* console commands (called from CCC_RegisterCommands).
void register_console_commands();
// Engine hook, see CGamePersistent::OnFrame.
void on_frame();
// Engine hooks (W9): this thread is about to block and will not pump the network for a while: a level is torn
// down or loaded, a save is written (CLevel::net_Stop, CGamePersistent::Start, every CGamePersistent::LoadTitle
// stage, CALifeStorageManager::save). In a session the link is held alive from a helper thread until this machine
// plays normally again: no load step queued, precache frames done, a client live (coop_link_hold.h; Manager::frame
// re-arms it until then). At most kLoadHoldMs after the last arm. Any thread.
void on_blocking_work();
// Engine hook: skip the "press any key" screen after a level load (a joining client, or an
// instance started with -coop_host / -coop_connect for automated tests).
bool skip_keypress_on_load();
// The launcher's Steam mode started this game with a lobby: Steam's "invite a friend" window can open over the game.
bool steam_invite_ready();
bool steam_invite_window();
} // namespace coop

// Player nick shown to others (console var coop_nick, saved in user.ltx).
extern string64 g_coop_nick;
