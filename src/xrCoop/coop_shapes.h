// Anomaly Coop — distance from a point to the shapes of a space restrictor (spheres and boxes, as CShapeData keeps
// them). The host lets a player vote for a level exit only near the exit's shapes (T12 review, Ruling 18).
// Pure C++17, header-only, no engine dependencies: used by the engine and by the unit-test exe.
#pragma once

#include <cmath>
#include <limits>

namespace coop
{
namespace shapes
{
// "Nowhere near": the answer for anything that is not a number or not a shape.
constexpr float kFar = std::numeric_limits<float>::infinity();

namespace detail
{
inline bool finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
} // namespace detail

// Distance from p to the ball of center c and radius r; 0 inside or on it. kFar if a number is not finite or r < 0.
inline float to_sphere(const float p[3], const float c[3], float r)
{
	if (!detail::finite3(p) || !detail::finite3(c) || !std::isfinite(r) || r < 0.f)
		return kFar;
	const double dx = double(p[0]) - c[0], dy = double(p[1]) - c[1], dz = double(p[2]) - c[2];
	const double d = std::sqrt(dx * dx + dy * dy + dz * dz) - double(r);
	return d > 0. ? float(d) : 0.f;
}

// Distance from p to a box stored as the engine stores one (CShapeData box after the object's transform, the
// matrix CCF_Shape::Contact builds): the image of the unit cube [-0.5, 0.5]^3, with i, j, k the images of its axes
// (the box's full edge vectors) and c the image of its center. The edges are taken as orthogonal (a level editor box
// is rotated and scaled, never sheared); 0 inside or on it. kFar if a number is not finite or an edge is shorter than
// 1 mm (a flat box has no inside to stand in).
inline float to_box(const float p[3], const float i[3], const float j[3], const float k[3], const float c[3])
{
	if (!detail::finite3(p) || !detail::finite3(i) || !detail::finite3(j) || !detail::finite3(k) || !detail::finite3(c))
		return kFar;
	const double d[3] = {double(p[0]) - c[0], double(p[1]) - c[1], double(p[2]) - c[2]};
	const float* edges[3] = {i, j, k};
	double outside = 0.;
	for (const float* e : edges)
	{
		const double len = std::sqrt(double(e[0]) * e[0] + double(e[1]) * e[1] + double(e[2]) * e[2]);
		if (!(len >= 1e-3))
			return kFar;
		const double along = (d[0] * e[0] + d[1] * e[1] + d[2] * e[2]) / len; // signed, from the center
		const double past = std::fabs(along) - len * 0.5;
		if (past > 0.)
			outside += past * past;
	}
	return float(std::sqrt(outside));
}
} // namespace shapes
} // namespace coop
