#include "core/selftest.hpp"
#include "core/editor.hpp"
#include "core/external_editor.hpp"
#include "core/platform.hpp"

#include "core/engine.hpp"
#include "core/asset_database.hpp"
#include "core/scene_serializer.hpp"
#include "world/actor.hpp"
#include "world/editor_primitive_actor.hpp"
#include "world/static_mesh_component.hpp"
#include "world/physics_attribute.hpp"
#include "world/joint_component.hpp"
#include "world/nav_agent_component.hpp"
#include "world/particle_emitter_component.hpp"
#include "world/cpp_script_component.hpp"
#include "world/ui_canvas_component.hpp"
#include "world/terrain_component.hpp"
#include "world/animation_player.hpp"
#include "world/skeleton.hpp"
#include "physics/physics_engine.hpp"
#include "navigation/navmesh.hpp"
#include "renderer/lightmapper.hpp"
#include "renderer/gl_loader.hpp"
#include "scripting/cminus_interpreter.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

// Headless exercise of the subsystems that only run when a person clicks something.
//
// Rendering can be checked by launching the editor and looking at it. Baking a
// lightmap, building a navmesh, driving a joint or stepping a particle emitter
// cannot - those paths are reached from a button or from Play mode, so a plain
// launch never touches them and a crash in one stays hidden until the day someone
// presses the button.
//
// This is not a correctness suite. It asserts the few invariants that are cheap and
// unambiguous, and otherwise exists to make every one of those paths actually
// execute at least once.

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) {
        std::cout << "  [ ok ] " << what << std::endl;
    } else {
        ++g_failures;
        std::cout << "  [FAIL] " << what << std::endl;
    }
}

void section(const char* name) {
    std::cout << "\n--- " << name << " ---" << std::endl;
}

// A closed box of six thin cubes with a floor, which is enough geometry for a
// navmesh to find, a lightmap to bake and a physics body to land on.
void build_test_room(std::vector<std::shared_ptr<Actor>>& actors) {
    struct Piece { const char* name; Vector3 position; Vector3 scale; };
    const Piece pieces[] = {
        { "TestFloor",     { 0.0f, 0.0f, 0.0f },  { 12.0f, 0.5f, 12.0f } },
        { "TestWallNorth", { 0.0f, 2.0f, -6.0f }, { 12.0f, 4.0f, 0.5f } },
        { "TestWallSouth", { 0.0f, 2.0f, 6.0f },  { 12.0f, 4.0f, 0.5f } },
        { "TestWallEast",  { 6.0f, 2.0f, 0.0f },  { 0.5f, 4.0f, 12.0f } },
        { "TestWallWest",  { -6.0f, 2.0f, 0.0f }, { 0.5f, 4.0f, 12.0f } },
    };

    for (const Piece& piece : pieces) {
        auto actor = std::make_shared<EditorPrimitiveActor>(piece.name, "Cube");
        Transform& transform = actor->get_actor_transform();
        transform.position = { piece.position.x, piece.position.y, piece.position.z };
        transform.scale = piece.scale;
        // Static so the lightmapper takes it as both receiver and occluder.
        actor->is_static = true;

        auto* physics = actor->create_component<PhysicsAttribute>("Physics");
        physics->mass = 0.0f; // static collider
        physics->box_half_extents = { 0.5f, 0.5f, 0.5f };
        actors.push_back(actor);
    }
}

} // namespace

