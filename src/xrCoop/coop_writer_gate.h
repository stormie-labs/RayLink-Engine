// Anomaly Coop - the writer gate of script writes. After the call gate (coop_calls.h decide) the engine asks the writer rule
// (writer::gate, coop_writer_rules.h) about the script writes that create under an object, release it or give it an info:
// alife():create (every form), create_ammo, register, clone_weapon, release, give_info, disable_info and level.spawn_item
// (alife_simulator_script.cpp, level_script.cpp; the engine half is coop_router.cpp writer_refuses). The co-op host and single player
// write everything (Run) and a player's machine keeps the call gate's verdict (Mirror): for them the call site does what the call gate
// says, in both settings (tests/test_writer_gate.cpp, the golden table). The world server and a map server get the writer's verdict.
//
// The default setting, Log, only counts verdicts (the census in coop_status, one log line per call and script), with one exception:
// the writes the other server undoes or makes itself are refused in both settings (refused_in_both_settings). Under
// coop_writer_gate = enforce a Refuse verdict is refused. Ask and Order have no engine path: their call sites keep the call
// gate's answer in both settings, counted.
// Pure C++17, header-only, no engine dependencies: used by the engine (a stock xrServer file included) and the unit-test exe.
#pragma once

#include "coop_text.h"
#include "coop_writer_rules.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop
{
namespace calls
{
// What the engine does with one gated call on this machine: the answer of the call gate (coop_calls.h decide) and of the writer
// gate after it (decide_writer_gate).
enum class Gate : uint8_t
{
	Run,     // the stock engine code
	Request, // the request path to the host (the gated function knows it)
	Refuse   // nothing; the function returns what the stock code returns for "nothing" (nil, false)
};

inline const char* gate_name(Gate g)
{
	switch (g)
	{
	case Gate::Run: return "run";
	case Gate::Request: return "request";
	case Gate::Refuse: return "refuse";
	}
	return "run";
}

// The machine setting coop_writer_gate (the console command, coop_router.cpp; the value is the router's writer_gate()):
//   Log      the default: counts per call and script what the gate would refuse, ask or order, one log line each, and changes
//            nothing but the writes the other server undoes or makes itself (refused_in_both_settings);
//   Enforce  refuses what the verdict refuses.
// The rule is the same in both settings.
enum class WriterGate : uint8_t
{
	Log,
	Enforce,
};

inline const char* writer_gate_name(WriterGate g)
{
	switch (g)
	{
	case WriterGate::Log: return "log";
	case WriterGate::Enforce: return "enforce";
	}
	return "log";
}

// "log" or "enforce" (letter case and surrounding blanks ignored); false (out unchanged) for anything else.
inline bool parse_writer_gate(std::string_view text, WriterGate& out)
{
	const std::string_view t = ascii::trim(text, ascii::space_or_eol);
	if (ascii::iequals(t, "log"))
		out = WriterGate::Log;
	else if (ascii::iequals(t, "enforce"))
		out = WriterGate::Enforce;
	else
		return false;
	return true;
}

// What one gated write does once the writer gate has looked at it:
//   KeepCallGate  the call gate decides (Run, Request, Refuse) for Run, Mirror, and any verdict under Log (but refused_in_both_settings);
//   Refuse        nothing: the stock "nothing" (nil, false), counted, one log line per call and script;
//   Ask           a map server: its journal's Ask entry to its world, which executes it if it writes the object; not applied here first;
//   Order         the world server: the numbered far order to the map server that writes it, held until answered.
// Ask and Order follow the call gate and count in both settings because the engine has no delivery path for them.
enum class WriteAct : uint8_t
{
	KeepCallGate,
	Refuse,
	Ask,
	Order,
};

inline WriteAct write_act(writer::GateVerdict v, WriterGate setting)
{
	if (setting == WriterGate::Log)
		return WriteAct::KeepCallGate;
	switch (v)
	{
	case writer::GateVerdict::Refuse: return WriteAct::Refuse;
	case writer::GateVerdict::Ask: return WriteAct::Ask;
	case writer::GateVerdict::Order: return WriteAct::Order;
	case writer::GateVerdict::Run:
	case writer::GateVerdict::Mirror: break;
	}
	return WriteAct::KeepCallGate;
}

// The census counts what the gate does not run here (or would not, under Log): Refuse, Ask, Order.
inline bool census_counts(writer::GateVerdict v)
{
	return v == writer::GateVerdict::Refuse || v == writer::GateVerdict::Ask || v == writer::GateVerdict::Order;
}

// The call site follows the call gate unless the writer gate refuses the write.
inline Gate decide_write(Gate call_gate, WriteAct act)
{
	return act == WriteAct::Refuse ? Gate::Refuse : call_gate;
}

// The scope of the code a write comes from, from the router domain of the innermost wrapper of coop_route.script on the Lua stack
// (coop.route.scoped): host -> Decision; level, owner, view -> LevelCode (it runs on the map server of the map as well); none,
// kUnknownScope (a time event or unique call set by code of no known scope) or no wrapper at all -> Unknown, which the world server
// counts as a decision (smart respawns, death consequences, C++ to Lua paths).
constexpr std::string_view kUnknownScope = "?";

inline writer::CodeScope scope_of_domain(std::string_view domain)
{
	if (ascii::iequals(domain, "host"))
		return writer::CodeScope::Decision;
	if (ascii::iequals(domain, "level") || ascii::iequals(domain, "owner") || ascii::iequals(domain, "view"))
		return writer::CodeScope::LevelCode;
	return writer::CodeScope::Unknown;
}

inline const char* scope_name(writer::CodeScope s)
{
	switch (s)
	{
	case writer::CodeScope::LevelCode: return "level code";
	case writer::CodeScope::Decision: return "decision";
	case writer::CodeScope::Unknown: return "unknown";
	}
	return "unknown";
}

// What a gated write names:
//   Object        an object of this world: what a release or an info changes, or the parent a create puts the new object under; its
//                 root is writer::root_of's;
//   NewResident   a create with no parent of a thing that is no unit (an item, a box, a zone): the new object is its own root, a
//                 resident of the map it stands on;
//   NewUnit       a create with no parent of a squad or a creature;
//   NewOfSection  a create with no parent whose kind the engine reads from the section only where it changes the answer.
enum class Target : uint8_t
{
	Object,
	NewResident,
	NewUnit,
	NewOfSection,
};

struct WriteTarget
{
	Target kind = Target::Object;
	uint16_t id = writer::kNoId; // Object: the object, or the parent of a create
	uint16_t level = writer::kNoLevel; // the New kinds: the map the new object stands on
	const char* what = nullptr; // for the log line, and the section of NewOfSection
};

// The writer of the new root of a create with no parent: a unit this process makes is its own, the world server's and a
// map server's on its own map (born under its slice and held from its birth; also while it starts); anything else is a
// resident of its map, written by whoever writes that map's residents (`resident`: the Mode's answer for a resident there). A map
// server never writes another map, so a unit it would make there counts as that map's resident.
inline writer::Writer new_root_writer(writer::ProcessKind p, Target kind, bool on_own_level, const writer::Writer& resident)
{
	if (kind != Target::NewUnit)
		return resident;
	switch (p)
	{
	case writer::ProcessKind::World: return writer::this_process();
	case writer::ProcessKind::Level: return on_own_level ? writer::this_process() : resident;
	case writer::ProcessKind::Whole:
	case writer::ProcessKind::Mirror: break;
	}
	return resident;
}

// Is the root the ghost: actor 0 of a process whose actor plays nobody (the world server, a map server)?
inline bool is_ghost(const writer::RootFacts& root, bool actor_is_player)
{
	return !actor_is_player && root.kind == writer::RootKind::Character && root.root == writer::kGhostId;
}

// What the engine found about one gated write on this process: its kind, the writer of the target's root (Mode::writer_of, or
// new_root_writer for a create with no parent), the scope of the code, whether that root stands on the map this process leads, and
// whether it is the ghost.
struct WriteFacts
{
	writer::ProcessKind process;
	writer::Writer writer;
	writer::CodeScope scope;
	bool root_on_own_level;
	bool ghost;
	bool root_on_other_level; // a map server: the root stands on a known map that is not its own
	bool new_root = false; // a create with no parent: the new object is its own root
};

// The refused writes that the other server undoes or makes itself whatever this one does: refused here in both settings, so Log
// changes nothing either server would keep. A mod's markers made and taken back on every map of the game by one script that runs on
// the world server and on each map server show all three:
//   a map server's create on another map (a new root there, or under a root that stands there): the world's acceptance refuses its
//     Spawn as not the writer (accept::spawn_on_senders_map, accept::accepts), and the repair releases it here a round trip later;
//   a map server's release on another map: the world refuses its Destroy as not the writer (accept::sender_writes: a map server never
//     writes another map), and the repair makes the object again here; the world, which writes it, releases it itself;
//   the world server's level-code create of a new root on a map a map server leads (the row WS | a Mk | level code): that map's server
//     runs the same code and makes its own, so the world's would be a second of each, released by that map server with its own.
// Under Log each of them ran, and each map server's printed a refusal on both servers.
inline bool refused_in_both_settings(const WriteFacts& f, writer::WriteOp op, writer::GateVerdict v)
{
	if (v != writer::GateVerdict::Refuse)
		return false;
	switch (f.process)
	{
	case writer::ProcessKind::Level:
		return f.root_on_other_level && (op == writer::WriteOp::Create || op == writer::WriteOp::Release);
	case writer::ProcessKind::World:
		return op == writer::WriteOp::Create && f.new_root && f.scope == writer::CodeScope::LevelCode &&
		    f.writer.is == writer::WriterIs::Other;
	case writer::ProcessKind::Whole:
	case writer::ProcessKind::Mirror: break;
	}
	return false;
}

struct WriteDecision
{
	writer::GateVerdict verdict;
	WriteAct act;
	Gate gate; // the call gate's verdict or a writer refusal
};

// The whole writer gate of one write: the writer's verdict, what it does under the setting, and the gate the call site acts on.
inline WriteDecision decide_writer_gate(const WriteFacts& f, writer::WriteOp op, Gate call_gate, WriterGate setting)
{
	const writer::GateVerdict v = writer::gate(f.process, f.writer, op, f.scope, f.root_on_own_level, f.ghost);
	const WriteAct act = refused_in_both_settings(f, op, v) ? WriteAct::Refuse : write_act(v, setting);
	return WriteDecision{v, act, decide_write(call_gate, act)};
}

// ------------------------------------------------------------------------------------------------- the census (coop_status)

//   Refuse, Ask, Order  the verdict (census_counts);
//   RunIntoLed          the world server ran a create into a map a map server leads, from code that may run on that map server as
//                       well (a world-server write of unknown scope into a led map; each case wants a route line): it runs in both
//                       settings, and only the census can name it.
enum class CensusKind : uint8_t
{
	Refuse,
	Ask,
	Order,
	RunIntoLed,
};

// The census kind of a counted verdict (census_counts); Refuse for any other.
inline CensusKind census_kind(writer::GateVerdict v)
{
	return v == writer::GateVerdict::Ask ? CensusKind::Ask : v == writer::GateVerdict::Order ? CensusKind::Order : CensusKind::Refuse;
}

inline const char* census_name(CensusKind k)
{
	switch (k)
	{
	case CensusKind::Refuse: return "refuse";
	case CensusKind::Ask: return "ask";
	case CensusKind::Order: return "order";
	case CensusKind::RunIntoLed: return "run into led";
	}
	return "refuse";
}

// What the census counts of one write:
//   - a verdict the gate does not run here (or would not, under Log): Refuse, Ask, Order;
//   - on the world server, a create that runs under a root a map server writes (`root_writer`: Mode::writer_of for the target's root,
//     or for the map of a new root, before new_root_writer), from code not known to run on the world only: an unknown scope (a time
//     event or unique call of no known scope, a C++ to Lua path; the table runs it as a decision) or level code (it makes a new unit
//     there, the world's own by new_root_writer, while that map's server runs the same level code). That map server may make the same
//     object: RunIntoLed. A decision (the host domain) is not counted: the router keeps it off the map server.
// Nothing else is counted: the co-op host, a player's machine, a map server's own writes.
struct CensusNote
{
	bool counted;
	CensusKind kind;
};

inline CensusNote census_note(writer::ProcessKind p, const writer::Writer& root_writer, writer::WriteOp op, writer::CodeScope scope,
    writer::GateVerdict v)
{
	if (census_counts(v))
		return CensusNote{true, census_kind(v)};
	const bool into_led = p == writer::ProcessKind::World && v == writer::GateVerdict::Run && op == writer::WriteOp::Create &&
	    root_writer.is == writer::WriterIs::Other && scope != writer::CodeScope::Decision;
	return CensusNote{into_led, CensusKind::RunIntoLed};
}

// The kind of a new object of a class with no parent (coop_router.cpp section_kind): a squad or a monster is a unit (writer::is_unit;
// a new creature lives: CSE_ALifeCreatureAbstract starts with health 1), anything else a resident of its map.
inline Target new_kind_of_class(bool group, bool monster)
{
	return writer::is_unit(group, monster, true) ? Target::NewUnit : Target::NewResident;
}

// What the gate refused, asked or ordered (or would have, under Log), and the world's creates that ran into a led map (census_note),
// per (kind, call, script), for coop_status: the census read before enforce. The first note of a key gets its one log line; the table is bounded: a key
// past kMaxKeys counts in the totals only, without a line.
class WriterCensus
{
public:
	static constexpr size_t kMaxKeys = 512;

	struct Totals
	{
		uint64_t refuse = 0, ask = 0, order = 0, led = 0; // led: RunIntoLed
	};

	// Counts one write; true when its (kind, call, script) is new since clear(): the caller writes its log line.
	bool note(CensusKind k, std::string_view call, std::string_view script)
	{
		switch (k)
		{
		case CensusKind::Refuse: ++m_totals.refuse; break;
		case CensusKind::Ask: ++m_totals.ask; break;
		case CensusKind::Order: ++m_totals.order; break;
		case CensusKind::RunIntoLed: ++m_totals.led; break;
		}
		std::string key;
		key.reserve(call.size() + script.size() + 16);
		key.append(census_name(k)).append(" ").append(call).append(" @ ").append(script);
		const auto it = m_keys.find(key);
		if (it != m_keys.end())
		{
			++it->second;
			return false;
		}
		if (m_keys.size() >= kMaxKeys)
		{
			++m_dropped;
			return false;
		}
		m_keys.emplace(std::move(key), 1);
		return true;
	}

	const Totals& totals() const { return m_totals; }
	size_t keys() const { return m_keys.size(); }
	uint64_t dropped() const { return m_dropped; }

	// The keys counted most, most first (a tie by key), at most n: "refuse alife:create @ some_script" -> count.
	std::vector<std::pair<std::string, uint64_t>> top(size_t n) const
	{
		std::vector<std::pair<std::string, uint64_t>> all(m_keys.begin(), m_keys.end());
		const auto more = [](const std::pair<std::string, uint64_t>& a, const std::pair<std::string, uint64_t>& b)
		{ return a.second != b.second ? a.second > b.second : a.first < b.first; };
		const size_t k = n < all.size() ? n : all.size();
		std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(k), all.end(), more);
		all.resize(k);
		return all;
	}

	void clear()
	{
		m_totals = Totals();
		m_keys.clear();
		m_dropped = 0;
	}

private:
	Totals m_totals;
	std::map<std::string, uint64_t> m_keys;
	uint64_t m_dropped = 0;
};

// ------------------------------------------------------------------------------------------------- the id pool of a map server
// A map server takes the ids of its own pool (its slice and the world's blocks: Mode::own_free_id, xrServer::PerformIDgen). What it
// does when the pool has no id left depends on where the id is taken:
//   Start  the start of a create a caller can still refuse with the stock "nothing": a script create before any stock code runs
//          (alife_simulator_script.cpp), a new entity of a spawn packet (xrServer::Process_spawn's own "not created", NULL);
//   Chain  an id a stock chain that already runs needs: the supplies of an object being made (CSE_ALifeObject::spawn_supplies,
//          CSE_ALifeMonsterBase::spawn_supplies, CSE_ALifeTraderAbstract::spawn_supplies dereference what spawn_item returns), a
//          spawn-graph group's member. No caller there can take a refusal.
enum class IdStep : uint8_t
{
	Start,
	Chain,
};

//   StockId  the stock generator's id, outside the pool, logged loudly: the world refuses its Spawn (IdRange) and the repair
//            releases it one round trip later;
//   Refuse   not created.
enum class NoId : uint8_t
{
	StockId,
	Refuse,
};

// Enforce refuses a create at its start; Log refuses nothing; a chain is never refused.
inline NoId no_id(WriterGate setting, IdStep step)
{
	return setting == WriterGate::Enforce && step == IdStep::Start ? NoId::Refuse : NoId::StockId;
}
} // namespace calls
} // namespace coop
