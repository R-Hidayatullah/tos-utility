#include "world.h"

#include <algorithm>
#include <cmath>

#include "server.h"
#include "zone_send.h"

namespace tos::game {

// ---- map ---------------------------------------------------------------

void Map::add(const std::shared_ptr<Actor>& a) {
    std::lock_guard<std::mutex> lk(mu_);
    a->map = this;
    actors_[a->handle] = a;
}

void Map::remove(uint32_t handle) {
    std::lock_guard<std::mutex> lk(mu_);
    actors_.erase(handle);
}

std::shared_ptr<Actor> Map::get(uint32_t handle) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = actors_.find(handle);
    return it == actors_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Actor>> Map::actors() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::shared_ptr<Actor>> out;
    out.reserve(actors_.size());
    for (const auto& kv : actors_) out.push_back(kv.second);
    return out;
}

std::vector<std::shared_ptr<Character>> Map::characters() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::shared_ptr<Character>> out;
    for (const auto& kv : actors_)
        if (kv.second->is_character())
            out.push_back(std::static_pointer_cast<Character>(kv.second));
    return out;
}

size_t Map::character_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& kv : actors_)
        if (kv.second->is_character()) ++n;
    return n;
}

void Map::broadcast(const Bytes& pkt, uint32_t except_handle) const {
    broadcast_to(characters(), pkt, except_handle);
}

void Map::broadcast_to(const std::vector<std::shared_ptr<Character>>& to,
                       const Bytes& pkt, uint32_t except_handle) const {
    // The recipient list is gathered under the lock and written to outside it:
    // a blocked socket must not stall the map.
    for (const auto& c : to) {
        if (c->handle == except_handle) continue;
        auto sess = c->live_session();
        if (!sess || !sess->in_world) continue;
        sess->send_raw(pkt);
    }
}

std::shared_ptr<Monster> Map::make_monster(int32_t monster_id,
                                           const Position& at, float range) {
    const data::MonsterData* md = world_->data().monster(monster_id);
    if (!md) return nullptr;

    auto m = std::make_shared<Monster>(&world_->data().properties);
    m->handle = world_->handles().monster();
    m->monster_id = monster_id;
    m->data = md;
    m->name = md->name.empty() ? md->class_name : md->name;
    m->level = md->level;
    m->position = at;
    m->anchor = at;
    m->anchor_range = range;
    // SCR_Get_MON_MSPD returns WlkMSPD by default and only reads RunMSPD when
    // MOVE_TYPE_CURRENT says the monster is running, so the resting speed is
    // the walk speed. A zero stays zero -- that is how the table marks
    // something that cannot move -- and it matches the wire: of the 122
    // entities in capture_1785545696.bin every one whose id resolves carries
    // its WlkMSPD, never its RunMSPD, and 46 of them carry 0.
    m->walk_speed = md->walk_speed;
    m->run_speed = md->run_speed > 0 ? md->run_speed : md->walk_speed;
    m->move_speed = m->walk_speed;

    // monster.ies has no HP column -- the live game derives it from level and
    // the stat tables. A level-scaled figure keeps combat meaningful without
    // pretending to a precision the data does not support.
    m->max_hp = 40 + md->level * 20;
    m->hp = m->max_hp;
    m->attack_power = 2 + md->level;

    // Only Faction="Monster" rows take aggro. Keying this off the NPC table
    // alone was not enough: monster_npc.ies is 93% Faction="Neutral" and
    // monster.ies carries 137 Neutral rows plus summons and crystals of its
    // own, none of which should ever chase a player.
    if (md->hostile() && m->walk_speed > 0) {
        m->aggro_range = md->search_range > 0 ? md->search_range : 120.0f;
        m->reach = md->attack_range > 0 ? md->attack_range : 25.0f;
    }

    m->scale = md->scale > 0 ? md->scale : 1.0f;

    // Exactly two properties, Range then Scale, because that is exactly what
    // the live server sends: all 162 ZC_ENTER_MONSTER in
    // relay/dumps/capture_20260801_214001.bin carry a 16-byte blob holding
    // Monster.Range (6314) and Monster.Scale (6364) and nothing else. Scale
    // matches the monster row on all 130 spawns whose id resolves.
    //
    // HP, MHP, MSPD and Lv used to go here. The client does not read them from
    // this blob -- hp/maxhp are struct fields at +0x26/+0x2A, MSPD at +0x36,
    // and level sits inside the 20 bytes at +0x3E its handler skips -- so they
    // were only ever making our packet longer than the real one.
    m->properties.set("Range", m->gen_range);
    m->properties.set("Scale", m->scale);
    return m;
}

