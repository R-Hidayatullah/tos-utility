// Actors: what exists in the world and what the client is told about.
//
// The split follows the client's own packet families rather than an invented
// hierarchy -- a Character is what ZC_ENTER_PC describes, a Monster is what
// ZC_ENTER_MONSTER describes, and NPCs are monsters with Faction "Npc", which
// is exactly how the client's data models them.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gamedata.h"
#include "properties.h"

namespace tos
{

    struct Session;

    namespace game
    {

        class Map;

        struct Position
        {
            float x = 0, y = 0, z = 0;

            float dist_sq(const Position &o) const
            {
                float dx = x - o.x, dy = y - o.y, dz = z - o.z;
                return dx * dx + dy * dy + dz * dz;
            }
        };

        // The client transmits facing as the cosine and sine of the yaw, not an angle.
        struct Direction
        {
            float cos = 1, sin = 0;

            static Direction from_angle(float radians);
            float angle() const;
        };

        enum class ActorKind
        {
            Character,
            Monster,
            Npc
        };

        // A learned skill. The client keys skills by object id in the skill list and
        // by class id everywhere else, so both are carried.
        struct Skill
        {
            int32_t id = 0;
            int32_t level = 1;
            int64_t object_id = 0;
            double cooldown_until = 0;
            const data::SkillData *data = nullptr;

            bool ready(double now) const { return now >= cooldown_until; }
        };

        // One item instance. The client keys inventory entries by object id and by the
        // slot they occupy, and reads everything else off the item's own properties.
        struct Item
        {
            int32_t id = 0;
            int64_t object_id = 0;
            int32_t amount = 1;
            int32_t price = 0;
            int32_t slot = 0; // inventory position, or an EquipSlot
            const data::ItemData *data = nullptr;
        };

        // The 36 equipment slots, in the order the appearance block writes them.
        enum class EquipSlot : uint8_t
        {
            HairAccessory,
            SubsidiaryAccessory,
            Hair,
            Top,
            Gloves,
            Shoes,
            Helmet,
            Armband,
            RightHand,
            LeftHand,
            Outer1,
            Ring1,
            Ring2,
            Outer2,
            Pants,
            Ring3,
            Ring4,
            Bracelet1,
            Bracelet2,
            Necklace,
            Hat,
            Lens,
            Wing,
            SpecialCostume,
            EffectCostume,
            Seal,
            Doll,
            Ark,
            Trinket,
            Relic,
            RightHandSub,
            LeftHandSub,
            Earring,
            Belt,
            Shoulder,
            Core,
            Count
        };

        // Handles are allocated from disjoint ranges per kind. Nothing on the wire
        // requires that, but it makes a handle in a log immediately legible.
        class HandleAllocator
        {
        public:
            uint32_t character() { return next_pc_++; }
            uint32_t monster() { return next_mob_++; }

        private:
            std::atomic<uint32_t> next_pc_{0x00600000};
            std::atomic<uint32_t> next_mob_{0x00A00000};
        };

        class Actor
        {
        public:
            virtual ~Actor() = default;
            virtual ActorKind kind() const = 0;

            uint32_t handle = 0;
            std::string name;
            Position position;
            Direction direction;
            Map *map = nullptr;

            bool moving = false;
            float move_speed = 35.0f; // the client's unbuffed PC base

            int32_t hp = 1, max_hp = 1;
            int32_t sp = 0, max_sp = 0;
            int32_t level = 1;

            bool dead() const { return hp <= 0; }
            bool is_character() const { return kind() == ActorKind::Character; }
        };

        class Character : public Actor
        {
        public:
            ActorKind kind() const override { return ActorKind::Character; }

            explicit Character(const data::PropertyTable *props)
                : properties("PC", props), etc_properties("PCEtc", props) {}

            // Identity, as the barrack and zone packets need it.
            int64_t account_id = 0;
            int64_t object_id = 0;
            int64_t social_user_id = 0;
            std::string team_name;
            uint8_t gender = 1; // 1 male, 2 female, matching the client
            uint8_t hair = 0;
            uint32_t skin_color = 0xFFFFFFFF;
            int32_t job_id = 1001; // Swordsman
            int32_t stance = 0;
            int32_t map_id = 0;
            uint8_t slot = 0; // barrack slot, also the list index
            uint8_t channel = 1;
            Position barrack_position;
            Direction barrack_direction;
            bool sitting = false;

