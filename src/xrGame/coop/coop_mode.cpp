// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop — the holder of the session's mode and the switchboard of the two layers (xrCoop/coop_mode.h, docs/MODES.md section 4).
//
// The switchboard runs the layers' boots every frame and asks them which mode a start of the Core is. It decides nothing itself: the
// server layer reads its own keys (-coop_dedicated, -coop_mapserver) and the Welcome's mode, the co-op layer takes the rest. Its list
// of the two layers is the one place where the Core names them (the lead's layer row for this file; tests/test_mode_answers.cpp
// mode_switchboard_names_each_layer_once keeps it to that line).
#include "stdafx.h"
#include "xrCoop/coop_mode.h"

namespace coop
{
namespace srv
{
Layer& layer(); // srv/coop_srv_mode.cpp
} // namespace srv
namespace hostplay
{
Layer& layer(); // hostplay/coop_hostplay_mode.cpp
} // namespace hostplay

namespace
{
ModeSlot& slot()
{
	static ModeSlot s;
	return s;
}

// The server layer first: a start is the server's when its own keys or the Welcome say so, the co-op's otherwise.
struct Layers
{
	Layer* list[2];
};
const Layers& layers()
{
	static const Layers all{{&srv::layer(), &hostplay::layer()}};
	return all;
}
} // namespace

Mode& mode() { return slot().get(); }

bool install_mode(Mode& m)
{
	if (!slot().install(m))
	{
		::Msg("! [coop] mode: '%s' refused, the session's mode is '%s' (one mode per session)", m.name(), slot().get().name());
		return false;
	}
	return true;
}

void reset_mode() { slot().reset(); }

Mode* mode_for(Entry entry, SessionMode said)
{
	for (Layer* layer : layers().list)
		if (Mode* m = layer->mode_of(entry, said))
			return m;
	return nullptr;
}

bool boot_layers()
{
	for (Layer* layer : layers().list)
		if (layer->boot_frame())
			return true; // this process's sessions are that layer's: the boots after it stay out
	return false;
}

void register_layers_console()
{
	for (Layer* layer : layers().list)
		layer->register_console();
}

void register_layers_lua(lua_State* L)
{
	for (Layer* layer : layers().list)
		layer->register_lua(L);
}

bool place_ghost(CALifeObjectRegistry& objects)
{
	for (Layer* layer : layers().list)
		if (layer->place_ghost(objects))
			return true; // that layer's ghost: the Core swaps no role
	return false;
}

void console_line(const char* line)
{
	if (!line)
		return;
	for (Layer* layer : layers().list)
		layer->console_line(line);
}

bool layer_join_request(std::string& address, std::string& via)
{
	for (Layer* layer : layers().list)
		if (layer->join_request(address, via))
			return true;
	return false;
}
} // namespace coop
