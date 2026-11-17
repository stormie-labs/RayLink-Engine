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
#include <atomic> // Anomaly Coop
#include <mutex> // Anomaly Coop

// Anomaly Coop: xrCore's stack printer (xrDebugNew.cpp); xrDebug.h declares only a one-argument one that has no definition
void LogStackTrace(LPCSTR header, bool printStack);

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

namespace
{
// Anomaly Coop (F2 T11): one call of a Lua method of the binder, unless the binder router says a client does not make it
// (coop_router.h, [binder_host]).
//   Done     - the method ran
//   Skipped  - HostOnly: a client does not call it (the C++ default stands)
//   Dropped  - the call threw: the caller clear()s the binder, as the stock engine does. In the Release build (no C++
//              exceptions) this catches structured exceptions only, not Lua errors: those are fatal, and a binder that
//              must survive them is wrapped in pcall by scripts\coop_client_binders.script (Ruling F2-35)
enum class Call
{
	Done,
	Skipped,
	Dropped
};

// Anomaly Coop (2026-11-17, RL-K958-GVST): what the dropped call was. The stock engine swallows a native exception inside a
// binder's Lua call and deletes the binder without a word. When it is the actor's, no actor_on_update runs any more: the
// player sees a game that stopped working, and the log held nothing. The exception filter runs before the stack is
// unwound, so the Lua stack and the C++ stack it prints are the place of the exception. It only looks: the call is
// dropped as before. Nothing is printed for a stack overflow (there is no stack to print with).
int dropped_call(const char* binding, coop::router::BinderMethod method, const EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
	if (!r || r->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
		return EXCEPTION_EXECUTE_HANDLER;
	// an exception inside the printing below comes to this filter again, on the same thread, with its lock held
	static thread_local bool s_inside = false;
	if (s_inside)
		return EXCEPTION_EXECUTE_HANDLER;
	struct Inside
	{
		Inside() { s_inside = true; }
		~Inside() { s_inside = false; }
	} inside;
	static std::atomic<u32> s_seen{0};
	const u32 n = s_seen.fetch_add(1);
	if (n >= 16)
		return EXCEPTION_EXECUTE_HANDLER;
	const size_t m = size_t(method);
	LPCSTR name = m < coop::routes::kBinderMethodCount ? coop::routes::kBinderMethodNames[m].data() : "?";
	Msg("! [binder] a native exception 0x%08x at %p inside the Lua method %s of the binder [%s]: the engine drops this binder, its Lua methods are not called any more",
	    u32(r->ExceptionCode), r->ExceptionAddress, name, binding && *binding ? binding : "?");
	if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
		Msg("! [binder] it %s the address %p", r->ExceptionInformation[0] == 1 ? "wrote to" : "read",
		    (void*)r->ExceptionInformation[1]);
	// print_stack() writes nothing in a release build: the lines of the Lua stack go to the log as lua_error_not_crash's do
	for (auto const& line : get_lua_stack(ai().script_engine().lua()))
		Msg("!   %s", line.c_str());
	if (n < 3)
	{
		static std::mutex s_stack_lock; // DbgHelp behind the stack walker is single-threaded
		std::lock_guard<std::mutex> lock(s_stack_lock);
		LogStackTrace("! [binder] C++ stack of that exception:", true);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

template <typename F>
Call call_routed(const char* binding, coop::router::BinderMethod method, F&& call)
{
	if (coop::router::binder_action(binding, method) == coop::routes::BinderAction::HostOnly)
		return Call::Skipped;
	__try
	{
		call();
		return Call::Done;
	}
	__except (dropped_call(binding, method, GetExceptionInformation()))
	{
		return Call::Dropped;
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
	m_script_binding = shared_str(); // Anomaly Coop
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
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::Reinit,
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
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::Reload,
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
		// Anomaly Coop: the call goes through the binder router (F2 T11); a skipped net_spawn answers what the binder
		// object does without one: true
		BOOL result = TRUE;
		switch (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::NetSpawn,
			[&] { result = (BOOL)m_object->net_Spawn(object); }))
		{
		case Call::Done:
			return result;
		case Call::Dropped:
			clear();
			break;
		case Call::Skipped:
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
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::NetDestroy,
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
		// Anomaly Coop: host-only bindings do not update on a client (F2 T11)
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::Update,
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
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::Save,
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
		if (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::Load,
				[&] { m_object->load(&input_packet); }) == Call::Dropped)
			clear();
	}
}

BOOL CScriptBinder::net_SaveRelevant()
{
	if (m_object)
	{
		// Anomaly Coop: the call goes through the binder router (F2 T11); a skipped one answers false
		BOOL result = FALSE;
		switch (call_routed(m_script_binding.c_str(), coop::router::BinderMethod::NetSaveRelevant,
			[&] { result = (BOOL)m_object->net_SaveRelevant(); }))
		{
		case Call::Done:
			return (result);
		case Call::Dropped:
			clear();
			break;
		case Call::Skipped:
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
