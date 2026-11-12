#include "stdafx.h"
#pragma hdrstop

#include "ModelPool.h"

#ifndef _EDITOR
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/fmesh.h"
#include "fhierrarhyvisual.h"
#include "SkeletonAnimated.h"
#include "fvisual.h"
#include "fprogressive.h"
#include "fskinned.h"
#include "flod.h"
#include "ftreevisual.h"
#include "ParticleGroup.h"
#include "ParticleEffect.h"
#else
    #include "fmesh.h"
    #include "fvisual.h"
    #include "fprogressive.h"
    #include "ParticleEffect.h"
    #include "ParticleGroup.h"
	#include "fskinned.h"
    #include "fhierrarhyvisual.h"
    #include "SkeletonAnimated.h"
	#include "IGame_Persistent.h"
#endif

#include <new>

// Anomaly Coop: the pool's containers are shared by the main thread, the spawn-prefetch thread (Level.cpp,
// ProcessPrefetchEvents -> models_PrefetchOne) and the prefetch task of IGame_Persistent::Prefetch (models_Prefetch).
// See ModelPool.h (PoolLock) and temp\modelpool\DESIGN.md.
namespace
{
// Were CModelPool members written by whichever thread was loading or deleting a base: one thread's
// "loading a base" (no child duplication) or "deleting a base" (discard children) leaked into the other thread's work.
thread_local BOOL t_bForceDiscard = FALSE;
thread_local BOOL t_bAllowChildrenDuplicate = TRUE;

// Contention detector: a line per distinct pair of entry points (waiter, holder), at most kMaxContentionLines.
constexpr u32 kMaxContentionLines = 20;
struct ContentionPair
{
	const char* waiter;
	const char* holder;
};
ContentionPair g_contention_pairs[kMaxContentionLines]; // written only under PoolLock
u32 g_contention_pair_count = 0;                         // written only under PoolLock
std::atomic<u32> g_contentions{ 0 };                     // every contended entry
std::atomic<u32> g_contentions_reported{ 0 };            // value of g_contentions at the last summary line

// A damaged std::map / multimap (size says non-empty, the tree is empty) must not be erased or cleared: its nodes may
// be freed already. Forget it instead: a fresh empty container is built over it, the old head node leaks.
template <typename Container>
void abandon_damaged(Container& c)
{
	::new (static_cast<void*>(&c)) Container();
}
} // namespace

class CModelPool::PoolGuard : xray::noncopyable
{
public:
	PoolGuard(CModelPool& pool, const char* where) : m_pool(pool)
	{
		if (!m_pool.PoolLock.TryEnter()) // a recursive section: fails only while another thread holds it
		{
			const u32 holder = m_pool.PoolOwner.load(std::memory_order_relaxed);
			const char* holder_where = m_pool.PoolOwnerWhere.load(std::memory_order_relaxed);
			m_pool.PoolLock.Enter();
			note_contention(where, holder, holder_where);
		}
		if (m_pool.PoolDepth++ == 0)
		{
			m_pool.PoolOwner.store(GetCurrentThreadId(), std::memory_order_relaxed);
			m_pool.PoolOwnerWhere.store(where, std::memory_order_relaxed);
		}
	}
	~PoolGuard()
	{
		if (--m_pool.PoolDepth == 0)
		{
			m_pool.PoolOwner.store(0, std::memory_order_relaxed);
			m_pool.PoolOwnerWhere.store(nullptr, std::memory_order_relaxed);
		}
		m_pool.PoolLock.Leave();
		if (m_report_where) // logged after the lock is released: the log never runs under PoolLock
			Msg("[coop] ModelPool: thread %u waited for thread %u in %s (holder in %s)", GetCurrentThreadId(),
				m_report_holder, m_report_where, m_report_holder_where ? m_report_holder_where : "?");
	}

private:
	void note_contention(const char* where, u32 holder, const char* holder_where) // under PoolLock
	{
		g_contentions.fetch_add(1, std::memory_order_relaxed);
		const char* hw = holder_where ? holder_where : "?";
		for (u32 i = 0; i < g_contention_pair_count; ++i)
			if (g_contention_pairs[i].waiter == where && g_contention_pairs[i].holder == hw)
				return;
		if (g_contention_pair_count >= kMaxContentionLines)
			return;
		g_contention_pairs[g_contention_pair_count++] = { where, hw };
		m_report_where = where;
		m_report_holder = holder;
		m_report_holder_where = holder_where;
	}

