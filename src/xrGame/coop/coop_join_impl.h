// coop::join: what its files share (coop_join.cpp, _characters, _transition, _transition_load, _swap; the map of them is in
// coop_join.h). Private to them. The state of a session lives in four parts, each with its reset(); reset_state() resets all.
#pragma once

#include "coop_join.h"
#include "coop_levels.h"
#include "xrCoop/coop_arrival.h"
#include "xrCoop/coop_ids.h"
#include "xrCoop/coop_limits.h"
#include "xrCoop/coop_link_hold.h"
#include "xrCoop/coop_newchar.h"
#include "xrCoop/coop_transition.h"
#include "game_graph.h"
#include "game_news.h"

namespace coop
{
namespace join
{
constexpr u32 kRefusalsLogged = 5; // refused or ignored messages of one kind: the first few are logged
constexpr u64 kTripAnswerMs = 10000; // a client whose trip got no answer in this long stops waiting for it
constexpr u64 kMapSaveMaxAgeMs = 5 * 60 * 1000; // the save of a move stays for a map server this long

struct HostPeer
{
	u16 proxy = kNoId;
	bool loading = false; // world sent, waiting for WorldLoaded
	u64 sent_at = 0;
	// A mover of the simultaneous load: he loads the transition's save with the host (no SessionReset, no
	// JoinRequest); his WorldLoaded after LoadWait Enter ends his join. Cleared when it ends or he asks for the world.
	bool prepared = false;
};

// The host's joins and the client's own join (coop_join.cpp).
struct JoinState
{
	xr_vector<GAME_NEWS_DATA> my_news; // client: his PDA log, over every world of the session
	// host
	xr_map<PeerId, HostPeer> peers;
	// client
	u16 my_proxy = kNoId;
	u64 receive_started = 0;
	bool load_issued = false;
	bool swap_done = false;
	bool arrival_check = false; // the host put our character blind, the ground comes after the graph loads
	u64 last_progress_log = 0;
	// host: the world we serve (world_generation() when it was seen); a load or a level change
	// replaces it (epoch++ in on_world_created, the session's world epoch with it)
	u32 world_seen = 0;
	u32 epoch = 1;
	bool reset_pending = false;
	// host: one join in flight per player, one per 5 s
	JoinGate gate;
	// host: JoinRequests that came while the host's own level was still coming up (host_precaching), sent after it
	xr_vector<PeerId> precache_wait;
	// host: how long they wait for it (the precache of an unfocused window never ends)
	link_hold::PrecacheWait precache_clock;
	u32 joins_refused = 0; // diagnostics: refused JoinRequest / WorldLoaded / TransitionVote (the first few logged)
	u32 loaded_ignored = 0;
	u32 owners_bad = 0; // client: malformed AvatarList

	void reset() { *this = JoinState(); }
};
extern JoinState g_join;

// A player's character: the host's new keys and choosers, the client's choice window (coop_join_characters.cpp).
struct CharState
{
	// host: the keys this session made a new character for (coop_max_new_characters); a new session starts over
	// (on_host_started). Only keys that had no character spend it (death::new_key_charge).
	limits::NewCharacters new_characters;
	// host: the keys that had a character in this session (a returning player, a new character made, a
	// retired one); with the retired keys of the world (host_retired_in_world) they neither check nor spend the limit
	std::set<std::string> had_character;
	// host: the keys whose character retired in this session (with [coop_retired] of the world: a bare new
	// character unless coop_hardcore_new_kit is on)
	std::set<std::string> retired;
	u32 characters_refused = 0; // diagnostics: new keys refused over the limit (the first few logged)
	// host: the players with a new key who are choosing their character before the world
	newchar::Choosers choosers;
	u32 choices_ignored = 0; // diagnostics: CharChoice nobody asked for, or unreadable
	u32 choices_refused = 0; // choices the host's Lua refused (the player was asked again)
	u32 characters_made = 0; // new characters made at their faction's start before the world
	// client: the host's offer of a new character, in the choice window until it gives a choice
	enum class CharStep : u8
	{
		None,
		Offered, // to be shown (coop_menu_char.join_offer)
		Shown, // the window is open: its choice is taken by join_choice
		Sent, // the CharChoice is on its way: the world, or the offer again with a refusal
	} char_step = CharStep::None;
	// client: join_offer ran (the window may be open) and join_closed has not since. Apart from char_step:
	// a SessionReset voids the offer (char_step None) and leaves the window open for the host's next offer, but the
	// world or the end of the session may come instead.
	bool char_window = false;
	CharOfferMsg char_offer;
	u64 char_next_try = 0;
	// client: the branch of client_char_frame the log named last (char_note): one line per change
	std::string char_note = "idle";