int Map::spawn_from_anchors(int limit) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (anchors_spawned_) return 0;
        anchors_spawned_ = true;
    }

    const data::GameData& gd = world_->data();
    const std::vector<data::Anchor>* anchors = gd.anchors(class_name_);
    if (!anchors) return 0;
    const auto* gen_types = gd.gen_types(class_name_);

    int n = 0;
    for (const data::Anchor& a : *anchors) {
        if (limit > 0 && n >= limit) break;

        // GenType first. The anchor's NPCID is a stale column: where both
        // resolve they disagree more often than they agree, and the capture
        // sides with GenType every time (see data::GenTypeEntry).
        const data::GenTypeEntry* g = nullptr;
        if (gen_types) {
            auto it = gen_types->find(a.gen_type);
            if (it != gen_types->end()) g = &it->second;
        }

        const data::MonsterData* md =
            g ? gd.monster(g->class_type) : nullptr;
        if (!md && a.npc_id) md = gd.monster(a.npc_id);
        if (!md) continue;

        auto m = make_monster(md->id, {a.x, a.y, a.z}, a.range);
        if (!m) continue;
        m->direction = Direction::from_angle(a.direction * 3.14159265f / 180.0f);

        // The generator row overrides the monster row: it carries the name
        // this particular spawn shows and the dialog scripts the client asks
        // for when you click it, which is what makes a shop NPC a shop NPC
        // rather than an anonymous copy of its class.
        if (g) {
            m->gen_type = g->gen_type;
            // The generator's Range column, not AnchorRange and not GenRange.
            // It rides out as the Monster.Range property and agrees with the
            // wire on 67 of the 70 captured spawns whose generator resolves --
            // the three that differ are ids that exist in two maps' tables and
            // are a lookup collision in the check, not a disagreement.
            m->gen_range = g->range;
            m->properties.set("Range", m->gen_range);
            if (!g->name.empty()) m->name = g->name;
            m->unique_name = g->unique_name;
            m->dialog = g->dialog;
            m->enter_script = g->enter;
            m->leave_script = g->leave;
            if (g->level > 1) m->level = g->level;
            if (g->respawn_time > 0) m->respawn_delay = g->respawn_time / 1000.0;
            // GenRange -1 means "exactly here"; the anchor radius is the
            // fallback for generators that do not pin the spot.
            if (g->gen_range >= 0 && g->gen_range < m->anchor_range)
                m->anchor_range = g->gen_range;
        }
        if (m->name.empty() && !a.name.empty()) m->name = a.name;
        add(m);
        ++n;
    }
    return n;
}

std::vector<std::shared_ptr<Monster>> Map::spawn(int32_t monster_id,
                                                 const Position& at,
                                                 float spread, int count) {
    std::vector<std::shared_ptr<Monster>> out;
    std::uniform_real_distribution<float> jitter(-spread, spread);
    for (int i = 0; i < count; ++i) {
        Position p = at;
        if (spread > 0) {
            p.x += jitter(world_->rng());
            p.z += jitter(world_->rng());
        }
        auto m = make_monster(monster_id, p, spread > 0 ? spread : 60.0f);
        if (!m) break;
        add(m);
        out.push_back(std::move(m));
    }
    return out;
}