int run_selftest(Engine& engine) {
    g_checks = 0;
    g_failures = 0;

    std::cout << "\n========== Lithium self-test ==========" << std::endl;

    std::vector<std::shared_ptr<Actor>>& actors = engine.get_actors();
    actors.clear();
    build_test_room(actors);
    check(actors.size() == 5, "test room built");

    // --- Scene references survive a rename ----------------------------------
    // The end-to-end version of the asset-identity claim, through a real .lithium
    // file rather than the database API. This is the failure the sidecars exist to
    // prevent, so it is asserted on the actual save/load path a project uses.
    section("Scene survives an asset rename");
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        const fs::path root = fs::temp_directory_path(ec) / "lithium_scene_rename_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        const fs::path original = root / "Crate.mesh";
        const fs::path renamed  = root / "Crate_Final.mesh";
        { std::ofstream f(original); f << "placeholder"; }

        AssetDatabase& db = AssetDatabase::get();
        db.scan({ root.string() });
        const std::string guid = db.guid_for_path(original.string());
        check(!guid.empty(), "the referenced asset has an identity");

        // A scene with one actor pointing at that mesh.
        std::vector<std::shared_ptr<Actor>> scene_actors;
        {
            auto a = std::make_shared<Actor>("Crate");
            a->shape_type = "StaticMesh";
            a->mesh_path = AssetDatabase::normalize_path(original.string());
            scene_actors.push_back(a);
        }

        const fs::path scene_file = root / "level.lithium";
        SceneSerializer::save_scene(scene_file.string(), scene_actors);
        check(fs::exists(scene_file, ec), "scene saved");

        // The saved file must carry both halves.
        {
            std::ifstream in(scene_file);
            nlohmann::json scene_json;
            in >> scene_json;
            bool found_guid = false;
            for (const auto& actor_json : scene_json["actors"]) {
                if (actor_json.contains("mesh_guid") &&
                    actor_json["mesh_guid"] == guid) { found_guid = true; break; }
            }
            check(found_guid, "the saved scene stores the mesh GUID alongside its path");
        }

        // Rename the asset, sidecar and all, exactly as a person reorganising a
        // content folder would.
        fs::rename(original, renamed, ec);
        fs::rename(fs::path(original.string() + ".meta"),
                   fs::path(renamed.string() + ".meta"), ec);
        check(!ec, "asset and its sidecar renamed");
        check(!fs::exists(original, ec), "the old path is genuinely gone");
        db.scan({ root.string() });

        std::vector<std::shared_ptr<Actor>> loaded;
        SceneSerializer::load_scene(scene_file.string(), loaded);
        check(loaded.size() == 1, "scene reloaded");
        if (loaded.size() == 1) {
            const std::string expected = AssetDatabase::normalize_path(renamed.string());
            check(loaded[0]->mesh_path == expected,
                  "the actor now points at the renamed asset");
            // Before the sidecars this is exactly what happened instead.
            check(loaded[0]->mesh_path != AssetDatabase::normalize_path(original.string()),
                  "and not at the path that no longer exists");
        }

        fs::remove_all(root, ec);
    }

    // --- Native C++ scripting ----------------------------------------------
    // An exported game must not need a compiler on the player's machine. That
    // depends on two things holding: a script can be built to a module ahead of
    // time, and the runtime prefers that module over invoking the compiler. Both
    // are asserted here, because a break in either only shows up on someone else's
    // machine where there is no toolchain to fall back on.
    section("Native C++ scripting");
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        const fs::path root = fs::temp_directory_path(ec) / "lithium_script_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        const fs::path source = root / "SelfTestScript.cpp";
        {
            std::ofstream f(source);
            f << "#include \"world/actor.hpp\"\n"
              << "extern \"C\" {\n"
              << "void on_begin_play(Actor* self) { (void)self; }\n"
              << "void on_tick(Actor* self, float dt) {\n"
              << "    self->get_actor_transform().rotation.y += dt;\n"
              << "}\n"
              << "}\n";
        }

        const std::string module_name = CppScriptComponent::module_name_for(source.string());
        check(module_name.find("SelfTestScript") == 0, "module name derives from the script name");

        const fs::path module_path = root / module_name;
        std::string log;
        const bool built = CppScriptComponent::compile_script(source.string(), module_path.string(), log);

        if (!built && log.find("No C++ compiler") != std::string::npos) {
            // No toolchain on this machine. That is exactly the situation an
            // exported game is in, and it is not a failure of the engine.
            std::cout << "  [skip] no C++ compiler present; ahead-of-time build not exercised"
                      << std::endl;
        } else {
            check(built, "a script compiles ahead of time to a loadable module");
            if (!built) std::cout << log << std::endl;
            check(fs::exists(module_path, ec), "the module lands where export would put it");

            // The runtime looks for modules under kModuleDir relative to the working
            // directory, which is what an exported game's layout provides.
            const fs::path shipped_dir = fs::path(CppScriptComponent::kModuleDir);
            fs::create_directories(shipped_dir, ec);
            const fs::path shipped = shipped_dir / module_name;
            fs::copy_file(module_path, shipped, fs::copy_options::overwrite_existing, ec);
            check(!ec, "module staged into the shipped Scripts directory");

            // Point a component at a source path that does NOT exist, so the only way
            // it can come up is via the precompiled module. That is precisely the
            // exported-game case: the .cpp is not shipped, only the module is.
            const fs::path absent_source = root / "SelfTestScript.cpp";
            fs::remove(absent_source, ec);

            Actor probe("ScriptProbe");
            auto* script = probe.create_component<CppScriptComponent>("Script", absent_source.string());
            check(script != nullptr, "script component constructed");
            if (script) {
                check(!script->has_error,
                      "a shipped module loads with no source and no compiler invocation");

                const float before = probe.get_actor_transform().rotation.y;
                script->tick(0.5f);
                check(probe.get_actor_transform().rotation.y > before,
                      "its on_tick actually runs and mutates the actor");
            }

            fs::remove(shipped, ec);
            fs::remove_all(shipped_dir, ec);
        }

        fs::remove_all(root, ec);
    }

    // --- Asset identity ----------------------------------------------------
    // The whole point of the GUID sidecars is that a reference survives the file
    // being renamed. That is the case that used to break scenes silently, so it is
    // the case asserted here - against real files on disk, not a mocked map.
    section("Asset database");
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        const fs::path root = fs::temp_directory_path(ec) / "lithium_assetdb_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        const fs::path original = root / "Prop.mesh";
        const fs::path renamed  = root / "Prop_Renamed.mesh";
        { std::ofstream f(original); f << "not a real mesh"; }

        AssetDatabase& db = AssetDatabase::get();
        db.scan({ root.string() });

        const std::string guid = db.guid_for_path(original.string());
        check(guid.size() == 32, "an asset is assigned a 32-character GUID");
        check(fs::exists(original.string() + ".meta", ec), "a .meta sidecar is written beside it");

        // A scene saves both halves; this is what that JSON looks like.
        nlohmann::json ref;
        db.write_ref(ref, "mesh_path", original.string());
        check(ref.contains("mesh_guid"), "write_ref stores the GUID alongside the path");

        // Now do the thing that used to lose the reference.
        fs::rename(original, renamed, ec);
        check(!ec, "asset renamed on disk");
        fs::rename(fs::path(original.string() + ".meta"),
                   fs::path(renamed.string() + ".meta"), ec);
        db.scan({ root.string() });

        check(db.path_for_guid(guid) == AssetDatabase::normalize_path(renamed.string()),
              "the GUID now resolves to the new path");

        const std::string resolved = db.read_ref(ref, "mesh_path");
        check(resolved == AssetDatabase::normalize_path(renamed.string()),
              "a scene reference written before the rename still resolves");
        check(resolved != original.string(), "and no longer points at the old path");

        // A scene written before any of this existed has a path and no GUID. It has
        // to keep loading exactly as it did.
        nlohmann::json legacy;
        legacy["mesh_path"] = "Content/Legacy.mesh";
        check(db.read_ref(legacy, "mesh_path") == "Content/Legacy.mesh",
              "a GUID-less legacy reference falls back to its path");

        // An unknown GUID must not resolve to something arbitrary.
        nlohmann::json dangling;
        dangling["mesh_path"] = "Content/Gone.mesh";
        dangling["mesh_guid"] = std::string(32, 'a');
        check(db.read_ref(dangling, "mesh_path") == "Content/Gone.mesh",
              "an unresolvable GUID falls back rather than inventing a path");

        fs::remove_all(root, ec);
    }

    // --- Collision layers --------------------------------------------------
    section("Collision layers");
    {
        PhysicsEngine::reset_layers();
        check(PhysicsEngine::layers_should_collide(0, 1), "layers collide by default");

        PhysicsEngine::set_layers_collide(1, 2, false);
        check(!PhysicsEngine::layers_should_collide(1, 2), "a disabled pair stops colliding");
        check(!PhysicsEngine::layers_should_collide(2, 1), "and the matrix stays symmetric");
        check(PhysicsEngine::layers_should_collide(1, 3), "other pairs are unaffected");

        // The packed encoding is the part most likely to be got wrong by a later
        // change, and it silently breaks all collision filtering when it is.
        const uint16_t moving = PhysicsEngine::make_object_layer(5, true);
        const uint16_t stationary = PhysicsEngine::make_object_layer(5, false);
        check(PhysicsEngine::gameplay_layer_of(moving) == 5, "object layer decodes its gameplay layer");
        check(PhysicsEngine::object_layer_is_moving(moving), "moving flag survives the round trip");
        check(!PhysicsEngine::object_layer_is_moving(stationary), "static flag survives the round trip");
        check(PhysicsEngine::make_object_layer(0, false) == PhysicsEngine::LAYER_NON_MOVING,
              "layer 0 static still equals the legacy LAYER_NON_MOVING");
        check(PhysicsEngine::make_object_layer(0, true) == PhysicsEngine::LAYER_MOVING,
              "layer 0 moving still equals the legacy LAYER_MOVING");
        PhysicsEngine::reset_layers();
    }

    // --- Physics, queries and joints ---------------------------------------
    section("Physics queries and joints");
    {
        for (auto& actor : actors) actor->begin_play();

        // A dynamic body to query against and to hang a joint from.
        auto ball = std::make_shared<EditorPrimitiveActor>("TestBall", "Sphere");
        ball->get_actor_transform().position = { 0.0, 6.0, 0.0 };
        auto* ball_physics = ball->create_component<PhysicsAttribute>("Physics");
        ball_physics->mass = 1.0f;
        ball_physics->collider_type = PhysicsAttribute::Collider_Sphere;
        ball_physics->sphere_radius = 0.5f;
        actors.push_back(ball);
        ball->begin_play();
        check(ball_physics->has_body(), "dynamic body created");

        PhysicsEngine& physics = PhysicsEngine::get_instance();

        // Down the line x = 3, which is clear of the ball sitting at the origin. The
        // floor is a unit cube scaled to y = 0.5, so its half-extent is 0.25 and its
        // top surface is at y = 0.25: a ray from y = 10 must travel exactly 9.75m.
        RaycastHit floor_hit;
        const bool hit_floor = physics.raycast(DVector3{ 3.0, 10.0, 0.0 },
                                               Vector3{ 0.0f, -1.0f, 0.0f }, 100.0f, floor_hit);
        check(hit_floor, "raycast finds the floor");
        if (hit_floor) {
            check(floor_hit.actor != nullptr, "raycast reports which actor it hit");
            check(floor_hit.actor && floor_hit.actor->get_name() == "TestFloor",
                  "and names the floor specifically");
            check(floor_hit.normal.y > 0.9f, "raycast surface normal points up");
            std::ostringstream detail;
            detail << "raycast distance " << std::fixed << std::setprecision(3) << floor_hit.distance
                   << "m matches the predicted 9.750m";
            check(std::abs(floor_hit.distance - 9.75f) < 0.05f, detail.str());
        }

        // Down the line x = 0, where the ball sits at y = 6 with radius 0.5. The
        // closest hit must be the ball at 3.5m, not the floor behind it - which is
        // what proves the query returns nearest-first rather than whatever it met.
        RaycastHit ball_hit;
        const bool hit_ball = physics.raycast(DVector3{ 0.0, 10.0, 0.0 },
                                              Vector3{ 0.0f, -1.0f, 0.0f }, 100.0f, ball_hit);
        check(hit_ball && ball_hit.actor && ball_hit.actor->get_name() == "TestBall",
              "raycast returns the nearest hit, not the furthest");
        if (hit_ball) {
            std::ostringstream detail;
            detail << "ball hit distance " << std::fixed << std::setprecision(3) << ball_hit.distance
                   << "m matches the predicted 3.500m";
            check(std::abs(ball_hit.distance - 3.5f) < 0.05f, detail.str());
        }

        std::vector<RaycastHit> all_hits;
        physics.raycast_all(DVector3{ 0.0, 10.0, 0.0 }, Vector3{ 0.0f, -1.0f, 0.0f }, 100.0f, all_hits);
        check(!all_hits.empty(), "raycast_all returns hits");

        RaycastHit sphere_hit;
        check(physics.sphere_cast(DVector3{ 0.0, 10.0, 0.0 }, Vector3{ 0.0f, -1.0f, 0.0f },
                                  0.4f, 100.0f, sphere_hit),
              "sphere_cast finds the floor");

        std::vector<Actor*> overlapped;
        physics.overlap_sphere(DVector3{ 0.0, 0.0, 0.0 }, 4.0f, overlapped);
        check(!overlapped.empty(), "overlap_sphere finds the floor");

        // A layer mask that excludes everything must find nothing. This is the test
        // that actually proves filtering is wired through to the query.
        RaycastHit masked;
        const bool masked_hit = physics.raycast(DVector3{ 0.0, 10.0, 0.0 },
                                                Vector3{ 0.0f, -1.0f, 0.0f }, 100.0f, masked, 0u);
        check(!masked_hit, "an empty layer mask filters the raycast out");

        // Hinge joint from the ball to the world.
        auto* joint = ball->create_component<JointComponent>("Joint");
        joint->joint_type = JointComponent::Joint_Hinge;
        joint->connected_actor.clear(); // anchored to the world
        joint->anchor = { 0.0f, 0.0f, 0.0f };
        joint->axis = { 0.0f, 1.0f, 0.0f };
        joint->begin_play();
        joint->update_joint(1.0f / 60.0f);
        check(joint->has_joint(), "hinge joint created against the world");

        for (int frame = 0; frame < 30; ++frame) {
            joint->update_joint(1.0f / 60.0f);
            physics.tick(1.0f / 60.0f);
            physics.collect_contact_events();
        }
        check(true, "30 physics steps with a live joint did not crash");
    }

    // --- Navmesh -----------------------------------------------------------
    section("Navigation");
    {
        NavBuildSettings settings;
        settings.cell_size = 0.4f;
        settings.agent_radius = 0.35f;
        settings.agent_height = 1.8f;

        std::string report;
        const bool built = NavMesh::get().build(actors, settings, report);
        std::cout << "  " << report << std::endl;
        check(built, "navmesh built from the room");

        if (built) {
            check(NavMesh::get().get_usable_node_count() > 0, "navmesh has reachable surface");

            DVector3 sampled;
            check(NavMesh::get().sample_position(DVector3{ 2.0, 5.0, 2.0 }, 8.0f, sampled),
                  "a point in mid-air snaps onto the floor");

            std::vector<DVector3> path;
            const bool found = NavMesh::get().find_path(DVector3{ -4.0, 1.0, -4.0 },
                                                        DVector3{ 4.0, 1.0, 4.0 }, path);
            check(found && path.size() >= 2, "a path across the room is found");
            if (found) {
                double length = 0.0;
                for (size_t i = 0; i + 1 < path.size(); ++i) length += (path[i + 1] - path[i]).length();
                std::ostringstream detail;
                detail << "path length " << std::fixed << std::setprecision(2) << length
                       << "m is at least the straight-line distance of 11.31m";
                // Straight line across the diagonal is sqrt(8^2 + 8^2) = 11.31. A path
                // shorter than that would mean the straightening cut a corner through
                // geometry.
                check(length >= 11.0, detail.str());
            }
        }
    }

    // --- Particles ---------------------------------------------------------
    section("Particles");
    {
        auto emitter_actor = std::make_shared<EditorPrimitiveActor>("TestEmitter", "Cube");
        emitter_actor->get_actor_transform().position = { 0.0, 3.0, 0.0 };
        auto* emitter = emitter_actor->create_component<ParticleEmitterComponent>("Particles");
        emitter->emit_rate = 200.0f;
        emitter->max_particles = 500;
        emitter->collision_enabled = true;
        actors.push_back(emitter_actor);

        for (int frame = 0; frame < 60; ++frame) emitter->tick(1.0f / 60.0f);
        const size_t alive = emitter->get_particles().size();
        std::cout << "  " << alive << " particles alive after 1 second" << std::endl;
        check(alive > 0, "emitter produced particles");
        check(alive <= static_cast<size_t>(emitter->max_particles), "the particle cap is respected");

        // The old emitter released at most one particle per frame whatever the rate
        // was, which quietly capped every effect at the frame rate.
        check(alive > 60, "emission rate is not silently capped at one per frame");

        emitter->is_emitting = false;
        for (int frame = 0; frame < 300; ++frame) emitter->tick(1.0f / 60.0f);
        check(emitter->get_particles().empty(), "particles expire once emission stops");
    }

    // --- UI ----------------------------------------------------------------
    section("Game UI");
    {
        auto ui_actor = std::make_shared<EditorPrimitiveActor>("TestUI", "Cube");
        auto* canvas = ui_actor->create_component<UICanvasComponent>("UI Canvas");
        actors.push_back(ui_actor);

        UIWidget* panel = canvas->add_widget(UIWidget::Widget_Panel, nullptr, "Panel");
        UIWidget* button = canvas->add_widget(UIWidget::Widget_Button, panel, "StartButton");
        check(panel && button, "widgets created");
        check(canvas->find("StartButton") == button, "a widget is found by name");
        check(canvas->contains(button), "the canvas knows it owns the widget");

        // Two widgets asking for the same name would make one of them permanently
        // unreachable from script.
        UIWidget* second = canvas->add_widget(UIWidget::Widget_Button, panel, "StartButton");
        check(second && second->name != "StartButton", "a duplicate name is made unique");

        canvas->remove_widget(button);
        check(canvas->find("StartButton") == nullptr, "a removed widget is gone");
        check(!canvas->contains(button), "and the canvas no longer claims it");
    }

    // --- Baked lighting ----------------------------------------------------
    section("Baked lighting");
    {
        Lightmapper::BakeSettings settings;
        // Deliberately small: this is a crash and plumbing check, not a quality one,
        // and a full-quality bake would take minutes.
        settings.atlas_size = 256;
        settings.texels_per_unit = 1.5f;
        settings.rays_per_texel = 8;
        settings.probe_spacing = 6.0f;

        std::string report;
        const bool baked = Lightmapper::get().bake(actors, settings, nullptr, report);
        std::cout << "  " << report << std::endl;
        check(baked, "lightmap bake completed");

        if (baked) {
            check(Lightmapper::get().is_baked(), "the lightmapper reports a baked result");

            Lightmapper::get().apply_to_actors(actors);
            int with_lightmap = 0;
            for (auto& actor : actors) {
                if (auto* mesh = actor->get_component<StaticMeshComponent>()) {
                    if (mesh->has_lightmap()) ++with_lightmap;
                }
            }
            std::cout << "  " << with_lightmap << " actor(s) received lightmap uvs" << std::endl;
            check(with_lightmap > 0, "static actors received lightmap uvs");

            // Inside the room, where every ray either hits a wall or escapes upward,
            // the probe has to carry some light or the bake produced nothing usable.
            const Lightmapper::AmbientCube inside = Lightmapper::get().sample_probes(DVector3{ 0.0, 2.0, 0.0 });
            const Vector3 up = inside.evaluate(Vector3{ 0.0f, 1.0f, 0.0f });
            std::ostringstream detail;
            detail << "probe inside the room carries light (up-facing irradiance "
                   << std::fixed << std::setprecision(3) << up.x << ", " << up.y << ", " << up.z << ")";
            check(up.x + up.y + up.z > 0.0f, detail.str());

            const std::string path = "/tmp/lithium_selftest.lightmap";
            check(Lightmapper::get().save(path), "lightmap saved");
            check(Lightmapper::get().load(path), "lightmap loaded back");
            std::remove(path.c_str());
        }
    }

    // --- Terrain -----------------------------------------------------------
    section("Terrain");
    {
        auto terrain_actor = std::make_shared<EditorPrimitiveActor>("TestTerrain", "Cube");
        auto* terrain = terrain_actor->create_component<TerrainComponent>("Terrain");
        actors.push_back(terrain_actor);

        check(terrain->get_resolution() >= TerrainComponent::kMinResolution, "terrain allocated");

        const float before = terrain->sample_height(0.0f, 0.0f);
        terrain->sculpt(TerrainComponent::Sculpt_Raise, 0.0f, 0.0f, 8.0f, 2.0f, 0.0f);
        const float after = terrain->sample_height(0.0f, 0.0f);
        std::ostringstream detail;
        detail << "raise brush moved the surface up (" << std::fixed << std::setprecision(3)
               << before << " -> " << after << ")";
        check(after > before, detail.str());

        terrain->paint_layer(1, 0.0f, 0.0f, 6.0f, 1.0f);
        terrain->paint_foliage(0.0f, 0.0f, 6.0f, 1.0f, false);
        check(true, "paint brushes ran without crashing");

        terrain->foliage_mesh_path.clear();
        const size_t instances = terrain->get_foliage_instances().size();
        std::cout << "  " << instances << " foliage instances scattered" << std::endl;
        check(instances > 0, "foliage scattered over the painted area");

        // A ray straight down from above the raised centre must find the surface.
        DVector3 terrain_hit;
        check(terrain->raycast(DVector3{ 0.0, 50.0, 0.0 }, Vector3{ 0.0f, -1.0f, 0.0f },
                               200.0f, terrain_hit),
              "terrain raycast hits the sculpted surface");

        terrain->reset();
        check(terrain->sample_height(0.0f, 0.0f) == 0.0f, "reset flattens the terrain");
    }

    // --- C-Minus scripting ---------------------------------------------------
    //
    // The language's own tests. The interpreter is reached only by running a
    // script, so without this every path below - scoping, arrays, vec3 arithmetic,
    // the error reporting that replaced silent zeros - stays unexercised by a
    // plain launch.
    {
        section("C-Minus scripting");

        // Parses source and runs it, returning the interpreter so the caller can
        // inspect what the script left behind.
        const auto run = [](const std::string& source, CMinus::Interpreter& interp) -> std::string {
            try {
                CMinus::Lexer lexer(source, "selftest");
                CMinus::Parser parser(lexer.tokenize(), "selftest");
                auto program = parser.parse();
                interp.execute(program);
                return {};
            } catch (const CMinus::ScriptError& e) {
                return e.detail.empty() ? std::string("error") : e.detail;
            } catch (const std::exception& e) {
                return e.what();
            }
        };

        {
            CMinus::Interpreter interp;
            const std::string err = run("a = 2 + 3 * 4; b = (2 + 3) * 4; c = 7 % 4;", interp);
            check(err.empty(), "arithmetic runs");
            check(interp.get_variable("a").x == 14.0f, "multiplication binds tighter than addition");
            check(interp.get_variable("b").x == 20.0f, "parentheses override precedence");
            check(interp.get_variable("c").x == 3.0f, "modulo");
        }

        {
            // Vec3 as a first-class value, which is the point of the type existing:
            // before it, a position had to be carried as three separate scalars.
            CMinus::Interpreter interp;
            const std::string err = run("v = vec3(1, 2, 3); w = v * 2; s = w.y; n = length(vec3(3, 4, 0));", interp);
            check(err.empty(), "vector arithmetic runs");
            const CMinus::Value w = interp.get_variable("w");
            check(w.is_vec3() && w.x == 2.0f && w.y == 4.0f && w.z == 6.0f, "a vec3 scales by a scalar");
            check(interp.get_variable("s").x == 4.0f, "a component reads back with .y");
            check(interp.get_variable("n").x == 5.0f, "length of a 3-4-5 triangle");
        }

        {
            CMinus::Interpreter interp;
            const std::string err = run("p = vec3(0, 0, 0); p.y = 3; q = p.y;", interp);
            check(err.empty(), "component assignment runs");
            check(interp.get_variable("q").x == 3.0f, "writing one component leaves the others alone");
            check(interp.get_variable("p").z == 0.0f, "and does not disturb z");
        }

        {
            // Growing on read is load-bearing: existing scripts read a table before
            // ever assigning to it and expect zero.
            CMinus::Interpreter interp;
            const std::string err = run("t[5] = 9; u = t[5]; z = t[40];", interp);
            check(err.empty(), "arrays run");
            check(interp.get_variable("u").x == 9.0f, "an array element reads back");
            check(interp.get_variable("z").x == 0.0f, "an unwritten element reads as zero");
        }

        {
            CMinus::Interpreter interp;
            const std::string err = run("array fixed[4]; fixed[9] = 1;", interp);
            check(!err.empty(), "a declared array rejects an out-of-range index");
        }

        {
            // Hoisting: a script may call a helper declared further down the file.
            CMinus::Interpreter interp;
            const std::string err = run("r = twice(21); function twice(n) { return n * 2; }", interp);
            check(err.empty(), "a function may be called before it is declared");
            check(interp.get_variable("r").x == 42.0f, "and returns its value");
        }

        {
            // A helper must not be able to clobber game state that happens to share
            // a name with one of its locals.
            CMinus::Interpreter interp;
            const std::string err = run(
                "score = 10; function helper() { score = 999; } helper(); "
                "function bump() { global score = 50; } bump();", interp);
            check(err.empty(), "scoping runs");
            check(interp.get_variable("score").x == 50.0f,
                  "a plain assignment in a function is local, and 'global' opts in");
        }

        {
            CMinus::Interpreter interp;
            const std::string err = run(
                "n = 0; for (i = 0; i < 10; i = i + 1) { if (i == 5) { break; } n = n + 1; }", interp);
            check(err.empty(), "loops run");
            check(interp.get_variable("n").x == 5.0f, "break leaves the loop");
        }

        {
            CMinus::Interpreter interp;
            const std::string err = run("while (1) { }", interp);
            check(!err.empty(), "an infinite loop is caught rather than hanging the engine");
        }

        {
            // Both used to evaluate silently to 0, which is what made a typo present
            // as "the game is subtly wrong" rather than as an error.
            CMinus::Interpreter interp;
            check(!run("x = 1 / 0;", interp).empty(), "division by zero is reported");
            check(!run("x = no_such_function(1);", interp).empty(), "an unknown function is reported");
            check(!run("x = sin(1, 2, 3);", interp).empty(), "a wrong argument count is reported");
            check(!run("x = length(5);", interp).empty(), "a type error is reported");
        }

        {
            // Event functions - the shape the visual script editor compiles to.
            //
            // The program is held here rather than inside run(): the interpreter
            // keeps raw pointers into the AST it was bound to (hosts store it as
            // parsed_program and re-enter every frame), so it has to outlive every
            // call made against it.
            CMinus::Interpreter interp;
            std::vector<std::unique_ptr<CMinus::ASTNode>> program;
            std::string err;
            try {
                CMinus::Lexer lexer("function on_tick() { global ticked = 1; scratch = 7; } "
                                    "function on_begin_play() { }", "selftest");
                CMinus::Parser parser(lexer.tokenize(), "selftest");
                program = parser.parse();
                interp.execute(program);
            } catch (const std::exception& e) {
                err = e.what();
            }
            check(err.empty(), "an event-function script binds");
            check(interp.has_script_function("on_tick"), "on_tick is found");
            check(!interp.has_script_function("on_collision_enter"), "an undefined event is simply absent");
            CMinus::Value result;
            check(interp.call_script_function("on_tick", {}, &result), "on_tick is callable");
            check(interp.get_variable("ticked").x == 1.0f,
                  "a 'global' write from an event function is visible afterwards");
            // The other half of the same rule: a plain assignment stayed local and
            // did not leak into the script's state.
            check(interp.get_variable("scratch").x == 0.0f,
                  "and a plain assignment in that function did not escape it");
        }

        {
            // The real thing. The sample game is the largest script that exists, and
            // it exercises the parser far harder than anything written here.
            namespace fs = std::filesystem;
            const fs::path script = "Content/LithiumTest/lithium_test.cminus";
            std::error_code ec;
            if (fs::exists(script, ec)) {
                std::ifstream in(script);
                std::stringstream buffer;
                buffer << in.rdbuf();
                try {
                    CMinus::Lexer lexer(buffer.str(), script.string());
                    CMinus::Parser parser(lexer.tokenize(), script.string());
                    auto program = parser.parse();
                    check(!program.empty(), "the sample game script parses");
                    // Run it with no actor attached: every actor-bound builtin it
                    // calls must report that rather than dereferencing null.
                    CMinus::Interpreter interp;
                    interp.script_name = script.string();
                    try {
                        interp.execute(program);
                        check(true, "the sample game script runs headlessly");
                    } catch (const CMinus::ScriptError&) {
                        // Expected: it drives an actor it does not have here.
                        check(true, "the sample game script reports rather than crashing without an actor");
                    }
                } catch (const std::exception& e) {
                    check(false, std::string("the sample game script parses: ") + e.what());
                }
            }
        }
    }

    // --- Inverse kinematics --------------------------------------------------
    //
    // A synthetic three-bone chain along +Y, one unit per segment, so the reach is
    // known exactly and the solver's answer can be checked against arithmetic
    // rather than against a screenshot.
    {
        section("Inverse kinematics");

        Skeleton skeleton;
        skeleton.global_inverse_transform = Matrix4x4::identity();
        // root at origin, mid one unit up, tip two units up.
        for (int i = 0; i < 3; ++i) {
            Bone bone;
            bone.name = (i == 0) ? "thigh" : (i == 1) ? "shin" : "foot";
            bone.parent_index = i - 1;
            bone.local_bind_transform = (i == 0)
                ? Matrix4x4::identity()
                : Matrix4x4::translation({ 0.0f, 1.0f, 0.0f });
            bone.inverse_bind_pose =
                Matrix4x4::translation({ 0.0f, -static_cast<float>(i), 0.0f });
            skeleton.bones.push_back(bone);
        }

        std::vector<AnimationClip> no_clips;
        AnimationPlayer player(&skeleton, &no_clips);

        const int chain = player.add_two_bone_ik("foot");
        check(chain >= 0, "a two-bone chain resolves from the tip");

        int ik_root = -1, ik_mid = -1, ik_end = -1;
        player.get_ik_bones(chain, ik_root, ik_mid, ik_end);
        check(ik_root == 0 && ik_mid == 1 && ik_end == 2,
              "and it picks the tip's parent and grandparent");

        // A tip whose parent has no parent cannot be driven.
        check(player.add_two_bone_ik("shin") < 0,
              "a tip without a grandparent is rejected");

        Vector3 rest;
        check(player.get_bone_mesh_position(ik_end, rest), "the tip has a mesh position");
        check(std::fabs(rest.y - 2.0f) < 1e-3f, "which starts two units up the chain");

        // Reachable: 1.2 units along +X and 1.0 up is well inside a reach of 2.
        const Vector3 target{ 1.2f, 1.0f, 0.0f };
        player.set_ik_target(chain, target);
        player.set_ik_enabled(chain, true);
        player.set_ik_weight(chain, 1.0f);
        player.update(0.016f);

        Vector3 solved;
        player.get_bone_mesh_position(ik_end, solved);
        const float reach_error = std::sqrt((solved.x - target.x) * (solved.x - target.x) +
                                            (solved.y - target.y) * (solved.y - target.y) +
                                            (solved.z - target.z) * (solved.z - target.z));
        {
            std::ostringstream detail;
            detail << "the tip reaches a target inside its range (error "
                   << std::fixed << std::setprecision(4) << reach_error << ")";
            check(reach_error < 0.02f, detail.str());
        }

        // Bone lengths are what a rotation-only solve must not change.
        Vector3 solved_root, solved_mid;
        player.get_bone_mesh_position(ik_root, solved_root);
        player.get_bone_mesh_position(ik_mid, solved_mid);
        const auto length_of = [](const Vector3& a, const Vector3& b) {
            return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                             (a.z - b.z) * (a.z - b.z));
        };
        check(std::fabs(length_of(solved_root, solved_mid) - 1.0f) < 1e-3f,
              "the upper bone keeps its length");
        check(std::fabs(length_of(solved_mid, solved) - 1.0f) < 1e-3f,
              "the lower bone keeps its length");

        // Out of reach straightens the limb toward the target instead of tearing.
        const Vector3 far_target{ 10.0f, 0.0f, 0.0f };
        player.set_ik_target(chain, far_target);
        player.update(0.016f);
        player.get_bone_mesh_position(ik_end, solved);
        const float extension = std::sqrt(solved.x * solved.x + solved.y * solved.y +
                                          solved.z * solved.z);
        check(extension > 1.98f && extension < 2.01f,
              "an unreachable target straightens the limb to full reach, not beyond");
        check(solved.x > 1.9f, "and points it at the target");

        // Disabling releases the chain back to the animated pose.
        player.set_ik_enabled(chain, false);
        player.update(0.016f);
        player.get_bone_mesh_position(ik_end, solved);
        check(std::fabs(solved.y - 2.0f) < 1e-3f, "disabling returns the limb to its pose");

        // Weight blends rather than snapping.
        player.set_ik_target(chain, target);
        player.set_ik_enabled(chain, true);
        player.set_ik_weight(chain, 0.0f);
        player.update(0.016f);
        player.get_bone_mesh_position(ik_end, solved);
        check(std::fabs(solved.y - 2.0f) < 1e-2f, "zero weight leaves the pose untouched");
    }

    // --- Tangent frames ------------------------------------------------------
    //
    // Normal mapping is only correct if the tangent frame is: a normal map is
    // authored in tangent space, so a wrong frame lights the surface from the wrong
    // direction and looks like a lighting bug rather than a geometry one.
    {
        section("Tangent frames");

        Actor probe("TangentProbe");
        auto* mesh = probe.create_component<StaticMeshComponent>("Mesh");

        // A quad in the XY plane facing +Z, with U running along +X and V along +Y.
        // The tangent is therefore known in advance: it must be +X.
        std::vector<Vertex> quad(4);
        quad[0].position = { 0.0f, 0.0f, 0.0f }; quad[0].uv = { 0.0f, 0.0f };
        quad[1].position = { 1.0f, 0.0f, 0.0f }; quad[1].uv = { 1.0f, 0.0f };
        quad[2].position = { 1.0f, 1.0f, 0.0f }; quad[2].uv = { 1.0f, 1.0f };
        quad[3].position = { 0.0f, 1.0f, 0.0f }; quad[3].uv = { 0.0f, 1.0f };
        for (Vertex& v : quad) { v.normal = { 0.0f, 0.0f, 1.0f }; v.color = { 1.0f, 1.0f, 1.0f }; }
        const std::vector<unsigned int> quad_indices = { 0, 1, 2, 0, 2, 3 };

        mesh->set_geometry(quad, quad_indices);
        mesh->generate_tangents();
        const auto& tangents = mesh->get_tangents();
        check(tangents.size() == quad.size(), "a tangent is produced per vertex");

        if (tangents.size() == quad.size()) {
            bool all_along_x = true;
            bool all_unit = true;
            bool all_orthogonal = true;
            for (size_t i = 0; i < tangents.size(); ++i) {
                const Vector4& t = tangents[i].tangent;
                if (t.x < 0.99f) all_along_x = false;
                const float length = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
                if (std::fabs(length - 1.0f) > 1e-3f) all_unit = false;
                // Dot with the vertex normal (+Z) must vanish.
                if (std::fabs(t.z) > 1e-3f) all_orthogonal = false;
            }
            check(all_along_x, "U running along +X yields a tangent along +X");
            check(all_unit, "every tangent is unit length");
            check(all_orthogonal, "and perpendicular to the vertex normal");
        }

        // Mirrored UVs must flip the handedness, or the mirrored half of a
        // symmetric character lights inverted.
        std::vector<Vertex> mirrored = quad;
        mirrored[0].uv = { 1.0f, 0.0f };
        mirrored[1].uv = { 0.0f, 0.0f };
        mirrored[2].uv = { 0.0f, 1.0f };
        mirrored[3].uv = { 1.0f, 1.0f };
        mesh->set_geometry(mirrored, quad_indices);
        mesh->generate_tangents();
        const auto& flipped = mesh->get_tangents();
        if (flipped.size() == mirrored.size()) {
            check(flipped[0].tangent.x < -0.99f, "mirrored UVs reverse the tangent");
        }

        // Degenerate UVs must not produce NaN. Every vertex on one texel gives a
        // zero determinant, which is exactly the case that divides by zero if the
        // guard is missing.
        std::vector<Vertex> degenerate = quad;
        for (Vertex& v : degenerate) v.uv = { 0.5f, 0.5f };
        mesh->set_geometry(degenerate, quad_indices);
        mesh->generate_tangents();
        const auto& safe = mesh->get_tangents();
        bool finite = true;
        for (const VertexTangent& vt : safe) {
            if (!std::isfinite(vt.tangent.x) || !std::isfinite(vt.tangent.y) ||
                !std::isfinite(vt.tangent.z) || !std::isfinite(vt.tangent.w)) {
                finite = false;
            }
        }
        check(finite, "degenerate UVs produce a usable frame rather than NaN");
    }

    // --- External code editors -----------------------------------------------
    //
    // Every editor wants a line number in a different shape, and getting one wrong
    // means the editor opens an empty buffer named "script.cminus:12" instead of the
    // file. The table is the thing worth checking; launching a real editor is not
    // something a headless test can do.
    {
        section("External code editors");

        const auto& editors = ExternalEditor::registry();
        check(editors.size() > 4, "the editor registry is populated");
        check(std::string(editors[ExternalEditor::kBuiltIn].display_name) == "Built-in Editor",
              "index 0 is the built-in editor");
        check(std::string(editors[ExternalEditor::kSystemDefault].display_name) == "System Default",
              "index 1 is the system default handler");

        // Every entry past the two specials must name at least one executable and
        // say how to pass it a file, or selecting it could only ever fail.
        bool all_launchable = true;
        bool all_mention_file = true;
        for (size_t i = 2; i < editors.size(); ++i) {
            if (editors[i].candidates.empty()) all_launchable = false;
            bool mentions_file = false;
            for (const std::string& argument : editors[i].argument_template) {
                if (argument.find("{file}") != std::string::npos) mentions_file = true;
            }
            if (!mentions_file) all_mention_file = false;
        }
        check(all_launchable, "every editor names at least one executable");
        check(all_mention_file, "and every argument template passes the file");

        // An entry claiming line support has to actually use the line, or the
        // setting is a lie the UI repeats.
        bool line_claims_hold = true;
        for (size_t i = 2; i < editors.size(); ++i) {
            bool uses_line = false;
            for (const std::string& argument : editors[i].argument_template) {
                if (argument.find("{line}") != std::string::npos) uses_line = true;
            }
            if (editors[i].supports_line != uses_line) line_claims_hold = false;
        }
        check(line_claims_hold, "supports_line matches whether the template uses {line}");

        // The known syntaxes, spot-checked by name so a careless edit to the table
        // is caught rather than shipped.
        const auto template_of = [&](const char* name) {
            std::string joined;
            for (const auto& definition : editors) {
                if (std::string(definition.display_name) != name) continue;
                for (const std::string& argument : definition.argument_template) {
                    if (!joined.empty()) joined += ' ';
                    joined += argument;
                }
            }
            return joined;
        };
        check(template_of("Visual Studio Code") == "--goto {file}:{line}",
              "VS Code uses --goto file:line");
        check(template_of("Sublime Text") == "{file}:{line}",
              "Sublime takes file:line as one argument");
        check(template_of("Kate") == "-l {line} {file}", "Kate uses -l line file");
        check(template_of("Notepad++") == "-n{line} {file}",
              "Notepad++ glues the line to the flag");
        check(template_of("MonoDevelop") == "{file};{line}",
              "MonoDevelop separates the line with a semicolon");
        check(template_of("Visual Studio") == "/edit {file}",
              "Visual Studio opens with /edit and no line");

        // An unresolvable editor must report failure rather than silently doing
        // nothing, so the UI can fall back to the built-in editor.
        check(!ExternalEditor::open_file(999, "", "/tmp/nonexistent_probe.cminus", 1),
              "an empty custom command reports failure");
        check(!ExternalEditor::open_file(2, "", "", 1),
              "an empty path reports failure");

        // Detection, against binaries whose presence is not in doubt.
        check(Platform::executable_exists("sh"), "a program on PATH is detected");
        check(!Platform::executable_exists("lithium_no_such_program_xyz"),
              "a program that is not installed is not");
        check(!Platform::executable_exists(""), "an empty name is not a program");

        // The launch path itself: fork, detach, exec. /bin/true is chosen because it
        // does nothing observable and exits immediately - the point is that the call
        // succeeds and returns without waiting, not what the child does.
        check(Platform::launch_detached("true", {}), "a detached process launches");
        check(!Platform::launch_detached("", {}), "launching nothing reports failure");
        check(!Platform::launch_detached("lithium_no_such_program_xyz", {"arg"}) == false,
              "a missing program still returns from the fork rather than hanging");
    }

    // --- Outliner folders ----------------------------------------------------
    //
    // Folders are strings on the actor, so the whole feature is path manipulation
    // and that is where it can go wrong: a rename has to carry nested folders with
    // it, a delete must not take the actors down with it, and prefix matching must
    // not confuse "Lights" with "LightsB".
    {
        section("Outliner folders");

        std::vector<std::shared_ptr<Actor>> filed;
        const auto make = [&](const char* name, const char* folder) {
            auto actor = std::make_shared<Actor>(name);
            actor->set_folder_path(folder);
            filed.push_back(actor);
            return actor;
        };

        auto sun = make("Sun", "Lighting");
        auto lamp = make("Lamp", "Lighting/Interior");
        auto decoy = make("Decoy", "LightingRig");   // shares a prefix, is not inside
        auto loose = make("Loose", "");

        check(sun->get_folder_path() == "Lighting", "an actor remembers its folder");
        check(loose->get_folder_path().empty(), "and the root is the empty path");

        // Round-trip through the scene file, including a folder holding nothing.
        namespace fs = std::filesystem;
        const fs::path scene_file = fs::temp_directory_path() / "lithium_folder_test.lithium";
        SceneSerializer::save_scene(scene_file.string(), filed, { "Empty/Nested" });

        std::vector<std::shared_ptr<Actor>> reloaded;
        check(SceneSerializer::load_scene(scene_file.string(), reloaded), "scene with folders saves and loads");

        std::string lamp_folder;
        for (const auto& actor : reloaded) {
            if (actor && actor->get_name() == "Lamp") lamp_folder = actor->get_folder_path();
        }
        check(lamp_folder == "Lighting/Interior", "a nested folder survives the round trip");

        const auto& loaded_folders = SceneSerializer::last_loaded_folders();
        bool has_empty = false;
        for (const std::string& folder : loaded_folders) {
            if (folder == "Empty/Nested") has_empty = true;
        }
        check(has_empty, "a folder holding no actors is preserved");

        std::error_code ec;
        fs::remove(scene_file, ec);
    }

    // --- Duplicate naming ----------------------------------------------------
    //
    // "Name", then "Name (1)", "Name (2)". A space, then the number in parentheses.
    {
        section("Duplicate naming");

        std::set<std::string> used;
        const auto taken = [&used](const std::string& candidate) {
            return used.count(candidate) > 0;
        };

        check(Editor::next_available_name("Cube", taken) == "Cube",
              "a free name is used as-is, with no suffix");

        used.insert("Cube");
        check(Editor::next_available_name("Cube", taken) == "Cube (1)",
              "the first duplicate is 'Cube (1)'");

        used.insert("Cube (1)");
        check(Editor::next_available_name("Cube", taken) == "Cube (2)",
              "the second is 'Cube (2)'");

        // Duplicating a duplicate continues the sequence rather than nesting.
        used.insert("Cube (2)");
        check(Editor::next_available_name("Cube (2)", taken) == "Cube (3)",
              "duplicating 'Cube (2)' gives 'Cube (3)', not 'Cube (2) (1)'");

        // Gaps are filled, so deleting the middle of a run reuses the number.
        used.erase("Cube (1)");
        check(Editor::next_available_name("Cube", taken) == "Cube (1)",
              "a freed number is reused");

        // A name that merely ends in parentheses is not a duplicate suffix.
        std::set<std::string> parens = { "Barrel (Broken)" };
        const auto parens_taken = [&parens](const std::string& candidate) {
            return parens.count(candidate) > 0;
        };
        check(Editor::next_available_name("Barrel (Broken)", parens_taken) == "Barrel (Broken) (1)",
              "a non-numeric suffix is kept, not treated as a counter");

        std::set<std::string> nospace = { "Cube(1)" };
        const auto nospace_taken = [&nospace](const std::string& candidate) {
            return nospace.count(candidate) > 0;
        };
        check(Editor::next_available_name("Cube(1)", nospace_taken) == "Cube(1) (1)",
              "a counter without the space is not one");
    }

    // --- Shadow cascades -----------------------------------------------------
    //
    // The cascade fit is pure maths against the current view and projection, so it
    // can be checked without drawing. A bad fit does not error - it silently stops
    // selecting a cascade, and every shadow in the scene disappears.
    {
        section("Shadow cascades");

        if (g_engine) {
            Renderer& r = g_engine->get_renderer();
            const float aspect = 16.0f / 9.0f;
            r.set_view_matrix(Matrix4x4::identity());
            r.set_projection_matrix(Matrix4x4::perspective(45.0f, aspect, 0.1f, 1000.0f));
            r.shadow_distance = 250.0f;
            r.begin_shadow_pass(0);   // refits every cascade
            r.end_shadow_pass();

            const int count = Renderer::shadow_cascade_count();
            bool splits_increase = true;
            bool radii_positive = true;
            bool matrices_finite = true;
            float previous = 0.0f;
            for (int i = 0; i < count; ++i) {
                const float split = r.cascade_split(i);
                if (!(split > previous)) splits_increase = false;
                previous = split;
                if (!(r.cascade_fit_radius(i) > 0.0f)) radii_positive = false;
                for (float v : r.cascade_matrix(i).m) {
                    if (!std::isfinite(v)) matrices_finite = false;
                }
            }
            check(splits_increase, "cascade splits increase with distance");
            check(radii_positive, "every cascade has a positive fit radius");
            check(matrices_finite, "every cascade matrix is finite");

            {
                std::ostringstream detail;
                detail << "the last cascade reaches the shadow distance ("
                       << std::fixed << std::setprecision(1) << r.cascade_split(count - 1)
                       << " of 250)";
                check(r.cascade_split(count - 1) > 200.0f, detail.str());
            }

            // The point of the fit: a caster in front of the camera has to land in
            // some cascade. If none accepts it, nothing is ever drawn into the
            // shadow map and the scene renders unshadowed.
            bool accepted_somewhere = false;
            for (int i = 0; i < count; ++i) {
                r.begin_shadow_pass(i);
                if (r.cascade_accepts_caster(Vector3{ 0.0f, 0.0f, -10.0f }, 1.0f)) {
                    accepted_somewhere = true;
                }
                r.end_shadow_pass();
            }
            check(accepted_somewhere, "a caster 10 units in front of the camera lands in a cascade");

            bool near_accepted = false;
            for (int i = 0; i < count; ++i) {
                r.begin_shadow_pass(i);
                if (r.cascade_accepts_caster(Vector3{ 0.0f, 0.0f, -2.0f }, 0.5f)) near_accepted = true;
                r.end_shadow_pass();
            }
            check(near_accepted, "and so does one 2 units away");

            // The G-buffer grew to six targets when emission colour and specular
            // tint were added. If this GPU cannot bind that many, every draw after
            // the fourth or fifth attachment is silently dropped and the deferred
            // pass reads uninitialised memory - which looks like broken lighting,
            // not like an error.
            GLint max_draw_buffers = 0, max_attachments = 0;
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
            glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_attachments);
            {
                std::ostringstream detail;
                detail << "the GPU allows " << max_draw_buffers << " draw buffers and "
                       << max_attachments << " colour attachments; the G-buffer needs 6";
                check(max_draw_buffers >= 6 && max_attachments >= 6, detail.str());
            }
        }
    }

    // --- TESLA denoiser ------------------------------------------------------
    //
    // Checked against the library directly rather than through a render: a full
    // path trace in a self-test would take minutes, and what needs verifying is
    // that the filter is present, runs on this CPU, and actually removes noise.
    {
        section("TESLA denoiser");

        if (g_engine) {
            Renderer& r = g_engine->get_renderer();
            const bool available = r.tesla.denoise_available();
            check(available, "the build has Open Image Denoise");

            if (available) {
                // Denoising with nothing accumulated must fail rather than upload
                // an empty image over the viewport.
                check(!r.tesla.run_denoiser(), "denoising an empty accumulation is refused");
                check(r.tesla.display_texture() == r.tesla.accumulation_texture(),
                      "and the raw accumulation stays on screen");

                // The regression this exists for: the CPU accumulation buffers are
                // sized for both backends but only the CPU path writes them. Reading
                // them while the GPU backend owns the estimate yields an all-zero
                // image, which the filter denoises into a black frame and uploads
                // over a perfectly good viewport. Sizing alone is not a safe guard.
                TeslaSettings& ts = r.tesla.settings();
                const bool was_gpu = ts.use_gpu;
                ts.use_gpu = true;
                check(!r.tesla.run_denoiser(),
                      "denoising is refused on the GPU backend with nothing traced");
                check(r.tesla.display_texture() == r.tesla.accumulation_texture(),
                      "so a black frame is never presented in place of the render");
                ts.use_gpu = was_gpu;
            }
        }
    }

    // --- Result ------------------------------------------------------------
    std::cout << "\n========== " << (g_failures == 0 ? "ALL PASS" : "FAILURES") << ": "
              << (g_checks - g_failures) << " / " << g_checks << " checks ==========\n" << std::endl;

    NavMesh::get().clear();
    Lightmapper::get().clear();
    actors.clear();
    return g_failures;
}
