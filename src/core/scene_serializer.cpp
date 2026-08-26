#include "core/scene_serializer.hpp"
#include <nlohmann/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "world/editor_primitive_actor.hpp"
#include "world/directional_light_actor.hpp"
#include "world/spinning_cube_actor.hpp"
#include "world/light_components.hpp"
#include "world/static_mesh_component.hpp"
#include "core/model_importer.hpp"
#include "world/physics_attribute.hpp"
#include "world/character_controller_component.hpp"
#include "world/joint_component.hpp"
#include "world/ui_canvas_component.hpp"
#include "world/lod_group_component.hpp"
#include "world/nav_agent_component.hpp"
#include "world/terrain_component.hpp"
#include "world/audio_component.hpp"
#include "world/particle_emitter_component.hpp"
#include "renderer/lightmapper.hpp"
#include "core/engine.hpp"
#include "renderer/renderer.hpp"
#include "world/cminus_component.hpp"
#include "world/cpp_script_component.hpp"
#include "world/lua_script_component.hpp"
#include "core/resource_manager.hpp"
#include "core/mesh_resource.hpp"

using json = nlohmann::json;

namespace {

// A UI widget and everything under it. Recursive because the tree is, and shared by
// the canvas save/load below so a widget kind can never be written in one place and
// forgotten in the other.
json widget_to_json(const UIWidget& w) {
    json j;
    j["type"] = w.type;
    j["name"] = w.name;
    j["visible"] = w.visible;
    j["interactive"] = w.interactive;
    j["anchor_min"] = { w.anchor_min.x, w.anchor_min.y };
    j["anchor_max"] = { w.anchor_max.x, w.anchor_max.y };
    j["offset_min"] = { w.offset_min.x, w.offset_min.y };
    j["offset_max"] = { w.offset_max.x, w.offset_max.y };
    j["background_color"] = { w.background_color.x, w.background_color.y, w.background_color.z, w.background_color.w };
    j["border_color"] = { w.border_color.x, w.border_color.y, w.border_color.z, w.border_color.w };
    j["border_thickness"] = w.border_thickness;
    j["corner_radius"] = w.corner_radius;
    j["text"] = w.text;
    j["text_color"] = { w.text_color.x, w.text_color.y, w.text_color.z, w.text_color.w };
    j["font_scale"] = w.font_scale;
    j["h_align"] = w.h_align;
    j["v_align"] = w.v_align;
    j["word_wrap"] = w.word_wrap;
    j["padding"] = w.padding;
    j["image_path"] = w.image_path;
    j["image_tint"] = { w.image_tint.x, w.image_tint.y, w.image_tint.z, w.image_tint.w };
    j["hover_color"] = { w.hover_color.x, w.hover_color.y, w.hover_color.z, w.hover_color.w };
    j["pressed_color"] = { w.pressed_color.x, w.pressed_color.y, w.pressed_color.z, w.pressed_color.w };
    j["disabled_color"] = { w.disabled_color.x, w.disabled_color.y, w.disabled_color.z, w.disabled_color.w };
    j["value"] = w.value;
    j["min_value"] = w.min_value;
    j["max_value"] = w.max_value;
    j["fill_color"] = { w.fill_color.x, w.fill_color.y, w.fill_color.z, w.fill_color.w };

    if (!w.children.empty()) {
        json kids = json::array();
        for (const auto& child : w.children) {
            if (child) kids.push_back(widget_to_json(*child));
        }
        j["children"] = kids;
    }
    return j;
}

Vector2 vec2_from(const json& j, const Vector2& fallback) {
    if (!j.is_array() || j.size() < 2) return fallback;
    return { j[0].get<float>(), j[1].get<float>() };
}

Vector4 vec4_from(const json& j, const Vector4& fallback) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
}

std::unique_ptr<UIWidget> widget_from_json(const json& j) {
    auto w = std::make_unique<UIWidget>();
    w->type = j.value("type", (int)UIWidget::Widget_Panel);
    w->name = j.value("name", std::string("Widget"));
    w->visible = j.value("visible", true);
    w->interactive = j.value("interactive", true);
    if (j.contains("anchor_min")) w->anchor_min = vec2_from(j["anchor_min"], w->anchor_min);
    if (j.contains("anchor_max")) w->anchor_max = vec2_from(j["anchor_max"], w->anchor_max);
    if (j.contains("offset_min")) w->offset_min = vec2_from(j["offset_min"], w->offset_min);
    if (j.contains("offset_max")) w->offset_max = vec2_from(j["offset_max"], w->offset_max);
    if (j.contains("background_color")) w->background_color = vec4_from(j["background_color"], w->background_color);
    if (j.contains("border_color")) w->border_color = vec4_from(j["border_color"], w->border_color);
    w->border_thickness = j.value("border_thickness", w->border_thickness);
    w->corner_radius = j.value("corner_radius", w->corner_radius);
    w->text = j.value("text", std::string());
    if (j.contains("text_color")) w->text_color = vec4_from(j["text_color"], w->text_color);
    w->font_scale = j.value("font_scale", w->font_scale);
    w->h_align = j.value("h_align", w->h_align);
    w->v_align = j.value("v_align", w->v_align);
    w->word_wrap = j.value("word_wrap", w->word_wrap);
    w->padding = j.value("padding", w->padding);
    w->image_path = j.value("image_path", std::string());
    if (j.contains("image_tint")) w->image_tint = vec4_from(j["image_tint"], w->image_tint);
    if (j.contains("hover_color")) w->hover_color = vec4_from(j["hover_color"], w->hover_color);
    if (j.contains("pressed_color")) w->pressed_color = vec4_from(j["pressed_color"], w->pressed_color);
    if (j.contains("disabled_color")) w->disabled_color = vec4_from(j["disabled_color"], w->disabled_color);
    w->value = j.value("value", w->value);
    w->min_value = j.value("min_value", w->min_value);
    w->max_value = j.value("max_value", w->max_value);
    if (j.contains("fill_color")) w->fill_color = vec4_from(j["fill_color"], w->fill_color);

    if (j.contains("children")) {
        for (const auto& child_json : j["children"]) {
            if (auto child = widget_from_json(child_json)) w->children.push_back(std::move(child));
        }
    }
    return w;
}