	CModelPool& m_pool;
	const char* m_report_where = nullptr;
	const char* m_report_holder_where = nullptr;
	u32 m_report_holder = 0;
};

dxRender_Visual* CModelPool::Instance_Create(u32 type)
{
	dxRender_Visual* V = NULL;

	// Check types
	switch (type)
	{
	case MT_NORMAL: // our base visual
		V = xr_new<Fvisual>();
		break;
	case MT_HIERRARHY:
		V = xr_new<FHierrarhyVisual>();
		break;
	case MT_PROGRESSIVE: // dynamic-resolution visual
		V = xr_new<FProgressive>();
		break;
	case MT_SKELETON_ANIM:
		V = xr_new<CKinematicsAnimated>();
		break;
	case MT_SKELETON_RIGID:
		V = xr_new<CKinematics>();
		break;
	case MT_SKELETON_GEOMDEF_PM:
		V = xr_new<CSkeletonX_PM>();
		break;
	case MT_SKELETON_GEOMDEF_ST:
		V = xr_new<CSkeletonX_ST>();
		break;
	case MT_PARTICLE_EFFECT:
		V = xr_new<PS::CParticleEffect>();
		break;
	case MT_PARTICLE_GROUP:
		V = xr_new<PS::CParticleGroup>();
		break;
#ifndef _EDITOR
	case MT_LOD:
		V = xr_new<FLOD>();
		break;
	case MT_TREE_ST:
		V = xr_new<FTreeVisual_ST>();
		break;
	case MT_TREE_PM:
		V = xr_new<FTreeVisual_PM>();
		break;
#endif
	default:
		FATAL("Unknown visual type");
		break;
	}
	R_ASSERT(V);
	V->Type = type;
	return V;
}

dxRender_Visual* CModelPool::Instance_Duplicate(dxRender_Visual* V)
{
	R_ASSERT(V);
	dxRender_Visual* N = Instance_Create(V->Type);
	N->Copy(V);
	N->Spawn();
	{
		// inc ref counter. Anomaly Coop: exclusive, it writes refs (a shared lock let two threads lose an increment)
		xrSRWLockGuard g(ModelsLock);
		auto it = std::lower_bound(Models.begin(), Models.end(), V, std::less<>{});
		if (it != Models.end() && it->model == V)
		{
			it->refs++;
		}
		return N;
	}
}

dxRender_Visual* CModelPool::Instance_Load(const char* N, BOOL allow_register, bool assert)
{
	dxRender_Visual* V;
	string_path fn;
	string_path name;

	// Add default ext if no ext at all
	if (0 == strext(N)) strconcat(sizeof(name), name, N, ".ogf");
	else xr_strcpy(name, sizeof(name), N);

	// Load data from MESHES or LEVEL
	if (!FS.exist(N))
	{
		if (!FS.exist(fn, "$level$", name))
			if (!FS.exist(fn, "$game_meshes$", name))
			{
#ifdef _EDITOR
				Msg("!Can't find model file '%s'.",name);
                return 0;
#else
				if (assert)	
					Debug.fatal(DEBUG_INFO, "Can't find model file '%s'.", name);
				else
					return nullptr;
#endif
			}
	}
	else
	{
		xr_strcpy(fn, N);
	}

	// Actual loading
#ifdef DEBUG
	if (bLogging)		Msg		("- Uncached model loading: %s",fn);
#endif // DEBUG

	IReader* data = FS.r_open(fn);
	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(N, data, 0);
	FS.r_close(data);
	g_pGamePersistent->RegisterModel(V);

	// Registration
	if (allow_register) 
		V = Instance_Register(N, V);

	return V;
}

dxRender_Visual* CModelPool::Instance_Load(LPCSTR name, IReader* data, BOOL allow_register)
{
	dxRender_Visual* V;

	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(name, data, 0);

	// Registration
	if (allow_register) 
		V = Instance_Register(name, V);
	return V;
}

