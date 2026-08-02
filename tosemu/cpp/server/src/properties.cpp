#include "properties.h"

#include "packet.h"

namespace tos::game {

void Properties::set(const std::string& name, float value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) {
        index_.emplace(name, order_.size());
        order_.push_back({name, value, "", false});
    } else {
        order_[it->second].number = value;
        order_[it->second].is_string = false;
    }
}

void Properties::set(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) {
        index_.emplace(name, order_.size());
        order_.push_back({name, 0, value, true});
    } else {
        order_[it->second].text = value;
        order_[it->second].is_string = true;
    }
}

void Properties::add(const std::string& name, float delta) {
    set(name, get(name) + delta);
}

bool Properties::has(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return index_.count(name) != 0;
}

float Properties::get(const std::string& name, float def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) return def;
    const Entry& e = order_[it->second];
    return e.is_string ? def : e.number;
}

std::string Properties::get_string(const std::string& name,
                                   const std::string& def) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) return def;
    const Entry& e = order_[it->second];
    return e.is_string ? e.text : def;
}

size_t Properties::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return order_.size();
}

size_t Properties::byte_count() const {
    if (!table_) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const Entry& e : order_) {
        if (!table_->id(ns_, e.name)) continue;
        n += 4;                                  // id
        n += e.is_string ? 2 + e.text.size() + 1 // length, bytes, NUL
                         : 4;                    // float
    }
    return n;
}

void Properties::write(PacketWriter& w) const {
    if (!table_) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (const Entry& e : order_) {
        int32_t id = table_->id(ns_, e.name);
        // Skipped, not written as id 0: the client reads this stream
        // positionally, so one bad id corrupts everything after it.
        if (!id) continue;
        w.i32v(id);
        if (e.is_string)
            w.lpstr(e.text);
        else
            w.f32(e.number);
    }
}

}  // namespace tos::game