// One actor as JSON. Extracted from save_scene so prefabs serialise through exactly
// the same path - a prefab is just a single actor's entry written to its own file,
// and duplicating this would mean prefabs silently losing whatever the scene format
// gained next.
json actor_to_json(Actor* actor) {
    {
        json actor_json;
        actor_json["name"] = actor->get_name();
        actor_json["shape_type"] = actor->shape_type;
        actor_json["mesh_path"] = actor->mesh_path;
        actor_json["material_path"] = actor->material_path;
        actor_json["is_invisible"] = actor->is_invisible;
        actor_json["actor_color"] = { actor->actor_color.x, actor->actor_color.y, actor->actor_color.z };
        actor_json["metallic"] = actor->metallic;
        actor_json["roughness"] = actor->roughness;
        actor_json["clearcoat"] = actor->clearcoat;
        actor_json["clearcoat_roughness"] = actor->clearcoat_roughness;
        actor_json["sheen"] = actor->sheen;
        actor_json["subsurface"] = actor->subsurface;

        Transform& t = actor->get_actor_transform();
        actor_json["transform"] = {
            {"position", {t.position.x, t.position.y, t.position.z}},
            {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z}},
            {"scale", {t.scale.x, t.scale.y, t.scale.z}}
        };

        // Determine if there are specific light components
        if (actor->get_component<PointLightComponent>()) actor_json["light_type"] = "PointLight";
        else if (actor->get_component<SpotLightComponent>()) actor_json["light_type"] = "SpotLight";
        else if (actor->get_component<AreaLightComponent>()) actor_json["light_type"] = "AreaLight";
        else if (actor->get_component<SkyLightComponent>()) actor_json["light_type"] = "SkyLight";
        else if (dynamic_cast<DirectionalLightActor*>(actor)) actor_json["light_type"] = "DirectionalLight";

        // Sky settings live on the sun and were not saved, so a scene could not say
        // "this is an interior at night" - it always reloaded under the default
        // daytime environment, which lights every surface through the ceiling.
        if (auto* dir_actor = dynamic_cast<DirectionalLightActor*>(actor)) {
            actor_json["sky_mode"] = dir_actor->sky_mode;
            actor_json["void_color"] = { dir_actor->void_color.x, dir_actor->void_color.y, dir_actor->void_color.z };
            actor_json["enable_3d_clouds"] = dir_actor->enable_3d_clouds;
        }

        if (auto light = actor->get_component<LightComponent>()) {
            actor_json["light_color"] = { light->color.x, light->color.y, light->color.z };
            actor_json["light_intensity"] = light->intensity;
        }

        // Scripts.
        //
        // Without this a scene can describe a level but never a game: the components
        // that give an actor behaviour were dropped on save, so a saved scene came
        // back as a static diorama and the standalone runtime - which loads a scene
        // and goes straight to PlayInEditor - had nothing to run.
        for (const auto& comp : actor->get_components()) {
            if (auto* cm = dynamic_cast<CMinusComponent*>(comp.get())) {
                actor_json["script"] = { {"type", "cminus"}, {"path", cm->script_path} };
                break;
            }
            if (auto* cs = dynamic_cast<CppScriptComponent*>(comp.get())) {
                actor_json["script"] = { {"type", "cpp"}, {"path", cs->script_path} };
                break;
            }
            if (auto* ls = dynamic_cast<LuaScriptComponent*>(comp.get())) {
                json script_json = { {"type", "lua"}, {"path", ls->script_path} };
                // Only the overrides. The script's own defaults live in the script,
                // and writing them here would freeze a value the author later changed.
                if (!ls->property_overrides.empty()) {
                    json properties = json::array();
                    for (const LuaAPI::ScriptProperty& property : ls->property_overrides) {
                        json entry;
                        entry["name"] = property.name;
                        switch (property.type) {
                            case LuaAPI::ScriptProperty::Type::Number:
                                entry["type"] = "number";
                                entry["value"] = property.number_value;
                                break;
                            case LuaAPI::ScriptProperty::Type::String:
                                entry["type"] = "string";
                                entry["value"] = property.string_value;
                                break;
                            case LuaAPI::ScriptProperty::Type::Boolean:
                                entry["type"] = "boolean";
                                entry["value"] = property.boolean_value;
                                break;
                        }
                        properties.push_back(entry);
                    }
                    script_json["properties"] = properties;
                }
                actor_json["script"] = script_json;
                break;
            }
        }

        if (auto phys = actor->get_component<PhysicsAttribute>()) {
            actor_json["physics"] = {
                {"mass", phys->mass},
                {"friction", phys->friction},
                {"restitution", phys->restitution},
                {"simulate_gravity", phys->simulate_gravity},
                {"is_trigger", phys->is_trigger},
                {"collision_layer", phys->collision_layer},
                {"collider_type", phys->collider_type},
                {"box_half_extents", {phys->box_half_extents.x, phys->box_half_extents.y, phys->box_half_extents.z}},
                {"sphere_radius", phys->sphere_radius},
                {"capsule_radius", phys->capsule_radius},
                {"capsule_half_height", phys->capsule_half_height},
                {"cylinder_radius", phys->cylinder_radius},
                {"cylinder_half_height", phys->cylinder_half_height}
            };
        }

        // Joints are written as an array: an actor in a ragdoll or a linkage is
        // constrained to more than one thing, and get_component<> only ever finds
        // the first, so a single record would silently drop the rest on save.
        {
            json joints_json = json::array();
            for (const auto& comp : actor->get_components()) {
                auto* joint = dynamic_cast<JointComponent*>(comp.get());
                if (!joint) continue;
                joints_json.push_back({
                    {"joint_type", joint->joint_type},
                    {"connected_actor", joint->connected_actor},
                    {"anchor", {joint->anchor.x, joint->anchor.y, joint->anchor.z}},
                    {"axis", {joint->axis.x, joint->axis.y, joint->axis.z}},
                    {"enable_limits", joint->enable_limits},
                    {"limit_min", joint->limit_min},
                    {"limit_max", joint->limit_max},
                    {"min_distance", joint->min_distance},
                    {"max_distance", joint->max_distance},
                    {"swing_angle", joint->swing_angle},
                    {"twist_min", joint->twist_min},
                    {"twist_max", joint->twist_max},
                    {"enable_spring", joint->enable_spring},
                    {"spring_frequency", joint->spring_frequency},
                    {"spring_damping", joint->spring_damping},
                    {"enable_motor", joint->enable_motor},
                    {"motor_target_velocity", joint->motor_target_velocity},
                    {"motor_max_force", joint->motor_max_force},
                    {"friction", joint->friction}
                });
            }
            if (!joints_json.empty()) actor_json["joints"] = joints_json;
        }

        // Audio was never written at all, so a scene with a sound on it came back
        // silent. Everything the component owns goes out, including the spatial
        // settings that decide whether it is audible from more than a metre away.
        if (auto emitter = actor->get_component<ParticleEmitterComponent>()) {
            actor_json["particles"] = {
                {"is_emitting", emitter->is_emitting},
                {"emit_rate", emitter->emit_rate},
                {"burst_count", emitter->burst_count},
                {"burst_interval", emitter->burst_interval},
                {"max_particles", emitter->max_particles},
                {"simulation_space", emitter->simulation_space},
                {"shape", emitter->shape},
                {"shape_radius", emitter->shape_radius},
                {"shape_extents", {emitter->shape_extents.x, emitter->shape_extents.y, emitter->shape_extents.z}},
                {"cone_angle", emitter->cone_angle},
                {"lifetime_min", emitter->lifetime_min},
                {"lifetime_max", emitter->lifetime_max},
                {"speed_min", emitter->speed_min},
                {"speed_max", emitter->speed_max},
                {"size_min", emitter->size_min},
                {"size_max", emitter->size_max},
                {"rotation_speed_min", emitter->rotation_speed_min},
                {"rotation_speed_max", emitter->rotation_speed_max},
                {"start_color", {emitter->start_color.x, emitter->start_color.y, emitter->start_color.z}},
                {"start_alpha", emitter->start_alpha},
                {"end_color", {emitter->end_color.x, emitter->end_color.y, emitter->end_color.z}},
                {"end_alpha", emitter->end_alpha},
                {"size_start_scale", emitter->size_start_scale},
                {"size_end_scale", emitter->size_end_scale},
                {"gravity", emitter->gravity},
                {"drag", emitter->drag},
                {"acceleration", {emitter->acceleration.x, emitter->acceleration.y, emitter->acceleration.z}},
                {"blend_mode", emitter->blend_mode},
                {"texture_path", emitter->texture_path},
                {"intensity", emitter->intensity},
                {"collision_enabled", emitter->collision_enabled},
                {"collision_bounce", emitter->collision_bounce},
                {"collision_layer_mask", emitter->collision_layer_mask},
                {"die_on_collision", emitter->die_on_collision},
                {"sub_emitter_actor", emitter->sub_emitter_actor},
                {"sub_emitter_trigger", emitter->sub_emitter_trigger},
                {"sub_emitter_count", emitter->sub_emitter_count}
            };
        }

        if (auto audio = actor->get_component<AudioComponent>()) {
            actor_json["audio"] = {
                {"file_path", audio->get_file_path()},
                {"looping", audio->get_looping()},
                {"spatial", audio->get_spatial()},
                {"volume", audio->get_volume()},
                {"pitch", audio->get_pitch()},
                {"bus", audio->bus},
                {"doppler_factor", audio->doppler_factor},
                {"min_distance", audio->min_distance},
                {"max_distance", audio->max_distance},
                {"rolloff", audio->rolloff},
                {"attenuation_model", audio->attenuation_model},
                {"cone_inner_angle", audio->cone_inner_angle},
                {"cone_outer_angle", audio->cone_outer_angle},
                {"cone_outer_gain", audio->cone_outer_gain}
            };
        }

        if (auto terrain = actor->get_component<TerrainComponent>()) {
            // The heightmap goes to a sidecar file. A default path is chosen from
            // the actor's name so a terrain added in the editor and saved straight
            // away does not silently lose its sculpting.
            if (terrain->data_path.empty()) {
                std::string safe_name = actor->get_name();
                for (char& c : safe_name) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
                }
                terrain->data_path = "Content/Terrain_" + safe_name + ".terrain";
            }
            terrain->save_data(terrain->data_path);

            json layers = json::array();
            for (int layer = 0; layer < TerrainComponent::kLayerCount; ++layer) {
                layers.push_back({ {"texture", terrain->layer_texture_path[layer]},
                                   {"tiling", terrain->layer_tiling[layer]} });
            }
            actor_json["terrain"] = {
                {"data_path", terrain->data_path},
                {"resolution", terrain->get_resolution()},
                {"world_size", terrain->get_world_size()},
                {"layers", layers},
                {"metallic", terrain->metallic},
                {"collision_layer", terrain->collision_layer},
                {"roughness", terrain->roughness},
                {"foliage_mesh_path", terrain->foliage_mesh_path},
                {"foliage_density", terrain->foliage_density},
                {"foliage_min_scale", terrain->foliage_min_scale},
                {"foliage_max_scale", terrain->foliage_max_scale},
                {"foliage_seed", terrain->foliage_seed},
                {"foliage_max_slope_degrees", terrain->foliage_max_slope_degrees},
                {"foliage_max_instances", terrain->foliage_max_instances}
            };
        }

        if (auto agent = actor->get_component<NavAgentComponent>()) {
            actor_json["nav_agent"] = {
                {"speed", agent->speed},
                {"angular_speed", agent->angular_speed},
                {"stopping_distance", agent->stopping_distance},
                {"waypoint_tolerance", agent->waypoint_tolerance},
                {"rotate_to_face", agent->rotate_to_face},
                {"auto_repath", agent->auto_repath},
                {"repath_interval", agent->repath_interval}
            };
        }

        if (auto lod = actor->get_component<LODGroupComponent>()) {
            json levels = json::array();
            for (const auto& level : lod->levels) {
                levels.push_back({ {"mesh_path", level.mesh_path},
                                   {"screen_height", level.screen_height} });
            }
            actor_json["lod_group"] = {
                {"minimum_detail_distance", lod->minimum_detail_distance},
                {"cull_screen_height", lod->cull_screen_height},
                {"levels", levels}
            };
        }

        // UI canvases, as an array for the same reason joints are: a pause menu and
        // a HUD on the same actor are two canvases with different sort orders.
        {
            json canvases_json = json::array();
            for (const auto& comp : actor->get_components()) {
                auto* canvas = dynamic_cast<UICanvasComponent*>(comp.get());
                if (!canvas) continue;
                json c_json;
                c_json["reference_resolution"] = { canvas->reference_resolution.x, canvas->reference_resolution.y };
                c_json["scale_mode"] = canvas->scale_mode;
                c_json["match_width_or_height"] = canvas->match_width_or_height;
                c_json["sort_order"] = canvas->sort_order;
                c_json["visible"] = canvas->visible;
                c_json["show_in_editor"] = canvas->show_in_editor;
                json widgets = json::array();
                for (const auto& widget : canvas->roots) {
                    if (widget) widgets.push_back(widget_to_json(*widget));
                }
                c_json["widgets"] = widgets;
                canvases_json.push_back(c_json);
            }
            if (!canvases_json.empty()) actor_json["ui_canvases"] = canvases_json;
        }

        if (auto character = actor->get_component<CharacterControllerComponent>()) {
            actor_json["character_controller"] = {
                {"capsule_radius", character->capsule_radius},
                {"capsule_half_height", character->capsule_half_height},
                {"walk_speed", character->walk_speed},
                {"sprint_multiplier", character->sprint_multiplier},
                {"jump_speed", character->jump_speed},
                {"gravity_scale", character->gravity_scale},
                {"max_slope_angle", character->max_slope_angle},
                {"step_height", character->step_height},
                {"use_player_input", character->use_player_input},
                {"mouse_look", character->mouse_look},
                {"mouse_sensitivity", character->mouse_sensitivity},
                {"collision_layer", character->collision_layer}
            };
        }

        return actor_json;
    }
}

