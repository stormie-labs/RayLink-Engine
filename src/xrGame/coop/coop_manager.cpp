#include "stdafx.h"
#include "coop_manager.h"

#include "coop_join.h"
#include "coop_sync.h"
#include "coop_script.h"
#include "coop_autosave.h" // F3
#include "coop_dialog.h" // hud_message_n: the host-silence line
#include "xrCoop/coop_link_hold.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

string64 g_coop_nick = "";

namespace coop
{
namespace
{
void host_silence_off(); // below, by Manager::frame
void log_line(const char* line) { ::Msg("%s", line); }

// W9: silence before a peer is dropped. A process that stops pumping without a hold (crash dialog, hang) is gone
// after this; ENet rounds it up to its next retransmission (10-16 s). Was 120 s, which kept a crashed client
// "connected" for two minutes.
constexpr u32 kPeerTimeoutMs = 10000;
// Live 2026-10-24 (owner): "if the server does not answer, everything freezes on the client and he cannot move; out only
// after 30 seconds". A live client whose host has said nothing (not even an acknowledgement) for kHostSilenceFreezeMs
// pauses its game and shows the seconds left; the host back - it runs on. A client's own silence timeout is
// kHostSilenceLeaveMs: then on_disconnected takes him out of the host's world (join::on_session_lost).
constexpr u32 kHostSilenceFreezeMs = 2000;
constexpr u32 kHostSilenceLeaveMs = 30000;
// W9: how long one loading activity holds the link (re-armed by every load stage and by every frame until this machine
// plays normally again, coop_link_hold.h): the longest single stage we wait for, e.g. the texture upload of a big
// level on a slow disk.
constexpr u32 kLoadHoldMs = link_hold::kLoadHoldMs;

// R1 I2: a crash (FATAL dialog, access violation) must look like silence to the peers at once. Without it a client
// that crashed while loading stays "connected", his character in the world, until the load hold runs out
// (kLoadHoldMs), and his restarted game cannot take his place before. Installed while a session runs (only a session
// holds the link), in front of the handler that was there (the console's cvar dump).
crashhandler* g_next_crash_handler = nullptr;
bool g_crash_hook_exit_registered = false;
// Set by on_crash (any thread), never cleared: no frame and no load stage holds the link after a crash (review of the
// live fixes: the next frame of the main thread armed the hold again for kLoadHoldMs; link_hold::FrameState::crashed).
// The flag only spares the work: the guarantee is the sealed hold (Session::seal_connection_hold), since a caller that
// read the flag just before on_crash still arms (review of protocol 8).
std::atomic<bool> g_crashed{false};

// Every crash path of xrDebug runs it: FATAL / R_ASSERT (backend, before the crash save), an access violation
// (UnhandledFilter), CHECK_OR_EXIT (do_exit).
void on_crash()
{
	g_crashed.store(true);
	Manager::get().session().seal_connection_hold(); // atomics only: fine from any thread, also mid-crash
	if (g_next_crash_handler)
		g_next_crash_handler();
}

// Main thread. Idempotent; leaves alone a handler someone installed after ours.
void set_crash_hook(bool on)
{
	if (on == (Debug.get_crashhandler() == &on_crash))
		return;
	if (on)
	{
		g_next_crash_handler = Debug.get_crashhandler();
		Debug.set_crashhandler(&on_crash);
	}
	else
	{
		Debug.set_crashhandler(g_next_crash_handler);
		g_next_crash_handler = nullptr;
	}
}

// At exit with a session still running: off before Manager (and its Session) is destroyed.
void remove_crash_hook() { set_crash_hook(false); }

// "-key value" from the command line; false if absent. Value is empty if the key has none.
bool cmd_param(LPCSTR key, xr_string& value)
{
	const char* p = Core.Params ? strstr(Core.Params, key) : nullptr;
	if (!p)
		return false;
	p += xr_strlen(key);
	if (*p && *p != ' ')
		return false; // a longer key with the same prefix
	while (*p == ' ')
		++p;
	value.clear();
	if (*p == '-')
		return true;
	while (*p && *p != ' ')
		value.push_back(*p++);
	return true;
}

// Test automation without input devices (P8): flags make an instance host, connect,
// dump its status and take screenshots on its own.
struct AutoStart
{
	bool parsed = false;
	bool host = false;
	u16 host_port = kDefaultPort;
	bool host_done = false;
	xr_string connect;
	u64 next_connect = 0;
	u32 connect_tries = 0;
	bool welcomed = false;
	u32 status_every_s = 0;
	u64 next_status = 0;
	u32 shots_every_s = 0;
	u64 next_shot = 0;
} g_auto;

u64 fnv1a(const void* data, size_t size, u64 h = 14695981039346656037ull)
{
	const u8* p = static_cast<const u8*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

// Files under gamedata that Anomaly (and addons like MCM) write at run time: user options, caches,
// logs. Two players with identical content have different copies of them, so they are not content.
bool is_runtime_written(LPCSTR name)
{
	string_path low;
	xr_strcpy(low, name);
	xr_strlwr(low);
	for (char* c = low; *c; ++c)
		if (*c == '/')
			*c = '\\';
	// localization.ltx: the game writes the chosen language at start (live check 2026-10-23: a second instance
	// started after the host had it, the host did not, and the join was refused as a different mod list)
	static const char* exact[] = {"configs\\axr_options.ltx", "configs\\cache_dbg.ltx", "configs\\localization.ltx",
	    "configs\\localization.ltx.bak"};
	for (const char* e : exact)
		if (!xr_strcmp(low, e))
			return true;
	const size_t n = xr_strlen(low);
	auto ends = [&](const char* ext) { const size_t k = xr_strlen(ext); return n >= k && !xr_strcmp(low + n - k, ext); };
	return ends(".log") || ends(".txt") || ends(".bak") || ends(".tmp");
}

// Path relative to the game root, lowercase, backslashes.
xr_string relative_to_root(LPCSTR full)
{
	string_path root;
	FS.update_path(root, "$fs_root$", "");
	xr_string f = full, r = root;
	xr_strlwr(f.data());
	xr_strlwr(r.data());
	for (char& c : f) if (c == '/') c = '\\';
	for (char& c : r) if (c == '/') c = '\\';
	if (!r.empty() && f.compare(0, r.size(), r) == 0)
		f.erase(0, r.size());
	return f;
}
} // namespace

const char* phase_name(Phase p)
{
	switch (p)
	{
	case Phase::Off: return "off";
	case Phase::Hosting: return "hosting";
	case Phase::Connecting: return "connecting";
	case Phase::Handshaked: return "handshaked";
	case Phase::ReceivingWorld: return "receiving-world";
	case Phase::LoadingWorld: return "loading-world";
	case Phase::Live: return "live";
	}
	return "?";
}

u8 roles_of(Phase p)
{
	switch (p)
	{
	case Phase::Off: return 0;
	case Phase::Hosting: return u8(ProcessRole::WorldServer) | u8(ProcessRole::MapAuthority) | u8(ProcessRole::Player);
	case Phase::Connecting:
	case Phase::Handshaked:
	case Phase::ReceivingWorld:
	case Phase::LoadingWorld:
	case Phase::Live: return u8(ProcessRole::Player) | u8(ProcessRole::Mirror);
	}
	return 0;
}

Manager& Manager::get()
{
	static Manager instance;
	return instance;
}

Manager::Manager()
{
	m_session.set_log(&log_line);
	m_session.set_world_gate([this] { return world_state(); }); // level-change review: messages for a missing world
	// F3a T4: the host player's link, for the life of the process (a session that does not host leaves it alone). Only
	// the messages moved onto it pass (link_enabled; none yet): the host keeps its direct paths until T7 moves them.
	m_link.set_gate(&link_enabled);
	m_session.attach_local(&m_link);
	install_session_callbacks();
}

u64 Manager::now_ms() const { return GetTickCount64(); }

bool world_up() { return g_pGameLevel && g_pGameLevel->bReady && g_actor && ai().get_alife(); }

bool world_gap() { return g_pGameLevel && !world_up(); }

wgate::World Manager::world_state() const
{
	const bool up = world_up();
	if (m_phase == Phase::Hosting)
		return up ? wgate::World::Up : wgate::World::Hole; // a world comes back: the host builds it
	if (m_phase == Phase::Live)
		return up ? wgate::World::Up : wgate::World::Absent; // its level is gone: it joins again from the host's save
	return wgate::World::Absent; // connecting, choosing a character, receiving, loading
}

void Manager::set_phase(Phase p)
{
	if (p == m_phase)
		return;
	::Msg("[coop] phase %s -> %s", phase_name(m_phase), phase_name(p));
	m_phase = p;
	m_roles = roles_of(p); // F3a T4
	set_crash_hook(p != Phase::Off); // R1 I2: a crash releases the link hold
	if (p != Phase::Off && !g_crash_hook_exit_registered)
	{
		g_crash_hook_exit_registered = true;
		std::atexit(&remove_crash_hook); // registered after Manager's construction: runs before its destructor
	}
	// G18: a session world never stands still. A pause from before the session (new game, menu) is lifted
	// when this machine starts hosting or enters the world; in the session CanBePaused() refuses new ones.
	if ((p == Phase::Hosting || p == Phase::Live) && Device.Paused())
		Device.Pause(FALSE, TRUE, TRUE, "coop_session");
}

const char* Manager::build_id()
{
	// Same exe on both sides is the supported setup; compile stamp + protocol identify it.
	static string128 id = "";
	if (!id[0])
		xr_sprintf(id, "anomaly-coop p%u %s %s", kProtocolVersion, __DATE__, __TIME__);
	return id;
}

const char* Manager::nick() const
{
	if (g_coop_nick[0])
		return g_coop_nick;
	return Core.UserName;
}

const PlayerKey& Manager::player_key()
{
	if (m_key_loaded)
		return m_key;

	string_path fn;
	FS.update_path(fn, "$app_data_root$", "coop_player.key");
	bool ok = false;
	if (FILE* f = std::fopen(fn, "rb"))
	{
		ok = std::fread(m_key.data(), 1, m_key.size(), f) == m_key.size();
		std::fclose(f);
	}
	if (!ok)
	{
		m_key = generate_player_key();
		if (FILE* f = std::fopen(fn, "wb"))
		{
			std::fwrite(m_key.data(), 1, m_key.size(), f);
			std::fclose(f);
			::Msg("[coop] new player key created: %s (id %08x)", fn, player_key_hash(m_key));
		}
		else
			::Msg("! [coop] cannot write player key %s, using a temporary one", fn);
	}
	m_key_loaded = true;
	return m_key;
}

const Manifest& Manager::manifest()
{
	if (m_manifest_ready)
		return m_manifest;

	m_manifest.clear();
	// Archives: db\*.db* and db\mods\*.
	for (const auto& a : FS.m_archives)
	{
		ManifestEntry e;
		e.path = relative_to_root(a.path.c_str()).c_str();
		e.hash = fnv1a(&a.size, sizeof(a.size));
		m_manifest.push_back(std::move(e));
	}
	// Loose files in gamedata (addons). Name + size: cheap and catches different versions.
	FS_FileSet files;
	FS.file_list(files, "$game_data$", FS_ListFiles);
	for (const FS_File& f : files)
	{
		if (f.attrib & FS_File::flVFS)
			continue;
		if (is_runtime_written(f.name.c_str()))
			continue; // per-player settings and caches the game rewrites while running: not content
		ManifestEntry e;
		e.path = xr_string("gamedata\\").append(f.name).c_str();
		const long size = f.size;
		e.hash = fnv1a(&size, sizeof(size));
		m_manifest.push_back(std::move(e));
	}
	m_manifest_ready = true;
	::Msg("[coop] content manifest: %u entries, fingerprint %016llx", (u32)m_manifest.size(),
	    manifest_fingerprint(m_manifest));
	return m_manifest;
}

SessionConfig Manager::make_config(u16 port)
{
	SessionConfig cfg;
	cfg.port = port;
	cfg.build_id = build_id();
	cfg.manifest = manifest();
	cfg.key = player_key();
	cfg.nick = nick();
	// A level load does not pump the network for tens of seconds: on_blocking_work() holds the link meanwhile.
	cfg.peer_timeout_ms = kPeerTimeoutMs;
	cfg.connect_timeout_ms = 15000;
	return cfg;
}

void Manager::install_session_callbacks()
{
	m_session.on_peer_joined = [this](const PeerInfo& p)
	{
		::Msg("[coop] player joined: #%u '%s' %s", p.id, p.nick.c_str(), p.address.c_str());
		if (is_host())
			join::on_peer_joined(p);
		sync::on_peer_joined(p);
	};
	m_session.on_peer_left = [this](PeerId id, const std::string& reason)
	{
		::Msg("[coop] player left: #%u (%s)", id, reason.c_str());
		sync::on_peer_left(id);
		if (is_host())
			join::on_peer_left(id);
		else
			diag::arm_destroyed_object_stacks(); // F0 final review (task 14): his avatar leaves this world too
	};
	m_session.on_welcome = [this](const WelcomeMsg& w)
	{
		::Msg("[coop] accepted by host '%s' as #%u", w.host_nick.c_str(), w.slot);
		set_phase(Phase::Handshaked);
		join::on_welcome(w);
	};
	m_session.on_rejected = [this](const RejectMsg& r, const ManifestDiff& diff)
	{
		::Msg("! [coop] host refused: %s — %s", reject_reason_name(r.reason), r.text.c_str());
		if (r.reason == RejectReason::ContentMismatch)
		{
			::Msg("! [coop] mod list differs: %u missing here, %u extra here, %u different",
			    (u32)diff.missing_here.size(), (u32)diff.extra_here.size(), (u32)diff.differ.size());
			for (const auto& s : diff.missing_here) ::Msg("!   missing here: %s", s.c_str());
			for (const auto& s : diff.extra_here) ::Msg("!   extra here:   %s", s.c_str());
			for (const auto& s : diff.differ) ::Msg("!   different:    %s", s.c_str());
		}
		// -coop_connect retries only what changes by itself (the host still loading, a lost answer). Anything else
		// refuses every retry and pops the refusal window again every few seconds (live check 2026-10-23).
		if (r.reason != RejectReason::HostNotReady && r.reason != RejectReason::Timeout && !g_auto.connect.empty())
		{
			::Msg("! [coop] autostart: no more connection attempts after this refusal");
			g_auto.connect.clear();
		}
		script::on_rejected(r.reason, r.text.c_str(), diff); // W3d: refusal window (R-1.3, R-1.4)
		join::on_session_lost(r.text.c_str());
		set_phase(Phase::Off);
	};
	m_session.on_disconnected = [this](const std::string& reason)
	{
		::Msg("! [coop] disconnected: %s", reason.c_str());
		host_silence_off(); // the pause of a silent host does not outlive the session
		script::on_session_end(reason.c_str()); // W4 menu: last error in the co-op window
		join::on_session_lost(reason.c_str());
		sync::reset();
		set_phase(Phase::Off);
	};
	m_session.on_chat = [](PeerId from, const std::string& text)
	{
		const PeerInfo* p = mgr().session().peer(from);
		const char* who = p ? p->nick.c_str() : (from == kHostPeer ? "host" : "?");
		::Msg("[coop] <%s> %s", who, text.c_str());
		script::on_chat(who, text.c_str()); // HUD line (coop_main.on_chat)
	};
	m_session.on_blob_progress = [](PeerId from, u32 id, u32 kind, u64 done, u64 total)
	{
		join::on_blob_progress(from, id, kind, done, total);
	};
	m_session.on_blob_complete = [](PeerId from, u32 id, u32 kind, std::vector<u8>& data)
	{
		join::on_blob_complete(from, id, kind, data);
	};

	// F3a T4: what the modules set up once per process (settings, console commands, the route and call-gate tables); the
	// handler tables are installed by role when a session starts (install_handlers).
	sync::init();
}

void Manager::install_handlers(bool hosting)
{
	m_session.clear_handlers();
	m_link.clear_handlers();
	if (hosting)
	{
		// the world server: its handlers on the session; the host player, a client of it: his on the link
		join::install_server(m_session);
		sync::install_server(m_session);
		join::install_client(m_link);
		sync::install_client(m_link);
	}
	else
	{
		// a remote client (player and mirror): the client's handlers on the session connected to the host
		join::install_client(m_session);
		sync::install_client(m_session);
	}
}

bool Manager::start_host(u16 port)
{
	if (active())
	{
		::Msg("! [coop] already in a session (%s), use coop_disconnect first", phase_name(m_phase));
		return false;
	}
	if (!join::host_can_start())
		return false;

	install_handlers(true); // F3a T4
	std::string err;
	if (!m_session.host(make_config(port), &err))
	{
		::Msg("! [coop] cannot host on port %u: %s", port, err.c_str());
		return false;
	}
	set_phase(Phase::Hosting);
	join::on_host_started();
	sync::on_session_started();
	return true;
}

bool Manager::start_client(const char* address, u16 port)
{
	if (active())
	{
		::Msg("! [coop] already in a session (%s), use coop_disconnect first", phase_name(m_phase));
		return false;
	}
	install_handlers(false); // F3a T4
	std::string err;
	SessionConfig cfg = make_config(port);
	cfg.peer_timeout_ms = kHostSilenceLeaveMs; // the client waits for a silent host this long (frozen meanwhile)
	if (!m_session.connect(cfg, address, port, &err))
	{
		::Msg("! [coop] cannot connect to %s:%u: %s", address, port, err.c_str());
		return false;
	}
	set_phase(Phase::Connecting);
	::Msg("[coop] connecting to %s:%u as '%s'", address, port, nick());
	return true;
}

void Manager::stop(const char* reason)
{
	if (!active())
		return;
	autosave::host_on_session_end(); // F3: the host's last world save of the session, while the level still runs
	m_session.shutdown(reason);
	script::on_session_end(reason); // W4 menu: last error in the co-op window
	join::on_session_lost(reason);
	sync::reset();
	set_phase(Phase::Off);
}

void Manager::send_chat(const char* text)
{
	if (!m_session.is_active())
	{
		::Msg("! [coop] not in a session");
		return;
	}
	m_session.send_chat(text);
	::Msg("[coop] <%s> %s", nick(), text);
	script::on_chat(nick(), text); // our own line in the HUD too
}

void Manager::set_netsim(const NetSim& sim)
{
	m_session.set_netsim(sim);
	if (sim.enabled())
		::Msg("[coop] netsim: lag %u ms, jitter %u ms, loss %.1f%%", sim.lag_ms, sim.jitter_ms, sim.loss * 100.f);
	else
		::Msg("[coop] netsim off");
}

void Manager::autostart()
{
	if (!g_auto.parsed)
	{
		g_auto.parsed = true;
		xr_string v;
		if (cmd_param("-coop_nick", v) && !v.empty())
			xr_strcpy(g_coop_nick, v.c_str());
		if (cmd_param("-coop_host", v))
		{
			g_auto.host = true;
			if (!v.empty())
				g_auto.host_port = (u16)atoi(v.c_str());
		}
		if (cmd_param("-coop_connect", v))
			g_auto.connect = v;
		if (cmd_param("-coop_autostatus", v))
			g_auto.status_every_s = (u32)atoi(v.c_str());
		if (cmd_param("-coop_shots", v))
			g_auto.shots_every_s = (u32)atoi(v.c_str());
		if (g_auto.host || !g_auto.connect.empty())
			::Msg("[coop] autostart: host=%d port=%u connect='%s' status=%us shots=%us", g_auto.host, g_auto.host_port,
			      g_auto.connect.c_str(), g_auto.status_every_s, g_auto.shots_every_s);
	}
	const u64 now = now_ms();
	const bool level_ready = g_pGameLevel && g_actor && !Device.dwPrecacheFrame && ai().get_alife();

	if (g_auto.host && !g_auto.host_done && !active() && level_ready)
	{
		g_auto.host_done = true;
		start_host(g_auto.host_port);
	}
	if (!g_auto.connect.empty() && active() && m_phase != Phase::Connecting)
		g_auto.welcomed = true;
	if (!g_auto.connect.empty() && !active() && now >= g_auto.next_connect && g_auto.connect_tries < 120)
	{
		// Host not up yet: retry soon. Joined once and failed later: back off, do not hammer the host
		// (every attempt costs it a world save).
		g_auto.next_connect = now + (g_auto.welcomed ? 60000 : 5000);
		++g_auto.connect_tries;
		xr_string address = g_auto.connect;
		u16 port = kDefaultPort;
		const size_t colon = address.rfind(':');
		if (colon != xr_string::npos)
		{
			port = (u16)atoi(address.c_str() + colon + 1);
			address.erase(colon);
		}
		start_client(address.c_str(), port);
	}
	if (g_auto.status_every_s && active() && now >= g_auto.next_status)
	{
		g_auto.next_status = now + u64(g_auto.status_every_s) * 1000;
		print_status();
		if (world_up()) // level-change review: Level().Objects of a level that is not up has nothing to count
			sync::dump_state("summary");
	}
	if (g_auto.shots_every_s && level_ready && now >= g_auto.next_shot)
	{
		g_auto.next_shot = now + u64(g_auto.shots_every_s) * 1000;
		Device.Screenshot();
	}
}

namespace
{
bool g_silence_paused = false;
u32 g_silence_shown = ~0u;

void host_silence_off()
{
	if (!g_silence_paused)
		return;
	g_silence_paused = false;
	g_silence_shown = ~0u;
	if (Device.Paused())
		Device.Pause(FALSE, TRUE, TRUE, "coop_host_silent");
	::Msg("[coop] the host answers again: the game runs on");
}

void host_silence_step()
{
	const u32 silent = mgr().is_live_client() && g_pGameLevel ? mgr().session().ms_since_host_heard() : 0;
	if (silent < kHostSilenceFreezeMs)
	{
		host_silence_off();
		return;
	}
	if (!g_silence_paused)
	{
		g_silence_paused = true;
		if (!Device.Paused())
			Device.Pause(TRUE, TRUE, TRUE, "coop_host_silent");
		::Msg("! [coop] the host does not answer (%u ms): the game waits for it, out after %u s", silent,
		    kHostSilenceLeaveMs / 1000);
	}
	const u32 left = silent >= kHostSilenceLeaveMs ? 0 : (kHostSilenceLeaveMs - silent + 999) / 1000;
	if (left != g_silence_shown)
	{
		g_silence_shown = left;
		dialog::hud_message_n("st_coop_host_silent", "No connection to the host - waiting %u s", left);
	}
}
} // namespace

void Manager::frame()
{
	autostart();
	if (m_phase == Phase::Off)
		return;
	// W9: the link stays held until this machine plays normally again, not just until its last load step: the first
	// frames after a load (precache, the first update of every script, a client's way to live) can each stall longer
	// than the silence timeout (live check 2026-10-23: a GAMMA client was dropped there). Until then every frame
	// re-arms the hold, so a hang still goes silent kLoadHoldMs after its last frame; a crash releases it at once and
	// for good (on_crash seals it: this arm is refused even if the crash came after the read of g_crashed). Afterwards a
	// stall must look like silence to the peers.
	link_hold::FrameState fs;
	fs.load_steps_queued = !g_loading_events.empty();
	fs.precache_frames = g_pGameLevel && Device.dwPrecacheFrame != 0;
	fs.joining_world = m_phase == Phase::LoadingWorld;
	fs.crashed = g_crashed.load();
	m_session.hold_connection(link_hold::keep_held(fs) ? kLoadHoldMs : 0);
	m_session.update(now_ms());
	host_silence_step(); // live 2026-10-24: a silent host freezes the client
	join::frame();
	sync::frame();
}

void Manager::print_status() const
{
	::Msg("[coop] build '%s', protocol %u", build_id(), kProtocolVersion);
	::Msg("[coop] phase %s, self #%u, nick '%s'", phase_name(m_phase), m_session.self(), nick());
	if (!active())
		return;
	::Msg("[coop] silence timeout %u ms, keep-alive services %llu", kPeerTimeoutMs, m_session.keepalive_services());
	for (const PeerInfo& p : m_session.peers())
		::Msg("[coop]   #%u %-16s rtt %4u ms loss %4.1f%% %s %s", p.id, p.nick.c_str(), p.rtt_ms,
		    p.packet_loss * 100.f, p.state == PeerState::Joined ? "joined" : "connecting", p.address.c_str());
	const NetSim& sim = m_session.netsim();
	if (sim.enabled())
		::Msg("[coop] netsim: lag %u ms, jitter %u ms, loss %.1f%%", sim.lag_ms, sim.jitter_ms, sim.loss * 100.f);
	if (is_host())
	{
		// F3a T4: the host player's LocalLink; "refused" are the messages not moved onto it yet (the host's direct paths)
		const LocalLink::Stats& ls = m_link.stats();
		::Msg("[coop] local link (the host player): to the server %llu, to the host player %llu, refused %llu (not moved "
		      "onto the link), queued %u / %u, oversize %llu, overflow %llu, stale %llu (an older world)",
		    ls.to_server, ls.to_client, ls.refused, u32(m_link.queued_to_server()), u32(m_link.queued_to_client()),
		    ls.oversize, ls.overflow, ls.stale);
	}
	for (u16 type : m_session.stats_types())
	{
		const MsgStats& s = m_session.stats(type);
		::Msg("[coop]   %-18s out %6llu msg %9llu B | in %6llu msg %9llu B", msg_name(type), s.sent_count,
		    s.sent_bytes, s.recv_count, s.recv_bytes);
	}
	// Level-change review: game messages that met a world that was not there (xrCoop/coop_world_gate.h)
	const Session::WorldGateStats gs = m_session.world_gate_stats();
	const char* world_now = "up";
	switch (world_state())
	{
	case wgate::World::Up: break;
	case wgate::World::Hole: world_now = "HOLE (a level is being built or torn down)"; break;
	case wgate::World::Absent: world_now = "absent (this machine is not in the host's world)"; break;
	}
	::Msg("[coop] world now: %s; %llu gap(s) with a game message due; waiting %u (%u B): ran %llu, void %llu (older world), "
	      "expired %llu, pushed out %llu, sender gone %llu",
	    world_now, gs.gaps, u32(gs.queued), u32(gs.queued_bytes), gs.hold.replayed, gs.hold.void_epoch, gs.hold.expired,
	    gs.hold.evicted, gs.hold.gone);
	for (u16 type : m_session.stats_types())
	{
		const MsgStats& s = m_session.stats(type);
		if (s.world_held || s.world_dropped)
			::Msg("[coop]   no world: %-18s %6llu waited, %6llu dropped", msg_name(type), s.world_held, s.world_dropped);
	}
	join::print_status();
	sync::print_status();
}

void on_frame() { Manager::get().frame(); }

void on_blocking_work()
{
	Manager& m = Manager::get();
	if (m.active() && link_hold::may_arm(g_crashed.load()))
		m.session().hold_connection(kLoadHoldMs); // atomics only: fine from a loader thread; refused after a crash
}

bool skip_keypress_on_load()
{
	if (Manager::get().is_client())
		return true;
	return Core.Params && (strstr(Core.Params, "-coop_host") || strstr(Core.Params, "-coop_connect"));
}
} // namespace coop
