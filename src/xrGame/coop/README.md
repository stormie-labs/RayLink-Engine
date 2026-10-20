# Anomaly Coop — engine layer (src/xrGame/coop)

Co-op for S.T.A.L.K.E.R. Anomaly 1.5.3 on the open xray-monolith engine (revision ad9d841660, branch `coop`).
Project root: `D:\Projects\ForEverything\anomaly-coop` (docs: `DESIGN.md` §10, `SPEC.md`, `MODLOG.md`).

## Rules for everyone working here
- **Sources:** only this open engine, OpenXRay (MIT), ENet (MIT), GameNetworkingSockets (BSD-3), general knowledge,
  and the project docs (`SPEC.md`, `DESIGN.md`, `logic-md/*.md` = behaviour notes). Never copy code from closed mods.
- **Thin engine hooks.** Logic lives in new files under `src/xrGame/coop/`. Inside existing engine files add only short
  calls into coop code, each marked with a comment containing `Anomaly Coop`. Never reformat engine code.
- **Code style:** tabs, `m_` members, engine types (`u16`, `xr_vector`, `shared_str`), no exceptions, no `using namespace` in
  headers. Inside `namespace coop`, call the engine logger as `::Msg(...)` — plain `Msg` is the `coop::Msg` enum.
  Log lines start with `[coop]`. Errors start with `! [coop]`.
- **Compile check (no link):** `bash D:/Projects/ForEverything/anomaly-coop/tools/compile_files.sh <paths relative to src/xrGame>`
  e.g. `coop/coop_events.cpp Level_network.cpp`. It holds a lock (one compile at a time); a full build is done by the lead.
  New `.cpp/.h` files must be added to `xrGame.vcxproj` with
  `python D:/Projects/ForEverything/anomaly-coop/tools/vcxproj_add.py D:/Projects/ForEverything/anomaly-coop/engine/src/xrGame/xrGame.vcxproj Coop "coop\file.cpp" ...`
  (the lead may have pre-registered your files; the script is idempotent).
- Do not run git commands. Do not edit files owned by another work package (see the table at the end).

