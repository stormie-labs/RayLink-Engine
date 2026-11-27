# Anomaly Coop — the game side of the engine (src/xrGame/coop)

Co-op and the server for S.T.A.L.K.E.R. Anomaly on the open xray-monolith engine. The rules of the code are
`docs/CODE_RULES.md`; the two modes and the three layers are `docs/MODES.md`. This file is the map of this folder.

## The layers
| Layer | Where | What |
|---|---|---|
| Core (`core`) | `src/xrGame/coop/*.cpp`, `src/xrCoop/*.h` | what both modes need and do the same way: sync, items, death, money, tasks and checkpoints as rules, HUD, menus, the protocol |
| Co-op (`hostplay`) | `hostplay/`, `src/xrCoop/hostplay/`, namespace `coop::hostplay` | only where the host plays himself: the host player's LocalLink, the group's level change, Steam |
| Server (`srv`) | `srv/`, `src/xrCoop/srv/`, namespace `coop::srv` (and `coop::levels`, `coop::mapserver`, `coop::dedicated`) | only the world server and the map servers: seats, map servers, bridges, level transfer, the operator's console |

- The Core never asks which mode it runs in. What depends on the mode it asks the session's mode (`mode()`, `xrCoop/coop_mode.h`):
  `where(peer)`, `decider_of`, `decides_world()`, `leads_level()`, `actor_is_player()` (the full list: `docs/CODE_RULES.md`
  LAYER-1). `is_host()` / `is_client()` and the role helpers of `coop_manager.h` are the old names; new Core code does not use them.
- The Core reaches a layer only through the switchboard (`coop_mode.cpp`: `Layer` boots, `mode_for`, `register_layers_console`,
  `register_layers_lua`). Layers do not name each other. `tools/check_layers.py` checks all of it.
- Engine (stock) files get one-line hooks into a `coop::` function, marked `// coop:`, in the stock order of effects.

## The modules
- `coop_manager.*` owns the network sessions, the phase and the roles of this process, and runs every frame
  (`coop::on_frame()` from `CGamePersistent::OnFrame`, menu and game).
- `coop_sync.cpp` is the module table: every pass (once per process, the handler tables, session start, a peer left, reset,
  frame, `coop_status`) is a list of rows in its own order; a frame row has its gate (always, world up, world not up, no
  level in the making). A new module adds its row to each pass it takes part in.
- `coop_console.cpp` registers every command and setting once at the start of the process, before user.ltx is read; the
  layers' own come through `register_layers_console`.

## Threads
- Manager::frame, every module's `frame()`, the handlers of messages and Lua run on the main thread.
- ALife updates, `shedule_Update`, `xrServer::Process_spawn` and `CLevel::net_Update` may run on the game thread while the
  main thread renders. The game thread is joined before the next frame, and ALife and server hooks may run on it.
  Keep these hooks outside the co-op `frame()`. This rule has no runtime check.
- Data a hook fills is guarded by one lock type, `xrCriticalSection`; under the lock a container is swapped or copied,
  the sending happens in `frame()` (`docs/CODE_RULES.md` CPP-6, CPP-7). Hooks do not touch rendering state.
- A thread we start (the transport's keep-alive, Steam, the console window) has an owner that stops and joins it at exit;
  it touches neither D3D nor Lua.

## Messages
- `xrCoop/coop_protocol.h` holds the `coop::Msg` types, `xrCoop/coop_wire.h` the `Writer` / `Reader`; a new message follows
  `docs/CODE_RULES.md` CPP-17.
- `coop_net.h`: `net::host_broadcast`, `net::host_send` (queued while the peer loads the world), `net::client_send`.
- A module installs its handlers in `install_server(Session&)` (what the deciding process takes) and
  `install_client(Endpoint&)` (what a player's machine takes); the session's mode installs the tables when a session starts.
  Handlers get `(PeerId from, Reader& r)`.
- Every message is idempotent (a repeat changes nothing) and tolerates objects that do not exist yet. Ids on the wire are in
  the host's numbering (`join::to_host` / `join::to_local`).

## Diagnostics
- `coop_status`, `coop_dump [id]`, `coop_netsim <lag> [jitter] [loss%]`, `coop_ini_stats`, `coop_memory_map`; a server's
  process also `coop_mapserver_stats`. A module adds its counters to its `print_status()`.
- Log lines start with `[coop]`, errors with `! [coop]`. Inside `namespace coop` the engine logger is `::Msg(...)`: plain `Msg`
  is the `coop::Msg` enum.
