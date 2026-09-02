// Lithium Engine - a custom C++20 game engine.
// Copyright (c) 2026 Muhammad Burair Abbas
//
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.

#include "core/engine.hpp"
#include "core/selftest.hpp"
#include "core/platform.hpp"
#include "core/asset_database.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <string.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
#include <string>
using json = nlohmann::json;

Engine* g_engine = nullptr;

// Every asset path in the engine ("Content/...", "EngineContent/...") is relative to
// the working directory, which means launching from anywhere other than the install
// folder finds nothing. Rather than rewrite hundreds of call sites, anchor the working
// directory to the executable itself at startup.
//
// Any file argument the user passed is resolved to an absolute path *first*, so a
// relative path on the command line still means what the user's shell meant by it.
static void anchor_working_directory_to_executable(int argc, char* argv[],
                                                   std::vector<std::string>& out_args) {
    out_args.clear();
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (i > 0 && !a.empty() && a[0] != '-' && std::filesystem::exists(a)) {
            std::error_code ec;
            std::filesystem::path abs = std::filesystem::absolute(a, ec);
            if (!ec) a = abs.string();
        }
        out_args.push_back(a);
    }

    std::filesystem::path dir = Platform::executable_dir();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
}

int main(int argc, char* argv[]) {
    // Mesa-only override that lets an older Intel driver expose OpenGL 4.5.
    // Meaningless on Windows, where the vendor driver reports its real version, and
    // setenv() is not part of the MSVC/MinGW CRT.
#if LITHIUM_PLATFORM_LINUX
    setenv("MESA_GL_VERSION_OVERRIDE", "4.5", 1);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "450", 1);
#endif

    // Mints the .meta sidecars for a content tree and exits.
    //
    // Handled before the working directory is anchored to the executable, because
    // this is aimed at a source tree the caller names rather than at whatever sits
    // beside the binary. GUIDs have to be authored next to the source assets and
    // committed: if they were only ever minted into a build output, every clean
    // build would issue new ones and every scene reference would break - which is
    // the exact failure the sidecars exist to prevent.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--index-assets") != 0) continue;

        std::vector<std::string> roots;
        for (int j = i + 1; j < argc; ++j) {
            if (argv[j][0] == '-') break;
            roots.push_back(argv[j]);
        }
        if (roots.empty()) roots = { "Content", "EngineContent" };

        AssetDatabase::get().scan(roots);
        std::cout << "Indexed " << AssetDatabase::get().asset_count() << " assets." << std::endl;
        return 0;
    }

    std::vector<std::string> anchored_args;
    anchor_working_directory_to_executable(argc, argv, anchored_args);
    std::vector<char*> argv_storage;
    for (auto& a : anchored_args) argv_storage.push_back(const_cast<char*>(a.c_str()));
    argv = argv_storage.data();
    
    std::ofstream log_file("LithiumEngine_Startup.log");
    std::streambuf* old_cerr = std::cerr.rdbuf(log_file.rdbuf());
    std::streambuf* old_cout = std::cout.rdbuf(log_file.rdbuf());
    
    Engine engine;
    g_engine = &engine;

    std::string initial_scene = "";

    // This renderer targets the OpenGL 4.5 core profile and has no second backend.
    // The config's legacy "graphics_api" key is read only to warn when it asks for
    // something that does not exist, rather than being silently ignored.
    std::string config_path = "engine_config.json";
    if (std::filesystem::exists(config_path)) {
        try {
            std::ifstream config_file(config_path);
            json config_json;
            config_file >> config_json;
            if (config_json.contains("graphics_api")) {
                std::string api_str = config_json["graphics_api"];
                if (api_str != "opengl") {
                    std::cerr << "engine_config.json requests graphics_api \"" << api_str
                              << "\", which this build does not provide. Using OpenGL."
                              << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse engine config: " << e.what() << std::endl;
        }
    }

    float explicit_screenshot_delay = -1.0f;
    bool run_selftest_requested = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--vulkan") == 0) {
            std::cerr << "--vulkan: this build has no Vulkan backend. Using OpenGL."
                      << std::endl;
        } else if (strcmp(argv[i], "--opengl") == 0) {
            // Accepted for compatibility with existing launch scripts; already the
            // only backend.
        } else if (std::string(argv[i]).find(".lithium") != std::string::npos) {
            initial_scene = argv[i];
        } else if (strncmp(argv[i], "--auto-screenshot=", 18) == 0) {
            // Automated/CI screenshot capture: after a short warm-up delay, grab a
            // frame straight from the GL backbuffer (via Engine::take_screenshot,
            // the same path F9 uses) and exit. Useful for headless verification
            // where there's no interactive session to press F9 in.
            engine.auto_screenshot_path = std::string(argv[i]).substr(18); // text after "--auto-screenshot="
            // Default warm-up: let async resource loads and TAA history settle.
            // Only applied if --screenshot-delay hasn't already set one, so the two
            // flags can be given in either order.
            if (engine.auto_screenshot_delay < 0.0f) engine.auto_screenshot_delay = 12.0f;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            run_selftest_requested = true;
        } else if (strcmp(argv[i], "--auto-launch") == 0) {
            engine.auto_launch = true;
        } else if (strncmp(argv[i], "--screenshot-delay=", 19) == 0) {
            // Overrides the default warm-up delay, so early states (the main menu
            // and launcher) can be captured too rather than only the settled editor.
            explicit_screenshot_delay = static_cast<float>(std::atof(std::string(argv[i]).substr(19).c_str()));
        }
    }

    if (explicit_screenshot_delay >= 0.0f) engine.auto_screenshot_delay = explicit_screenshot_delay;

    if (!std::filesystem::exists(config_path)) {
        json config_json;
        config_json["graphics_api"] = "opengl";
        std::ofstream out_file(config_path);
        out_file << config_json.dump(4);
    }

    bool is_standalone = false;
#ifdef STANDALONE_GAME
    is_standalone = true;
#endif

    if (!engine.initialize(initial_scene, is_standalone)) {
        std::cerr << "Failed to initialize engine." << std::endl;
        return -1;
    }

    if (run_selftest_requested) {
        // The subsystems under test need the renderer and physics up, which
        // initialize() leaves until a project is actually opened.
        if (!engine.initialize_runtime()) {
            std::cerr << "Self-test could not bring up the runtime." << std::endl;
            return -1;
        }
        const int failures = run_selftest(engine);
        engine.shutdown();
        std::cerr.rdbuf(old_cerr);
        std::cout.rdbuf(old_cout);
        return failures;
    }

    std::cout << "Engine initialized successfully. Starting launcher..." << std::endl;
    engine.run();

    std::cout << "Engine shutting down." << std::endl;
    
    std::cerr.rdbuf(old_cerr);
    std::cout.rdbuf(old_cout);
    return 0;
}