	void reset() { *this = CharState(); }
};
extern CharState g_char;

// The waiting point of a level change (xrCoop/coop_transition.h): the host's board, this player's zone, vote and placard
// (coop_join_transition.cpp).
struct BoardState
{
	u32 votes_refused = 0; // host: refused TransitionVote (the first few logged)
	// host: who is ready for which exit; the group goes when the board says so (host_board_step)
	transition::Board board;
	u64 board_ticked = 0; // host: when the board was run last
	u64 board_sent_at = 0; // host: when TransitionState went out last
	u32 board_sent_sig = 0; // host: what it showed (state, exit, who is ready): a change goes out at once
	bool board_sent_idle = true;
	bool board_lead_known = false; // host: the last tick had a leading exit (coop_accept_transition goes through it)
	u32 board_lead = 0;
	bool releasing = false; // host: the level change in flight is the board's own (host_gate_level_change lets it by)
	u64 released_at = 0; // host: when the group was sent off; the old world lives a few frames more, nobody claims in them
	// host: a level change the host's own game asked for with no exit zone behind it (a guide, jump_to_level): what the
	// group goes to when everybody said yes
	struct GuideTrip
	{
		bool set = false;
		GameGraph::_GRAPH_ID graph = 0;
		u32 node = 0;
		Fvector position = {0.f, 0.f, 0.f};
		Fvector angles = {0.f, 0.f, 0.f};
		std::string level;
		Fvector from_position = {0.f, 0.f, 0.f}; // and where he stood: he withdraws by walking away
		PeerId by = kHostPeer;
	} guide;
	// host: the board's last message (coop_status); the host player's placard reads his own copy (view) from the link
	transition::StateMsg board_view;
	// both: the world server's board as this player's placard shows it (the host player: what the link brought him)
	transition::StateMsg view;
	u64 view_at = 0;
	u32 view_bad = 0; // unreadable TransitionState
	// host: the go. The board said go (TransitionState Going: every moving player's screen darkens); the host's
	// own level change follows transition::kGoFadeMs later (host_board_step), no claim is taken meanwhile.
	bool going = false;
	u64 going_at = 0;
	u32 going_goal = 0;
	transition::Status going_status;
	// both: this player. `want` is what he does (stands in an exit's zone or a route restrictor into it, comes along from
	// afar, or said yes to a guide trip).
	bool want = false;
	u32 want_goal = 0;
	bool zone_in = false; // his actor touches an enabled exit, or stands in a route restrictor into one
	u16 zone_changer = kNoId; // ... that one (host numbering)
	u16 zone_route = kNoId; // ... through this route restrictor (host numbering), kNoId: the changer's own zone
	u16 withdrawn_from = kNoId; // he cancelled at this exit with the key: no claim until he leaves its zone
	bool guide_yes = false; // client: he said yes to the host's guide trip
	// He held the key away from the exit the board waits at: he comes along ("go with the group") to consent_goal
	// (host numbering) until he holds it again, walks into an exit, or that wait is over (client_on_transition_state)
	bool consent = false;
	u32 consent_goal = 0;
	// The route restrictors (sr_teleport) this machine learned this world, each with the level changer its drop
	// point is in (coop.transition_route, scripts\coop_transition_route.script; this machine's numbering)
	transition::Routes routes;
	u64 zone_scanned = 0;
	float my_dist = -1.f; // metres from him to the leading exit (< 0 unknown)
	Fvector exit_pos = {0.f, 0.f, 0.f}; // the leading exit, this machine's numbering
	bool exit_known = false;
	bool held_note = false; // the "you cannot go on" line of the log was printed for this wait
	u32 votes_sent = 0, holds_applied = 0; // diagnostics
	u32 trips_refused = 0; // client: level changes with no level changer here where the group travels together
	transition::Intent intent;
	transition::Hold hold;
	limits::Pacer vote_pacer;

