#include "gamedata.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace tos::data {

// ---- property table ----------------------------------------------------

bool PropertyTable::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        // "<namespace> <name> <Number|String> <id>"
        std::istringstream is(s);
        std::string ns, name, type, id;
        if (!(is >> ns >> name >> type >> id)) continue;

        Ns& e = ns_[ns];
        int32_t v = 0;
        try {
            v = int32_t(std::stol(id));
        } catch (...) {
            continue;
        }
        e.ids[name] = v;
        e.names[v] = name;
        e.strings[name] = (type == "String" || type == "string");
        ++count_;
    }
    return count_ != 0;
}

int32_t PropertyTable::id(const std::string& ns, const std::string& name) const {
    auto n = ns_.find(ns);
    if (n == ns_.end()) return 0;
    auto it = n->second.ids.find(name);
    return it == n->second.ids.end() ? 0 : it->second;
}

bool PropertyTable::is_string(const std::string& ns, const std::string& name) const {
    auto n = ns_.find(ns);
    if (n == ns_.end()) return false;
    auto it = n->second.strings.find(name);
    return it != n->second.strings.end() && it->second;
}

const std::string* PropertyTable::name_of(const std::string& ns, int32_t v) const {
    auto n = ns_.find(ns);
    if (n == ns_.end()) return nullptr;
    auto it = n->second.names.find(v);
    return it == n->second.names.end() ? nullptr : &it->second;
}

// ---- game data ---------------------------------------------------------

bool GameData::open_client(const std::string& game_root) {
    fs_ = std::make_unique<ipf::FileSystem>();
    int n = fs_->scan_game_root(game_root);
    if (n == 0 || fs_->unique_count() == 0) {
        fs_.reset();
        return false;
    }
    return true;
}

bool GameData::read_ies(const std::string& vpath, ies::Table& out) const {
    if (!fs_) return false;
    Bytes raw;
    if (!fs_->read(vpath, raw)) return false;
    return out.parse(raw);
}

