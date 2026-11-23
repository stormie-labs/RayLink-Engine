// Anomaly Coop (package O4-thm; reports #128, #63, #801). CTextureDescrMngr::Load (TextureDescrManager.cpp) reads the
// THM descriptions on two loader threads, $game_textures$ and $level$. They used to write into one pair of xr_map with no
// lock while the main thread already read it (undefined behaviour: a broken tree hangs or crashes at startup). Now each
// loader fills its own maps, and the first reader waits for both and merges them. The two pure parts live here, so
// tests run them without the engine:
//  - merge_records: moves one loader's records into the shared maps exactly as LoadTHM would have written them there;
//  - load_gate: the first reader waits for the loaders and runs the merge once; later readers return at once.
// Core: every process with a renderer loads THMs the same way, whatever its role.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace texture_descr_merge
{
// Moves the records one loader made (from_td / from_cs: LoadTHM into empty maps) into the shared maps td / cs, leaving
// td / cs exactly as LoadTHM would have left them had it written those THMs into td / cs directly:
//  - every record's name is in both shared maps (LoadTHM makes s_detail_scalers[fn] even for a THM without a detail);
//  - m_spec: the record's replaces the older one (LoadTHM rewrites it for every THM it keeps);
//  - m_assoc: replaced only by a record with a detail, otherwise the older one stays;
//  - the scaler of a record with a detail: an older scaler object keeps its address and takes the new scale (shaders
//    hold that pointer, and UnLoad keeps the scalers), else the record's own scaler moves in.
// Merging the $game_textures$ loader and then the $level$ loader therefore gives what LoadTHM($game_textures$) followed
// by LoadTHM($level$) gave before the loaders were threads: $level$ over $game_textures$. from_td / from_cs end empty.
// Free::destroy(T*&) frees one object and nulls the pointer (xr_delete in the engine); a null pointer is left alone.
template <class Free, class MapTD, class MapCS>
void merge_records(MapTD& td, MapCS& cs, MapTD& from_td, MapCS& from_cs)
{
	for (auto& rec : from_td)
	{
		auto& desc = td[rec.first];
		auto& dts = cs[rec.first];
		auto& src = rec.second;
		if (src.m_assoc)
		{
			Free::destroy(desc.m_assoc);
			desc.m_assoc = src.m_assoc;
			src.m_assoc = nullptr;
			const auto own = from_cs.find(rec.first);
			if (own != from_cs.end() && own->second)
			{
				if (dts)
				{
					dts->scale = own->second->scale;
					Free::destroy(own->second);
				}
				else
				{
					dts = own->second;
					own->second = nullptr;
				}
			}
		}
		Free::destroy(desc.m_spec);
		desc.m_spec = src.m_spec;
		src.m_spec = nullptr;
	}
	for (auto& rest : from_cs)
		Free::destroy(rest.second); // a scaler with no detail record (LoadTHM never leaves one): freed, not leaked
	from_td.clear();
	from_cs.clear();
}

// The loaders of one Load. start(n): the owner, before it spawns them. job_done(): each loader, as the very last thing it
// does with the owner's data. settle(merge): every reader (and Load, UnLoad, the destructor) waits until all loaders
// called job_done; the first such caller runs merge() once, under the gate's lock, so readers on other threads wait for
// it and then see its result; later callers return at once.
class load_gate
{
public:
	void start(unsigned jobs)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_running = jobs;
		m_pending.store(true, std::memory_order_release);
	}

	void job_done()
	{
		std::lock_guard<std::mutex> lock(m_lock);
		--m_running;
		m_idle.notify_all();
	}

	bool pending() const { return m_pending.load(std::memory_order_acquire); }

	template <class Merge>
	void settle(Merge&& merge)
	{
		if (!pending())
			return;
		std::unique_lock<std::mutex> lock(m_lock);
		m_idle.wait(lock, [this] { return m_running == 0; });
		if (m_pending.load(std::memory_order_relaxed))
		{
			merge();
			m_pending.store(false, std::memory_order_release);
		}
	}

private:
	std::mutex m_lock;
	std::condition_variable m_idle;
	unsigned m_running = 0;
	std::atomic<bool> m_pending{false};
};
} // namespace texture_descr_merge