void Map::tick(double now) {
    // Nothing to simulate with nobody watching, and skipping it keeps an idle
    // server at zero CPU.
    std::vector<std::shared_ptr<Character>> pcs = characters();
    if (pcs.empty()) {
        last_tick_ = 0;
        return;
    }

    // MSPD is world units per SECOND -- the client divides a distance by it to
    // get a travel time -- so a step is speed * elapsed, never speed per tick.
    // Stepping a whole speed per tick ran every monster at 2x on this 2 Hz
    // loop and would have changed their speed again if the tick rate moved.
    double dt = last_tick_ > 0 ? now - last_tick_ : 0.0;
    last_tick_ = now;
    // A tick that came back from a long stall must not teleport anything.
    if (dt > 1.0) dt = 1.0;

    std::vector<std::shared_ptr<Monster>> movers;
    std::vector<std::shared_ptr<Monster>> speed_changed;
    std::vector<std::shared_ptr<Monster>> respawned;
    std::vector<std::pair<std::shared_ptr<Monster>, std::shared_ptr<Character>>> hits;
    for (const auto& a : actors()) {
        if (a->is_character()) continue;
        auto m = std::static_pointer_cast<Monster>(a);

        if (m->despawned) {
            if (m->respawn_at > 0 && now >= m->respawn_at) {
                m->despawned = false;
                m->respawn_at = 0;
                m->hp = m->max_hp;
                m->position = m->anchor;
                respawned.push_back(m);
            }
            continue;
        }
        if (m->dead()) continue;

        // ---- aggro. Pick the nearest live character inside search range and
        // walk at it; swing once in reach. Losing the target when it dies or
        // strays keeps a monster from chasing a corpse forever.
        if (m->aggro_range > 0) {
            std::shared_ptr<Character> target;
            if (m->target) {
                for (const auto& c : pcs)
                    if (c->handle == m->target && !c->dead()) target = c;
            }
            float best = m->aggro_range * m->aggro_range;
            if (!target) {
                for (const auto& c : pcs) {
                    if (c->dead()) continue;
                    float d2 = c->position.dist_sq(m->position);
                    if (d2 > best) continue;
                    best = d2;
                    target = c;
                }
            }

            if (target) {
                float d2 = target->position.dist_sq(m->position);
                // Give up well outside the range it noticed you at, so a
                // monster does not flicker in and out of pursuit.
                if (d2 > m->aggro_range * m->aggro_range * 4) {
                    m->target = 0;
                } else {
                    m->target = target->handle;
                    float dx = target->position.x - m->position.x;
                    float dz = target->position.z - m->position.z;
                    float len = std::sqrt(dx * dx + dz * dz);
                    if (len > 0.001f) m->direction = {dx / len, dz / len};

                    if (d2 > m->reach * m->reach) {
                        // A chase is the client's MOVE_TYPE_CURRENT == 2, so
                        // it runs rather than walks.
                        if (m->move_speed != m->run_speed) {
                            m->move_speed = m->run_speed;
                            speed_changed.push_back(m);
                        }
                        float step = std::min(float(m->move_speed * dt), len);
                        m->position.x += m->direction.cos * step;
                        m->position.z += m->direction.sin * step;
                        m->moving = true;
                        movers.push_back(m);
                    } else if (now >= m->next_attack) {
                        m->moving = false;
                        m->next_attack = now + 2.0;
                        hits.emplace_back(m, target);
                        movers.push_back(m);
                    }
                    continue;
                }
            }
        }

        // A zero WlkMSPD is the table saying this thing cannot move at all --
        // statues, signposts, fishing crates. 46 of the 122 entities in the
        // capture are exactly that.
        if (m->anchor_range <= 0 || m->walk_speed <= 0) continue;

        // Dropping back to a walk after a chase is a speed change like any
        // other and has to be announced.
        if (m->move_speed != m->walk_speed) {
            m->move_speed = m->walk_speed;
            speed_changed.push_back(m);
        }

        // Wander: walk a short leg, then rest. Turning back at the anchor
        // radius keeps a spawn from drifting off its own spawn point.
        //
        // The position advances every tick for the whole leg, not once when it
        // ends. The client slides the actor at MSPD from the moment a
        // ZC_MOVE_DIR with the flag set arrives until one clears it, so a
        // server that moved a single step per leg drifted a full leg behind
        // whatever was on screen.
        if (m->moving) {
            m->position.x += float(m->direction.cos * m->move_speed * dt);
            m->position.z += float(m->direction.sin * m->move_speed * dt);
            bool turned = false;
            float ox = m->position.x - m->anchor.x;
            float oz = m->position.z - m->anchor.z;
            float d2 = ox * ox + oz * oz;
            if (d2 > m->anchor_range * m->anchor_range && d2 > 0) {
                float len = std::sqrt(d2);
                m->direction = {-ox / len, -oz / len};
                turned = true;                // a turn has to be announced
            }
            if (now < m->next_think) {
                if (turned) movers.push_back(m);
                continue;
            }
            // Ending the leg with the flag clear is what stops the walk
            // animation client-side; the broadcast below carries it.
            m->moving = false;
            m->next_think = now + 2.0;
        } else {
            if (now < m->next_think) continue;
            std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);
            m->direction = Direction::from_angle(ang(world_->rng()));
            m->moving = true;
            m->next_think = now + 2.0;
        }
        movers.push_back(m);
    }

    float t = world_->server_time();
    const ServerContext* ctx = nullptr;
    for (const auto& c : characters()) {
        if (auto sess = c->live_session()) { ctx = sess->ctx; break; }
    }
    if (!ctx) return;

    for (const auto& m : respawned)
        broadcast_to(pcs, send::make_ZC_ENTER_MONSTER(*ctx, *m, true));
    // Speed first: a ZC_MOVE_DIR carries the speed the client should extrapolate
    // with, so the walk/run switch has to be in effect before the move lands.
    for (const auto& m : speed_changed)
        broadcast_to(pcs, send::make_ZC_MSPD(*ctx, *m));
    for (const auto& m : movers)
        broadcast_to(pcs, send::make_ZC_MOVE_DIR(*ctx, *m, t));

    // Monster attacks land after movement so the swing follows the step.
    for (const auto& hit : hits) {
        Monster& m = *hit.first;
        Character& c = *hit.second;
        if (c.dead()) continue;

        int32_t dmg = m.attack_power;
        c.hp = c.hp > dmg ? c.hp - dmg : 0;
        c.properties.set("HP", float(c.hp));

        broadcast_to(pcs, send::make_ZC_HIT_INFO(*ctx, m, c, dmg));
        broadcast_to(pcs, send::make_ZC_UPDATE_ALL_STATUS(*ctx, c));

        if (c.hp <= 0) {
            broadcast_to(pcs, send::make_ZC_DEAD(*ctx, c, m.handle));
            m.target = 0;
            // Revive on the spot rather than leaving the player stuck: there
            // is no resurrection UI yet, and a corpse that cannot stand up is
            // worse than a cheap respawn.
            c.hp = c.max_hp;
            c.properties.set("HP", float(c.hp));
            if (auto sess = c.live_session()) {
                sess->send_raw(send::make_ZC_UPDATE_ALL_STATUS(*ctx, c));
                sess->send_raw(send::make_ZC_ENTER_PC(*ctx, c));
            }
        }
    }
}