// Reads a prefab file and hands back the actor record inside it.
bool read_prefab_record(const std::string& filepath, json& out_record) {
    std::ifstream file(filepath);
    if (!file) return false;
    json prefab_json;
    try {
        file >> prefab_json;
    } catch (const std::exception&) {
        return false;
    }
    if (!prefab_json.contains("actor")) return false;
    out_record = prefab_json["actor"];
    return true;
}

// The keys of `instance` that differ from `prefab`. Compared per top-level key
// rather than deeply: "the physics block was changed" is the granularity a person
// thinks in, and a deep diff would produce a list of individual numbers nobody can
// act on.
json diff_against_prefab(const json& instance, const json& prefab) {
    json overrides = json::object();
    for (auto it = instance.begin(); it != instance.end(); ++it) {
        if (!prefab.contains(it.key()) || prefab[it.key()] != it.value()) {
            overrides[it.key()] = it.value();
        }
    }
    // A key the prefab has and the instance does not means a component was removed
    // from the instance. Recorded explicitly, or reloading would silently bring it
    // back from the prefab.
    for (auto it = prefab.begin(); it != prefab.end(); ++it) {
        if (!instance.contains(it.key())) overrides[it.key()] = nullptr;
    }
    return overrides;
}

// Prefab record with the instance's overrides laid over it.
json merge_overrides(const json& prefab, const json& overrides) {
    json merged = prefab;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (it.value().is_null()) {
            // Explicitly removed on the instance.
            merged.erase(it.key());
        } else {
            merged[it.key()] = it.value();
        }
    }
    return merged;
}

} // namespace

