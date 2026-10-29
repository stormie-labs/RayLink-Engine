<div align="center">

# RayLink Engine

**Co-op for S.T.A.L.K.E.R. Anomaly 1.5.3 and G.A.M.M.A.**

Explore the Zone together: one shared world, the same A-Life, the same tasks, the same emissions.

[**Download the launcher**](https://github.com/stormie-labs/RayLink-Launcher/releases/latest) ·
[Co-op builds](https://github.com/stormie-labs/RayLink-Coop/releases) ·
[Engine releases](https://github.com/stormie-labs/RayLink-Engine/releases)

</div>

---

## What it is

RayLink Engine is a co-op engine for S.T.A.L.K.E.R. Anomaly 1.5.3. It runs the single-player game you know, mods and
all, and adds a network layer: one player hosts his world, friends join it, and everyone plays in it at the same time.

It is half of RayLink. The other half, [RayLink-Coop](https://github.com/stormie-labs/RayLink-Coop), is the co-op layer
on top: scripts, configs, UI and compatibility with G.A.M.M.A. mods. You do not need to put them together by hand: the
launcher does it.

## Features

| | |
|---|---|
| **Shared world** | The host's world is the only truth: NPCs, mutants, items, stashes, doors and physics are the same for everyone. |
| **A-Life near every player** | The Zone lives around each player, not only around the host: squads, fights and AI see every player. |
| **Shared story and tasks** | Tasks, PDA news, infoportions and relations are shared; any player can turn a task in. |
| **Down, not out** | A fallen player bleeds out and can be revived with a medkit; death modes: checkpoint (campfires, tents) or hardcore. |
| **Together everywhere** | Level transitions as a group (hold the use key at the exit), shared sleep, shared emissions and psi-storms. |
| **Stable netcode** | ENet over UDP, a network thread that keeps the ping independent of your frame rate, a net graph in the options. |
| **Up to 8 players** | The host and 7 friends. |

## Quick start

1. **Install the game.** S.T.A.L.K.E.R. Anomaly 1.5.3. For G.A.M.M.A., install it as usual (Mod Organizer 2).
2. **Install the launcher.** Download `RayLinkSetup.exe` from
   [RayLink-Launcher releases](https://github.com/stormie-labs/RayLink-Launcher/releases/latest) and run it.
   No admin rights needed.
3. **Point it at your game.** Pick G.A.M.M.A. or Anomaly (top left) and show the launcher your Anomaly folder (and Mod
   Organizer for G.A.M.M.A.).
4. **Update the build.** The launcher downloads the latest co-op build and builds a *stand*: a playable copy of your game
   made of hard links, so it takes almost no disk space. Your own Anomaly and G.A.M.M.A. folders are never changed.
5. **Play.** In the main menu open **Co-op**: host a game, or join a friend by IP address.

> **Everyone must run the same build.** Host and clients compare their protocol at the handshake; a different build is
> refused. Press *Update the build* before you play together.

## Playing together

- **Hosting.** Start or load a game and host it from the Co-op window. Friends need to reach your UDP port **27960**: on a
  LAN it just works; over the internet forward the port on your router or use a virtual LAN (Radmin VPN, ZeroTier,
  Tailscale).
- **Joining.** Co-op window → enter the host's address (`1.2.3.4` or `1.2.3.4:27960`). Your character is kept by the host:
  when you come back to the same world, you continue where you left off.
- **Same game data.** For G.A.M.M.A. everyone needs the same G.A.M.M.A. version and mod list: the host checks the content
  of a joining client and tells what differs.

### Console commands

| Command | What it does |
|---|---|
| `coop_host [port]` | Host the current game (default port 27960). |
| `coop_connect <ip>[:port]` | Join a host. |
| `coop_disconnect` | Leave the session. |
| `coop_status` | Print the state of the session, the world sync and the network. |

Command line: `-coop_host [port]`, `-coop_connect <ip>[:port]`, `-coop_nick <name>`.

## Requirements

- Windows 10 or 11, 64-bit; a CPU with AVX.
- S.T.A.L.K.E.R. Anomaly 1.5.3 (G.A.M.M.A. optional).
- The launcher's stand must be on the same drive as the game (hard links do not cross drives).

## Building from source

- Visual Studio 2022 (or Build Tools) with the C++ desktop workload and the Windows SDK.
- Open `src/engine-vs2022.sln`, configuration **DX11-AVX**, platform **x64**, build. Or from a shell:

  ```
  MSBuild src\engine-vs2022.sln -p:Configuration=DX11-AVX -p:Platform=x64 -m
  ```

- The exe lands in `_build/_game/bin_dbg/`; ship it as `AnomalyCoopDX11AVX.exe` next to the co-op gamedata of
  RayLink-Coop.

Where the co-op code lives: `src/xrCoop` (network core, protocol, rules; no game dependencies, covered by unit tests) and
`src/xrGame/coop` (the game side: world sync, players, survival, tasks hooks).

## Troubleshooting

| Problem | What to do |
|---|---|
| Which engine build do I run? | The main menu's version line ends with *RayLink Engine b&lt;N&gt;* (so do the window title and the first line of the log); the console command `version` prints it too. A local build without a number says *RayLink Engine dev*. |
| *Access denied* while the launcher builds the stand | The game (or its second window, or a crashed process) still runs from the stand: close it. |
| Refused at join: different build | Both press *Update the build* in the launcher. |
| Refused at join: content differs | Different G.A.M.M.A. version or mod list: copy the host's setup, then *Build the stand* again. |
| High ping on a LAN | Check `coop_status` and the net graph; a Wi-Fi link or a VPN adds latency. |

## Credits

RayLink Engine is built on [**themrdemonized/xray-monolith**](https://github.com/themrdemonized/xray-monolith), the
Anomaly modded exes, and on the work of everyone who contributed to it; its full history and credits are kept in that
repository. It also carries engine improvements adapted from [OpenXRay](https://github.com/OpenXRay/xray-16) and
[IX-Ray](https://github.com/ixray-team/ixray-1.6-stcop). S.T.A.L.K.E.R. and the X-Ray engine are by GSC Game World;
Anomaly is by the Anomaly team.

## License

The engine source is GSC Game World's, redistributed under the terms in [`License.txt`](License.txt): keep the copyright
notice, **non-commercial use only**. RayLink is a free fan project and is not affiliated with GSC Game World.
