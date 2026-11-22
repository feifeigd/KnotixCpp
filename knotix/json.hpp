#pragma once

#include "schema/tool.h"

#include <nlohmann/json.hpp>
using nlohmann::json;

#include <cmath>
#include <string>
#include <string_view>


inline json const& jsonGet(json const& value, std::string const& key) {
    static json const nullValue;
    if (!value.is_object())
        return nullValue;
    auto const it = value.find(key);
    return it != value.end() ? it.value() : nullValue;
}

namespace knotix {

inline bool isIntegerValue(json const& value) {
    if (!value.is_number())
        return false;
    auto const number = value.get<double>();
    return std::isfinite(number) && std::trunc(number) == number;
}

inline std::string_view dataTypeName(DataType type) {
    static const std::string nullName    = json{DataType::Null}.get<std::string>();
    static const std::string objectName  = json{DataType::Object}.get<std::string>();
    static const std::string numberName  = json{DataType::Number}.get<std::string>();
    static const std::string integerName = json{DataType::Integer}.get<std::string>();
    static const std::string stringName  = json{DataType::String}.get<std::string>();
    static const std::string arrayName   = json{DataType::Array}.get<std::string>();
    static const std::string booleanName = json{DataType::Boolean}.get<std::string>();

    switch (type) {
        case DataType::Null:    return nullName;
        case DataType::Object:  return objectName;
        case DataType::Number:  return numberName;
        case DataType::Integer: return integerName;
        case DataType::String:  return stringName;
        case DataType::Array:   return arrayName;
        case DataType::Boolean: return booleanName;
    }
    return "unknown";
}

inline std::string_view typeOf(json const& value) {
    using ValueType = json::value_t;
    switch (value.type()) {
        case ValueType::null: return dataTypeName(DataType::Null);
        case ValueType::object: return dataTypeName(DataType::Object);
        case ValueType::number_integer:
        case ValueType::number_unsigned: return dataTypeName(DataType::Integer);
        case ValueType::number_float:
            return dataTypeName(isIntegerValue(value) ? DataType::Integer : DataType::Number);
        case ValueType::string: return dataTypeName(DataType::String);
        case ValueType::array: return dataTypeName(DataType::Array);
        case ValueType::boolean: return dataTypeName(DataType::Boolean);
        case ValueType::binary: return "binary";
        case ValueType::discarded: return "discarded";
    }
    return "unknown";
}

} // namespace knotix

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
