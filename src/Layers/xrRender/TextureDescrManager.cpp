#include "stdafx.h"
#pragma hdrstop
#include "TextureDescrManager.h"
#include "ETextureParams.h"
#include "profiler.h"

// eye-params
float r__dtex_range = 50;

class cl_dt_scaler : public R_constant_setup
{
public:
	float scale;

	cl_dt_scaler(float s) : scale(s)
	{
	};

	virtual void setup(R_constant* C)
	{
		RCache.set_c(C, scale, scale, scale, 1 / r__dtex_range);
	}
};

void fix_texture_thm_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if (_ext &&
		(0 == stricmp(_ext, ".tga") ||
			0 == stricmp(_ext, ".thm") ||
			0 == stricmp(_ext, ".dds") ||
			0 == stricmp(_ext, ".bmp") ||
			0 == stricmp(_ext, ".ogm") ||
            0 == stricmp(_ext, ".gif")))
		*_ext = 0;
}

struct TH_LoadTHM
{
	using map_TD = xr_map<shared_str, CTextureDescrMngr::texture_desc>;
	using map_CS = xr_map<shared_str, cl_dt_scaler*>;

	LPCSTR initial;
	texture_descr_merge::load_gate* gate;
	map_TD s_texture_details; // O4-thm: this loader's own maps; no other thread touches them before the merge
	map_CS s_detail_scalers;
};

struct TH_FreeTHM // texture_descr_merge::merge_records frees with xr_delete, as LoadTHM does
{
	template <class T>
	static void destroy(T*& p) { xr_delete(p); }
};

void CTextureDescrMngr::LoadTHMThread(void* args)
{
	PROF_EVENT();

	TH_LoadTHM* p = (TH_LoadTHM*)args;
	LoadTHM(p->initial, p->s_texture_details, p->s_detail_scalers);
	p->gate->job_done(); // the last touch of the manager's data: the merge may free *p right after
}

void CTextureDescrMngr::LoadTHM(LPCSTR initial, map_TD& s_texture_details, map_CS& s_detail_scalers)
{
	PROF_EVENT();

	FS_FileSet flist;
	FS.file_list(flist, initial, FS_ListFiles, "*.thm");

	STextureParams tp;
	string_path fn;

	for (const FS_File& fs_iter : flist)
	{
		FS.update_path(fn, initial, fs_iter.name.c_str());
		IReader* F = FS.r_open(fn);
		xr_strcpy(fn, fs_iter.name.c_str());
		fix_texture_thm_name(fn);

		R_ASSERT(F->find_chunk(THM_CHUNK_TYPE));
		F->r_u32();
		tp.Clear();
		tp.Load(*F);
		FS.r_close(F);
		if (STextureParams::ttImage == tp.type || STextureParams::ttTerrain == tp.type || STextureParams::ttNormalMap ==
			tp.type)
		{
			texture_desc& desc = s_texture_details[fn];
			cl_dt_scaler*& dts = s_detail_scalers[fn];

			if (tp.detail_name.size() && tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail)
			)
			{
				if (desc.m_assoc)
					xr_delete(desc.m_assoc);

				desc.m_assoc = xr_new<texture_assoc>();
				desc.m_assoc->detail_name = tp.detail_name;
				if (dts)
					dts->scale = tp.detail_scale;
				else
					/*desc.m_assoc->cs*/dts = xr_new<cl_dt_scaler>(tp.detail_scale);

				desc.m_assoc->usage = 0;

				if (tp.flags.is(STextureParams::flDiffuseDetail))
					desc.m_assoc->usage |= (1 << 0);

				if (tp.flags.is(STextureParams::flBumpDetail))
					desc.m_assoc->usage |= (1 << 1);
			}
			if (desc.m_spec)
				xr_delete(desc.m_spec);

			desc.m_spec = xr_new<texture_spec>();
			desc.m_spec->m_material = tp.material + (tp.material < 4 ? tp.material_weight : 0);
			desc.m_spec->m_use_steep_parallax = false;

			if (tp.bump_mode == STextureParams::tbmUse)
			{
				desc.m_spec->m_bump_name = tp.bump_name;
			}
			else if (tp.bump_mode == STextureParams::tbmUseParallax)
			{
				desc.m_spec->m_bump_name = tp.bump_name;
				desc.m_spec->m_use_steep_parallax = true;
			}
		}
	}
}

