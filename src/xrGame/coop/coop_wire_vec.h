// Anomaly Coop: an engine vector on the wire, as the three floats of Writer::vec3 / Reader::vec3 (xrCoop/coop_wire.h).
#pragma once

#include "xrCoop/coop_wire.h"

namespace coop
{
inline void put_vec(Writer& w, const Fvector& v)
{
	const float f[3] = {v.x, v.y, v.z};
	w.vec3(f);
}

// The vector read, as Reader::f32 reads each part (the caller checks the reader).
inline Fvector get_vec(Reader& r)
{
	float f[3] = {0.f, 0.f, 0.f};
	r.vec3(f);
	return Fvector().set(f[0], f[1], f[2]);
}
} // namespace coop