// ---- world -------------------------------------------------------------

Map* World::map(int32_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = maps_.find(id);
    if (it != maps_.end()) return it->second.get();

    const data::MapData* md = data_.map(id);
    std::string cls = md ? md->class_name : ("map_" + std::to_string(id));
    auto m = std::make_unique<Map>(id, cls, this);
    Map* raw = m.get();
    maps_.emplace(id, std::move(m));
    return raw;
}

Map* World::map_by_class_name(const std::string& name) {
    const data::MapData* md = data_.map(name);
    return md ? map(md->id) : nullptr;
}

void World::tick() {
    double now = now_sec();
    std::vector<Map*> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : maps_) all.push_back(kv.second.get());
    }
    for (Map* m : all) m->tick(now);
}

void World::add_session(const std::shared_ptr<Session>& s) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.push_back(s);
}

void World::remove_session(uint32_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [&](const std::shared_ptr<Session>& p) {
                                       return p->id == id;
                                   }),
                    sessions_.end());
}

std::vector<std::shared_ptr<Session>> World::sessions(uint32_t except) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::shared_ptr<Session>> out;
    for (const auto& p : sessions_)
        if (p->id != except) out.push_back(p);
    return out;
}

float World::server_time() const {
    // The client anchors its clock to ZC_START_GAME and then extrapolates, so
    // this only has to be a monotonic seconds count that never jumps.
    return float(now_sec());
}

}  // namespace tos::game
