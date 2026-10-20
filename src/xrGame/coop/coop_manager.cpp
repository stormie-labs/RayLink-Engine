#include "stdafx.h"
#include "coop_manager.h"

#include "coop_join.h"
#include "coop_sync.h"
#include "coop_script.h"

#include <cstdio>

string64 g_coop_nick = "";

namespace coop
{
namespace
{
void log_line(const char* line) { ::Msg("%s", line); }

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

Manager& Manager::get()
{
	static Manager instance;
	return instance;
}

Manager::Manager()
{
	m_session.set_log(&log_line);
	install_session_callbacks();
}

u64 Manager::now_ms() const { return GetTickCount64(); }

void Manager::set_phase(Phase p)
{
	if (p == m_phase)
		return;
	::Msg("[coop] phase %s -> %s", phase_name(m_phase), phase_name(p));
	m_phase = p;
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
	// A client loading a level does not pump the network for tens of seconds.
	cfg.peer_timeout_ms = 120000;
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
		join::on_session_lost(r.text.c_str());
		set_phase(Phase::Off);
	};
	m_session.on_disconnected = [this](const std::string& reason)
	{
		::Msg("! [coop] disconnected: %s", reason.c_str());
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

	join::install(m_session);
	sync::install(m_session);
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
	std::string err;
	if (!m_session.connect(make_config(port), address, port, &err))
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
	m_session.shutdown(reason);
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
		if (g_pGameLevel)
			sync::dump_state("summary");
	}
	if (g_auto.shots_every_s && level_ready && now >= g_auto.next_shot)
	{
		g_auto.next_shot = now + u64(g_auto.shots_every_s) * 1000;
		Device.Screenshot();
	}
}

void Manager::frame()
{
	autostart();
	if (m_phase == Phase::Off)
		return;
	m_session.update(now_ms());
	join::frame();
	sync::frame();
}

void Manager::print_status() const
{
	::Msg("[coop] build '%s', protocol %u", build_id(), kProtocolVersion);
	::Msg("[coop] phase %s, self #%u, nick '%s'", phase_name(m_phase), m_session.self(), nick());
	if (!active())
		return;
	for (const PeerInfo& p : m_session.peers())
		::Msg("[coop]   #%u %-16s rtt %4u ms loss %4.1f%% %s %s", p.id, p.nick.c_str(), p.rtt_ms,
		    p.packet_loss * 100.f, p.state == PeerState::Joined ? "joined" : "connecting", p.address.c_str());
	const NetSim& sim = m_session.netsim();
	if (sim.enabled())
		::Msg("[coop] netsim: lag %u ms, jitter %u ms, loss %.1f%%", sim.lag_ms, sim.jitter_ms, sim.loss * 100.f);
	for (u16 type : m_session.stats_types())
	{
		const MsgStats& s = m_session.stats(type);
		::Msg("[coop]   %-18s out %6llu msg %9llu B | in %6llu msg %9llu B", msg_name(type), s.sent_count,
		    s.sent_bytes, s.recv_count, s.recv_bytes);
	}
	join::print_status();
	sync::print_status();
}

void on_frame() { Manager::get().frame(); }

bool skip_keypress_on_load()
{
	if (Manager::get().is_client())
		return true;
	return Core.Params && (strstr(Core.Params, "-coop_host") || strstr(Core.Params, "-coop_connect"));
}
} // namespace coop
