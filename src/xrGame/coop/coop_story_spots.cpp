// coop::story, PDA map spots: the shared spots, who placed them, and a client's requests to add or remove one.
#include "stdafx.h"
#include "coop_story_impl.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"

#include "Level.h"
#include "map_manager.h"
#include "map_location.h"
#include "map_spot.h"
#include "ui/xrUIXmlParser.h"

extern CUIXml* g_uiSpotXml; // map_location.cpp: map_spots.xml, loaded on the first map spot

namespace coop
{
namespace story
{
// ------------------------------------------------------------------ map spots

xr_map<SpotKey, SpotAuthor> g_spot_authors; // host: spots placed at a client's request
xr_map<SpotKey, u64> g_spot_pending; // client: spots sent to the host, not confirmed yet

// Purely local engine spots: own position markers, hit sectors, relation markers, debug, and every
// type with the prefix "coop_" (markers the co-op scripts place per machine, e.g. player positions).
bool spot_type_local(LPCSTR type)
{
	static const char* const kLocal[] = {"actor_location", "actor_location_p", "mp_hit_sector_location", "debug_stalker",
		"deadbody_location", "friend_location", "neutral_location", "enemy_location"};
	if (!type || !*type || !strncmp(type, "coop_", 5))
		return true;
	for (const char* l : kLocal)
		if (!xr_strcmp(type, l))
			return true;
	return false;
}

// CMapLocation R_ASSERTs on a spot type that map_spots.xml does not have (another mod set, a bad request):
// check before AddMapLocation for anything that came from the network.
bool spot_type_known(const shared_str& type)
{
	if (!type.size())
		return false;
	if (!g_uiSpotXml)
	{
		g_uiSpotXml = xr_new<CUIXml>(); // the same lazy load as CMapLocation::LoadSpot
		g_uiSpotXml->Load(CONFIG_PATH, UI_PATH, "map_spots.xml");
	}
	if (g_uiSpotXml->NavigateToNode(type.c_str(), 0))
		return true;
	static xr_set<shared_str> s_logged; // tasks and resyncs ask again every few seconds; once per type
	if (s_logged.insert(type).second)
		::Msg("! [coop] story: map spot type '%s' is unknown here (different mods?), its spots and task spots are skipped",
		    type.c_str());
	return false;
}

// Spots that belong to the shared PDA: not task-linked (tasks carry their own), not relation markers.
bool spot_replicable(CMapLocation* ml, const shared_str& type)
{
	if (!ml || spot_type_local(cs(type)) || ml->m_owner_task_id.size())
		return false;
	return smart_cast<CRelationMapLocation*>(ml) == nullptr;
}

namespace
{
// Host: a client may remove only a shared spot that the host placed at its own request (g_spot_authors):
// never a task spot, nor a spot of the host's or another player's scripts (both ran the same script: the host's).
bool spot_placed_by(PeerId peer, CMapLocation* ml, const shared_str& type, u16 id)
{
	if (!spot_replicable(ml, type))
		return false;
	auto it = g_spot_authors.find(SpotKey{type, id});
	return it != g_spot_authors.end() && it->second.author == peer;
}

// Does object `id` have a spot of the shared PDA? Its other spots are this machine's own.
bool has_replicable_spot(u16 id)
{
	for (const SLocationKey& k : spots().Locations())
		if (k.object_id == id && spot_replicable(k.location, k.spot_type))
			return true;
	return false;
}

xr_string tagged_hint(const shared_str& hint, PeerId author)
{
	xr_string h = cs(hint);
	if (author == kHostPeer || author == mgr().self() || author == kInvalidPeer)
		return h;
	const PeerInfo* p = mgr().session().peer(author);
	if (!p)
		return h;
	if (!h.empty())
		h += " ";
	h += "(";
	h += p->nick.c_str();
	h += ")";
	return h;
}
} // namespace

void write_spot(Writer& w, const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author)
{
	w.str(cs(type));
	w.u16(host_id);
	w.str(hint.c_str());
	w.boolean(ser);
	w.u32(author);
}

// Host: spot body for broadcast / full list.
void host_spot_body(Writer& w, CMapLocation* ml, const shared_str& type, u16 id)
{
	auto it = g_spot_authors.find(SpotKey{type, id});
	xr_string hint;
	PeerId author = kHostPeer;
	if (it != g_spot_authors.end())
	{
		hint = cs(it->second.hint);
		author = it->second.author;
	}
	else if (LPCSTR h = ml->GetHint())
		hint = h;
	write_spot(w, type, id, hint, ml->Serializable(), author);
}

void apply_spot_add(const shared_str& type, u16 host_id, const xr_string& hint, bool ser, PeerId author)
{
	const u16 id = local_id_of(host_id);
	if (!type.size() || id == kNoId || spot_type_local(cs(type)))
		return; // a local type (e.g. "coop_*") is never taken from the network
	g_spot_pending.erase(SpotKey{type, host_id});
	Applying guard(Kind::Spot, type, id);
	CMapLocation* ml = spots().GetMapLocation(type, id);
	if (!ml)
	{
		if (!spot_type_known(type))
			return;
		ml = spots().AddMapLocation(type, id);
	}
	const xr_string text = tagged_hint(shared_str(hint.c_str()), author);
	if (!text.empty())
		ml->SetHint(shared_str(text.c_str()));
	ml->SetSerializable(ser);
}

void remove_replicable_spots_of(u16 id)
{
	xr_vector<CMapLocation*> list;
	spots().GetMapLocations(id, list);
	for (CMapLocation* ml : list)
	{
		const shared_str type = ml->spot_type;
		if (spot_replicable(ml, type))
		{
			Applying guard(Kind::Spot, type, id);
			spots().RemoveMapLocation(ml);
		}
	}
}

DomainHash hash_spots()
{
	DomainHash d;
	if (!in_game())
		return d;
	const u64 now = mgr().now_ms();
	xr_set<SpotKey> keys;
	for (const SLocationKey& k : spots().Locations())
	{
		if (!k.location || !k.location->Serializable() || !spot_replicable(k.location, k.spot_type))
			continue;
		const SpotKey key{k.spot_type, host_id_of(k.object_id)};
		auto p = g_spot_pending.find(key);
		if (p != g_spot_pending.end() && now - p->second < kSpotPendingMs)
			continue; // ours, not confirmed yet
		keys.insert(key);
	}
	for (const SpotKey& k : keys)
	{
		Fnv f;
		f.str(k.type);
		f.pod(k.id);
		d.add(f.h);
	}
	return d;
}

void send_spot_list(PeerId to)
{
	if (!in_game())
		return;
	Writer w(4096);
	w.u8(SpotFull);
	u32 n = 0;
	Writer body(4096);
	for (const SLocationKey& k : spots().Locations())
	{
		if (!spot_replicable(k.location, k.spot_type))
			continue;
		host_spot_body(body, k.location, k.spot_type, k.object_id);
		++n;
	}
	w.u32(n);
	if (body.size())
		w.bytes(body.ptr(), body.size());
	net::host_send(to, Msg::MapSpot, w);
	++g_stats.spots_out;
}

void client_apply_spot_list(Reader& r)
{
	struct S
	{
		shared_str type;
		u16 id;
		xr_string hint;
		bool ser;
		PeerId author;
	};
	const u32 n = r.u32();
	xr_vector<S> list;
	for (u32 i = 0; i < n && r.ok(); ++i)
	{
		S s;
		s.type = to_shared(r.str());
		s.id = r.u16();
		s.hint = r.str().c_str();
		s.ser = r.boolean();
		s.author = r.u32();
		list.push_back(s);
	}
	if (!r.ok() || !in_game())
		return;
	xr_set<SpotKey> host;
	for (const S& s : list)
	{
		host.insert(SpotKey{s.type, s.id});
		apply_spot_add(s.type, s.id, s.hint, s.ser, s.author);
	}
	// Story spots the host does not have go away; transient local ones are left alone.
	const u64 now = mgr().now_ms();
	xr_vector<CMapLocation*> gone;
	for (const SLocationKey& k : spots().Locations())
	{
		if (!k.location || !k.location->Serializable() || !spot_replicable(k.location, k.spot_type))
			continue;
		const SpotKey key{k.spot_type, host_id_of(k.object_id)};
		auto p = g_spot_pending.find(key);
		if (host.count(key) || (p != g_spot_pending.end() && now - p->second < kSpotPendingMs))
			continue;
		gone.push_back(k.location);
	}
	for (CMapLocation* ml : gone)
	{
		Applying guard(Kind::Spot, shared_str(ml->spot_type), ml->ObjectID());
		spots().RemoveMapLocation(ml);
	}
	++g_stats.spots_in;
	::Msg("[coop] story: map spots resynced from the host: %u spots, %u removed here", n, (u32)gone.size());
}

void host_on_spot_request(PeerId from, u8 op, Reader& r)
{
	++g_stats.requests_in;
	ExecFor ef(from);
	if (op == SpotAdd)
	{
		const shared_str type = to_shared(r.str());
		const u16 id = r.u16();
		const xr_string hint = r.str().c_str();
		const bool ser = r.boolean();
		r.u32(); // author: the sender
		if (!r.ok() || !type.size() || id == kNoId || spot_type_local(cs(type)))
			return;
		if (CMapLocation* ml = spots().GetMapLocation(type, id))
		{
			if (ser && !ml->Serializable())
				ml->SetSerializable(true);
			return; // the host has it already (both ran the same script)
		}
		if (!spot_type_known(type))
			return;
		CMapLocation* ml = spots().AddMapLocation(type, id); // the hook broadcasts it (at the next flush)
		// After AddMapLocation, whose hook clears a stale author of this key.
		SpotAuthor& a = g_spot_authors[SpotKey{type, id}];
		a.author = from;
		a.hint = shared_str(hint.c_str());
		const xr_string text = tagged_hint(a.hint, from);
		if (!text.empty())
			ml->SetHint(shared_str(text.c_str()));
		ml->SetSerializable(ser);
	}
	else if (op == SpotRemove)
	{
		const shared_str type = to_shared(r.str());
		const u16 id = r.u16();
		if (!r.ok() || !type.size() || id == kNoId)
			return;
		if (!spot_placed_by(from, spots().GetMapLocation(type, id), type, id))
		{
			++g_stats.spot_removes_refused; // the next spot resync gives it back to the client
			return;
		}
		spots().RemoveMapLocation(type, id); // the hook broadcasts it
	}
	else if (op == SpotRemoveAll)
	{
		const u16 id = r.u16();
		if (!r.ok() || id == kNoId)
			return;
		// Only the client's own spots of the object, one by one (each removal is broadcast by the hook).
		xr_vector<CMapLocation*> list;
		spots().GetMapLocations(id, list);
		xr_vector<shared_str> own;
		for (CMapLocation* ml : list)
		{
			const shared_str type = ml->spot_type;
			if (spot_placed_by(from, ml, type, id))
				own.push_back(type);
			else
				++g_stats.spot_removes_refused;
		}
		for (const shared_str& type : own)
			spots().RemoveMapLocation(type, id);
	}
}
// ------------------------------------------------------------------ hooks: map spots

void on_spot_added(const shared_str& spot_type, u16 id)
{
	if (!mgr().active() || spot_type_local(cs(spot_type)))
		return;
	if (mode().decides_world())
		g_spot_authors.erase(SpotKey{spot_type, id}); // a new spot has no author until a client's request sets it
	else
	{
		if (!client_live())
			return;
		if (applying_any())
		{
			if (!applying_this(Kind::Spot, spot_type, id) && g_apply.kind != Kind::Task)
				++g_stats.derived_dropped;
			return;
		}
	}
	Out o;
	o.type = Msg::MapSpot;
	o.spot_add = true;
	o.spot_type = spot_type;
	o.spot_id = id;
	queue_out(std::move(o)); // a full queue is counted and logged there
}

void on_spot_remove(const shared_str& spot_type, u16 id)
{
	if (!mgr().active() || !g_pGameLevel || spot_type_local(cs(spot_type)))
		return;
	if (!mode().decides_world() && (!client_live() || applying_any()))
		return;
	CMapLocation* ml = spots().GetMapLocation(spot_type, id);
	if (!spot_replicable(ml, spot_type))
		return;
	Writer w(48);
	w.u8(SpotRemove);
	w.str(cs(spot_type));
	w.u16(host_id_of(id));
	if (mode().decides_world())
	{
		g_spot_authors.erase(SpotKey{spot_type, id});
		++g_stats.spots_out;
	}
	else
		++g_stats.requests_out;
	push_out(Msg::MapSpot, std::move(w));
}

void on_spots_remove_all(u16 id)
{
	if (!mgr().active() || !g_pGameLevel)
		return;
	if (!mode().decides_world() && (!client_live() || applying_any()))
		return;
	if (mode().decides_world())
		for (auto it = g_spot_authors.begin(); it != g_spot_authors.end();) // the authors go with the spots
			it = it->first.id == id ? g_spot_authors.erase(it) : std::next(it);
	if (!has_replicable_spot(id))
		return; // nothing of the shared PDA: every receiver would find nothing to remove
	Writer w(8);
	w.u8(SpotRemoveAll);
	w.u16(host_id_of(id));
	push_out(Msg::MapSpot, std::move(w));
	++(mode().decides_world() ? g_stats.spots_out : g_stats.requests_out);
}

bool spot_is_local(LPCSTR type) { return spot_type_local(type); }
} // namespace story
} // namespace coop