void SceneSerializer::save_scene(const std::string& filepath, const std::vector<std::shared_ptr<Actor>>& actors) {
    json scene_json;
    scene_json["actors"] = json::array();
    for (const auto& actor : actors) {
        json record = actor_to_json(actor.get());

        // A linked instance stores its prefab and only what it changed. That is what
        // makes editing the prefab propagate: anything not listed here is read back
        // from the prefab on load.
        json prefab_record;
        if (!actor->prefab_source.empty() &&
            read_prefab_record(actor->prefab_source, prefab_record)) {
            json entry;
            entry["prefab"] = actor->prefab_source;
            entry["overrides"] = diff_against_prefab(record, prefab_record);
            scene_json["actors"].push_back(entry);
            continue;
        }

        scene_json["actors"].push_back(record);
    }

    // The environment map is a property of the scene, not of the engine: an interior
    // lit by a night sky and one lit by midday sun are different levels, and until
    // now reloading either gave you whatever HDRI happened to be resident.
    if (g_engine) {
        scene_json["environment"] = { {"hdri", g_engine->get_renderer().env_map_path} };
    }

    // Baked lighting goes to a sidecar named after the scene. It is megabytes of
    // float data and it belongs to this scene specifically - another scene's static
    // geometry is somewhere else entirely.
    if (Lightmapper::get().is_baked()) {
        const std::string lightmap_path =
            std::filesystem::path(filepath).replace_extension(".lightmap").string();
        if (Lightmapper::get().save(lightmap_path)) {
            scene_json["lightmap"] = lightmap_path;
        }
    }

    std::ofstream file(filepath);
    if (!file) return;
    file << scene_json.dump(4);
    file.close();
}

