#include "knotix/schema/tool.h"

#include "knotix/json.hpp"

#include "check_json.hpp"

#include "caf/test/test.hpp"

// Note: json(type) with parentheses. json{type} would create a one-element
// array instead of the enum's JSON string.

SUITE("knotix.schema.tool") {

TEST("DataType serializes to its JSON Schema name") {
    check_json_eq(json(DataType::Null), json("null"));
    check_json_eq(json(DataType::Object), json("object"));
    check_json_eq(json(DataType::Number), json("number"));
    check_json_eq(json(DataType::Integer), json("integer"));
    check_json_eq(json(DataType::String), json("string"));
    check_json_eq(json(DataType::Array), json("array"));
    check_json_eq(json(DataType::Boolean), json("boolean"));
}

TEST("DataType round-trips through JSON") {
    for (auto type : {DataType::Null, DataType::Object, DataType::Number, DataType::Integer,
                      DataType::String, DataType::Array, DataType::Boolean}) {
        check_json_eq(json(type).get<DataType>(), type);
    }
}

TEST("DataType deserializes every documented name") {
    check_json_eq(json("null").get<DataType>(), DataType::Null);
    check_json_eq(json("object").get<DataType>(), DataType::Object);
    check_json_eq(json("number").get<DataType>(), DataType::Number);
    check_json_eq(json("integer").get<DataType>(), DataType::Integer);
    check_json_eq(json("string").get<DataType>(), DataType::String);
    check_json_eq(json("array").get<DataType>(), DataType::Array);
    check_json_eq(json("boolean").get<DataType>(), DataType::Boolean);
}

} // SUITE("knotix.schema.tool")