dxRender_Visual* CModelPool::Instance_Register(LPCSTR N, dxRender_Visual* V)
{
	// Registration
	shared_str S(N);
	xrSRWLockGuard g(ModelsLock);

	// Double-check for duplicate model
	for (auto& M : Models)
	{
		if (M.name == S)
		{
			// Increment the reference count
			M.refs++;

			// Destroy the redundant one we just loaded
			V->Release();
			xr_delete(V);

			// Return the existing model
			return M.model;
		}
	}

	ModelDef M;
	M.name = S;
	M.model = V;

	auto it = std::lower_bound(Models.begin(), Models.end(), V, std::less<>{});
	Models.insert(it, std::move(M));

	return V;
}


void CModelPool::Destroy()
{
	// Pool
	{
		PoolGuard g(*this, "Destroy");
		if (!Pool.empty() && Pool.begin() == Pool.end())
		{
			Msg("! ModelPool: Pool size %u but the tree is empty: the pool was damaged, it is dropped", u32(Pool.size()));
			abandon_damaged(Pool);
		}
		Pool.clear();
	}

	// Registry. Anomaly Coop: each entry is read under PoolLock, the visual is deleted outside it (Discard erases the
	// entry under the lock). A damaged tree (size says non-empty but the tree is empty, or the entry just deleted is
	// still first) ends as a log line instead of erasing end() and freeing the head node twice (0xC0000374 at quit).
	dxRender_Visual* last = nullptr;
	for (;;)
	{
		dxRender_Visual* V;
		{
			PoolGuard g(*this, "Destroy");
			if (Registry.empty())
				break;
			REGISTRY_IT it = Registry.begin();
			if (it == Registry.end() || (last && it->first == last))
			{
				Msg("! ModelPool: Registry size %u but the tree is %s: the registry was damaged, it is dropped",
					u32(Registry.size()), it == Registry.end() ? "empty" : "stuck");
				abandon_damaged(Registry);
				break;
			}
			V = (dxRender_Visual*)it->first;
			if (!V)
			{
				Registry.erase(it);
				continue;
			}
		}
#ifdef _DEBUG
		Msg				("ModelPool: Destroy object: '%s'",*V->dbg_name);
#endif
		last = V;
		DeleteInternal(V,TRUE);
	}

	// Base/Reference
	{
		xrSRWLockGuard g(ModelsLock);
		for (auto& M : Models)
		{
			M.model->Release();
			xr_delete(M.model);
		}
		Models.clear();
	}

	// cleanup motions container
	g_pMotionsContainer->clean(false);
}

CModelPool::CModelPool()
{
	bLogging = TRUE;
	g_pMotionsContainer = xr_new<motions_container>();
}

CModelPool::~CModelPool()
{
	Destroy();
	xr_delete(g_pMotionsContainer);
}

dxRender_Visual* CModelPool::Instance_Find(LPCSTR N)
{
	shared_str S(N);
	dxRender_Visual* Model = 0;
	xrSRWLockGuard g(ModelsLock, true);

	for (auto& M : Models)
	{
		if (S == M.name)
		{
			Model = M.model;
			break;
		}
	}
	return Model;
}

