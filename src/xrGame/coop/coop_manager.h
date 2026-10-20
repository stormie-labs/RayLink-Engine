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
	void set_phase(Phase p);
	bool active() const { return m_phase != Phase::Off; }
	bool is_host() const { return m_phase == Phase::Hosting; }
	// Client in any phase after transport start.
	bool is_client() const { return m_phase != Phase::Off && m_phase != Phase::Hosting; }
	bool is_live_client() const { return m_phase == Phase::Live; }

	Session& session() { return m_session; }
	const Session& session() const { return m_session; }
	PeerId self() const { return m_session.self(); }

	// Persistent identity of this game folder (appdata\coop_player.key), created on first use.
	const PlayerKey& player_key();
	const char* nick() const;
	static const char* build_id();
	const Manifest& manifest();

	u64 now_ms() const;

private:
	Manager();
	void install_session_callbacks();
	void autostart();
	SessionConfig make_config(u16 port);

	Session m_session;
	Phase m_phase = Phase::Off;
	PlayerKey m_key{};
	bool m_key_loaded = false;
	Manifest m_manifest;
	bool m_manifest_ready = false;
};

inline Manager& mgr() { return Manager::get(); }

// Registers coop_* console commands (called from CCC_RegisterCommands).
void register_console_commands();
// Engine hook, see CGamePersistent::OnFrame.
void on_frame();
// Engine hooks (W9): this thread is about to block and will not pump the network for a while: a level is torn
// down or loaded, a save is written (CLevel::net_Stop, CGamePersistent::Start, every CGamePersistent::LoadTitle
// stage, CALifeStorageManager::save). In a session the link is held alive from a helper thread until the next
// normal frame (at most kLoadHoldMs after the last call). Any thread.
void on_blocking_work();
// Engine hook: skip the "press any key" screen after a level load (a joining client, or an
// instance started with -coop_host / -coop_connect for automated tests).
bool skip_keypress_on_load();
} // namespace coop

// Player nick shown to others (console var coop_nick, saved in user.ltx).
extern string64 g_coop_nick;