void GameData::load_tables() {
    if (!fs_) return;

    // ---- maps
    if (ies::Table t; read_ies("ies/map.ies", t)) {
        maps_.reserve(t.size());
        for (const ies::Row& r : t.rows()) {
            MapData m;
            m.id = r.class_id;
            m.class_name = r.class_name;
            m.name = t.text(r, "Name");
            m.eng_name = t.text(r, "EngName");
            m.type = t.text(r, "MapType");
            m.x = t.number(r, "DefGenX");
            m.y = t.number(r, "DefGenY");
            m.z = t.number(r, "DefGenZ");
            map_by_id_.emplace(m.id, maps_.size());
            map_by_name_.emplace(to_lower(m.class_name), maps_.size());
            maps_.push_back(std::move(m));
        }
    }

    // ---- monsters and npcs
    //
    // These are TWO tables, not one. monster.ies (4288 rows) holds the hostile
    // world; monster_npc.ies (2016 rows) holds town NPCs, props and session
    // objects, and nothing in monster.ies duplicates an id from it. Reading
    // only monster.ies left every anchored NPC unresolvable, so make_monster()
    // returned null and spawn_from_anchors() silently skipped it -- in the
    // Klaipeda capture 96 of the 122 live spawns come from the NPC table, and
    // 10 of the map's 12 distinct anchor NPCIDs.
    for (const char* path : {"ies/monster.ies", "ies/monster_npc.ies"}) {
        ies::Table t;
        if (!read_ies(path, t)) continue;
        bool npc_table = std::string(path).find("_npc") != std::string::npos;
        monsters_.reserve(monsters_.size() + t.size());
        for (const ies::Row& r : t.rows()) {
            MonsterData m;
            m.id = r.class_id;
            m.class_name = r.class_name;
            m.name = t.text(r, "Name");
            m.faction = t.text(r, "Faction");
            m.rank = t.text(r, "MonRank");
            m.size = t.text(r, "Size");
            m.race = t.text(r, "RaceType");
            m.type = t.text(r, "ClassType");
            m.level = t.integer(r, "Level", 1);
            // No default: a zero here is meaningful. SCR_Get_MON_MSPD returns
            // 0 outright when WlkMSPD is 0, which is how statues, signposts
            // and fishing crates stay put. Substituting 20/30 made every prop
            // in town wander off its anchor.
            m.walk_speed = t.number(r, "WlkMSPD", 0.0f);
            m.run_speed = t.number(r, "RunMSPD", 0.0f);
            m.scale = t.number(r, "Scale", 1.0f);
            m.search_range = t.number(r, "SearchRange");
            m.attack_range = t.number(r, "ATK_RANGE");
            m.from_npc_table = npc_table;
            mon_by_id_.emplace(m.id, monsters_.size());
            mon_by_name_.emplace(to_lower(m.class_name), monsters_.size());
            monsters_.push_back(std::move(m));
        }
    }

    // ---- jobs
    if (ies::Table t; read_ies("ies/job.ies", t)) {
        jobs_.reserve(t.size());
        for (const ies::Row& r : t.rows()) {
            JobData j;
            j.id = r.class_id;
            j.class_name = r.class_name;
            j.name = t.text(r, "Name");
            j.ctrl_type = t.text(r, "CtrlType");
            j.rank = t.integer(r, "Rank", 1);
            j.barrack_stance = t.integer(r, "BarrackStance");
            job_by_id_.emplace(j.id, jobs_.size());
            job_by_name_.emplace(to_lower(j.class_name), jobs_.size());
            jobs_.push_back(std::move(j));
        }
    }

    // ---- skills
    if (ies::Table t; read_ies("ies/skill.ies", t)) {
        skills_.reserve(t.size());
        for (const ies::Row& r : t.rows()) {
            SkillData s;
            s.id = r.class_id;
            s.class_name = r.class_name;
            s.name = t.text(r, "Name");
            s.max_level = t.integer(r, "MaxLevel", 1);
            s.cooldown = t.number(r, "BasicCoolDown");
            s.range = t.number(r, "SklSR");
            s.use_type = t.text(r, "UseType");
            skill_by_id_.emplace(s.id, skills_.size());
            skill_by_name_.emplace(to_lower(s.class_name), skills_.size());
            skills_.push_back(std::move(s));
        }
    }

    // ---- skill trees, joined onto jobs and skills by class name
    if (ies::Table t; read_ies("ies/skilltree.ies", t)) {
        for (const ies::Row& r : t.rows()) {
            // "Char1_1_3" -> job "Char1_1"; the suffix is the tree slot.
            size_t cut = r.class_name.find_last_of('_');
            if (cut == std::string::npos) continue;
            const JobData* jd = job(r.class_name.substr(0, cut));
            if (!jd) continue;

            SkillTreeEntry e;
            e.skill_class_name = t.text(r, "SkillName");
            const SkillData* sd = skill(e.skill_class_name);
            if (!sd) continue;
            e.skill_id = sd->id;
            e.unlock_level = t.integer(r, "UnlockClassLevel", 1);
            e.max_level = t.integer(r, "MaxLevel", 1);
            skill_trees_[jd->id].push_back(std::move(e));
        }
    }

    // ---- items. The client splits these across several tables keyed the same
    // way: item.ies holds consumables and materials, item_equip.ies the 6768
    // pieces of equipment, and the rest are expansions.
    for (const char* vpath : {"ies/item.ies", "ies/item_equip.ies",
                              "ies/item_equip_ep12.ies", "ies/item_equip_ep13.ies",
                              "ies/item_event.ies", "ies/item_premium.ies",
                              "ies/item_ep12.ies", "ies/item_ep13.ies"}) {
        ies::Table t;
        if (!read_ies(vpath, t)) continue;
        for (const ies::Row& r : t.rows()) {
            if (item_by_id_.count(r.class_id)) continue;
            ItemData it;
            it.id = r.class_id;
            it.class_name = r.class_name;
            it.name = t.text(r, "Name");
            it.type = t.text(r, "ItemType");
            it.group = t.text(r, "GroupName");
            it.equip_type = t.text(r, "Journal");
            it.class_type = t.text(r, "ClassType");
            it.weight = t.number(r, "Weight");
            it.price = t.integer(r, "Price");
            it.max_stack = t.integer(r, "MaxStack", 1);
            it.use_level = t.integer(r, "UseLv", 1);
            item_by_id_.emplace(it.id, items_.size());
            item_by_name_.emplace(to_lower(it.class_name), items_.size());
            items_.push_back(std::move(it));
        }
    }

    // ---- stances. Two joins: condition rows name a stance, stance.ies gives
    // it a number. Without this every character is sent stance 0 and the
    // client has no animation to play, which is the T-pose.
    {
        std::unordered_map<std::string, int32_t> stance_ids;
        if (ies::Table t; read_ies("ies/stance.ies", t)) {
            for (const ies::Row& r : t.rows())
                stance_ids.emplace(r.class_name, r.class_id);
        }
        if (ies::Table t; read_ies("ies/stancecondition.ies", t)) {
            for (const ies::Row& r : t.rows()) {
                if (t.text(r, "Riding") == "TRUE") continue;
                std::string name = t.text(r, "Stance1");
                auto id = stance_ids.find(name);
                if (name.empty() || id == stance_ids.end()) continue;
                std::string key = to_lower(t.text(r, "UseJob")) + "|" +
                                  to_lower(t.text(r, "RHand")) + "|" +
                                  to_lower(t.text(r, "LHand"));
                stances_.emplace(key, id->second);
            }
        }
    }

    // ---- pc base stat rates, keyed by CtrlType
    //
    // This was read as "indexed by class level", which it is not: the table has
    // six rows and ClassID 1..6 enumerates the base archetypes (None, Warrior,
    // Wizard, Archer, Cleric, Scout). Indexing it by character level gave a
    // Warrior row to a level-2 character and nothing at all above level 6.
    if (ies::Table t; read_ies("ies/statbase_pc.ies", t)) {
        for (const ies::Row& r : t.rows()) {
            PcStatBase s;
            s.hp = t.number(r, "MHP", 100.0f);
            s.sp = t.number(r, "MSP", 100.0f);
            s.def = t.number(r, "DEF", 100.0f);
            s.mdef = t.number(r, "MDEF", 100.0f);
            s.rhp = t.number(r, "RHP", 100.0f);
            s.rsp = t.number(r, "RSP", 100.0f);
            s.move_speed = t.number(r, "MOVE_SPEED", 100.0f);
            pc_stats_.emplace(r.class_name, s);
        }
    }
}

