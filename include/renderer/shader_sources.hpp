#pragma once

#include <string>

// Shader source the engine owns and shares between passes.
//
// The geometry vertex stage is here rather than inline in the renderer because a
// user-authored surface shader has to use exactly the same one. That is what makes a
// custom material skin, cast shadows and work under large-world coordinates without
// its author writing a line of vertex code - and duplicating it would mean a change
// to skinning silently breaking every custom shader.
namespace ShaderSources {

const std::string& geometry_vertex();

} // namespace ShaderSources
