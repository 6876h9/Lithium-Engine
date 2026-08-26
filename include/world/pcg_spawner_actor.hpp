#pragma once

#include "world/actor.hpp"
#include "world/static_mesh_component.hpp"
#include "core/noise.hpp"
#include <unordered_map>
#include <vector>
#include <string>

// Represents a 2D grid cell in the massive world
struct PCGChunkId {
    int x, z;
    bool operator==(const PCGChunkId& other) const {
        return x == other.x && z == other.z;
    }
};

namespace std {
    template<> struct hash<PCGChunkId> {
        size_t operator()(const PCGChunkId& id) const {
            return std::hash<int>()(id.x) ^ (std::hash<int>()(id.z) << 1);
        }
    };
}

class PCGSpawnerActor : public Actor {
public:
    PCGSpawnerActor(const std::string& name, const std::string& mesh_path, double chunk_sz = 100.0) 
        : Actor(name), mesh_file(mesh_path), chunk_size(chunk_sz) {
    }

    void tick(float delta_time) override {
        Actor::tick(delta_time);
        
        // Note: In a real architecture, Engine provides the camera position to tick().
        // For simplicity, we assume the camera_pos is provided via a global or passed in.
        // We will implement `generate_around_camera` directly and call it from Engine.
    }

    void generate_around_camera(const DVector3& camera_pos) {
        // Determine camera chunk
        int cam_cx = static_cast<int>(std::floor(camera_pos.x / chunk_size));
        int cam_cz = static_cast<int>(std::floor(camera_pos.z / chunk_size));

        std::vector<PCGChunkId> active_chunks;

        // 3x3 grid around camera
        for (int x = -1; x <= 1; ++x) {
            for (int z = -1; z <= 1; ++z) {
                PCGChunkId id = {cam_cx + x, cam_cz + z};
                active_chunks.push_back(id);
                
                if (loaded_chunks.find(id) == loaded_chunks.end()) {
                    generate_chunk(id);
                }
            }
        }

        // Unload chunks that are out of range
        std::vector<PCGChunkId> to_unload;
        for (const auto& pair : loaded_chunks) {
            bool is_active = false;
            for (const auto& active : active_chunks) {
                if (pair.first == active) {
                    is_active = true;
                    break;
                }
            }
            if (!is_active) {
                to_unload.push_back(pair.first);
            }
        }

        for (const auto& id : to_unload) {
            unload_chunk(id);
        }
    }

private:
    std::string mesh_file;
    double chunk_size;

    struct ChunkData {
        std::vector<StaticMeshComponent*> components;
    };

    std::unordered_map<PCGChunkId, ChunkData> loaded_chunks;

    void generate_chunk(PCGChunkId id) {
        ChunkData data;
        
        double start_x = id.x * chunk_size;
        double start_z = id.z * chunk_size;

        // Grid-based sampling within the chunk
        double step = 10.0; // Distance between potential spawn points
        
        for (double x = 0; x < chunk_size; x += step) {
            for (double z = 0; z < chunk_size; z += step) {
                double world_x = start_x + x;
                double world_z = start_z + z;

                // Evaluate Simplex Noise
                // Scale coordinates for noise frequency
                double nx = world_x * 0.02;
                double nz = world_z * 0.02;
                
                double val = SimplexNoise::noise(nx, nz);

                if (val > 0.4) { // Threshold for spawning
                    // Spawn a mesh component
                    StaticMeshComponent* comp = create_component<StaticMeshComponent>(mesh_file);
                    
                    // LWC: The offset is local to the spawner actor (which is at 0,0,0 usually).
                    // Wait, StaticMeshComponent renders based on its transform, multiplied by owner transform.
                    // We must ensure the component has the correct local position relative to the actor!
                    comp->transform.position = DVector3{world_x, 0.0, world_z};
                    
                    // Add random rotation and scale for variety
                    double random_scale = 0.5 + (std::abs(val) * 0.5);
                    comp->transform.scale = {static_cast<float>(random_scale), static_cast<float>(random_scale), static_cast<float>(random_scale)};
                    comp->transform.rotation = {0.0f, static_cast<float>(val * 3.14159), 0.0f};

                    data.components.push_back(comp);
                }
            }
        }

        loaded_chunks[id] = data;
    }

    void unload_chunk(PCGChunkId id) {
        ChunkData& data = loaded_chunks[id];
        for (auto* c : data.components) {
            remove_component(c);
        }
        loaded_chunks.erase(id);
    }
};
