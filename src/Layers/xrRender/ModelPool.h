// ModelPool.h: interface for the CModelPool class.
//////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>

// refs
class dxRender_Visual;

namespace PS
{
	struct SEmitter;
};

// defs
class ECORE_API CModelPool
{
private:
	friend class CRender;

	struct str_pred
	{
		IC bool operator()(const shared_str& x, const shared_str& y) const
		{
			return xr_strcmp(x, y) < 0;
		}
	};

	struct ModelDef
	{
		shared_str name;
		dxRender_Visual* model;
		u32 refs;

		ModelDef()
		{
			refs = 0;
			model = 0;
		}

		// Order by pointer
		bool operator<(const ModelDef& other) const { return model < other.model; }
		bool operator<(const dxRender_Visual* other) const { return model < other; }
		friend bool operator<(dxRender_Visual* ptr, const ModelDef& other) { return ptr < other.model; }
	};

	typedef xr_multimap<shared_str, dxRender_Visual*, str_pred> POOL;
	typedef POOL::iterator POOL_IT;
	typedef xr_map<dxRender_Visual*, shared_str> REGISTRY;
	typedef REGISTRY::iterator REGISTRY_IT;
private:
	xr_vector<ModelDef> Models; // Reference / Base
	xr_vector<dxRender_Visual*> ModelsToDelete; // 
    xr_vector<dxRender_Visual*> ModelsToDeleteDeffer; // 
	REGISTRY Registry; // Just pairing of pointer / Name
	POOL Pool; // Unused / Inactive
	BOOL bLogging;
	// Anomaly Coop: bForceDiscard and bAllowChildrenDuplicate were members shared by every thread; they are per-thread
	// now (thread_local in ModelPool.cpp): they mean "this thread is deleting a base" / "this thread is loading a base".
	xrCriticalSection deffered_del_lock;
	xrSRWLock ModelsLock;
	// Anomaly Coop: PoolLock guards Registry, Pool and ModelsToDelete (ModelsToDeleteDeffer stays under
	// deffered_del_lock, Models under ModelsLock). The spawn-prefetch thread and the main thread both create and delete
	// models; the unlocked maps were corrupted (0xC0000374 at quit). It is held only around container operations, never
	// across model loading, Spawn, shader resets or deletes. Lock order: deffered_del_lock -> ModelsLock -> PoolLock
	// (PoolLock is the innermost: nothing else of the pool is acquired while it is held). See temp\modelpool\DESIGN.md.
	xrCriticalSection PoolLock;
	std::atomic<u32> PoolOwner{ 0 };                    // thread id holding PoolLock (diagnostic only)
	std::atomic<const char*> PoolOwnerWhere{ nullptr }; // the entry point it holds it in (diagnostic only)
	u32 PoolDepth = 0;                                  // recursion depth of the owner; touched only under PoolLock
	class PoolGuard;
	friend class PoolGuard;

	void Destroy();
public:
	CModelPool();
	virtual ~CModelPool();
	dxRender_Visual* Instance_Create(u32 Type);
	dxRender_Visual* Instance_Duplicate(dxRender_Visual* V);
	dxRender_Visual* Instance_Load(LPCSTR N, BOOL allow_register, bool assert = true);
	dxRender_Visual* Instance_Load(LPCSTR N, IReader* data, BOOL allow_register);
	dxRender_Visual* Instance_Register(LPCSTR N, dxRender_Visual* V);
	dxRender_Visual* Instance_Find(LPCSTR N);

	dxRender_Visual* CreatePE(PS::CPEDef* source);
	dxRender_Visual* CreatePG(PS::CPGDef* source);
	dxRender_Visual* Create(LPCSTR name, IReader* data = 0, bool assert = true);
	dxRender_Visual* CreateChild(LPCSTR name, IReader* data);
	void DeleteDeffered(dxRender_Visual* &V);
	void Delete(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void Discard(dxRender_Visual* & V, BOOL b_complete);
	void DeleteInternal(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void DeleteQueue();
	void DeleteQueuedDeffer();

	void Logging(BOOL bEnable) { bLogging = bEnable; }

	void Prefetch();
	void Prefetch_One(LPCSTR N, bool assert = true);
	bool Exists(LPCSTR N);
	void ClearPool(BOOL b_complete);

	void dump();

	void memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system);
#ifdef _EDITOR
	void					OnDeviceDestroy		();
	void 					Render				(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD);
	void 					RenderSingle		(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD);
#endif
};
