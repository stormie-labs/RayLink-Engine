#ifndef _TextureDescrManager_included_
#define _TextureDescrManager_included_

#pragma once
#include "ETextureParams.h"
#include "texture_descr_merge.h"

class cl_dt_scaler;
struct TH_LoadTHM;

class CTextureDescrMngr
{
	struct texture_assoc
	{
		shared_str detail_name;
		//R_constant_setup*	cs;
		u8 usage;

		texture_assoc() : /*cs(NULL),*/ usage(0)
		{
		}

		~texture_assoc()
		{
			/*xr_delete(cs);*/
		}
	};

	struct texture_spec
	{
		shared_str m_bump_name;
		float m_material;
		bool m_use_steep_parallax;
	};

public:
	static struct texture_desc
	{
		texture_assoc* m_assoc;
		texture_spec* m_spec;

		texture_desc(): m_assoc(NULL), m_spec(NULL)
		{
		}
	};

	DEFINE_MAP(shared_str, texture_desc, map_TD, map_TDIt);
	DEFINE_MAP(shared_str, cl_dt_scaler*, map_CS, map_CSIt);

private:
	map_TD m_texture_details;
	map_CS m_detail_scalers;

	// O4-thm: the loaders of the last Load ($game_textures$, $level$), each with its own maps, until WaitLoaded merges them
	TH_LoadTHM* m_thm_loads[2] = {nullptr, nullptr};
	mutable texture_descr_merge::load_gate m_thm_gate;

	static void LoadTHM(LPCSTR initial, map_TD& s_texture_details, map_CS& s_detail_scalers);
	static void LoadTHMThread(void* args);
	void WaitLoaded() const; // every lookup, Load, UnLoad and the destructor: the loaders end and are merged first
	u32 MergeLoaded(); // under the gate's lock: both loaders ended; returns the records merged

public:
	~CTextureDescrMngr();
	void Load();
	void UnLoad();
public:
	shared_str GetBumpName(const shared_str& tex_name) const;
	float GetMaterial(const shared_str& tex_name) const;
	void GetTextureUsage(const shared_str& tex_name, BOOL& bDiffuse, BOOL& bBump) const;
	BOOL GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup* & CS) const;
	BOOL UseSteepParallax(const shared_str& tex_name) const;
};
#endif
