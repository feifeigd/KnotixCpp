#pragma once

#include <nlohmann/json.hpp>

// Tool 函数 JSON 参数类型
enum class DataType {
    Null,
    Object,
    Number,
    Integer,
    String,
    Array,
    Boolean,
};

NLOHMANN_JSON_SERIALIZE_ENUM(DataType, {
    {DataType::Null,    "null"},
    {DataType::Object,  "object"},
    {DataType::Number,  "number"},
    {DataType::Integer, "integer"},
    {DataType::String,  "string"},
    {DataType::Array,   "array"},
    {DataType::Boolean, "boolean"},
})
