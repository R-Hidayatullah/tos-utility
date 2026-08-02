// Object properties.
//
// Almost everything the client knows about an actor arrives as a property
// stream rather than as struct fields: a run of (id, value) pairs where the id
// comes from a namespace-scoped table ("PC", "Monster", "Item", "Skill") and
// the value is a float or a length-prefixed string. ZC_OBJECT_PROPERTY,
// ZC_ENTER_MONSTER, the inventory and the skill list all carry one.
//
// Ids are resolved through data::PropertyTable. An unknown name resolves to 0
// and is skipped on write rather than sent as id 0, because a bogus id shifts
// the client's parse of every property after it.
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gamedata.h"

namespace tos {

class PacketWriter;

namespace game {

class Properties {
public:
    Properties(std::string ns, const data::PropertyTable* table)
        : ns_(std::move(ns)), table_(table) {}

    const std::string& ns() const { return ns_; }

    void set(const std::string& name, float value);
    void set(const std::string& name, const std::string& value);
    void add(const std::string& name, float delta);
    bool has(const std::string& name) const;

    float get(const std::string& name, float def = 0) const;
    std::string get_string(const std::string& name,
                           const std::string& def = "") const;

    // Bytes the properties occupy on the wire; several packets carry this as
    // a length before the stream itself.
    size_t byte_count() const;
    void write(PacketWriter& w) const;

    size_t size() const;

private:
    struct Entry {
        std::string name;
        float number = 0;
        std::string text;
        bool is_string = false;
    };

    std::string ns_;
    const data::PropertyTable* table_;
    mutable std::mutex mu_;
    // Insertion-ordered: the client does not care, but a stable order makes
    // captured and generated packets diffable.
    std::vector<Entry> order_;
    std::unordered_map<std::string, size_t> index_;
};

}  // namespace game
}  // namespace tos
