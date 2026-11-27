// The records of the D3D state caches (samplers, rasterizer, depth-stencil, blend) and what a failed create leaves in them.
// A create call that fails (ID3D11Device::CreateSamplerState and its kin answer an error code) leaves a null
// object, and the caches check nothing in release. A record with that null state was kept, and the next shader whose state
// had the same hash met it first and called GetDesc through the null pointer: an access violation, a read of the address 0,
// in dx10SamplerStateCache::FindState. So a record holds a live state or does not exist: a failed create keeps no record
// (the next shader with that state asks the device again, the slot stays unbound until then), and the search passes over a
// record whose state a re-creation (SetMaxAnisotropy, SetMipLODBias) lost.
//
// Header-only, no D3D, no windows.h, no engine calls: the unit tests include this file as it is
// (anomaly-coop tests/test_dx10_state_records.cpp). A record is any type with `m_crc` and `m_pState`, whose state has
// GetDesc(Desc*); records is a vector of them.
#pragma once

#include <stddef.h>

namespace dx10_state_records
{
constexpr size_t not_found = static_cast<size_t>(-1);

// The index of the record with this hash whose live state has the description desc (equal(candidate, desc)), else
// not_found. A record without a state never matches and is never asked.
template <class Records, class Desc, class Equal>
size_t find(const Records& records, const Desc& desc, unsigned int crc, Equal&& equal)
{
	for (size_t i = 0; i < records.size(); ++i)
	{
		const auto& record = records[i];
		if (record.m_crc != crc || record.m_pState == nullptr)
			continue;
		Desc candidate;
		record.m_pState->GetDesc(&candidate);
		if (equal(candidate, desc))
			return i;
	}
	return not_found;
}

struct Added
{
	size_t index = not_found; // the new record's index, not_found when the create failed
	long code = 0; // what create answered (an HRESULT)
};

// create(&state) makes the state and answers an HRESULT. Only a created state (a code that is not a failure and an object)
// gets a record, at the end of records.
template <class Records, class Create>
Added add(Records& records, unsigned int crc, Create&& create)
{
	typename Records::value_type record{};
	record.m_crc = crc;
	record.m_pState = nullptr;
	Added result;
	result.code = create(&record.m_pState);
	if (result.code < 0 || record.m_pState == nullptr)
		return result;
	records.push_back(record);
	result.index = records.size() - 1;
	return result;
}

// Whether the failure with this count (1 for the first) gets its log line: the first eight, then one in a thousand, so a
// device that refuses every state does not flood the log and the count still shows how many failed.
constexpr bool log_failure(unsigned int count)
{
	return count != 0 && (count <= 8 || count % 1000 == 0);
}
} // namespace dx10_state_records