	void reset() { *this = BoardState(); }
};
extern BoardState g_board;

// The simultaneous load of a group transition, the held loading screen, a player's own level move and the seat holds
// (coop_join_transition_load.cpp).
struct LoadState
{
	// host: the simultaneous load of the group's transition (xrCoop/coop_transition.h PreparedLoad)
	transition::PreparedLoad load;
	u16 load_level = levels::wire::kNoLevel; // the game graph level it goes to
	bool load_world_pending = false; // the next world created is the transition's: on_world_created keeps its epoch
	bool load_pumping = false; // host_prepare is handing the saves to the network: a new join waits
	u64 load_go_at = 0; // when the group went (the clock of the log lines)
	u64 load_up_at = 0; // when the server's world came up
	u64 load_sent_at = 0; // LoadWait Waiting: the last one
	u32 load_sent_sig = 0; // ... what it said
	u32 loads_ignored = 0; // LoadReady of no load of ours (the first few logged)
	// a player (a remote client, or the host player in the host's process): his screen of that load
	transition::LoadScreen screen;
	u16 screen_level = levels::wire::kNoLevel;
	bool screen_line = false; // the "Waiting for" line is on the loading screen; screen_line_was is the header it covers
	bool screen_input_off = false; // the held screen took the level's input; screen_input_was is what it was before
	bool screen_input_was = false;
	u32 screen_input_world = 0; // g_world_generation when screen_input_was was read
	u64 load_pumped_at = 0; // the go's saves went to the network (the reload watchdog counts from here)
	std::string screen_line_was;
	u32 load_waits_bad = 0; // unreadable LoadWait
	// host: a player whose level has no map server up yet stands on his loading screen until it has
	struct Held
	{
		levels::LoadBarrier barrier;
		u16 level = levels::wire::kNoLevel;
	};
	xr_map<PeerId, Held> held;
	// host: the save the players of a move load: a map server that starts for them gets the same one (held_save)
	struct MapSave
	{
		u32 world = 0;
		u64 at = 0;
		std::vector<u8> engine;
		std::vector<u8> script;
	} map_save;
	// a client: his LevelMove. The host answers Go (the join's SessionReset follows), Refused, or Together (the
	// exit of a level entered together: he goes to the board like everybody else)
	u32 move_seq = 0;
	u16 move_changer = kNoId; // the exit he asked for last, host numbering
	u64 move_sent_at = 0;
	u16 move_refused = kNoId; // refused at this exit: not asked again until he leaves its zone
	bool move_board = false;
	// a player's machine (transition::SeatHold): the world goes only after his actor is out of a
	// managed seat. His LevelMove waits while he sits (Server: the map server of this level releases him while he is still its
	// player, client_send_move); the save of a group transition waits for his detach (Co-op, on_blob_complete). client_seat_holds
	// lets them go.
	transition::SeatHold move_hold;
	levels::wire::LevelMoveMsg held_move;
	bool move_held = false;
	transition::SeatHold save_hold;
	std::vector<u8> seated_save; // not held_save() below: that is the host's save for a map server