const std::vector<Anchor>* GameData::anchors(const std::string& map_class_name) const {
    std::string key = to_lower(map_class_name);
    auto it = anchors_.find(key);
    if (it != anchors_.end()) return it->second.empty() ? nullptr : &it->second;

    std::vector<Anchor>& out = anchors_[key];     // caches the miss too
    ies::Table t;
    if (!read_ies("ies_mongen/anchor_" + key + ".ies", t)) return nullptr;

    out.reserve(t.size());
    for (const ies::Row& r : t.rows()) {
        Anchor a;
        a.gen_type = t.integer(r, "GenType");
        a.x = t.number(r, "PosX");
        a.y = t.number(r, "PosY");
        a.z = t.number(r, "PosZ");
        a.direction = t.number(r, "Direction");
        a.npc_id = t.integer(r, "NPCID");
        a.range = t.number(r, "AnchorRange");
        a.name = t.text(r, "Name");
        // A GenType is enough on its own -- and on many maps it is all there
        // is. Requiring an NPCID here threw away every anchor on c_Orsha, all
        // 91 of which carry NPCID 0.
        if (a.gen_type || a.npc_id) out.push_back(std::move(a));
    }
    return out.empty() ? nullptr : &out;
}

const std::unordered_map<int32_t, GenTypeEntry>* GameData::gen_types(
    const std::string& map_class_name) const {
    std::string key = to_lower(map_class_name);
    auto it = gen_types_.find(key);
    if (it != gen_types_.end())
        return it->second.empty() ? nullptr : &it->second;

    auto& out = gen_types_[key];                 // caches the miss too
    ies::Table t;
    if (!read_ies("ies_mongen/gentype_" + key + ".ies", t)) return nullptr;

    for (const ies::Row& r : t.rows()) {
        GenTypeEntry g;
        // ClassID and the GenType column agree on every map checked; ClassID
        // is the one the anchor joins against.
        g.gen_type = r.class_id;
        g.class_type = t.text(r, "ClassType");
        g.name = t.text(r, "Name");
        g.unique_name = t.text(r, "UniqueName");
        g.dialog = t.text(r, "Dialog");
        g.enter = t.text(r, "Enter");
        g.leave = t.text(r, "Leave");
        g.faction = t.text(r, "Faction");
        g.level = t.integer(r, "Lv", 1);
        g.respawn_time = t.number(r, "RespawnTime");
        g.max_pop = t.integer(r, "MaxPop", 1);
        g.gen_range = t.number(r, "GenRange");
        g.range = t.number(r, "Range");
        if (!g.class_type.empty()) out.emplace(g.gen_type, std::move(g));
    }
    return out.empty() ? nullptr : &out;
}