void CTextureDescrMngr::Load()
{
	WaitLoaded(); // a Load before the last one's loaders ended: those end and merge first, as two LoadTHM rounds did
	m_thm_loads[0] = new TH_LoadTHM{"$game_textures$", &m_thm_gate};
	m_thm_loads[1] = new TH_LoadTHM{"$level$", &m_thm_gate};
	m_thm_gate.start(2); // before the spawns: a loader may end before thread_spawn returns (scenario "ends-first")
	thread_spawn(LoadTHMThread, "X-Ray THM Loader 1", 0, m_thm_loads[0]);
	thread_spawn(LoadTHMThread, "X-Ray THM Loader 2", 0, m_thm_loads[1]);
	Sleep(5);
}

void CTextureDescrMngr::WaitLoaded() const
{
	if (!m_thm_gate.pending())
		return;
	CTimer waited;
	waited.Start();
	// the manager is never a const object (CResourceManager::m_textures_description); only the lookups are const
	CTextureDescrMngr* self = const_cast<CTextureDescrMngr*>(this);
	u32 records = 0;
	bool merged = false;
	m_thm_gate.settle([self, &records, &merged]() {
		records = self->MergeLoaded();
		merged = true;
	});
	if (merged) // logged by the one caller that merged, outside the gate's lock
		Msg("* THM descriptions: %u records merged into %u textures, waited %u ms", records,
		    u32(m_texture_details.size()), waited.GetElapsed_ms());
}

u32 CTextureDescrMngr::MergeLoaded()
{
	// LoadTHM($game_textures$) then LoadTHM($level$) into one pair of maps was the order before the loaders were
	// threads: $level$ goes over $game_textures$, field by field as LoadTHM writes (texture_descr_merge::merge_records)
	u32 records = 0;
	for (TH_LoadTHM*& load : m_thm_loads)
	{
		if (!load)
			continue;
		records += u32(load->s_texture_details.size());
		texture_descr_merge::merge_records<TH_FreeTHM>(m_texture_details, m_detail_scalers, load->s_texture_details,
		                                               load->s_detail_scalers);
		delete load;
		load = nullptr;
	}
	return records;
}

void CTextureDescrMngr::UnLoad()
{
	WaitLoaded(); // the last Load's loaders end and merge before the maps are freed
	for (auto& it : m_texture_details)
	{
		xr_delete(it.second.m_assoc);
		xr_delete(it.second.m_spec);
	}
	m_texture_details.clear();
}

CTextureDescrMngr::~CTextureDescrMngr()
{
	WaitLoaded(); // no loader may write into this object after it is gone
	map_CS::iterator I = m_detail_scalers.begin();
	map_CS::iterator E = m_detail_scalers.end();

	for (; I != E; ++I)
		xr_delete(I->second);

	m_detail_scalers.clear();
}

shared_str CTextureDescrMngr::GetBumpName(const shared_str& tex_name) const
{
	WaitLoaded();
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_bump_name;
		}
	}
	return "";
}

BOOL CTextureDescrMngr::UseSteepParallax(const shared_str& tex_name) const
{
	WaitLoaded();
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_use_steep_parallax;
		}
	}
	return FALSE;
}

float CTextureDescrMngr::GetMaterial(const shared_str& tex_name) const
{
	WaitLoaded();
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_spec)
		{
			return I->second.m_spec->m_material;
		}
	}
	return 1.0f;
}

void CTextureDescrMngr::GetTextureUsage(const shared_str& tex_name, BOOL& bDiffuse, BOOL& bBump) const
{
	WaitLoaded();
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_assoc)
		{
			u8 usage = I->second.m_assoc->usage;
			bDiffuse = !!(usage & (1 << 0));
			bBump = !!(usage & (1 << 1));
		}
	}
}

BOOL CTextureDescrMngr::GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup* & CS) const
{
	WaitLoaded();
	map_TD::const_iterator I = m_texture_details.find(tex_name);
	if (I != m_texture_details.end())
	{
		if (I->second.m_assoc)
		{
			texture_assoc* TA = I->second.m_assoc;
			res = TA->detail_name.c_str();
			map_CS::const_iterator It2 = m_detail_scalers.find(tex_name);
			CS = It2 == m_detail_scalers.end() ? 0 : It2->second; //TA->cs;
			return TRUE;
		}
	}
	return FALSE;
}