            int64_t exp = 0, max_exp = 0, total_exp = 0;
            int32_t stamina = 25000, max_stamina = 25000;
            int32_t silver = 0;

            Properties properties;
            Properties etc_properties;

            // Weak, NOT a raw pointer. The world tick sends to characters from its own
            // thread while a connection thread may be tearing the session down; a raw
            // pointer there is a use-after-free that only shows up under load. Locking
            // the weak_ptr keeps the session alive for the duration of the send.
            std::weak_ptr<Session> session;
            std::shared_ptr<Session> live_session() const { return session.lock(); }

            // The 36 equipment slots ZC_ENTER_PC's appearance block carries. Defaults
            // are the client's own "empty" items, so an unequipped character still
            // renders instead of showing nothing.
            std::vector<int32_t> equip;
            static const std::vector<int32_t> &default_equip();

            // Learned skills, in the order the job's tree lists them -- which is the
            // order the client lays them out in the skill window.
            std::vector<Skill> skills;
            Skill *find_skill(int32_t id);

            // Carried items, and what is worn. `equip` holds the item id per slot for
            // the appearance block; `equipped` holds the instance so it can come off
            // again.
            std::vector<Item> inventory;
            std::vector<Item> equipped;
            Item *find_item(int64_t object_id);
            Item *equipped_in(EquipSlot slot);
            // Adds to the inventory, stacking onto an existing entry when the item
            // allows it. Returns the entry that changed.
            Item *add_item(const data::GameData &gd, int32_t item_id, int32_t amount,
                           int64_t &next_object_id);
            bool remove_item(int64_t object_id, int32_t amount);
            // Moves an inventory item into its slot, returning what came off.
            bool equip_item(int64_t object_id, EquipSlot slot, int32_t &removed_id);
            float carried_weight() const;
            void rebuild_equip_ids();

            void apply_base_stats(const data::GameData &gd);
            // Grant everything the job's skill tree unlocks at or below this level.
            int learn_job_skills(const data::GameData &gd, int64_t &next_object_id);
            // Recompute `stance` from the job and what is in each hand. Must be called
            // whenever either changes: stance 0 leaves the client with no animation.
            void update_stance(const data::GameData &gd);
        };

        class Monster : public Actor
        {
        public:
            ActorKind kind() const override
            {
                return data && data->is_npc() ? ActorKind::Npc : ActorKind::Monster;
            }

            explicit Monster(const data::PropertyTable *props)
                : properties("Monster", props) {}

            const data::MonsterData *data = nullptr;
            int32_t monster_id = 0;
            int32_t gen_type = 0; // the generator row this came from

            // WlkMSPD and RunMSPD. move_speed holds whichever is in force right now:
            // the client's SCR_Get_MON_MSPD switches on MOVE_TYPE_CURRENT, so wandering
            // uses the walk speed and a chase uses the run speed, and every change has
            // to reach the client as a ZC_MSPD or the two simulations drift apart.
            float walk_speed = 0;
            float run_speed = 0;

            // From the map's generator row. These are the last four of the five
            // 256-byte names ZC_ENTER_MONSTER carries at +0x5E; the client asks for
            // `dialog` by name when you click the spawn, so an NPC without one is
            // just scenery.
            std::string unique_name;
            std::string dialog;
            std::string enter_script;
            std::string leave_script;

            // The two properties ZC_ENTER_MONSTER carries, and the only two the
            // live server ever sends on it: monster.ies Scale, and the
            // generator row's Range column. Scale is the one that shows --
            // without it every spawn renders at the client's default size, and
            // the real values run 0.15 to 0.8.
            float scale = 1.0f;
            float gen_range = 0;

            // Spawn bookkeeping: where it belongs and how far it may stray.
            Position anchor;
            float anchor_range = 0;
            double next_think = 0;
            double respawn_at = 0;
            double respawn_delay = 0;
            bool despawned = false;

            // Aggro. NPCs never take one; hostile monsters chase whoever comes inside
            // their SearchRange and swing when close enough.
            uint32_t target = 0;
            double next_attack = 0;
            int32_t attack_power = 0;
            float aggro_range = 0;
            float reach = 0;

            Properties properties;
        };

    } // namespace game
} // namespace tos
