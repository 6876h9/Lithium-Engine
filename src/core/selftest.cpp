#include "core/selftest.hpp"

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
#include "physics/physics_engine.hpp"
#include "navigation/navmesh.hpp"
#include "renderer/lightmapper.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

    // --- Result ------------------------------------------------------------
    std::cout << "\n========== " << (g_failures == 0 ? "ALL PASS" : "FAILURES") << ": "
              << (g_checks - g_failures) << " / " << g_checks << " checks ==========\n" << std::endl;

    NavMesh::get().clear();
    Lightmapper::get().clear();
    actors.clear();
    return g_failures;
}
