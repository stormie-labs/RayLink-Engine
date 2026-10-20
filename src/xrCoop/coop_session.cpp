// Anomaly Coop — Session implementation: handshake, routing, blobs, stats, NetSim.
// Single-threaded. User callbacks are never invoked from inside internal state changes: they are queued
// (m_pending) and run at safe points of update(), so a callback may freely call shutdown()/send()/kick().
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "coop_session.h"
#include "coop_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>

#pragma comment(lib, "bcrypt.lib")

namespace coop
{
namespace
{
constexpr size_t kBlobChunkSize = 64 * 1024; // payload bytes per BlobChunk
constexpr size_t kBulkQueueBytes = 512 * 1024; // max Bulk-lane bytes alive (queued + unacked) per peer
constexpr uint64_t kMaxBlobBytes = 256ull * 1024 * 1024;
constexpr size_t kMaxIncomingBlobsPerPeer = 4;
constexpr size_t kMaxChatBytes = 1024;
constexpr size_t kMaxNickBytes = 48;
constexpr size_t kMaxPacketBytes = 16 * 1024 * 1024; // sanity limit for one game message
constexpr uint32_t kPendingPeerSlots = 16; // transport slots beyond max_players so rejects can be delivered
constexpr int kMaxEventsPerUpdate = 8192;
constexpr uint32_t kShutdownMaxMs = 200;
constexpr uint32_t kDefaultConnectTimeoutMs = 10000;
constexpr uint32_t kDefaultPeerTimeoutMs = 15000;

uint64_t steady_ms()
{
	using namespace std::chrono;
	return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool random_bytes(void* out, size_t size)
{
	return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

uint64_t random_u64()
{
	uint64_t v = 0;
	if (!random_bytes(&v, sizeof(v)))
		v = steady_ms() * 0x9E3779B97F4A7C15ull;
	return v;
}

// Deterministic PRNG (splitmix64) for NetSim.
struct Rng
{
	uint64_t state = 0x9E3779B97F4A7C15ull;
	void seed(uint64_t s) { state = s ? s : 0x9E3779B97F4A7C15ull; }
	uint64_t next()
	{
		uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
	uint32_t below(uint32_t n) { return n ? uint32_t(next() % n) : 0; }
};

void utf8_truncate(std::string& s, size_t max_bytes)
{
	if (s.size() <= max_bytes)
		return;
	size_t cut = max_bytes;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
		--cut;
	s.resize(cut);
}

std::string sanitize_nick(const std::string& in, const char* fallback)
{
	std::string s;
	s.reserve(in.size());
	for (unsigned char ch : in)
		if (ch >= 0x20 && ch != 0x7F)
			s.push_back(char(ch));
	utf8_truncate(s, kMaxNickBytes);
	return s.empty() ? std::string(fallback) : s;
}

const uint32_t* crc_table()
{
	static const struct Table
	{
		uint32_t t[256];
		Table()
		{
			for (uint32_t i = 0; i < 256; ++i)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				t[i] = c;
			}
		}
	} table;
	return table.t;
}

struct OutBlob
{
	uint32_t id = 0;
	uint32_t kind = 0;
	std::vector<uint8_t> data;
	uint32_t crc = 0;
	size_t offset = 0;
	bool begun = false;
};

struct InBlob
{
	uint32_t kind = 0;
	uint64_t total = 0;
	uint32_t crc = 0;
	std::vector<uint8_t> data;
};

// One transport connection (host: every remote peer; client: the host).
struct Conn
{
	uint64_t serial = 0;
	net::PeerHandle peer = nullptr;
	PeerInfo info;
	bool hello_seen = false;
	uint64_t created_ms = 0;
	std::deque<OutBlob> out_blobs;
	std::map<uint32_t, InBlob> in_blobs;
	net::ByteCounterPtr bulk = std::make_shared<net::ByteCounter>();
	uint64_t out_due[size_t(Lane::Count)] = {}; // NetSim: keeps per-lane order under jitter
	uint64_t in_due[size_t(Lane::Count)] = {};
};

struct PeerListNotice
{
	PeerId slot = kInvalidPeer;
	std::string reason;
};
} // namespace

// ============================================================================ Impl

struct Session::Impl
{
	explicit Impl(Session& s) : m_self(s) {}

	Session& m_self;
	Role m_role = Role::None;
	SessionConfig m_cfg;
	net::Transport m_tp;
	LogFn m_log;
	NetSim m_sim;
	Rng m_rng;
	uint64_t m_now = 0;

	std::vector<std::unique_ptr<Conn>> m_conns;
	uint64_t m_next_serial = 1;
	std::map<PeerId, Conn*> m_joined; // host: slot -> connection
	Conn* m_server = nullptr; // client: connection to the host

	// host
	uint64_t m_host_fp = 0;
	uint64_t m_session_token = 0;
	uint32_t m_max_players = kMaxPlayers;

	// client
	enum class CState : uint8_t
	{
		Idle,
		Connecting,
		Joined
	};
	CState m_cstate = CState::Idle;
	PeerId m_self_id = kInvalidPeer;
	PeerInfo m_host_info;
	std::map<PeerId, PeerInfo> m_remote; // other players, from PeerList
	std::string m_server_text;
	bool m_deadline_set = false;
	uint64_t m_deadline = 0;
	bool m_transport_up = false;

	std::map<uint16_t, Handler> m_handlers;
	std::set<uint16_t> m_logged_unknown;
	std::set<std::string> m_logged_once;
	std::map<uint16_t, MsgStats> m_stats;
	uint32_t m_next_blob_id = 1;

	struct OutItem
	{
		uint64_t serial;
		Lane lane;
		net::Packet packet;
	};
	struct InItem
	{
		uint64_t serial;
		Lane lane;
		std::vector<uint8_t> bytes;
	};
	std::multimap<uint64_t, OutItem> m_out_q;
	std::multimap<uint64_t, InItem> m_in_q;

	std::deque<std::function<void()>> m_pending;
	bool m_running_pending = false;

	// ------------------------------------------------------------------ logging / callbacks

	void logf(const char* fmt, ...)
	{
		if (!m_log)
			return;
		char buf[1024];
		std::memcpy(buf, "[coop] ", 7);
		va_list ap;
		va_start(ap, fmt);
		std::vsnprintf(buf + 7, sizeof(buf) - 7, fmt, ap);
		va_end(ap);
		buf[sizeof(buf) - 1] = 0;
		try
		{
			m_log(buf);
		}
		catch (...)
		{
		}
	}

	bool first_time(const std::string& key) { return m_logged_once.insert(key).second; }

	void emit(std::function<void()> fn) { m_pending.push_back(std::move(fn)); }

	void run_pending()
	{
		if (m_running_pending)
			return;
		m_running_pending = true;
		while (!m_pending.empty())
		{
			std::function<void()> fn = std::move(m_pending.front());
			m_pending.pop_front();
			try
			{
				fn();
			}
			catch (...)
			{
				logf("exception thrown by a user callback was ignored");
			}
		}
		m_running_pending = false;
	}

	void emit_peer_joined(const PeerInfo& pi)
	{
		emit([this, pi] {
			if (m_self.on_peer_joined)
				m_self.on_peer_joined(pi);
		});
	}

	void emit_peer_left(PeerId id, std::string reason)
	{
		emit([this, id, reason] {
			if (m_self.on_peer_left)
				m_self.on_peer_left(id, reason);
		});
	}

	// ------------------------------------------------------------------ stats

	void count_sent(uint16_t type, size_t bytes)
	{
		MsgStats& s = m_stats[type];
		++s.sent_count;
		s.sent_bytes += bytes;
	}

	void count_recv(uint16_t type, size_t bytes)
	{
		MsgStats& s = m_stats[type];
		++s.recv_count;
		s.recv_bytes += bytes;
	}

	// ------------------------------------------------------------------ connections

	Conn* add_conn(net::PeerHandle peer)
	{
		auto c = std::make_unique<Conn>();
		c->serial = m_next_serial++;
		c->peer = peer;
		c->created_ms = m_now;
		net::Transport::set_user(peer, c.get());
		m_conns.push_back(std::move(c));
		return m_conns.back().get();
	}

	Conn* conn_of(net::PeerHandle peer) { return static_cast<Conn*>(net::Transport::user(peer)); }

	Conn* find_serial(uint64_t serial)
	{
		for (auto& c : m_conns)
			if (c->serial == serial)
				return c.get();
		return nullptr;
	}

	void drop_conn(Conn* c)
	{
		if (c->peer)
			net::Transport::set_user(c->peer, nullptr);
		for (auto it = m_joined.begin(); it != m_joined.end();)
			it = (it->second == c) ? m_joined.erase(it) : std::next(it);
		if (m_server == c)
			m_server = nullptr;
		for (auto it = m_conns.begin(); it != m_conns.end(); ++it)
			if (it->get() == c)
			{
				m_conns.erase(it);
				break;
			}
	}

	// Host: connection for a joined slot. Client: the host connection once joined.
	Conn* joined_conn(PeerId id)
	{
		if (m_role == Role::Host)
		{
			auto it = m_joined.find(id);
			return it == m_joined.end() ? nullptr : it->second;
		}
		if (m_role == Role::Client && m_cstate == CState::Joined && id == kHostPeer)
			return m_server;
		return nullptr;
	}

	// ------------------------------------------------------------------ sending

	uint32_t sim_delay()
	{
		int64_t d = int64_t(m_sim.lag_ms);
		if (m_sim.jitter_ms)
			d += int64_t(m_rng.below(2 * m_sim.jitter_ms + 1)) - int64_t(m_sim.jitter_ms);
		return d < 0 ? 0u : uint32_t(d);
	}

	// Counts the packet, applies NetSim and hands it to the transport.
	void send_packet(Conn& c, Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
		bool bypass_sim)
	{
		if (!c.peer)
			return;
		const size_t total = 2 + na + nb;
		count_sent(type, total);
		const bool sim_on = !bypass_sim && m_sim.enabled();
		if (sim_on && lane == Lane::Unreliable && m_sim.loss > 0.f && m_rng.unit() < double(m_sim.loss))
			return; // dropped by the simulated network

		net::Packet pkt = net::Transport::make_packet(lane, type, a, na, b, nb, lane == Lane::Bulk ? c.bulk : nullptr);
		if (!pkt)
		{
			logf("cannot allocate packet %s (%zu bytes)", msg_name(type), total);
			return;
		}
		if (!bypass_sim && (sim_on || !m_out_q.empty()))
		{
			uint64_t due = m_now + (sim_on ? sim_delay() : 0u);
			uint64_t& last = c.out_due[size_t(lane)];
			due = std::max(due, last);
			last = due;
			m_out_q.emplace(due, OutItem{c.serial, lane, std::move(pkt)});
			return;
		}
		if (!m_tp.send(c.peer, lane, std::move(pkt)) && first_time("sendfail" + std::to_string(c.serial)))
			logf("send %s to connection %llu failed (peer not connected)", msg_name(type),
				static_cast<unsigned long long>(c.serial));
	}

	void send_core(Conn& c, Msg type, const Writer& w, Lane lane = Lane::Reliable, bool bypass_sim = false)
	{
		send_packet(c, lane, uint16_t(type), w.ptr(), w.size(), nullptr, 0, bypass_sim);
	}

	void flush_out_q()
	{
		while (!m_out_q.empty() && m_out_q.begin()->first <= m_now)
		{
			OutItem item = std::move(m_out_q.begin()->second);
			m_out_q.erase(m_out_q.begin());
			Conn* c = find_serial(item.serial);
			if (c && c->peer)
				m_tp.send(c->peer, item.lane, std::move(item.packet));
		}
	}

	// ------------------------------------------------------------------ receive path

	void on_receive(Conn& c, uint8_t channel, std::vector<uint8_t>&& bytes)
	{
		const Lane lane = channel < uint8_t(Lane::Count) ? Lane(channel) : Lane::Reliable;
		const bool sim_on = m_sim.enabled();
		if (sim_on || !m_in_q.empty())
		{
			if (sim_on && lane == Lane::Unreliable && m_sim.loss > 0.f && m_rng.unit() < double(m_sim.loss))
				return;
			uint64_t due = m_now + (sim_on ? sim_delay() : 0u);
			uint64_t& last = c.in_due[size_t(lane)];
			due = std::max(due, last);
			last = due;
			m_in_q.emplace(due, InItem{c.serial, lane, std::move(bytes)});
			return;
		}
		dispatch(c, lane, std::move(bytes));
	}

	void pop_in_q()
	{
		while (m_tp.active() && !m_in_q.empty() && m_in_q.begin()->first <= m_now)
		{
			InItem item = std::move(m_in_q.begin()->second);
			m_in_q.erase(m_in_q.begin());
			Conn* c = find_serial(item.serial);
			if (c)
				dispatch(*c, item.lane, std::move(item.bytes));
			run_pending();
		}
	}

	// `c` must not be used by the caller after dispatch(): handlers may drop it.
	void dispatch(Conn& c, Lane /*lane*/, std::vector<uint8_t>&& bytes)
	{
		if (bytes.size() < 2)
		{
			malformed(c, "packet shorter than the type header");
			return;
		}
		const uint16_t type = uint16_t(bytes[0] | (uint16_t(bytes[1]) << 8));
		count_recv(type, bytes.size());

		if (type >= uint16_t(Msg::GameBase))
		{
			game_message(c, type, std::move(bytes));
			return;
		}

		Reader r(bytes.data() + 2, bytes.size() - 2);
		const bool host = m_role == Role::Host;
		switch (static_cast<Msg>(type))
		{
		case Msg::Hello:
			if (host)
				host_hello(c, r);
			else
				malformed(c, "unexpected Hello");
			break;
		case Msg::Welcome:
			if (!host)
				client_welcome(c, r);
			else
				malformed(c, "unexpected Welcome");
			break;
		case Msg::Reject:
			if (!host)
				client_reject(c, r);
			else
				malformed(c, "unexpected Reject");
			break;
		case Msg::PeerList:
			if (!host)
				client_peer_list(c, r);
			else
				malformed(c, "unexpected PeerList");
			break;
		case Msg::Bye:
			handle_bye(c, r);
			break;
		case Msg::Chat:
			handle_chat(c, r);
			break;
		case Msg::BlobBegin:
			blob_begin(c, r);
			break;
		case Msg::BlobChunk:
			blob_chunk(c, r);
			break;
		case Msg::BlobEnd:
			blob_end(c, r);
			break;
		default:
			if (m_logged_unknown.insert(type).second)
				logf("unknown core message %s dropped", msg_name(type));
			if (host)
				malformed(c, "unknown core message type");
			break;
		}
	}

	bool is_joined(const Conn& c) const
	{
		return m_role == Role::Host ? c.info.state == PeerState::Joined : m_cstate == CState::Joined;
	}

	PeerId peer_id_of(const Conn& c) const { return m_role == Role::Host ? c.info.id : kHostPeer; }

	void game_message(Conn& c, uint16_t type, std::vector<uint8_t>&& bytes)
	{
		if (!is_joined(c))
		{
			if (m_role == Role::Host)
				malformed(c, "game message before handshake");
			return;
		}
		if (m_handlers.find(type) == m_handlers.end())
		{
			if (m_logged_unknown.insert(type).second)
				logf("no handler for message %s (0x%04X), dropping", msg_name(type), unsigned(type));
			return;
		}
		const PeerId from = peer_id_of(c);
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
		emit([this, type, from, data] {
			auto it = m_handlers.find(type);
			if (it == m_handlers.end())
				return;
			Handler h = it->second; // copy: the handler may re-register itself
			Reader r(data->data() + 2, data->size() - 2);
			h(from, r);
		});
	}

	// Malformed message. Host: kick the sender. Client: log and ignore.
	void malformed(Conn& c, const char* what)
	{
		if (m_role == Role::Host)
		{
			std::string text = std::string("Malformed message: ") + what;
			if (c.info.state == PeerState::Joined)
				kick_conn(c, RejectReason::Malformed, text);
			else if (c.info.state == PeerState::Connecting)
				reject_conn(c, RejectReason::Malformed, text, nullptr);
		}
		else
			logf("malformed message from host ignored: %s", what);
	}

	// ------------------------------------------------------------------ transport events

	void handle_event(net::Event& ev)
	{
		switch (ev.type)
		{
		case net::Event::Type::Connect:
			on_connect(ev.peer);
			break;
		case net::Event::Type::Disconnect:
			on_disconnect(ev.peer);
			break;
		case net::Event::Type::Receive:
		{
			Conn* c = conn_of(ev.peer);
			if (c)
				on_receive(*c, ev.channel, std::move(ev.bytes));
			break;
		}
		default:
			break;
		}
	}

	void on_connect(net::PeerHandle peer)
	{
		if (m_role == Role::Host)
		{
			Conn* c = add_conn(peer);
			c->info.state = PeerState::Connecting;
			c->info.address = net::Transport::address_of(peer);
			net::Transport::set_timeouts(peer, m_cfg.peer_timeout_ms);
			logf("host: transport connection %llu from %s", static_cast<unsigned long long>(c->serial),
				c->info.address.c_str());
			return;
		}
		if (m_role == Role::Client && m_server && m_server->peer == peer)
		{
			m_transport_up = true;
			net::Transport::set_timeouts(peer, m_cfg.peer_timeout_ms);
			HelloMsg h;
			h.protocol = kProtocolVersion;
			h.build_id = m_cfg.build_id;
			h.content_fp = manifest_fingerprint(m_cfg.manifest);
			h.key = m_cfg.key;
			h.nick = m_cfg.nick;
			Writer w;
			h.write(w);
			send_core(*m_server, Msg::Hello, w);
			logf("client: connected to %s, hello sent", m_server_text.c_str());
		}
	}

	void on_disconnect(net::PeerHandle peer)
	{
		Conn* c = conn_of(peer);
		if (!c)
			return;
		if (m_role == Role::Host)
		{
			if (c->info.state == PeerState::Joined)
				remove_joined(*c, "connection lost");
			else if (c->info.state == PeerState::Connecting)
				logf("host: connection %llu closed before the handshake finished",
					static_cast<unsigned long long>(c->serial));
			drop_conn(c);
			return;
		}
		if (m_role == Role::Client && c == m_server)
		{
			if (m_cstate == CState::Connecting)
				fail_client(m_transport_up ? "connection to host lost before the handshake finished"
										   : "cannot connect to " + m_server_text + ": no response from host");
			else
				fail_client("connection to host lost");
		}
	}

	// ------------------------------------------------------------------ host: handshake

	void reject_conn(Conn& c, RejectReason reason, const std::string& text, const Manifest* host_manifest)
	{
		RejectMsg rm;
		rm.reason = reason;
		rm.text = text;
		if (host_manifest)
			rm.host_manifest = *host_manifest;
		Writer w;
		rm.write(w);
		send_core(c, Msg::Reject, w);
		logf("reject peer %llu: %s (%s)", static_cast<unsigned long long>(c.serial), reject_reason_name(reason),
			text.c_str());
		c.info.state = PeerState::Leaving;
		net::Transport::disconnect_later(c.peer);
	}

	void host_hello(Conn& c, Reader& r)
	{
		if (c.info.state != PeerState::Connecting || c.hello_seen)
		{
			malformed(c, "Hello outside of the handshake");
			return;
		}
		c.hello_seen = true;

		// Protocol first, from a copy of the reader, so that a future layout change still gets a clean reject.
		{
			Reader peek(r.cursor(), r.remaining());
			const uint32_t proto = peek.u32();
			if (!peek.ok())
			{
				malformed(c, "Hello too short");
				return;
			}
			if (proto != kProtocolVersion)
			{
				char text[256];
				std::snprintf(text, sizeof(text),
					"Protocol version mismatch: the host uses version %u, you use version %u. "
					"Install the same Anomaly Coop version as the host.",
					unsigned(kProtocolVersion), unsigned(proto));
				reject_conn(c, RejectReason::ProtocolMismatch, text, nullptr);
				return;
			}
		}

		HelloMsg h;
		if (!h.read(r))
		{
			malformed(c, "Hello");
			return;
		}

		if (h.build_id != m_cfg.build_id)
		{
			reject_conn(c, RejectReason::BuildMismatch,
				"Game build mismatch: the host runs build '" + m_cfg.build_id + "', you run '" + h.build_id +
					"'. Install the same Anomaly Coop build as the host.",
				nullptr);
			return;
		}
		if (h.content_fp != m_host_fp)
		{
			reject_conn(c, RejectReason::ContentMismatch,
				"Your game files differ from the host's (" + std::to_string(m_cfg.manifest.size()) +
					" files in the host's list). Install exactly the same mods and gamedata as the host.",
				&m_cfg.manifest);
			return;
		}
		bool duplicate = (h.key == m_cfg.key);
		for (const auto& kv : m_joined)
			if (kv.second->info.key == h.key)
				duplicate = true;
		if (duplicate)
		{
			reject_conn(c, RejectReason::DuplicateKey,
				"Your player key is already used in this session. The game folder was probably copied from "
				"another player. To get a new identity, close the game, delete appdata\\coop_player.key in your "
				"game folder and start the game again.",
				nullptr);
			return;
		}
		if (m_joined.size() + 1 >= m_max_players)
		{
			reject_conn(c, RejectReason::SessionFull,
				"The session is full (" + std::to_string(m_max_players) + " players).", nullptr);
			return;
		}

		PeerId slot = 1;
		while (m_joined.count(slot))
			++slot;
		c.info.id = slot;
		c.info.state = PeerState::Joined;
		c.info.key = h.key;
		c.info.nick = sanitize_nick(h.nick, ("Player" + std::to_string(slot)).c_str());
		m_joined[slot] = &c;

		WelcomeMsg w;
		w.slot = slot;
		w.session_token = m_session_token;
		w.host_nick = m_cfg.nick;
		w.max_players = m_max_players;
		Writer ww;
		w.write(ww);
		send_core(c, Msg::Welcome, ww);

		logf("host: peer %u '%s' joined from %s", unsigned(slot), c.info.nick.c_str(), c.info.address.c_str());
		emit_peer_joined(c.info);
		broadcast_peer_list({});
	}

	void broadcast_peer_list(const std::vector<PeerListNotice>& departed)
	{
		Writer w;
		w.varu(m_joined.size());
		for (const auto& kv : m_joined)
		{
			w.u32(kv.first);
			w.str(kv.second->info.nick);
			w.u32(player_key_hash(kv.second->info.key));
		}
		w.varu(departed.size());
		for (const PeerListNotice& d : departed)
		{
			w.u32(d.slot);
			w.str(d.reason);
		}
		for (const auto& kv : m_joined)
			send_core(*kv.second, Msg::PeerList, w);
	}

	// Host: a joined client is gone (Bye, kick or transport loss).
	void remove_joined(Conn& c, const std::string& reason)
	{
		if (c.info.state != PeerState::Joined)
			return;
		const PeerId id = c.info.id;
		c.info.state = PeerState::Leaving;
		m_joined.erase(id);
		logf("host: peer %u '%s' left: %s", unsigned(id), c.info.nick.c_str(), reason.c_str());
		emit_peer_left(id, reason);
		broadcast_peer_list({PeerListNotice{id, reason}});
	}

	void kick_conn(Conn& c, RejectReason reason, const std::string& text)
	{
		if (c.info.state != PeerState::Joined)
			return;
		RejectMsg rm;
		rm.reason = reason;
		rm.text = text;
		Writer w;
		rm.write(w);
		send_core(c, Msg::Reject, w);
		logf("host: kick peer %u: %s (%s)", unsigned(c.info.id), reject_reason_name(reason), text.c_str());
		remove_joined(c, text.empty() ? std::string(reject_reason_name(reason)) : text);
		net::Transport::disconnect_later(c.peer);
	}

	void handle_bye(Conn& c, Reader& r)
	{
		const std::string reason = r.str();
		if (!r.ok())
		{
			if (m_role == Role::Host)
			{
				malformed(c, "Bye");
				return;
			}
		}
		if (m_role == Role::Host)
		{
			// The sender is leaving: forget it now, silently reset the transport peer.
			remove_joined(c, reason);
			net::Transport::reset(c.peer);
			drop_conn(&c);
			return;
		}
		if (&c == m_server)
			fail_client(reason.empty() ? std::string("host closed the session") : reason);
	}

	// ------------------------------------------------------------------ client: handshake

	void client_welcome(Conn& c, Reader& r)
	{
		if (&c != m_server || m_cstate != CState::Connecting)
			return;
		WelcomeMsg w;
		if (!w.read(r) || w.slot == kInvalidPeer || w.slot == kHostPeer)
		{
			fail_client("protocol error: malformed Welcome from host");
			return;
		}
		m_cstate = CState::Joined;
		m_self_id = w.slot;
		m_host_info = PeerInfo();
		m_host_info.id = kHostPeer;
		m_host_info.state = PeerState::Joined;
		m_host_info.nick = w.host_nick;
		m_host_info.address = m_server_text;
		c.info = m_host_info;
		logf("client: welcome from '%s', my slot is %u", w.host_nick.c_str(), unsigned(w.slot));
		emit([this, w] {
			if (m_self.on_welcome)
				m_self.on_welcome(w);
		});
	}

	void client_reject(Conn& c, Reader& r)
	{
		if (&c != m_server)
			return;
		RejectMsg rm;
		if (!rm.read(r))
		{
			fail_client("protocol error: malformed Reject from host");
			return;
		}
		if (m_cstate == CState::Connecting)
		{
			ManifestDiff diff;
			if (rm.reason == RejectReason::ContentMismatch)
				diff = diff_manifests(m_cfg.manifest, rm.host_manifest);
			logf("client: rejected by host: %s (%s)", reject_reason_name(rm.reason), rm.text.c_str());
			hard_stop(true);
			emit([this, rm, diff] {
				if (m_self.on_rejected)
					m_self.on_rejected(rm, diff);
			});
			return;
		}
		// Kicked after having joined.
		std::string reason = reject_reason_name(rm.reason);
		if (!rm.text.empty())
			reason += ": " + rm.text;
		fail_client(reason);
	}

	void client_peer_list(Conn& c, Reader& r)
	{
		if (&c != m_server || m_cstate != CState::Joined)
			return;
		std::map<PeerId, PeerListEntry> entries;
		const uint64_t count = r.varu();
		if (!r.ok() || count > r.remaining() / 9)
		{
			malformed(c, "PeerList");
			return;
		}
		for (uint64_t i = 0; i < count; ++i)
		{
			PeerListEntry e;
			e.slot = r.u32();
			e.nick = r.str();
			e.key_hash = r.u32();
			if (!r.ok())
				break;
			entries[e.slot] = e;
		}
		std::map<PeerId, std::string> departed;
		const uint64_t dcount = r.varu();
		if (!r.ok() || dcount > r.remaining() / 5)
		{
			malformed(c, "PeerList");
			return;
		}
		for (uint64_t i = 0; i < dcount; ++i)
		{
			const PeerId slot = r.u32();
			std::string reason = r.str();
			if (!r.ok())
				break;
			departed[slot] = std::move(reason);
		}
		if (!r.ok())
		{
			malformed(c, "PeerList");
			return;
		}

		for (auto it = m_remote.begin(); it != m_remote.end();)
		{
			const PeerId id = it->first;
			if (entries.count(id) && !departed.count(id))
			{
				++it;
				continue;
			}
			const auto d = departed.find(id);
			emit_peer_left(id, d != departed.end() && !d->second.empty() ? d->second : std::string("left the session"));
			it = m_remote.erase(it);
		}
		for (const auto& kv : entries)
		{
			const PeerId id = kv.first;
			if (id == m_self_id || id == kHostPeer)
				continue;
			auto it = m_remote.find(id);
			if (it != m_remote.end())
			{
				it->second.nick = kv.second.nick;
				continue;
			}
			PeerInfo pi;
			pi.id = id;
			pi.state = PeerState::Joined;
			pi.nick = kv.second.nick;
			m_remote[id] = pi;
			emit_peer_joined(pi);
		}
	}

	// Client: tear everything down and report on_disconnected.
	void fail_client(std::string reason)
	{
		logf("client: disconnected: %s", reason.c_str());
		hard_stop(true);
		emit([this, reason] {
			if (m_self.on_disconnected)
				m_self.on_disconnected(reason);
		});
	}

	// ------------------------------------------------------------------ chat

	void handle_chat(Conn& c, Reader& r)
	{
		if (m_role == Role::Host)
		{
			std::string text = r.str();
			if (!r.ok() || c.info.state != PeerState::Joined)
			{
				malformed(c, "Chat");
				return;
			}
			utf8_truncate(text, kMaxChatBytes);
			const PeerId from = c.info.id;
			Writer w;
			w.u32(from);
			w.str(text);
			for (const auto& kv : m_joined)
				if (kv.first != from)
					send_core(*kv.second, Msg::Chat, w);
			emit([this, from, text] {
				if (m_self.on_chat)
					m_self.on_chat(from, text);
			});
			return;
		}
		if (&c != m_server || m_cstate != CState::Joined)
			return;
		const PeerId from = r.u32();
		std::string text = r.str();
		if (!r.ok())
		{
			malformed(c, "Chat");
			return;
		}
		emit([this, from, text] {
			if (m_self.on_chat)
				m_self.on_chat(from, text);
		});
	}

	// ------------------------------------------------------------------ blobs

	void blob_begin(Conn& c, Reader& r)
	{
		if (!is_joined(c))
		{
			malformed(c, "BlobBegin before handshake");
			return;
		}
		const uint32_t id = r.u32();
		const uint32_t kind = r.u32();
		const uint64_t total = r.u64();
		const uint32_t crc = r.u32();
		if (!r.ok() || total > kMaxBlobBytes || c.in_blobs.size() >= kMaxIncomingBlobsPerPeer ||
			c.in_blobs.count(id))
		{
			malformed(c, "BlobBegin");
			return;
		}
		InBlob b;
		b.kind = kind;
		b.total = total;
		b.crc = crc;
		if (m_role == Role::Client) // the host is trusted, a client announcing a size is not
			b.data.reserve(size_t(total));
		c.in_blobs.emplace(id, std::move(b));
	}

	void blob_chunk(Conn& c, Reader& r)
	{
		const uint32_t id = r.u32();
		auto it = r.ok() ? c.in_blobs.find(id) : c.in_blobs.end();
		if (it == c.in_blobs.end())
		{
			malformed(c, "BlobChunk for an unknown blob");
			return;
		}
		InBlob& b = it->second;
		const size_t n = r.remaining();
		if (b.data.size() + n > b.total)
		{
			c.in_blobs.erase(it);
			malformed(c, "BlobChunk beyond announced size");
			return;
		}
		b.data.insert(b.data.end(), r.cursor(), r.cursor() + n);
		const PeerId from = peer_id_of(c);
		const uint32_t kind = b.kind;
		const uint64_t done = b.data.size();
		const uint64_t total = b.total;
		emit([this, from, id, kind, done, total] {
			if (m_self.on_blob_progress)
				m_self.on_blob_progress(from, id, kind, done, total);
		});
	}

	void blob_end(Conn& c, Reader& r)
	{
		const uint32_t id = r.u32();
		auto it = r.ok() ? c.in_blobs.find(id) : c.in_blobs.end();
		if (it == c.in_blobs.end())
		{
			malformed(c, "BlobEnd for an unknown blob");
			return;
		}
		InBlob b = std::move(it->second);
		c.in_blobs.erase(it);
		const PeerId from = peer_id_of(c);
		if (b.data.size() != b.total)
		{
			logf("blob %u from peer %u dropped: got %zu of %llu bytes", unsigned(id), unsigned(from), b.data.size(),
				static_cast<unsigned long long>(b.total));
			return;
		}
		const uint32_t got = crc32(b.data.data(), b.data.size());
		if (got != b.crc)
		{
			logf("blob %u from peer %u dropped: CRC mismatch (expected %08X, got %08X)", unsigned(id), unsigned(from),
				unsigned(b.crc), unsigned(got));
			return;
		}
		auto data = std::make_shared<std::vector<uint8_t>>(std::move(b.data));
		const uint32_t kind = b.kind;
		emit([this, from, id, kind, data] {
			if (m_self.on_blob_complete)
				m_self.on_blob_complete(from, id, kind, *data);
		});
	}

	// Tops up the Bulk lane of every peer while less than kBulkQueueBytes are alive for it.
	void topup_blobs()
	{
		for (auto& cp : m_conns)
		{
			Conn& c = *cp;
			if (c.out_blobs.empty() || !is_joined(c))
				continue;
			while (!c.out_blobs.empty() && c.bulk->bytes < kBulkQueueBytes)
			{
				OutBlob& b = c.out_blobs.front();
				if (!b.begun)
				{
					Writer w;
					w.u32(b.id);
					w.u32(b.kind);
					w.u64(b.data.size());
					w.u32(b.crc);
					send_core(c, Msg::BlobBegin, w, Lane::Bulk);
					b.begun = true;
				}
				if (b.offset < b.data.size())
				{
					const size_t n = std::min(kBlobChunkSize, b.data.size() - b.offset);
					uint8_t hdr[4] = {uint8_t(b.id), uint8_t(b.id >> 8), uint8_t(b.id >> 16), uint8_t(b.id >> 24)};
					send_packet(c, Lane::Bulk, uint16_t(Msg::BlobChunk), hdr, sizeof(hdr), b.data.data() + b.offset, n,
						false);
					b.offset += n;
				}
				else
				{
					Writer w;
					w.u32(b.id);
					send_core(c, Msg::BlobEnd, w, Lane::Bulk);
					c.out_blobs.pop_front();
				}
			}
		}
	}

	// ------------------------------------------------------------------ periodic work

	void timers()
	{
		if (m_role == Role::Host)
		{
			std::vector<Conn*> expired;
			for (auto& cp : m_conns)
			{
				Conn& c = *cp;
				if (c.info.state == PeerState::Joined)
				{
					c.info.rtt_ms = net::Transport::rtt_ms(c.peer);
					c.info.packet_loss = net::Transport::packet_loss(c.peer);
				}
				else if (c.info.state == PeerState::Connecting && m_now >= c.created_ms &&
					m_now - c.created_ms >= m_cfg.connect_timeout_ms)
					expired.push_back(&c);
			}
			for (Conn* c : expired)
			{
				logf("host: connection %llu dropped: no Hello within %u ms", static_cast<unsigned long long>(c->serial),
					unsigned(m_cfg.connect_timeout_ms));
				net::Transport::disconnect_now(c->peer);
				drop_conn(c);
			}
			return;
		}
		if (m_role == Role::Client)
		{
			if (m_server && m_cstate == CState::Joined)
			{
				m_host_info.rtt_ms = net::Transport::rtt_ms(m_server->peer);
				m_host_info.packet_loss = net::Transport::packet_loss(m_server->peer);
			}
			if (m_cstate == CState::Connecting && m_deadline_set && m_now >= m_deadline)
				fail_client(std::string("timeout: ") +
					(m_transport_up ? "no Welcome from host " : "no response from host ") + m_server_text +
					" within " + std::to_string(m_cfg.connect_timeout_ms) + " ms");
		}
	}

	void pump()
	{
		net::Event ev;
		for (int i = 0; i < kMaxEventsPerUpdate && m_tp.active(); ++i)
		{
			const int rc = m_tp.service(ev);
			if (rc < 0)
			{
				if (first_time("service-error"))
					logf("transport error reported by ENet");
				break;
			}
			if (rc == 0)
				break;
			handle_event(ev);
			run_pending();
		}
	}

	void update(uint64_t now_ms)
	{
		m_now = now_ms;
		if (!m_tp.active())
		{
			run_pending();
			return;
		}
		if (m_role == Role::Client && !m_deadline_set)
		{
			m_deadline_set = true;
			m_deadline = m_now + m_cfg.connect_timeout_ms;
		}
		flush_out_q();
		pump();
		pop_in_q();
		if (!m_tp.active())
		{
			run_pending();
			return;
		}
		timers();
		run_pending();
		if (!m_tp.active())
			return;
		topup_blobs();
		m_tp.flush();
		run_pending();
	}

	// ------------------------------------------------------------------ teardown

	// Drops all connections and the transport without any waiting. `notify` sends a transport-level
	// disconnect to every peer (not used after a graceful Bye: it could overtake the Bye in the receiver).
	void hard_stop(bool notify)
	{
		if (notify)
			for (auto& c : m_conns)
				if (c->peer)
					net::Transport::disconnect_now(c->peer);
		m_out_q.clear();
		m_in_q.clear();
		for (auto& c : m_conns)
			if (c->peer)
				net::Transport::set_user(c->peer, nullptr);
		m_conns.clear();
		m_joined.clear();
		m_remote.clear();
		m_server = nullptr;
		m_tp.destroy();
		m_role = Role::None;
		m_cstate = CState::Idle;
		m_self_id = kInvalidPeer;
		m_host_info = PeerInfo();
		m_deadline_set = false;
		m_transport_up = false;
	}

	void graceful_close(const std::string& reason)
	{
		Writer w;
		w.str(reason);
		std::vector<net::PeerHandle> waiting;
		for (auto& cp : m_conns)
		{
			Conn& c = *cp;
			if (is_joined(c))
			{
				send_packet(c, Lane::Reliable, uint16_t(Msg::Bye), w.ptr(), w.size(), nullptr, 0, true);
				waiting.push_back(c.peer);
			}
			else if (c.peer)
				net::Transport::disconnect_now(c.peer);
		}
		if (waiting.empty())
			return;
		m_tp.flush();

		net::Event ev;
		auto all = [&waiting](bool (*pred)(net::PeerHandle)) {
			for (net::PeerHandle p : waiting)
				if (!pred(p))
					return false;
			return true;
		};
		// Phase 1: everything transmitted at least once.
		const uint64_t start = steady_ms();
		while (!all(&net::Transport::outgoing_idle) && steady_ms() - start < kShutdownMaxMs)
			m_tp.service(ev, 1);
		// Phase 2: give the Bye a chance to be acknowledged (and retransmitted once) on a real network.
		uint32_t max_rtt = 0;
		for (net::PeerHandle p : waiting)
			max_rtt = std::max(max_rtt, net::Transport::rtt_ms(p));
		const uint64_t ack_deadline = std::min<uint64_t>(start + kShutdownMaxMs, steady_ms() + 4ull * max_rtt + 20);
		while (!all(&net::Transport::all_acked) && steady_ms() < ack_deadline)
			m_tp.service(ev, 1);
	}
};

// ============================================================================ Session

Session::Session() : m(std::make_unique<Impl>(*this)) {}

Session::~Session()
{
	shutdown("session destroyed");
}

bool Session::host(const SessionConfig& cfg, std::string* err)
{
	Impl& s = *m;
	try
	{
		if (s.m_role != Role::None)
		{
			if (err)
				*err = "session is already active";
			return false;
		}
		s.m_cfg = cfg;
		s.m_cfg.nick = sanitize_nick(cfg.nick, "Host");
		if (!s.m_cfg.connect_timeout_ms)
			s.m_cfg.connect_timeout_ms = kDefaultConnectTimeoutMs;
		if (!s.m_cfg.peer_timeout_ms)
			s.m_cfg.peer_timeout_ms = kDefaultPeerTimeoutMs;
		s.m_max_players = std::max<uint32_t>(1, std::min<uint32_t>(cfg.max_players, kMaxPlayers));
		s.m_rng.seed(0x5EEDC00Full ^ (uint64_t(player_key_hash(cfg.key)) << 20));

		std::string e;
		if (!s.m_tp.listen(cfg.port, (s.m_max_players - 1) + kPendingPeerSlots, &e))
		{
			s.logf("host: cannot listen on port %u: %s", unsigned(cfg.port), e.c_str());
			if (err)
				*err = e;
			return false;
		}
		s.m_role = Role::Host;
		s.m_host_fp = manifest_fingerprint(s.m_cfg.manifest);
		s.m_session_token = random_u64();
		s.logf("host: listening on 0.0.0.0:%u", unsigned(cfg.port));
		return true;
	}
	catch (...)
	{
		s.m_tp.destroy();
		s.m_role = Role::None;
		if (err)
			*err = "internal error while starting the host";
		return false;
	}
}

bool Session::connect(const SessionConfig& cfg, const std::string& address, uint16_t port, std::string* err)
{
	Impl& s = *m;
	try
	{
		if (s.m_role != Role::None)
		{
			if (err)
				*err = "session is already active";
			return false;
		}
		s.m_cfg = cfg;
		s.m_cfg.nick = sanitize_nick(cfg.nick, ""); // empty is fine: the host names the player after the slot
		if (!s.m_cfg.connect_timeout_ms)
			s.m_cfg.connect_timeout_ms = kDefaultConnectTimeoutMs;
		if (!s.m_cfg.peer_timeout_ms)
			s.m_cfg.peer_timeout_ms = kDefaultPeerTimeoutMs;
		s.m_rng.seed(0xC11E17ull ^ (uint64_t(player_key_hash(cfg.key)) << 20));

		std::string e;
		net::PeerHandle peer = s.m_tp.connect(address, port, &e);
		if (!peer)
		{
			s.logf("client: cannot connect to %s:%u: %s", address.c_str(), unsigned(port), e.c_str());
			if (err)
				*err = e;
			return false;
		}
		s.m_role = Role::Client;
		s.m_cstate = Impl::CState::Connecting;
		s.m_deadline_set = false;
		s.m_transport_up = false;
		s.m_server_text = address + ":" + std::to_string(unsigned(port));
		Conn* c = s.add_conn(peer);
		c->info.id = kHostPeer;
		c->info.state = PeerState::Connecting;
		c->info.address = s.m_server_text;
		s.m_server = c;
		s.logf("client: connecting to %s", s.m_server_text.c_str());
		return true;
	}
	catch (...)
	{
		s.hard_stop(false);
		if (err)
			*err = "internal error while connecting";
		return false;
	}
}

void Session::shutdown(const char* reason)
{
	Impl& s = *m;
	try
	{
		if (s.m_role == Role::None)
		{
			s.m_pending.clear();
			return;
		}
		const std::string why = reason ? reason : "";
		s.logf("shutdown: %s", why.c_str());
		s.graceful_close(why);
	}
	catch (...)
	{
	}
	s.hard_stop(false);
	s.m_pending.clear();
}

void Session::update(uint64_t now_ms)
{
	try
	{
		m->update(now_ms);
	}
	catch (...)
	{
		m->logf("internal exception in update() was suppressed");
	}
}

Role Session::role() const
{
	return m->m_role;
}

PeerId Session::self() const
{
	switch (m->m_role)
	{
	case Role::Host: return kHostPeer;
	case Role::Client: return m->m_cstate == Impl::CState::Joined ? m->m_self_id : kInvalidPeer;
	default: return kInvalidPeer;
	}
}

bool Session::is_active() const
{
	switch (m->m_role)
	{
	case Role::Host: return m->m_tp.active();
	case Role::Client: return m->m_cstate == Impl::CState::Joined;
	default: return false;
	}
}

std::vector<PeerInfo> Session::peers() const
{
	std::vector<PeerInfo> out;
	if (m->m_role == Role::Host)
	{
		for (const auto& kv : m->m_joined)
			out.push_back(kv.second->info);
	}
	else if (m->m_role == Role::Client && m->m_cstate == Impl::CState::Joined)
	{
		out.push_back(m->m_host_info);
		for (const auto& kv : m->m_remote)
			out.push_back(kv.second);
	}
	return out;
}

const PeerInfo* Session::peer(PeerId id) const
{
	if (m->m_role == Role::Host)
	{
		auto it = m->m_joined.find(id);
		return it == m->m_joined.end() ? nullptr : &it->second->info;
	}
	if (m->m_role == Role::Client && m->m_cstate == Impl::CState::Joined)
	{
		if (id == kHostPeer)
			return &m->m_host_info;
		auto it = m->m_remote.find(id);
		return it == m->m_remote.end() ? nullptr : &it->second;
	}
	return nullptr;
}

void Session::on(Msg type, Handler h)
{
	const uint16_t t = static_cast<uint16_t>(type);
	if (t < static_cast<uint16_t>(Msg::GameBase))
	{
		m->logf("on(%s): core messages are handled by the session, handler ignored", msg_name(type));
		return;
	}
	if (h)
		m->m_handlers[t] = std::move(h);
	else
		m->m_handlers.erase(t);
}

void Session::send(PeerId to, Lane lane, Msg type, const Writer& payload)
{
	Impl& s = *m;
	try
	{
		if (s.m_role == Role::None)
			return;
		if (s.m_role == Role::Client && to != kHostPeer)
		{
			if (s.first_time("client-send-peer"))
				s.logf("send(%s): clients can only send to the host (peer 0), ignored", msg_name(type));
			return;
		}
		Conn* c = s.joined_conn(to);
		if (!c)
		{
			if (s.first_time("send-nopeer" + std::to_string(to)))
				s.logf("send(%s): peer %u is not joined, ignored", msg_name(type), unsigned(to));
			return;
		}
		if (payload.size() > kMaxPacketBytes)
		{
			s.logf("send(%s): payload of %zu bytes is too large, dropped", msg_name(type), payload.size());
			return;
		}
		s.send_packet(*c, lane, uint16_t(type), payload.ptr(), payload.size(), nullptr, 0, false);
	}
	catch (...)
	{
		s.logf("internal exception in send() was suppressed");
	}
}

void Session::broadcast(Lane lane, Msg type, const Writer& payload, PeerId except)
{
	Impl& s = *m;
	try
	{
		if (s.m_role == Role::Client)
		{
			if (except != kHostPeer)
				send(kHostPeer, lane, type, payload);
			return;
		}
		if (s.m_role != Role::Host)
			return;
		if (payload.size() > kMaxPacketBytes)
		{
			s.logf("broadcast(%s): payload of %zu bytes is too large, dropped", msg_name(type), payload.size());
			return;
		}
		for (const auto& kv : s.m_joined)
			if (kv.first != except)
				s.send_packet(*kv.second, lane, uint16_t(type), payload.ptr(), payload.size(), nullptr, 0, false);
	}
	catch (...)
	{
		s.logf("internal exception in broadcast() was suppressed");
	}
}

void Session::kick(PeerId id, RejectReason reason, const std::string& text)
{
	Impl& s = *m;
	try
	{
		if (s.m_role != Role::Host)
			return;
		Conn* c = s.joined_conn(id);
		if (!c)
		{
			s.logf("kick: peer %u is not joined", unsigned(id));
			return;
		}
		s.kick_conn(*c, reason, text);
	}
	catch (...)
	{
		s.logf("internal exception in kick() was suppressed");
	}
}

uint32_t Session::send_blob(PeerId to, uint32_t kind, std::vector<uint8_t> data)
{
	Impl& s = *m;
	try
	{
		Conn* c = nullptr;
		if (s.m_role == Role::Host)
			c = s.joined_conn(to);
		else if (s.m_role == Role::Client && to == kHostPeer)
			c = s.joined_conn(kHostPeer);
		if (!c)
		{
			s.logf("send_blob: peer %u is not joined, blob dropped", unsigned(to));
			return 0;
		}
		if (data.size() > kMaxBlobBytes)
		{
			s.logf("send_blob: %zu bytes exceed the %llu byte limit, blob dropped", data.size(),
				static_cast<unsigned long long>(kMaxBlobBytes));
			return 0;
		}
		OutBlob b;
		b.id = s.m_next_blob_id++;
		if (!s.m_next_blob_id)
			s.m_next_blob_id = 1;
		b.kind = kind;
		b.crc = crc32(data.data(), data.size());
		b.data = std::move(data);
		const uint32_t id = b.id;
		s.logf("blob %u (kind %u, %zu bytes) queued for peer %u", unsigned(id), unsigned(kind), b.data.size(),
			unsigned(to));
		c->out_blobs.push_back(std::move(b));
		return id;
	}
	catch (...)
	{
		s.logf("internal exception in send_blob() was suppressed");
		return 0;
	}
}

void Session::send_chat(const std::string& text)
{
	Impl& s = *m;
	try
	{
		std::string t = text;
		utf8_truncate(t, kMaxChatBytes);
		if (s.m_role == Role::Host)
		{
			Writer w;
			w.u32(kHostPeer);
			w.str(t);
			for (const auto& kv : s.m_joined)
				s.send_core(*kv.second, Msg::Chat, w);
		}
		else if (s.m_role == Role::Client && s.m_cstate == Impl::CState::Joined && s.m_server)
		{
			Writer w;
			w.str(t);
			s.send_core(*s.m_server, Msg::Chat, w);
		}
	}
	catch (...)
	{
		s.logf("internal exception in send_chat() was suppressed");
	}
}

void Session::set_log(LogFn fn)
{
	m->m_log = std::move(fn);
}

void Session::set_netsim(const NetSim& sim)
{
	m->m_sim = sim;
}

const NetSim& Session::netsim() const
{
	return m->m_sim;
}

const MsgStats& Session::stats(uint16_t type) const
{
	static const MsgStats zero;
	const auto it = m->m_stats.find(type);
	return it == m->m_stats.end() ? zero : it->second;
}

std::vector<uint16_t> Session::stats_types() const
{
	std::vector<uint16_t> out;
	for (const auto& kv : m->m_stats)
	{
		const MsgStats& s = kv.second;
		if (s.sent_count || s.recv_count)
			out.push_back(kv.first);
	}
	return out;
}

void Session::reset_stats()
{
	m->m_stats.clear();
}

// ============================================================================ free functions

PlayerKey generate_player_key()
{
	PlayerKey key{};
	if (!random_bytes(key.data(), key.size()))
	{
		// BCrypt failing is practically impossible; fall back to mixing time and addresses so the key is still unique.
		uint64_t a = random_u64() ^ steady_ms();
		uint64_t b = steady_ms() * 0xD6E8FEB86659FD93ull ^ reinterpret_cast<uintptr_t>(&key);
		std::memcpy(key.data(), &a, 8);
		std::memcpy(key.data() + 8, &b, 8);
	}
	return key;
}

uint32_t player_key_hash(const PlayerKey& key)
{
	uint32_t h = 2166136261u;
	for (uint8_t b : key)
	{
		h ^= b;
		h *= 16777619u;
	}
	return h;
}

uint32_t crc32(const void* data, size_t size, uint32_t seed)
{
	const uint32_t* table = crc_table();
	const uint8_t* p = static_cast<const uint8_t*>(data);
	uint32_t c = ~seed;
	for (size_t i = 0; i < size; ++i)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}
} // namespace coop