	void reset() { *this = LoadState(); }
};
extern LoadState g_load;

// Every part of the state above back to its start (a session starts or ends).
void reset_state();

// Where a new character appears: the point the host's Lua picked (its faction's start or camp, checked by
// host_place), else next to the host.
struct StartPoint
{
	bool set = false;
	Fvector pos{};
	u32 lvid = u32(-1);
	GameGraph::_GRAPH_ID gvid = GameGraph::_GRAPH_ID(-1);
	std::string where; // "start:<location>" / "camp:<smart>" (the log)
};

// --- coop_join.cpp
extern u32 g_world_generation; // CALifeSimulator constructions in this process
extern u32 g_mirror_generation; // g_world_generation of the world loaded from a host's save (0: none)
bool read_file(LPCSTR path, std::vector<u8>& out);
void save_path(string_path& out, LPCSTR name, LPCSTR ext);
bool host_send_save(PeerId from, const PeerInfo& p_ref, CSE_ALifeHumanStalker& proxy_ref);
void client_load_world(std::vector<u8>& data);
std::string nick_of_peer(PeerId id);

// --- coop_join_characters.cpp
CSE_ALifeHumanStalker* find_proxy_by_key(const std::string& digest);
void set_player_name(CSE_ALifeTraderAbstract& who, const std::string& nick);
CSE_ALifeHumanStalker* create_proxy(const PeerInfo& p, const std::string& key_digest, const StartPoint& at);
void mark_arrival(u16 proxy);
bool host_fall_back_to_level(PeerId from, CSE_ALifeHumanStalker& proxy, LPCSTR nick);
void take_offline(u16 proxy);
void bring_online(u16 proxy);
bool host_key_digest(PeerId from, const PeerInfo& p, std::string& out);
void host_count_new_character(const std::string& key_digest);
bool host_admits_new_key(PeerId from, const PeerInfo& p, const std::string& key_digest);
bool host_offer_character(PeerId from, const PeerInfo& p, const char* refusal);
bool host_make_creator(PeerId from, const PeerInfo& p);
void host_on_char_choice(PeerId from, Reader& r);
void client_on_char_offer(PeerId from, Reader& r);
void char_note(const char* key, const char* text);
void client_char_frame();
void client_char_closed(const char* why);

// --- coop_join_transition.cpp
std::string level_name_of(GameGraph::_GRAPH_ID gv);
u16 level_id_of(GameGraph::_GRAPH_ID gv);
std::string level_name_by_id(u16 id);
bool host_has_company();
CSE_ALifeLevelChanger* level_changer_of_goal(u16 id);
void host_board_step(u64 now);
void transition_step(u64 now);
void transition_world_replaced();
void host_on_transition_vote(PeerId from, Reader& r);
void client_on_transition_state(PeerId from, Reader& r);

// --- coop_join_transition_load.cpp
void host_load_forget(PeerId peer, const char* why);
void host_load_void(const char* why);
void host_load_watch(u64 now);
void host_load_step(u64 now);
void host_hold_for_server(PeerId from, u16 level);
void host_held_step(u64 now);
void load_screen_line(bool on);
void load_screen_input(bool held);
void load_screen_off();
void client_go_live();
void player_load_step(u64 now);
void world_leaving(u32 to_epoch);
bool host_release(u32 goal);
void client_stays_on_level();
bool client_send_move(const levels::wire::LevelMoveMsg& m, u64 now);
void client_level_move(u16 changer, u64 now);
void host_on_load_ready(PeerId from, Reader& r);
void client_on_load_wait(PeerId from, Reader& r);
void client_on_level_move_result(PeerId from, Reader& r);
bool prepared_save_awaited();
void client_seat_holds(u64 now);

// --- coop_join_swap.cpp
extern arrival::Journal g_arrival; // the journal of this player's last transition
void arrival_begin(const char* kind, u16 level);
void arrival_step(u64 now);
} // namespace join
} // namespace coop
