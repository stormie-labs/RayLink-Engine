////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "script_engine.h"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_game_object.h"
#include "gameobject.h"
#include "level.h"
#include "coop/coop_remote.h" // Anomaly Coop
#include "coop/coop_router.h" // Anomaly Coop

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

namespace
{
// Anomaly Coop (F2 T11): one call of a Lua method of the binder, the way the binder router says (coop_router.h).
//   Done     - the method ran
//   Skipped  - HostOnly: a client does not call it (the C++ default stands)
//   Failed   - KeepOnError: the Lua error is logged and counted, the binder stays (the result of the call is void)
//   Dropped  - the call threw and nothing keeps the binder: the caller clear()s it, as the stock engine does
enum class Call
{
	Done,
	Skipped,
	Failed,
	Dropped
};

template <typename F>
Call call_routed(const char* binding, u16& logged, coop::router::BinderMethod method, F&& call)
{
	const coop::routes::BinderAction action = coop::router::binder_action(binding, method);
	if (action == coop::routes::BinderAction::HostOnly)
		return Call::Skipped;
	try
	{
		// The Release engine has no C++ exceptions: a Lua error ends in a fatal error unless the scope takes it.
		coop::router::BinderKeepScope keep(action == coop::routes::BinderAction::KeepOnError, binding, method, logged);
		call();
		return keep.failed() ? Call::Failed : Call::Done;
	}
	catch (...)
	{
		return coop::router::binder_kept(action, binding, method, logged, "exception") ? Call::Failed : Call::Dropped;
	}
}
} // namespace

CScriptBinder::CScriptBinder()
{
	init();
}

CScriptBinder::~CScriptBinder()
{
	VERIFY(!m_object);
}

void CScriptBinder::init()
{
	m_object = 0;
	m_script_binding = 0; // Anomaly Coop
	m_coop_logged = 0; // Anomaly Coop
}

void CScriptBinder::clear()
{
	try
	{
		xr_delete(m_object);
	}
	catch (...)
	{
		m_object = 0;
	}
	init();
}

void CScriptBinder::reinit()
{
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::Reinit,
				[&] { m_object->reinit(); }) == Call::Dropped)
			clear();
	}
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reinit() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

void CScriptBinder::Load(LPCSTR section)
{
}

void CScriptBinder::reload(LPCSTR section)
{
	PROF_EVENT("CScriptBinder::reload");
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
#ifndef DBG_DISABLE_SCRIPTS
	VERIFY(!m_object);
	if (!pSettings->line_exist(section, "script_binding"))
		return;

	::luabind::functor<void> lua_function;
	if (!ai().script_engine().functor(pSettings->r_string(section, "script_binding"), lua_function))
	{
		ai().script_engine().script_log(ScriptStorage::eLuaMessageTypeError, "function %s is not loaded!",
		                                pSettings->r_string(section, "script_binding"));
		return;
	}

	CGameObject* game_object = smart_cast<CGameObject*>(this);

	m_script_binding = pSettings->r_string(section, "script_binding"); // Anomaly Coop: the key of the binder router (F2 T11)

	try
	{
		lua_function(game_object ? game_object->lua_game_object() : 0);
	}
	catch (...)
	{
		clear();
		return;
	}

	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::Reload,
				[&] { m_object->reload(section); }) == Call::Dropped)
			clear();
	}
#endif
#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::reload() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
	PROF_EVENT("CScriptBinder::net_Spawn");
#ifdef DEBUG_MEMORY_MANAGER
	size_t									start = 0;
	if (g_bMEMO)
		start							= Memory.mem_usage();
#endif // DEBUG_MEMORY_MANAGER
	CSE_Abstract* abstract = (CSE_Abstract*)DC;
	CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
	if (object && m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11); a skipped or failed net_spawn answers what the
		// binder object does without one: true
		BOOL result = TRUE;
		switch (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::NetSpawn,
			[&] { result = (BOOL)m_object->net_Spawn(object); }))
		{
		case Call::Done:
			return result;
		case Call::Dropped:
			clear();
			break;
		default:
			return (TRUE);
		}
	}

#ifdef DEBUG_MEMORY_MANAGER
	if (g_bMEMO) {
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
//		lua_gc				(ai().script_engine().lua(),LUA_GCCOLLECT,0);
		Msg					("CScriptBinder::net_Spawn() : %lld",Memory.mem_usage() - start);
	}
#endif // DEBUG_MEMORY_MANAGER

	return (TRUE);
}

void CScriptBinder::net_Destroy()
{
	PROF_EVENT("CScriptBinder::net_Destroy");
	if (m_object)
	{
#ifdef _DEBUG
		Msg						("* Core object %s is UNbinded from the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		// Anomaly Coop: the call goes through the binder router (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::NetDestroy,
				[&] { m_object->net_Destroy(); }) == Call::Dropped)
			clear();
	}
	xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
	if (IsGameTypeSingle())
	{
		VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef _DEBUG
		Msg					("* Core object %s is binded with the script object",smart_cast<CGameObject*>(this) ? *smart_cast<CGameObject*>(this)->cName() : "");
#endif // _DEBUG
		m_object = object;
	}
	else
	{
		xr_delete(object);
	}
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
	PROF_EVENT("CScriptBinder::shedule_Update");
	if (m_object)
	{
		if (coop::remote::suspend_binder(this)) // Anomaly Coop: no Lua logic for puppets
			return; // Anomaly Coop
		// Anomaly Coop: HostOnly bindings do not update on a client, KeepOnError ones survive a Lua error (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::Update,
				[&] { m_object->shedule_Update(time_delta); }) == Call::Dropped)
			clear();
	}
}

void CScriptBinder::save(NET_Packet& output_packet)
{
	PROF_EVENT("CScriptBinder::save");
	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::Save,
				[&] { m_object->save(&output_packet); }) == Call::Dropped)
			clear();
	}
}

void CScriptBinder::load(IReader& input_packet)
{
	PROF_EVENT("CScriptBinder::load");
	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11)
		if (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::Load,
				[&] { m_object->load(&input_packet); }) == Call::Dropped)
			clear();
	}
}

BOOL CScriptBinder::net_SaveRelevant()
{
	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11); a skipped or failed one answers false
		BOOL result = FALSE;
		switch (call_routed(m_script_binding, m_coop_logged, coop::router::BinderMethod::NetSaveRelevant,
			[&] { result = (BOOL)m_object->net_SaveRelevant(); }))
		{
		case Call::Done:
			return (result);
		case Call::Dropped:
			clear();
			break;
		default:
			break;
		}
	}
	return (FALSE);
}

void CScriptBinder::net_Relcase(CObject* object)
{
	PROF_EVENT("CScriptBinder::net_Relcase");
	CGameObject* game_object = smart_cast<CGameObject*>(object);
	if (m_object && game_object)
	{
		try
		{
			m_object->net_Relcase(game_object->lua_game_object());
		}
		catch (...)
		{
			clear();
		}
	}
}
