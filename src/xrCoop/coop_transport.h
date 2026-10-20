// Anomaly Coop — private ENet wrapper used by Session (not part of the public API).
// Keeps <enet/enet.h> and the Windows headers out of coop_session.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "coop_protocol.h"

struct _ENetHost;
struct _ENetPeer;
struct _ENetPacket;

namespace coop
{
namespace net
{
using PeerHandle = _ENetPeer*;

// Bytes of Bulk-lane packets that are alive (queued in Session or in ENet, not yet acknowledged).
// A packet holds a shared reference, so the counter outlives the connection that owns it.
struct ByteCounter
{
	size_t bytes = 0;
};
using ByteCounterPtr = std::shared_ptr<ByteCounter>;

// Owning, move-only handle of a not yet submitted ENet packet.
class Packet
{
public:
	Packet() noexcept = default;
	Packet(Packet&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
	Packet& operator=(Packet&& o) noexcept;
	Packet(const Packet&) = delete;
	Packet& operator=(const Packet&) = delete;
	~Packet();

	explicit operator bool() const { return m_p != nullptr; }
	size_t size() const;

private:
	friend class Transport;
	_ENetPacket* m_p = nullptr;
};

struct Event
{
	enum class Type : uint8_t
	{
		None,
		Connect,
		Disconnect,
		Receive
	};
	Type type = Type::None;
	PeerHandle peer = nullptr;
	uint8_t channel = 0;
	uint32_t data = 0;
	std::vector<uint8_t> bytes; // Receive: full packet (u16 type + payload)
};

class Transport
{
public:
	Transport() = default;
	~Transport() { destroy(); }
	Transport(const Transport&) = delete;
	Transport& operator=(const Transport&) = delete;

	// Server socket on 0.0.0.0:port with room for max_peers simultaneous transport connections.
	bool listen(uint16_t port, uint32_t max_peers, std::string* err);
	// Client socket; starts connecting. Returns the (only) peer or nullptr.
	PeerHandle connect(const std::string& address, uint16_t port, std::string* err);
	void destroy();
	bool active() const { return m_host != nullptr; }

	// Non-blocking (wait_ms == 0) event poll: 1 = event filled, 0 = nothing, -1 = transport error.
	int service(Event& ev, uint32_t wait_ms = 0);
	void flush();

	// Builds [u16 type][a][b] with the right ENet flags for the lane. `counter` (optional) tracks live bytes.
	static Packet make_packet(Lane lane, uint16_t type, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
		const ByteCounterPtr& counter);
	// Submits the packet. Ownership passes to ENet on success; on failure the packet is destroyed. false = not sent.
	bool send(PeerHandle peer, Lane lane, Packet&& packet);

	// Silence timeout for the peer; also switches ENet's unreliable-dropping congestion throttle off.
	static void set_timeouts(PeerHandle peer, uint32_t timeout_ms);
	static void disconnect_later(PeerHandle peer);
	static void disconnect_now(PeerHandle peer);
	static void reset(PeerHandle peer);

	static void* user(PeerHandle peer);
	static void set_user(PeerHandle peer, void* p);
	static bool outgoing_idle(PeerHandle peer); // every queued command was transmitted at least once
	static bool all_acked(PeerHandle peer); // idle and no reliable command awaiting acknowledgement
	static uint32_t rtt_ms(PeerHandle peer);
	static float packet_loss(PeerHandle peer); // 0..1
	static std::string address_of(PeerHandle peer); // "ip:port"

private:
	_ENetHost* m_host = nullptr;
	bool m_enet_ref = false;
};
} // namespace net
} // namespace coop