const MapData* GameData::map(int32_t id) const {
    auto it = map_by_id_.find(id);
    return it == map_by_id_.end() ? nullptr : &maps_[it->second];
}

const MapData* GameData::map(const std::string& class_name) const {
    auto it = map_by_name_.find(to_lower(class_name));
    return it == map_by_name_.end() ? nullptr : &maps_[it->second];
}

const MonsterData* GameData::monster(int32_t id) const {
    auto it = mon_by_id_.find(id);
    return it == mon_by_id_.end() ? nullptr : &monsters_[it->second];
}

const MonsterData* GameData::monster(const std::string& class_name) const {
    auto it = mon_by_name_.find(to_lower(class_name));
    return it == mon_by_name_.end() ? nullptr : &monsters_[it->second];
}

const JobData* GameData::job(int32_t id) const {
    auto it = job_by_id_.find(id);
    return it == job_by_id_.end() ? nullptr : &jobs_[it->second];
}

const JobData* GameData::job(const std::string& class_name) const {
    auto it = job_by_name_.find(to_lower(class_name));
    return it == job_by_name_.end() ? nullptr : &jobs_[it->second];
}

const SkillData* GameData::skill(int32_t id) const {
    auto it = skill_by_id_.find(id);
    return it == skill_by_id_.end() ? nullptr : &skills_[it->second];
}

const SkillData* GameData::skill(const std::string& class_name) const {
    auto it = skill_by_name_.find(to_lower(class_name));
    return it == skill_by_name_.end() ? nullptr : &skills_[it->second];
}

const ItemData* GameData::item(int32_t id) const {
    auto it = item_by_id_.find(id);
    return it == item_by_id_.end() ? nullptr : &items_[it->second];
}

const ItemData* GameData::item(const std::string& class_name) const {
    auto it = item_by_name_.find(to_lower(class_name));
    return it == item_by_name_.end() ? nullptr : &items_[it->second];
}

const ItemData* GameData::lowest_of_class_type(
    const std::string& class_type) const {
    const ItemData* best = nullptr;
    for (const ItemData& it : items_) {
        if (it.class_type != class_type) continue;
        // Skip the client's "NoWeapon_*" placeholders -- they are the empty
        // slot, not something to hand a new character.
        if (it.class_name.rfind("NoWeapon", 0) == 0) continue;
        if (!best || it.use_level < best->use_level ||
            (it.use_level == best->use_level && it.id < best->id))
            best = &it;
    }
    return best;
}

int32_t GameData::stance_for(int32_t job_id, const std::string& right_hand,
                             const std::string& left_hand) const {
    const JobData* j = job(job_id);
    if (!j) return 0;
    std::string base = to_lower(j->class_name) + "|";

    // Exact pair first, then right hand alone, then the bare-handed row --
    // the table has a row for each, and falling back keeps an unknown
    // off-hand from dropping the character to no stance at all.
    for (const std::string& key : {base + to_lower(right_hand) + "|" + to_lower(left_hand),
                                   base + to_lower(right_hand) + "|",
                                   base + "|"}) {
        auto it = stances_.find(key);
        if (it != stances_.end()) return it->second;
    }
    return j->barrack_stance;
}

const std::vector<SkillTreeEntry>& GameData::skill_tree(int32_t job_id) const {
    static const std::vector<SkillTreeEntry> kNone;
    auto it = skill_trees_.find(job_id);
    return it == skill_trees_.end() ? kNone : it->second;
}

PcStatBase GameData::pc_stat_base(const std::string& ctrl_type) const {
    auto it = pc_stats_.find(ctrl_type);
    return it == pc_stats_.end() ? PcStatBase{} : it->second;
}

}  // namespace tos::data