dxRender_Visual* CModelPool::Create(const char* name, IReader* data, bool assert)
{
#ifdef _EDITOR
	if (!name||!name[0])	return 0;
#endif
	string_path low_name;
	VERIFY(xr_strlen(name)<sizeof(low_name));
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;
	const shared_str key(low_name); // made before PoolLock: the string table has its own lock

	// 0. Search POOL. Anomaly Coop: the instance leaves the pool in the same locked section that finds it, so no other
	// thread can take it too; Spawn runs on the instance this thread now owns, outside the lock.
	dxRender_Visual* Pooled = nullptr;
	{
		PoolGuard g(*this, "Create");
		POOL_IT it = Pool.find(key);
		if (it != Pool.end())
		{
			Pooled = it->second;
			Pool.erase(it);
		}
	}
	if (Pooled)
	{
		// 1. Instance found
		Pooled->Spawn();
		return Pooled;
	}
	else
	{
		// 1. Search for already loaded model (reference, base model)
		dxRender_Visual* Base = Instance_Find(low_name);

		if (0 == Base)
		{
			// 2. If not found (loading runs outside every pool lock)
			t_bAllowChildrenDuplicate = FALSE;
			if (data) Base = Instance_Load(low_name, data, TRUE);
			else Base = Instance_Load(low_name, TRUE, assert);
			if (!Base)
			{
				// If not found and assert is false, return nullptr
				return nullptr;
			}
			t_bAllowChildrenDuplicate = TRUE;
#ifdef _EDITOR
			if (!Base)		return 0;
#endif
		}
		// 3. If found - return (cloned) reference
		dxRender_Visual* Model = Instance_Duplicate(Base);
		{
			PoolGuard g(*this, "Create");
			Registry.insert(mk_pair(Model, key));
		}
		return Model;
	}
}

dxRender_Visual* CModelPool::CreateChild(LPCSTR name, IReader* data)
{
	string256 low_name;
	VERIFY(xr_strlen(name)<256);
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	// 1. Search for already loaded model
	dxRender_Visual* Base = Instance_Find(low_name);
	//.	if (0==Base) Base	 	= Instance_Load(name,data,FALSE);
	if (0 == Base)
	{
		if (data) Base = Instance_Load(low_name, data,FALSE);
		else Base = Instance_Load(low_name,FALSE);
	}

	dxRender_Visual* Model = t_bAllowChildrenDuplicate ? Instance_Duplicate(Base) : Base;
	return Model;
}

extern  xr_atomic_bool ENGINE_API g_bRendering; 

void CModelPool::DeleteInternal(dxRender_Visual* & V, BOOL bDiscard)
{
	VERIFY(!g_bRendering);
	if (!V) return;
	V->Depart();
	if (bDiscard || t_bForceDiscard)
	{
		Discard(V, TRUE);
	}
	else
	{
		//
		bool registered;
		{
			PoolGuard g(*this, "DeleteInternal");
			registered = Registry.find(V) != Registry.end();
		}
		if (registered)
		{
			// Registry entry found - move it to pool and reset changed shader/texture if necessary
			// (the reset may create shaders and textures: outside the lock, on the visual this thread owns)
			xr_vector<IRenderVisual*>* children = V->get_children();
			if (children)
				for (auto* child : *children)
					child->ResetShaderTexture();
			else
				V->ResetShaderTexture();

			// Anomaly Coop: the entry is looked up again in the section that pools the visual: find and insert are
			// one locked section, no iterator outlives a lock.
			bool pooled = false;
			{
				PoolGuard g(*this, "DeleteInternal");
				REGISTRY_IT it = Registry.find(V);
				if (it != Registry.end())
				{
					Pool.insert(mk_pair(it->second, V));
					pooled = true;
				}
			}
			if (!pooled) // only a concurrent Destroy can do that; leaking beats a second delete
				Msg("! ModelPool: a visual lost its registry entry while it was being pooled: it is leaked");
		}
		else
		{
			// Registry entry not-found - just special type of visual / particles / etc.
			xr_delete(V);
		}
	}
	V = NULL;
}

void CModelPool::DeleteDeffered(dxRender_Visual* &V)
{
	if (nullptr==V)
		return;

	xrCriticalSectionGuard guard(&deffered_del_lock);

    if (std::find(ModelsToDeleteDeffer.begin(), ModelsToDeleteDeffer.end(), V) == ModelsToDeleteDeffer.end())
	    ModelsToDeleteDeffer.push_back(V);
	V = nullptr;
}

void CModelPool::Delete(dxRender_Visual* &V, BOOL bDiscard)
{
	if (nullptr==V)
		return;
	if (g_bRendering)
	{
		VERIFY(!bDiscard);
		PoolGuard g(*this, "Delete");
		ModelsToDelete.push_back(V);
	}
	else
	{
		DeleteInternal(V,bDiscard);
	}
	V =	nullptr;
}

