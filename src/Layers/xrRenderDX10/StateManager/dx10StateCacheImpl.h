#ifndef	dx10StateCacheImpl_included
#define	dx10StateCacheImpl_included
#pragma once

#include "../dx10StateUtils.h"

using dx10StateUtils::operator==;

template <class IDeviceState, class StateDecs>
IDeviceState*
dx10StateCache<IDeviceState, StateDecs>
::GetState(SimulatorStates& state_code)
{
	StateDecs desc;
	dx10StateUtils::ResetDescription(desc);
	state_code.UpdateDesc(desc);

	return GetState(desc);

	/*
	u32 crc = crc32( &desc, sizeof(desc));

	pResult = FindState( desc, crc);

	if (!pResult)
	{
		StateRecord rec;
		rec.m_crc = crc;
		CreateState(desc, &rec.m_pState);
		pResult = rec.m_pState;
		m_StateArray.push_back(rec);
	}

	return pResult;
	*/
}

template <class IDeviceState, class StateDecs>
IDeviceState*
dx10StateCache<IDeviceState, StateDecs>
::GetState(StateDecs& desc)
{
	IDeviceState* pResult;

	dx10StateUtils::ValidateState(desc);

	u32 crc = dx10StateUtils::GetHash(desc);

	pResult = FindState(desc, crc);

	if (!pResult)
	{
		// coop: a failed create keeps no record (dx10StateRecords.h): the caller gets null as before, the next one asks again
		const dx10_state_records::Added added =
			dx10_state_records::add(m_StateArray, crc, [&](IDeviceState** state) { return CreateState(desc, state); });
		if (added.index == dx10_state_records::not_found)
			dx10_state_create_failed(dx10_state_kind(static_cast<const IDeviceState*>(nullptr)), added.code);
		else
			pResult = m_StateArray[added.index].m_pState;
	}

	return pResult;
}

template <class IDeviceState, class StateDecs>
IDeviceState*
dx10StateCache<IDeviceState, StateDecs>
::FindState(const StateDecs& desc, u32 StateCRC)
{
	const size_t index = dx10_state_records::find(m_StateArray, desc, StateCRC,
		[](const StateDecs& a, const StateDecs& b) { return a == b; });
	if (index != dx10_state_records::not_found)
		return m_StateArray[index].m_pState;
	else
		return NULL;
	/*
	if (i!=m_StateArray.size())
		return m_StateArray[i].m_pState;
	else
		return NULL;
	    */
}

#endif	//	dx10StateCacheImpl_included
