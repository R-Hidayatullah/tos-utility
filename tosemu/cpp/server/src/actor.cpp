#include "actor.h"

#include <cmath>

namespace tos::game {

Direction Direction::from_angle(float radians) {
    return {std::cos(radians), std::sin(radians)};
}

float Direction::angle() const { return std::atan2(sin, cos); }

// The client's own "empty" equipment ids -- NoHat, NoWeapon and friends. A
// character with a zero here renders as a hole, so every slot gets one.
const std::vector<int32_t>& Character::default_equip() {
    static const std::vector<int32_t> v = {
        2,       2,    12101, 8, 6, 7, 10000, 11000, 9999996, 9999996,
        4,       9,    9,     4, 5, 9, 9,     9,     9,       10,
        2,       4,    4,     4, 4, 4, 4,     4,     4,       4,
        9999996, 9999996, 4,  4, 4, 4};
    return v;
}

Item* Character::find_item(int64_t object_id) {
    for (Item& i : inventory)
        if (i.object_id == object_id) return &i;
    for (Item& i : equipped)
        if (i.object_id == object_id) return &i;
    return nullptr;
}

Item* Character::equipped_in(EquipSlot slot) {
    for (Item& i : equipped)
        if (i.slot == int32_t(slot)) return &i;
    return nullptr;
}

Item* Character::add_item(const data::GameData& gd, int32_t item_id,
                          int32_t amount, int64_t& next_object_id) {
    const data::ItemData* d = gd.item(item_id);
    if (!d || amount <= 0) return nullptr;

    // Stack onto an existing entry when the item's own MaxStack allows it,
    // rather than filling the bag with duplicates.
    if (d->stackable()) {
        for (Item& i : inventory) {
            if (i.id != item_id) continue;
            if (i.amount + amount > d->max_stack) continue;
            i.amount += amount;
            return &i;
        }
    }

    Item it;
    it.id = item_id;
    it.object_id = next_object_id++;
    it.amount = d->stackable() ? amount : 1;
    it.price = d->price;
    it.data = d;
    it.slot = int32_t(inventory.size());
    inventory.push_back(it);
    return &inventory.back();
}

bool Character::remove_item(int64_t object_id, int32_t amount) {
    for (auto it = inventory.begin(); it != inventory.end(); ++it) {
        if (it->object_id != object_id) continue;
        if (amount > 0 && it->amount > amount) {
            it->amount -= amount;
            return true;
        }
        inventory.erase(it);
        return true;
    }
    return false;
}

bool Character::equip_item(int64_t object_id, EquipSlot slot,
                           int32_t& removed_id) {
    removed_id = 0;
    auto it = inventory.begin();
    for (; it != inventory.end(); ++it)
        if (it->object_id == object_id) break;
    if (it == inventory.end()) return false;

    // Whatever is already in the slot goes back to the bag, so equipping never
    // destroys an item.
    for (auto e = equipped.begin(); e != equipped.end(); ++e) {
        if (e->slot != int32_t(slot)) continue;
        removed_id = e->id;
        Item back = *e;
        back.slot = int32_t(inventory.size());
        equipped.erase(e);
        inventory.push_back(back);
        break;
    }

    Item worn = *it;
    worn.slot = int32_t(slot);
    inventory.erase(it);
    equipped.push_back(worn);
    rebuild_equip_ids();
    return true;
}

float Character::carried_weight() const {
    float w = 0;
    for (const Item& i : inventory)
        if (i.data) w += i.data->weight * float(i.amount);
    for (const Item& i : equipped)
        if (i.data) w += i.data->weight;
    return w;
}

void Character::update_stance(const data::GameData& gd) {
    auto class_type_in = [&](EquipSlot slot) -> std::string {
        for (const Item& i : equipped)
            if (i.slot == int32_t(slot) && i.data) return i.data->class_type;
        return "";
    };
    stance = gd.stance_for(job_id, class_type_in(EquipSlot::RightHand),
                           class_type_in(EquipSlot::LeftHand));
}

void Character::rebuild_equip_ids() {
    // The appearance block wants one id per slot in slot order, with the
    // client's own placeholder where nothing is worn -- a zero renders as a
    // hole rather than as bare skin.
    equip = default_equip();
    for (const Item& i : equipped) {
        if (i.slot >= 0 && size_t(i.slot) < equip.size())
            equip[size_t(i.slot)] = i.id;
    }
    properties.set("NowWeight", carried_weight());
}

Skill* Character::find_skill(int32_t id) {
    for (Skill& s : skills)
        if (s.id == id) return &s;
    return nullptr;
}

int Character::learn_job_skills(const data::GameData& gd,
                                int64_t& next_object_id) {
    int learned = 0;
    for (const data::SkillTreeEntry& e : gd.skill_tree(job_id)) {
        if (e.unlock_level > level) continue;
        if (find_skill(e.skill_id)) continue;

        Skill s;
        s.id = e.skill_id;
        s.level = 1;
        s.object_id = next_object_id++;
        s.data = gd.skill(e.skill_id);
        skills.push_back(s);
        ++learned;
    }
    return learned;
}

void Character::apply_base_stats(const data::GameData& gd) {
    // statbase_pc.ies is keyed by the job's CtrlType, not by level, and holds
    // percentage rates rather than absolute stats.
    const data::JobData* job = gd.job(job_id);
    data::PcStatBase base = gd.pc_stat_base(job ? job->ctrl_type : "");

    // Nothing derives an absolute HP pool from the client tables yet, so the
    // class rate is applied to a flat 100 rather than pretending to a curve we
    // have not recovered.
    max_hp = int32_t(100.0f * base.hp / 100.0f);
    max_sp = int32_t(100.0f * base.sp / 100.0f);
    if (max_hp <= 0) max_hp = 100;
    if (max_sp <= 0) max_sp = 100;
    if (hp <= 0 || hp > max_hp) hp = max_hp;
    if (sp <= 0 || sp > max_sp) sp = max_sp;

    // The client computes this itself in shared/script/calc_property_pc.lua:
    //
    //     local jobRate = SCR_GET_JOB_RATIO_STAT(self, "MOVE_SPEED")
    //     local value   = 35.0 * jobRate          -- jobRate = MOVE_SPEED/100
    //     ...
    //     return math.floor(value)
    //
    // so the base is 35, not 30, and the result is floored. That reproduces
    // the wire exactly: capture_1785545696.bin carries MSPD 35 on our own
    // unbuffed handle (147 of 179 ZC_MOVE_DIR), 45 while dashing -- the same
    // script adds a flat +10 for DashRun -- and 38 for the Archers on the
    // field, which is floor(35 * 110/100).
    constexpr float kBaseMoveSpeed = 35.0f;
    constexpr float kMaxMoveSpeed = 60.0f;       // PC_MAX_MSPD in the script
    float rate = base.move_speed > 0 ? base.move_speed / 100.0f : 1.0f;
    move_speed = std::floor(kBaseMoveSpeed * rate);
    if (move_speed > kMaxMoveSpeed) move_speed = kMaxMoveSpeed;

    properties.set("Lv", float(level));
    properties.set("MHP", float(max_hp));
    properties.set("HP", float(hp));
    properties.set("MSP", float(max_sp));
    properties.set("SP", float(sp));
    properties.set("MSPD", move_speed);
    // The client gates its own jump on this property; leaving it unset means
    // the jump key does nothing, because the client never asks.
    properties.set("JumpPower", 350.0f);
    properties.set("CastingSpeed", 100.0f);
    properties.set("MovingShot", 0.0f);
    properties.set("STR", 1);
    properties.set("CON", 1);
    properties.set("INT", 1);
    properties.set("MNA", 1);
    properties.set("DEX", 1);
    // Still the raw class rates. The client's formulas scale these by level
    // and stats; until those are ported they are sent as-is, which is the
    // same number the server sent before and is not an absolute defence.
    properties.set("DEF", base.def);
    properties.set("MDEF", base.mdef);
    properties.set("RHP", base.rhp);
    properties.set("RSP", base.rsp);
    properties.set("MaxWeight", 8000);
    properties.set("NowWeight", 0);
    properties.set("MaxSta", float(max_stamina));
    properties.set("Sta", float(stamina));
}

}  // namespace tos::game
