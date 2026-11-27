#ifndef	dx10StateCache_included
#define	dx10StateCache_included
#pragma once

#include "dx10StateRecords.h"

// coop: the log line of a D3D state the device did not create (kind: "sampler", "rasterizer", ...; code: its HRESULT, with
// the renderer's note when the device was lost). Counted, the first eight and then one in a thousand
// (dx10_state_records::log_failure).
void dx10_state_create_failed(LPCSTR kind, long code);
inline LPCSTR dx10_state_kind(const ID3DRasterizerState*) { return "rasterizer"; }
inline LPCSTR dx10_state_kind(const ID3DDepthStencilState*) { return "depth-stencil"; }
inline LPCSTR dx10_state_kind(const ID3DBlendState*) { return "blend"; }

template <class IDeviceState, class StateDecs>
class dx10StateCache
{
	//	Public interface
public:
	dx10StateCache();
	~dx10StateCache();

	void ClearStateArray();

	IDeviceState* GetState(SimulatorStates& state_code);
	IDeviceState* GetState(StateDecs& desc);
	//	Can be called on device destruction only!
	//	dx10State holds weak links on manager's states and 
	//	won't understand that state was destroyed
	//void	FlushStates();
	//	Private functionality

	//	Private declarations
private:
	struct StateRecord
	{
		u32 m_crc;
		IDeviceState* m_pState;
	};

private:
	HRESULT CreateState(StateDecs desc, IDeviceState** ppIState);
	IDeviceState* FindState(const StateDecs& desc, u32 StateCRC);

	//	Private data
private:
	//	This must be cleared on device destroy
	xr_vector<StateRecord> m_StateArray;
};

extern dx10StateCache<ID3DRasterizerState, D3D_RASTERIZER_DESC> RSManager;
extern dx10StateCache<ID3DDepthStencilState, D3D_DEPTH_STENCIL_DESC> DSSManager;
extern dx10StateCache<ID3DBlendState, D3D_BLEND_DESC> BSManager;

#include "dx10StateCacheImpl.h"

#endif	//	dx10StateCache_included