void CModelPool::DeleteQueue()
{
	// Anomaly Coop: the queue is taken under PoolLock and deleted outside it; what is queued meanwhile (another
	// thread's Delete while rendering, or this loop's own children) is taken by the next round, in the same order.
	xr_vector<dxRender_Visual*> batch;
	for (;;)
	{
		{
			PoolGuard g(*this, "DeleteQueue");
			batch.swap(ModelsToDelete);
		}
		if (batch.empty())
			break;
		for (u32 it = 0; it < batch.size(); it++)
			DeleteInternal(batch[it]);
		batch.clear();
	}
}

void CModelPool::DeleteQueuedDeffer()
{
	xrCriticalSectionGuard guard(&deffered_del_lock);

    for (dxRender_Visual* Vis : ModelsToDeleteDeffer)
    {
        if (Vis)
            DeleteInternal(Vis);
    }	

	ModelsToDeleteDeffer.clear();
}

void CModelPool::Discard(dxRender_Visual* & V, BOOL b_complete)
{
	// Anomaly Coop: the registry entry is found and erased in one locked section, before the visual is freed (an
	// address freed first could be registered again by another thread before the erase). ModelsLock is taken after
	// PoolLock is released (lock order: ModelsLock -> PoolLock, never the reverse).
	shared_str name;
	bool registered = false;
	{
		PoolGuard g(*this, "Discard");
		REGISTRY_IT it = Registry.find(V);
		if (it != Registry.end())
		{
			name = it->second;
			Registry.erase(it);
			registered = true;
		}
	}
	if (registered)
	{
		// Base
		xrSRWLockGuard g(ModelsLock);

		for (u32 i = 0; i < Models.size(); i++)
		{
			auto I = Models.begin() + i;
			if (I->name == name)
			{
				if (b_complete || strchr(*name, '#'))
				{
					VERIFY(I->refs>0);
					I->refs--;
					if (0 == I->refs)
					{
						t_bForceDiscard = TRUE;
						I->model->Release();
						xr_delete(I->model);
						Models.erase(I);
						t_bForceDiscard = FALSE;
					}
					break;
				}
				else
				{
					if (I->refs > 0)
						I->refs--;
					break;
				}
			}
		}
		// Registry (the entry was erased above)
		xr_delete(V);
	}
	else
	{
		// Registry entry not-found - just special type of visual / particles / etc.
		xr_delete(V);
	}
	V = NULL;
}

void CModelPool::Prefetch()
{
	Logging(FALSE);
	// prefetch visuals
	string256 section;
	strconcat(sizeof(section), section, "prefetch_visuals_", g_pGamePersistent->m_game_params.m_game_type);
	CInifile::Sect& sect = pSettings->r_section(section);
	for (CInifile::SectCIt I = sect.Data.begin(); I != sect.Data.end(); I++)
	{
		const CInifile::Item& item = *I;
		dxRender_Visual* V = Create(item.first.c_str());
		Delete(V,FALSE);
	}
	Logging(TRUE);
}

void CModelPool::Prefetch_One(LPCSTR N, bool assert)
{
	dxRender_Visual* V = Create(N, 0, assert);
	if (V)
		Delete(V,FALSE);
}

bool CModelPool::Exists(LPCSTR N)
{
	string_path low_name;
	VERIFY(xr_strlen(N) < sizeof(low_name));
	xr_strcpy(low_name, N);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	// Search pool and return early if exists
	{
		const shared_str key(low_name);
		PoolGuard g(*this, "Exists");
		if (Pool.find(key) != Pool.end())
			return true;
	}

	// Search for already loaded model (reference, base model) and return early if exists
	dxRender_Visual* Base = Instance_Find(low_name);
	if (Base)
		return true;

	// Prefetch model
	dxRender_Visual* V = Create(N, 0, false);
	if (V) 
	{
		Delete(V, FALSE);
		return true;
	}

	return false;
}

dxRender_Visual* CModelPool::CreatePE(PS::CPEDef* source)
{
	PS::CParticleEffect* V = (PS::CParticleEffect*)Instance_Create(MT_PARTICLE_EFFECT);
	V->Compile(source);
	return V;
}