namespace {

// Inverse of actor_to_json. Shared by scene loading and prefab instantiation for the
// same reason: one place that knows how to rebuild an actor from its record.
std::shared_ptr<Actor> actor_from_json(const json& actor_json) {
    {
        std::string name = actor_json["name"];
        std::string shape_type = actor_json["shape_type"];
        std::string mesh_path = actor_json.value("mesh_path", "");

        std::shared_ptr<Actor> new_actor;

        if (shape_type == "StaticMesh" && !mesh_path.empty()) {
            new_actor = std::make_shared<Actor>(name);
            new_actor->shape_type = shape_type;
            new_actor->mesh_path = mesh_path;
            
            auto mesh_comp = new_actor->create_component<StaticMeshComponent>("Mesh");
            auto mesh_res = ResourceManager::get().load_async<MeshResource>(mesh_path);
            mesh_comp->set_mesh_resource(mesh_res);
            new_actor->set_root_component(mesh_comp);
        } else if (actor_json.contains("light_type") && actor_json["light_type"] == "DirectionalLight") {
            new_actor = std::make_shared<DirectionalLightActor>(name);
            new_actor->shape_type = shape_type;
        } else {
            // Default EditorPrimitiveActor
            auto prim = std::make_shared<EditorPrimitiveActor>(name, shape_type);
            
            if (actor_json.contains("light_type")) {
                std::string lt = actor_json["light_type"];
                if (lt == "PointLight") prim->create_component<PointLightComponent>("Light");
                else if (lt == "SpotLight") prim->create_component<SpotLightComponent>("Light");
                else if (lt == "AreaLight") prim->create_component<AreaLightComponent>("Light");
                else if (lt == "SkyLight") prim->create_component<SkyLightComponent>("Light");
            }
            new_actor = std::move(prim);
        }

        // Restore properties
        new_actor->material_path = actor_json.value("material_path", "");
        new_actor->is_invisible = actor_json.value("is_invisible", false);
        
        auto color_arr = actor_json["actor_color"];
        new_actor->actor_color = { color_arr[0], color_arr[1], color_arr[2] };
        
        new_actor->metallic = actor_json.value("metallic", 0.0f);
        new_actor->roughness = actor_json.value("roughness", 0.4f);
        new_actor->clearcoat = actor_json.value("clearcoat", 0.0f);
        new_actor->clearcoat_roughness = actor_json.value("clearcoat_roughness", 0.1f);
        new_actor->sheen = actor_json.value("sheen", 0.0f);
        new_actor->subsurface = actor_json.value("subsurface", 0.0f);

        auto t_json = actor_json["transform"];
        Transform& t = new_actor->get_actor_transform();
        t.position = { t_json["position"][0], t_json["position"][1], t_json["position"][2] };
        t.rotation = { t_json["rotation"][0], t_json["rotation"][1], t_json["rotation"][2] };
        t.scale = { t_json["scale"][0], t_json["scale"][1], t_json["scale"][2] };

        // Load material if exists
        if (!new_actor->material_path.empty()) {
            auto mat = std::make_shared<Material>();
            if (mat->load_from_file(new_actor->material_path)) {
                new_actor->assigned_material = mat;
            }
        }

        if (auto* dir_actor = dynamic_cast<DirectionalLightActor*>(new_actor.get())) {
            dir_actor->sky_mode = actor_json.value("sky_mode", dir_actor->sky_mode);
            dir_actor->enable_3d_clouds = actor_json.value("enable_3d_clouds", dir_actor->enable_3d_clouds);
            if (actor_json.contains("void_color")) {
                auto vc = actor_json["void_color"];
                dir_actor->void_color = { vc[0], vc[1], vc[2] };
            }
        }

        if (auto light = new_actor->get_component<LightComponent>()) {
            if (actor_json.contains("light_color")) {
                auto lc = actor_json["light_color"];
                light->color = { lc[0], lc[1], lc[2] };
            }
            light->intensity = actor_json.value("light_intensity", light->intensity);
        }

        if (actor_json.contains("physics")) {
            auto phys = new_actor->create_component<PhysicsAttribute>("Physics");
            auto p_json = actor_json["physics"];
            phys->mass = p_json.value("mass", 1.0f);
            phys->friction = p_json.value("friction", 0.5f);
            phys->restitution = p_json.value("restitution", 0.0f);
            phys->simulate_gravity = p_json.value("simulate_gravity", true);
            phys->is_trigger = p_json.value("is_trigger", false);
            phys->collision_layer = p_json.value("collision_layer", 0);
            phys->collider_type = p_json.value("collider_type", 0);
            if (p_json.contains("box_half_extents")) {
                phys->box_half_extents = { p_json["box_half_extents"][0], p_json["box_half_extents"][1], p_json["box_half_extents"][2] };
            }
            phys->sphere_radius = p_json.value("sphere_radius", 0.5f);
            phys->capsule_radius = p_json.value("capsule_radius", 0.35f);
            phys->capsule_half_height = p_json.value("capsule_half_height", 0.5f);
            phys->cylinder_radius = p_json.value("cylinder_radius", 0.5f);
            phys->cylinder_half_height = p_json.value("cylinder_half_height", 0.5f);
        }

        if (actor_json.contains("character_controller")) {
            auto character = new_actor->create_component<CharacterControllerComponent>("CharacterController");
            auto c_json = actor_json["character_controller"];
            character->capsule_radius = c_json.value("capsule_radius", 0.3f);
            character->capsule_half_height = c_json.value("capsule_half_height", 0.6f);
            character->walk_speed = c_json.value("walk_speed", 5.0f);
            character->sprint_multiplier = c_json.value("sprint_multiplier", 1.8f);
            character->jump_speed = c_json.value("jump_speed", 4.5f);
            character->gravity_scale = c_json.value("gravity_scale", 1.0f);
            character->max_slope_angle = c_json.value("max_slope_angle", 45.0f);
            character->step_height = c_json.value("step_height", 0.4f);
            character->use_player_input = c_json.value("use_player_input", true);
            character->mouse_look = c_json.value("mouse_look", true);
            character->mouse_sensitivity = c_json.value("mouse_sensitivity", 0.0025f);
            character->collision_layer = c_json.value("collision_layer", 0);
        }

        if (actor_json.contains("joints")) {
            for (const auto& j_json : actor_json["joints"]) {
                auto joint = new_actor->create_component<JointComponent>("Joint");
                joint->joint_type = j_json.value("joint_type", (int)JointComponent::Joint_Hinge);
                joint->connected_actor = j_json.value("connected_actor", std::string());
                if (j_json.contains("anchor")) {
                    joint->anchor = { j_json["anchor"][0], j_json["anchor"][1], j_json["anchor"][2] };
                }
                if (j_json.contains("axis")) {
                    joint->axis = { j_json["axis"][0], j_json["axis"][1], j_json["axis"][2] };
                }
                joint->enable_limits = j_json.value("enable_limits", false);
                joint->limit_min = j_json.value("limit_min", -45.0f);
                joint->limit_max = j_json.value("limit_max", 45.0f);
                joint->min_distance = j_json.value("min_distance", -1.0f);
                joint->max_distance = j_json.value("max_distance", -1.0f);
                joint->swing_angle = j_json.value("swing_angle", 45.0f);
                joint->twist_min = j_json.value("twist_min", -45.0f);
                joint->twist_max = j_json.value("twist_max", 45.0f);
                joint->enable_spring = j_json.value("enable_spring", false);
                joint->spring_frequency = j_json.value("spring_frequency", 2.0f);
                joint->spring_damping = j_json.value("spring_damping", 1.0f);
                joint->enable_motor = j_json.value("enable_motor", false);
                joint->motor_target_velocity = j_json.value("motor_target_velocity", 0.0f);
                joint->motor_max_force = j_json.value("motor_max_force", 1000.0f);
                joint->friction = j_json.value("friction", 0.0f);
            }
        }

        if (actor_json.contains("particles")) {
            auto emitter = new_actor->create_component<ParticleEmitterComponent>("Particles");
            const auto& p_json = actor_json["particles"];
            emitter->is_emitting = p_json.value("is_emitting", true);
            emitter->emit_rate = p_json.value("emit_rate", 30.0f);
            emitter->burst_count = p_json.value("burst_count", 0);
            emitter->burst_interval = p_json.value("burst_interval", 1.0f);
            emitter->max_particles = p_json.value("max_particles", 1000);
            emitter->simulation_space = p_json.value("simulation_space", 1);
            emitter->shape = p_json.value("shape", (int)ParticleEmitterComponent::Shape_Cone);
            emitter->shape_radius = p_json.value("shape_radius", 0.5f);
            if (p_json.contains("shape_extents")) {
                emitter->shape_extents = { p_json["shape_extents"][0], p_json["shape_extents"][1], p_json["shape_extents"][2] };
            }
            emitter->cone_angle = p_json.value("cone_angle", 25.0f);
            emitter->lifetime_min = p_json.value("lifetime_min", 1.0f);
            emitter->lifetime_max = p_json.value("lifetime_max", 2.0f);
            emitter->speed_min = p_json.value("speed_min", 2.0f);
            emitter->speed_max = p_json.value("speed_max", 4.0f);
            emitter->size_min = p_json.value("size_min", 0.15f);
            emitter->size_max = p_json.value("size_max", 0.3f);
            emitter->rotation_speed_min = p_json.value("rotation_speed_min", -2.0f);
            emitter->rotation_speed_max = p_json.value("rotation_speed_max", 2.0f);
            if (p_json.contains("start_color")) {
                emitter->start_color = { p_json["start_color"][0], p_json["start_color"][1], p_json["start_color"][2] };
            }
            emitter->start_alpha = p_json.value("start_alpha", 1.0f);
            if (p_json.contains("end_color")) {
                emitter->end_color = { p_json["end_color"][0], p_json["end_color"][1], p_json["end_color"][2] };
            }
            emitter->end_alpha = p_json.value("end_alpha", 0.0f);
            emitter->size_start_scale = p_json.value("size_start_scale", 1.0f);
            emitter->size_end_scale = p_json.value("size_end_scale", 0.2f);
            emitter->gravity = p_json.value("gravity", 1.5f);
            emitter->drag = p_json.value("drag", 0.6f);
            if (p_json.contains("acceleration")) {
                emitter->acceleration = { p_json["acceleration"][0], p_json["acceleration"][1], p_json["acceleration"][2] };
            }
            emitter->blend_mode = p_json.value("blend_mode", 0);
            emitter->texture_path = p_json.value("texture_path", std::string());
            emitter->intensity = p_json.value("intensity", 2.0f);
            emitter->collision_enabled = p_json.value("collision_enabled", false);
            emitter->collision_bounce = p_json.value("collision_bounce", 0.35f);
            emitter->collision_layer_mask = p_json.value("collision_layer_mask", 0xFFFFFFFFu);
            emitter->die_on_collision = p_json.value("die_on_collision", false);
            emitter->sub_emitter_actor = p_json.value("sub_emitter_actor", std::string());
            emitter->sub_emitter_trigger = p_json.value("sub_emitter_trigger", 0);
            emitter->sub_emitter_count = p_json.value("sub_emitter_count", 8);
        }

        if (actor_json.contains("audio")) {
            auto audio = new_actor->create_component<AudioComponent>("Audio");
            const auto& a_json = actor_json["audio"];
            // Everything else is set before the file, because set_file_path is what
            // creates the sound and the creation reads the bus.
            audio->bus = a_json.value("bus", 0);
            audio->doppler_factor = a_json.value("doppler_factor", 0.0f);
            audio->min_distance = a_json.value("min_distance", 1.0f);
            audio->max_distance = a_json.value("max_distance", 60.0f);
            audio->rolloff = a_json.value("rolloff", 1.0f);
            audio->attenuation_model = a_json.value("attenuation_model", 1);
            audio->cone_inner_angle = a_json.value("cone_inner_angle", 360.0f);
            audio->cone_outer_angle = a_json.value("cone_outer_angle", 360.0f);
            audio->cone_outer_gain = a_json.value("cone_outer_gain", 0.25f);
            audio->set_looping(a_json.value("looping", false));
            audio->set_spatial(a_json.value("spatial", true));
            audio->set_volume(a_json.value("volume", 1.0f));
            audio->set_pitch(a_json.value("pitch", 1.0f));
            audio->set_file_path(a_json.value("file_path", std::string()));
        }

        if (actor_json.contains("terrain")) {
            auto terrain = new_actor->create_component<TerrainComponent>("Terrain");
            const auto& t_json = actor_json["terrain"];

            // Shape first: loading the sidecar overrides it, but a missing sidecar
            // must still leave a terrain of the right size rather than the default.
            terrain->resize(t_json.value("resolution", 128), t_json.value("world_size", 200.0f));

            terrain->data_path = t_json.value("data_path", std::string());
            if (!terrain->data_path.empty()) terrain->load_data(terrain->data_path);

            if (t_json.contains("layers")) {
                int layer = 0;
                for (const auto& layer_json : t_json["layers"]) {
                    if (layer >= TerrainComponent::kLayerCount) break;
                    terrain->layer_texture_path[layer] = layer_json.value("texture", std::string());
                    terrain->layer_tiling[layer] = layer_json.value("tiling", 40.0f);
                    ++layer;
                }
            }
            terrain->metallic = t_json.value("metallic", 0.0f);
            terrain->collision_layer = t_json.value("collision_layer", 0);
            terrain->roughness = t_json.value("roughness", 0.85f);
            terrain->foliage_mesh_path = t_json.value("foliage_mesh_path", std::string());
            terrain->foliage_density = t_json.value("foliage_density", 0.6f);
            terrain->foliage_min_scale = t_json.value("foliage_min_scale", 0.8f);
            terrain->foliage_max_scale = t_json.value("foliage_max_scale", 1.4f);
            terrain->foliage_seed = t_json.value("foliage_seed", 1337);
            terrain->foliage_max_slope_degrees = t_json.value("foliage_max_slope_degrees", 35.0f);
            terrain->foliage_max_instances = t_json.value("foliage_max_instances", 20000);
        }

        if (actor_json.contains("nav_agent")) {
            auto agent = new_actor->create_component<NavAgentComponent>("Nav Agent");
            const auto& a_json = actor_json["nav_agent"];
            agent->speed = a_json.value("speed", 3.5f);
            agent->angular_speed = a_json.value("angular_speed", 540.0f);
            agent->stopping_distance = a_json.value("stopping_distance", 0.4f);
            agent->waypoint_tolerance = a_json.value("waypoint_tolerance", 0.45f);
            agent->rotate_to_face = a_json.value("rotate_to_face", true);
            agent->auto_repath = a_json.value("auto_repath", true);
            agent->repath_interval = a_json.value("repath_interval", 0.5f);
        }

        if (actor_json.contains("lod_group")) {
            auto lod = new_actor->create_component<LODGroupComponent>("LOD Group");
            const auto& l_json = actor_json["lod_group"];
            lod->minimum_detail_distance = l_json.value("minimum_detail_distance", 0.0f);
            lod->cull_screen_height = l_json.value("cull_screen_height", 0.0f);
            if (l_json.contains("levels")) {
                for (const auto& level_json : l_json["levels"]) {
                    LODGroupComponent::LODLevel level;
                    level.mesh_path = level_json.value("mesh_path", std::string());
                    level.screen_height = level_json.value("screen_height", 0.25f);
                    lod->levels.push_back(level);
                }
            }
        }

        if (actor_json.contains("ui_canvases")) {
            for (const auto& c_json : actor_json["ui_canvases"]) {
                auto canvas = new_actor->create_component<UICanvasComponent>("UI Canvas");
                if (c_json.contains("reference_resolution")) {
                    canvas->reference_resolution = vec2_from(c_json["reference_resolution"],
                                                             canvas->reference_resolution);
                }
                canvas->scale_mode = c_json.value("scale_mode", canvas->scale_mode);
                canvas->match_width_or_height = c_json.value("match_width_or_height", canvas->match_width_or_height);
                canvas->sort_order = c_json.value("sort_order", canvas->sort_order);
                canvas->visible = c_json.value("visible", true);
                canvas->show_in_editor = c_json.value("show_in_editor", true);
                if (c_json.contains("widgets")) {
                    for (const auto& w_json : c_json["widgets"]) {
                        if (auto widget = widget_from_json(w_json)) {
                            canvas->roots.push_back(std::move(widget));
                        }
                    }
                }
            }
        }

        // Attached last, so the script's first tick sees a fully built actor:
        // transform, material and physics body are all in place by now.
        if (actor_json.contains("script")) {
            const auto& script_json = actor_json["script"];
            std::string script_type = script_json.value("type", "cminus");
            std::string script_path = script_json.value("path", "");
            if (!script_path.empty()) {
                if (script_type == "lua") {
                    auto* lua_component = new_actor->create_component<LuaScriptComponent>("Script", script_path);
                    if (script_json.contains("properties")) {
                        for (const auto& entry : script_json["properties"]) {
                            LuaAPI::ScriptProperty property;
                            property.name = entry.value("name", std::string());
                            if (property.name.empty()) continue;
                            const std::string type = entry.value("type", std::string("number"));
                            if (type == "string") {
                                property.type = LuaAPI::ScriptProperty::Type::String;
                                property.string_value = entry.value("value", std::string());
                            } else if (type == "boolean") {
                                property.type = LuaAPI::ScriptProperty::Type::Boolean;
                                property.boolean_value = entry.value("value", false);
                            } else {
                                property.type = LuaAPI::ScriptProperty::Type::Number;
                                property.number_value = entry.value("value", 0.0);
                            }
                            lua_component->property_overrides.push_back(property);
                        }
                        // The component already loaded the script in its constructor,
                        // so the overrides have to be pushed into the live VM now.
                        lua_component->apply_property_overrides();
                    }
                } else if (script_type == "cpp") {
                    new_actor->create_component<CppScriptComponent>("Script", script_path);
                } else {
                    new_actor->create_component<CMinusComponent>("Script", script_path);
                }
            }
        }

        new_actor->begin_play();
        return new_actor;
    }
}

} // namespace