## Architecture ("mirror world")
- Every player runs a normal single-player Anomaly game (own `Actor()`, UI, PDA, scripts). The **host** is authoritative.
- **Join:** the host writes a save (`.scop` + Anomaly's script storage `.scoc`) and streams it; the client loads it, so **all
  server entity ids are identical** on both machines.
- **Players:** a remote player is a stalker entity of section `coop_player_avatar` ("proxy") in the host world; it carries the
  player's inventory as normal world items in the host save. On a client, right after the save is read, the local actor (id 0)
  and the client's own proxy swap data (`coop_join.cpp: on_alife_objects_loaded`). Id mapping: only the pair (0, my_proxy) is
  swapped: `join::to_local(host_id)`, `join::to_host(local_id)`. **All ids on the wire are in host numbering.**
- **Absent players are not in the world** (W9): a player's character (`coop_player_avatar`, any machine) is online
  only while its player is in the session: on the host from WorldLoaded until he leaves (`coop::switch_vote`,
  `join::host_player_in_world`, `join::take_offline/bring_online`), never without a session. It stays in ALife and
  in the save, offline, with its inventory. Nobody can talk to it (talk off in `coop_remote.cpp`, silent refusal
  in `dialog::may_start_talk`).
- **Link liveness** (W9): a silent peer is dropped after 10 s (`coop_manager.cpp kPeerTimeoutMs`). Code that blocks
  the main thread for long (level teardown/load, writing a save) calls `coop::on_blocking_work()`; the transport
  then answers ENet from a helper thread (`Session::hold_connection`) until the next normal frame.
- **Client ALife is frozen** (no offline simulation). Online/offline is decided by the host (`coop_alife.cpp`,
  `entities::client_wants_online`). Host-side switching uses the distance to the nearest player (`observer_distance`).
- **Only the host creates/destroys server entities** (P7). Clients send requests; the host applies the first valid one and
  broadcasts the result; clients apply results with the same ids.
- **Remote-driven objects:** on clients every NPC/monster is driven by host snapshots (`coop_entities.cpp`), and every player
  avatar (on host and clients) by its player's snapshots (`coop_players.cpp`). Remote-driven objects must not think, select
  animations, run script binder logic or apply damage locally. `coop::sync::is_remote_driven(obj)` answers that.

## Threading (verified by reading the engine)
- Network pump: `coop::on_frame()` from `CGamePersistent::OnFrame` (main thread, FrameMove, also in the main menu).
- ALife update, object `shedule_Update`, `xrServer::Process_spawn` and `CLevel::net_Update` run in `GameThread`
  (worker) **while the main thread renders**; the worker is joined before the next FrameMove. So coop code called from either
  side never runs concurrently with the network pump, but must not touch rendering state. Hooks called from the worker
  should only record facts; sending happens in `frame()`.

## Messaging
- `xrCoop/coop_protocol.h` — `coop::Msg` types (append new ones only in the ranges there), `Writer`/`Reader` in `coop_wire.h`.
- `coop_net.h` — `net::host_broadcast`, `net::host_send` (queued while a client loads, P6), `net::client_send`.
- Register handlers in your module's `install(Session&)` via `s.on(Msg::X, handler)`; the lead calls `install` from
  `coop_sync.cpp`. Handlers get `(PeerId from, Reader& r)`. Check the role (`mgr().is_host()`, `mgr().is_live_client()`).
- Every message must be idempotent (a repeat changes nothing) and must tolerate objects that do not exist yet (P6).

## Diagnostics (P9)
- `coop_status`, `coop_dump [id]`, `coop_netsim <lag> [jitter] [loss%]`. Add your counters to your module's
  `print_status()`.

## Work packages and file ownership
| WP | Owner | Files |
|---|---|---|
| core | lead | coop_manager, coop_console, coop_join, coop_world, coop_sync, coop_net, coop_alife, coop_entities, coop_players; alife_* hooks, GamePersistent, console_commands, Actor.h |
| net | agent | `src/xrCoop/*`, `src/3rd party/enet/*`, `tests/*` |
| WP1 remote creatures | agent | coop_remote.*; ai/stalker/ai_stalker*.cpp/.h (except ai_stalker_fire.cpp), stalker_animation_manager*.cpp, CustomMonster.*, ai/monsters/basemonster/base_monster*.cpp, ai/monsters/control_animation_base*.cpp, Level_network_spawn.cpp, script_binder.cpp |
| WP2 events & requests | agent | coop_events.*, coop_requests.*; Level_network.cpp, xrServer_process_event*.cpp, alife_simulator_script.cpp, level_script.cpp, ai/stalker/ai_stalker_fire.cpp, Actor.cpp (hit section only) |
| WP3 world state | agent | coop_worldstate.*; alife_time_manager*, environment/weather hooks (xrEngine/Environment*.cpp only if unavoidable), game time console |
| WP4 story & tasks | agent | coop_story.*; GameTaskManager.cpp, GameTask.cpp, Actor_Events / info portion code, relation_registry*.cpp, map_manager.cpp |
| WP5 Lua & gamedata | agent | coop_script.* (luabind exports), script_engine hooks, `anomaly-coop/gamedata/**` |
| WP6 dialogs, trade, containers | agent | coop_dialog.*, coop_trade.*; PhraseDialog*.cpp, AI_PhraseDialogManager.cpp, PhraseDialogManager.cpp, ui/UITalkWnd*.cpp, ui/UIActorMenuTrade.cpp, ui/UIActorMenuDeadBodySearch.cpp, ui/UIActorMenu.cpp (open/close hooks only), Trade*.cpp, ai/trader/ai_trader.cpp |
| WP7 world objects | agent | coop_objects.*; PhysicObject.cpp, BreakableObject.cpp, DestroyablePhysicsObject.cpp, PHDestroyable.cpp, CustomZone.cpp, ZoneCampfire.cpp, Artefact.cpp, artefact_activation.cpp, Explosive.cpp, Missile.cpp, Grenade.cpp, Bolt.cpp, script door / interactable helpers (doors in Anomaly are CPhysicObject + scripts) |
| WP8 player survival & presentation | agent | coop_survival.*, coop_presence.*; ActorCondition.cpp, HUDManager.cpp, ui/UIHudStatesWnd*.cpp, Spectator.cpp; Actor.cpp hooks are applied by the lead (WP2 owns Actor.cpp now) |
| WP9 owned item state | agent | coop_items.*; Weapon.cpp, WeaponMagazined.cpp, WeaponMagazinedWGrenade.cpp, WeaponAmmo.cpp, inventory_item.cpp (state/condition only), eatable_item.cpp, ui/UIInvUpgrade.cpp, inventory_upgrade*.cpp |

Notes:
- `level_script.cpp`: WP2 owns spawn functions, WP3 time/weather functions, WP4 map-spot functions — touch only yours.
- `Actor.cpp`: WP2 hit section; anything else is requested from the lead.
- `stalker_animation_script.cpp` belongs to WP1 (stalker animation files).

### Wave 3 (2026-10-20 18:13)
| WP | Files |
|---|---|
| W3a rockets + router bypasses | coop_events.*, coop_requests.* (rocket/launch parts and Lua bypass gates only), RocketLauncher.cpp, CustomRocket.cpp, ExplosiveRocket.cpp, WeaponMagazinedWGrenade.cpp (launch path only), script_game_object3.cpp (attach_vehicle gate), alife_simulator_script.cpp (teleport/kill/switch gates) |
| W3b barter + money order | coop_trade.*, ui/UIActorMenuTrade.cpp, coop_requests.cpp (money/outbox flush only — coordinate with W3a: touch only ReqMoney/outbox functions) |
| W3c story follow-ups | coop_story.*, relation_registry_actions.cpp, `gamedata/scripts/coop_main.script` (task routing section only) |
| W3d host settings + mod diff screen | coop_script.*, `gamedata/scripts/coop_*.script` except coop_main.script task section, `gamedata/configs/**`, new `gamedata/configs/ui/*coop*.xml` |
| lead (R1 review running) | coop_manager, coop_console, coop_join, coop_world, coop_sync, coop_net, coop_alife, coop_entities, coop_players, coop_equip, alife_* hooks |