dxRender_Visual* CModelPool::CreatePG(PS::CPGDef* source)
{
	PS::CParticleGroup* V = (PS::CParticleGroup*)Instance_Create(MT_PARTICLE_GROUP);
	V->Compile(source);
	return V;
}

void CModelPool::ClearPool(BOOL b_complete)
{
	// Anomaly Coop: the pool is taken whole under PoolLock and discarded outside it (Discard frees visuals and takes
	// ModelsLock). Instances pooled by another thread meanwhile stay pooled for the next clear.
	POOL taken;
	{
		PoolGuard g(*this, "ClearPool");
		if (!Pool.empty() && Pool.begin() == Pool.end())
		{
			Msg("! ModelPool: Pool size %u but the tree is empty: the pool was damaged, it is dropped", u32(Pool.size()));
			abandon_damaged(Pool);
		}
		taken.swap(Pool);
	}
	POOL_IT _I = taken.begin();
	POOL_IT _E = taken.end();
	for (; _I != _E; _I++)
	{
		Discard(_I->second, b_complete);
	}
	taken.clear();

	const u32 contentions = g_contentions.load(std::memory_order_relaxed);
	if (contentions != g_contentions_reported.exchange(contentions, std::memory_order_relaxed))
		Msg("[coop] ModelPool: %u contended entries between threads so far (serialised by PoolLock)", contentions);
}

void CModelPool::dump()
{
	Log("--- model pool --- begin:");
	u32 sz = 0;
	u32 k = 0;
	xrSRWLockGuard g(ModelsLock, true);
	for (xr_vector<ModelDef>::iterator I = Models.begin(); I != Models.end(); I++)
	{
		CKinematics* K = PCKinematics(I->model);
		if (K)
		{
			u32 cur = K->mem_usage(false);
			sz += cur;
			Msg("#%3d: [%3d/%5d Kb] - %s", k++, I->refs, cur / 1024, I->name.c_str());
		}
	}
	Msg("--- models: %d, mem usage: %d Kb ", k, sz / 1024);
	sz = 0;
	k = 0;
	int free_cnt = 0;
	PoolGuard pg(*this, "dump"); // after ModelsLock: lock order ModelsLock -> PoolLock
	for (REGISTRY_IT it = Registry.begin(); it != Registry.end(); it++)
	{
		CKinematics* K = PCKinematics((dxRender_Visual*)it->first);
		VERIFY(K);
		if (K)
		{
			u32 cur = K->mem_usage(true);
			sz += cur;
			bool b_free = (Pool.find(it->second) != Pool.end());
			if (b_free) ++free_cnt;
			Msg("#%3d: [%s] [%5d Kb] - %s", k++, (b_free) ? "free" : "used", cur / 1024, it->second.c_str());
		}
	}
	Msg("--- instances: %d, free %d, mem usage: %d Kb ", k, free_cnt, sz / 1024);
	Log("--- model pool --- end.");
}

void CModelPool::memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system)
{
	vb_mem_video = 0;
	vb_mem_system = 0;
	ib_mem_video = 0;
	ib_mem_system = 0;

	xrSRWLockGuard g(ModelsLock, true); // Anomaly Coop: before the iterators are taken, not after
	xr_vector<ModelDef>::iterator it = Models.begin();
	xr_vector<ModelDef>::const_iterator en = Models.end();

	for (; it != en; ++it)
	{
		dxRender_Visual* ptr = it->model;
		Fvisual* vis_ptr = fast_dynamic_cast<Fvisual*>(ptr);

		if (vis_ptr == NULL)
			continue;
#if !defined(USE_DX10) && !defined(USE_DX11)
		D3DINDEXBUFFER_DESC IB_desc;
		D3DVERTEXBUFFER_DESC VB_desc;

		vis_ptr->m_fast->p_rm_Indices->GetDesc(&IB_desc);

		if (IB_desc.Pool == D3DPOOL_DEFAULT ||
			IB_desc.Pool == D3DPOOL_MANAGED)
			ib_mem_video += IB_desc.Size;

		if (IB_desc.Pool == D3DPOOL_MANAGED ||
			IB_desc.Pool == D3DPOOL_SCRATCH)
			ib_mem_system += IB_desc.Size;

		vis_ptr->m_fast->p_rm_Vertices->GetDesc(&VB_desc);

		if (VB_desc.Pool == D3DPOOL_DEFAULT ||
			VB_desc.Pool == D3DPOOL_MANAGED)
			vb_mem_video += IB_desc.Size;

		if (VB_desc.Pool == D3DPOOL_MANAGED ||
			VB_desc.Pool == D3DPOOL_SCRATCH)
			vb_mem_system += IB_desc.Size;

#else
		D3D_BUFFER_DESC IB_desc;
		D3D_BUFFER_DESC VB_desc;

		vis_ptr->m_fast->p_rm_Indices->GetDesc(&IB_desc);

		ib_mem_video += IB_desc.ByteWidth;
		ib_mem_system += IB_desc.ByteWidth;

		vis_ptr->m_fast->p_rm_Vertices->GetDesc(&VB_desc);

		vb_mem_video += IB_desc.ByteWidth;
		vb_mem_system += IB_desc.ByteWidth;

#endif
	}
}

