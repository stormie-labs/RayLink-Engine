// RayLink Engine build number, read by raylink_version.h. 0 = a development build ("RayLink Engine dev").
// Keep 0 in the source tree. A release build gets its number in the build tree only: the anomaly-coop project runs
// `python tools/ci_sync.py --build N` before MSBuild (it rewrites this file in engine-ci), and tools/package.ps1 then
// checks that the exe says "RayLink Engine bN" for package version 0.N.0.
#pragma once

#define RAYLINK_BUILD 0
