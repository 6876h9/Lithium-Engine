#pragma once

#include <functional>
#include <string>

// Headless assertions for the material graph code generator.
//
// These live beside the generator rather than in selftest.cpp because they assert on
// the exact text it emits, and a change to the emitter should fail in the same commit
// that made it. selftest.cpp calls this with its own check(), so a failure counts
// toward the suite total like any other.
//
// Nothing here needs a GL context: the generator only produces a string. Whether that
// string compiles is checked by the material graph panel at edit time, against the
// real driver.
namespace MaterialGraphSelfTest {

void run(const std::function<void(bool, const std::string&)>& check);

} // namespace MaterialGraphSelfTest
