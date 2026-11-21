#pragma once

#include <nlohmann/json.hpp>

#include <string>

using JsonValue = nlohmann::json;

inline JsonValue const& jsonGet(JsonValue const& value, std::string const& key) {
    static JsonValue const nullValue;
    if (!value.is_object())
        return nullValue;
    auto const it = value.find(key);
    return it != value.end() ? it.value() : nullValue;
}

namespace nlohmann {

template <class Inspector>
bool inspect(Inspector& inspector, json& value) {
    auto get = [&value] { return value.dump(); };
    auto set = [&value](std::string text) {
        try {
            value = json::parse(text);
            return true;
        } catch (json::exception const&) {
            return false;
        }
    };

    return inspector.apply(get, set);
}

} // namespace nlohmann
