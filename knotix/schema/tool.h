#pragma once

#include <string_view>

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

inline constexpr std::string_view to_string(DataType dataType) {
    switch(dataType){
        case DataType::Null: return "null";
        case DataType::Object: return "object";
        case DataType::Number: return "number";
        case DataType::Integer: return "integer";
        case DataType::String: return "string";
        case DataType::Array: return "array";
        case DataType::Boolean: return "boolean";
        default: return "unknown";
    }
}
