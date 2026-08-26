#include "world/actor.hpp"
#include <iostream>

extern "C" {

void on_begin_play(Actor* self) {
    std::cout << "[C++ Script] " << self->get_name() << " Begin Play!" << std::endl;
}

void on_tick(Actor* self, float delta_time) {
    // Rotate the actor over time
    Transform& trans = self->get_actor_transform();
    trans.rotation.y += 1.0f * delta_time;
    trans.rotation.x += 0.5f * delta_time;
}

}
