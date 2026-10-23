////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.h
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_Abstract;
class CScriptBinderObject;
class NET_Packet;

class CScriptBinder
{
protected:
	CScriptBinderObject* m_object;

private:
	// Anomaly Coop (F2 T11): the script_binding of the section this binder was made from (a string of system.ltx, valid
	// for the whole run; null for a binder Lua attached by hand) and one bit per coop::router::BinderMethod for the
	// methods whose first kept Lua error is in the log already. See coop/coop_router.h.
	LPCSTR m_script_binding;
	u16 m_coop_logged;

public:
	CScriptBinder();
	virtual ~CScriptBinder();
	void init();
	void clear();
	virtual void reinit();
	virtual void Load(LPCSTR section);
	virtual void reload(LPCSTR section);
	virtual BOOL net_Spawn(CSE_Abstract* DC);
	virtual void net_Destroy();
	virtual void shedule_Update(u32 time_delta);
	virtual void save(NET_Packet& output_packet);
	virtual void load(IReader& input_packet);
	virtual BOOL net_SaveRelevant();
	virtual void net_Relcase(CObject* object);
	void set_object(CScriptBinderObject* object);
	IC CScriptBinderObject* object();
};

#include "script_binder_inline.h"