bool SceneSerializer::apply_actor_to_prefab(const std::string& filepath, Actor* actor) {
    if (!actor || filepath.empty()) return false;
    // Written through save_prefab so the file keeps its version header and the
    // format stays in one place.
    return save_prefab(filepath, actor);
}

std::vector<std::string> SceneSerializer::list_prefab_overrides(Actor* actor) {
    std::vector<std::string> names;
    if (!actor || actor->prefab_source.empty()) return names;

    json prefab_record;
    if (!read_prefab_record(actor->prefab_source, prefab_record)) return names;

    const json overrides = diff_against_prefab(actor_to_json(actor), prefab_record);
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        names.push_back(it.key());
    }
    return names;
}

bool SceneSerializer::load_scene(const std::string& filepath, std::vector<std::shared_ptr<Actor>>& out_actors) {
    std::ifstream file(filepath);
    if (!file) return false;

    json scene_json;
    file >> scene_json;
    file.close();

    if (scene_json.contains("environment") && g_engine) {
        std::string hdri = scene_json["environment"].value("hdri", std::string());
        if (hdri.empty()) {
            g_engine->get_renderer().clear_environment_map();
        } else {
            g_engine->get_renderer().load_environment_map(hdri);
        }
    }

    out_actors.clear();
    if (!scene_json.contains("actors")) return false;

    for (const auto& actor_json : scene_json["actors"]) {
        if (actor_json.contains("prefab")) {
            const std::string prefab_path = actor_json["prefab"].get<std::string>();
            json prefab_record;
            if (!read_prefab_record(prefab_path, prefab_record)) {
                std::cerr << "[Prefab] " << prefab_path << " is missing; the instance "
                             "it backs cannot be loaded." << std::endl;
                continue;
            }
            const json overrides = actor_json.value("overrides", json::object());
            if (auto a = actor_from_json(merge_overrides(prefab_record, overrides))) {
                a->prefab_source = prefab_path;
                out_actors.push_back(a);
            }
            continue;
        }
        if (auto a = actor_from_json(actor_json)) out_actors.push_back(a);
    }

    // Baked lighting last: it hands each actor its lightmap uvs by name, so every
    // actor has to exist first.
    Lightmapper::get().clear();
    if (scene_json.contains("lightmap")) {
        const std::string lightmap_path = scene_json["lightmap"].get<std::string>();
        if (!Lightmapper::get().load(lightmap_path)) {
            std::cerr << "[Lightmap] " << lightmap_path
                      << " could not be read; the scene will fall back to probe light."
                      << std::endl;
        }
    }
    Lightmapper::get().apply_to_actors(out_actors);

    return true;
}

