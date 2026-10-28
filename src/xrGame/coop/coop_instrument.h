#pragma once
#include "xrCoop/coop_instrument.h"
class CActor;
class CGameObject;
class IDSGraphManager;
struct lua_State;
namespace coop { namespace players {
int lua_instrument(lua_State* L);
instrument::State capture_instrument();
bool instrument_pose(CGameObject* obj, const instrument::State& state);
bool own_instrument_held(const CActor* actor);
void instrument_frame();
void instrument_reset();
void instrument_forget(u16 id);
void instrument_render(CGameObject* obj, IDSGraphManager* DM);
} }
