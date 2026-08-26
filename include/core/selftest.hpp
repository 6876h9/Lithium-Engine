#pragma once

class Engine;

// Headless exercise of the subsystems a plain launch never reaches: the lightmap
// bake, the navmesh build, joints, particle simulation, the UI tree and terrain
// sculpting. All of those run from a button or from Play mode, so a crash in one
// stays hidden until the day someone presses it.
//
// Run with --selftest. Returns the number of failed checks, which the process uses
// as its exit code so a build script can act on it.
int run_selftest(Engine& engine);