// --- Prefabs ---------------------------------------------------------------
// A prefab is one actor's serialised record in its own file: spawn it, edit it,
// save it, and every later instantiation starts from that configuration. Because it
// reuses actor_to_json/actor_from_json it carries everything a scene entry does -
// mesh, material, lights, physics body, transform.

bool SceneSerializer::save_prefab(const std::string& filepath, Actor* actor) {
    if (!actor) return false;
    json prefab_json;
    prefab_json["prefab_version"] = 1;
    prefab_json["actor"] = actor_to_json(actor);

    std::ofstream file(filepath);
    if (!file) return false;
    file << prefab_json.dump(4);
    return true;
}

std::shared_ptr<Actor> SceneSerializer::load_prefab(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file) return nullptr;

    json prefab_json;
    try {
        file >> prefab_json;
    } catch (const std::exception& e) {
        std::cerr << "[Prefab] Failed to parse " << filepath << ": " << e.what() << std::endl;
        return nullptr;
    }
    if (!prefab_json.contains("actor")) {
        std::cerr << "[Prefab] " << filepath << " has no actor record." << std::endl;
        return nullptr;
    }
    try {
        return actor_from_json(prefab_json["actor"]);
    } catch (const std::exception& e) {
        std::cerr << "[Prefab] Failed to instantiate " << filepath << ": " << e.what() << std::endl;
        return nullptr;
    }
}