#ifdef _EDITOR
IC bool	_IsBoxVisible(dxRender_Visual* visual, const Fmatrix& transform)
{
    Fbox 		bb; 
    bb.xform	(visual->vis.box,transform);
    return 		::Render->occ_visible(bb);
}
IC bool	_IsValidShader(dxRender_Visual* visual, u32 priority, bool strictB2F)
{
	if (visual->shader)
        return (priority==visual->shader->E[0]->flags.iPriority)&&(strictB2F==visual->shader->E[0]->flags.bStrictB2F);
    return false;
}

void 	CModelPool::Render(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD)
{
    // render visual
    xr_vector<dxRender_Visual*>::iterator I,E;
    switch (m_pVisual->Type){
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            CKinematics* pV		= fast_dynamic_cast<CKinematics*>(m_pVisual); VERIFY(pV);
            if (fis_zero(m_fLOD,EPS)&&pV->m_lod){
		        if (_IsValidShader(pV->m_lod,priority,strictB2F)){
	                RCache.set_Shader		(pV->m_lod->shader?pV->m_lod->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        pV->m_lod->Render		(1.f);
                }
            }else{
                I = pV->children.begin		();
                E = pV->children.end		();
                for (; I!=E; I++){
                    if (_IsValidShader(*I,priority,strictB2F)){
                        RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
                        RCache.set_xform_world	(mTransform);
                        (*I)->Render		 	(m_fLOD);
                    }
                }
            }
        }
    }break;
    case MT_HIERRARHY:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            FHierrarhyVisual* pV		= fast_dynamic_cast<FHierrarhyVisual*>(m_pVisual); VERIFY(pV);
            I = pV->children.begin		();
            E = pV->children.end		();
            for (; I!=E; I++){
		        if (_IsValidShader(*I,priority,strictB2F)){
	                RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        (*I)->Render		 	(m_fLOD);
                }
            }
        }
    }break;
    case MT_PARTICLE_GROUP:{
        PS::CParticleGroup* pG			= fast_dynamic_cast<PS::CParticleGroup*>(m_pVisual); VERIFY(pG);
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            RCache.set_xform_world	  		(mTransform);
            for (PS::CParticleGroup::SItemVecIt i_it=pG->items.begin(); i_it!=pG->items.end(); i_it++){
                xr_vector<dxRender_Visual*>	visuals;
                i_it->GetVisuals			(visuals);
                for (xr_vector<dxRender_Visual*>::iterator it=visuals.begin(); it!=visuals.end(); it++)
                    Render					(*it,Fidentity,priority,strictB2F,m_fLOD);
            }
        }
    }break;
    case MT_PARTICLE_EFFECT:{
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
    }break;
    default:
        if (_IsBoxVisible(m_pVisual,mTransform)){
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
        break;
    }
}

void 	CModelPool::RenderSingle(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD)
{
	for (int p=0; p<4; p++){
    	Render(m_pVisual,mTransform,p,false,m_fLOD);
    	Render(m_pVisual,mTransform,p,true,m_fLOD);
    }
}
void CModelPool::OnDeviceDestroy()
{
	Destroy();
}
#endif
