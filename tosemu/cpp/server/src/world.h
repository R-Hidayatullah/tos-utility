// Maps and the world that owns them.
//
// A Map is the unit of visibility: the client is only ever told about actors
// on the map it is standing in, so every broadcast is scoped to one. That also
// makes the map the natural home for spawning, since the client's spawn table
// (ies_mongen/anchor_<map>.ies) is per-map too.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "actor.h"
#include "gamedata.h"
#include "packet.h"

namespace tos {

struct Session;

namespace game {

class World;

class Map {
public:
    Map(int32_t id, std::string class_name, World* world)
        : id_(id), class_name_(std::move(class_name)), world_(world) {}

    int32_t id() const { return id_; }
    const std::string& class_name() const { return class_name_; }

    void add(const std::shared_ptr<Actor>& a);
    void remove(uint32_t handle);
    std::shared_ptr<Actor> get(uint32_t handle) const;

    std::vector<std::shared_ptr<Actor>> actors() const;
    std::vector<std::shared_ptr<Character>> characters() const;
    size_t character_count() const;

    // Send a finished packet to every character here, optionally skipping one.
    void broadcast(const Bytes& pkt, uint32_t except_handle = 0) const;
    // Same, against a recipient list fetched once. The tick sends dozens of
    // packets per pass, and re-copying the actor table for each of them is
    // the bulk of its cost with a few hundred monsters on the map.
    void broadcast_to(const std::vector<std::shared_ptr<Character>>& to,
                      const Bytes& pkt, uint32_t except_handle = 0) const;

    // Populate from the client's own spawn table. Returns how many spawned.
    int spawn_from_anchors(int limit = 0);
    // Place `count` copies of one monster class around a point. The spawned
    // monsters are returned rather than announced, because announcing needs a
    // ServerContext and the map has no reason to know about connections.
    std::vector<std::shared_ptr<Monster>> spawn(int32_t monster_id,
                                                const Position& at,
                                                float spread, int count);

    // One simulation step. Wandering, aggro, monster attacks and respawns all
    // happen here; anything that has to reach a client is returned through
    // `ctx` rather than sent from the map itself.
    void tick(double now);

private:
    std::shared_ptr<Monster> make_monster(int32_t monster_id, const Position& at,
                                          float range);

    int32_t id_;
    std::string class_name_;
    World* world_;

    mutable std::mutex mu_;
    std::unordered_map<uint32_t, std::shared_ptr<Actor>> actors_;
    bool anchors_spawned_ = false;
    // Movement integrates over real elapsed time, not over ticks, so the tick
    // rate can change without changing how fast anything moves.
    double last_tick_ = 0;
};

class World {
public:
    World(const Table& table, const data::GameData& gd)
        : table_(table), data_(gd) {}

    const Table& table() const { return table_; }
    const data::GameData& data() const { return data_; }
    HandleAllocator& handles() { return handles_; }

    // Maps are created on demand: the client can ask for any of the 543 in
    // map.ies and only a handful are ever occupied.
    Map* map(int32_t id);
    Map* map_by_class_name(const std::string& name);

    void tick();

    // Every connected session, copied out so a slow socket cannot hold the
    // world lock while it drains.
    void add_session(const std::shared_ptr<Session>& s);
    void remove_session(uint32_t id);
    std::vector<std::shared_ptr<Session>> sessions(uint32_t except = 0) const;

    // Server time base the client anchors its clock to, in seconds.
    float server_time() const;

    std::mt19937& rng() { return rng_; }

private:
    const Table& table_;
    const data::GameData& data_;
    HandleAllocator handles_;

    mutable std::mutex mu_;
    std::unordered_map<int32_t, std::unique_ptr<Map>> maps_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::mt19937 rng_{0xC0FFEE};
};

}  // namespace game
}  // namespace tos
