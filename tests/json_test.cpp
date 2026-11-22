#include "knotix/json.hpp"

#include "caf/test/approx.hpp"
#include "check_json.hpp"

#include "caf/test/test.hpp"

#include <limits>

using namespace std::string_view_literals;

// Note: scalar expectations use json(value) with parentheses on purpose.
// json{value} would create a one-element array instead of a scalar.

SUITE("knotix.json") {

TEST("jsonGet returns the value for an existing key") {
    auto const object = json::object({{"a", 1}, {"b", "text"}});
    check_json_eq(jsonGet(object, "a"), json(1));
    check_json_eq(jsonGet(object, "b"), json("text"));
}

TEST("jsonGet returns null for a missing key without asserting") {
    auto const object = json::object({{"a", 1}});
    auto const& missing = jsonGet(object, "does-not-exist");
    check(missing.is_null());
    // Repeated lookups keep returning the same shared null instance.
    check_eq(&missing, &jsonGet(object, "other"));
}

TEST("jsonGet returns null when the value is not an object") {
    check(jsonGet(json(42), "a").is_null());
    check(jsonGet(json::array({1, 2}), "a").is_null());
    check(jsonGet(json("str"), "a").is_null());
    check(jsonGet(json(), "a").is_null());
}

TEST("isIntegerValue accepts integers and integral-valued floats") {
    check(knotix::isIntegerValue(json(0)));
    check(knotix::isIntegerValue(json(-7)));
    check(knotix::isIntegerValue(json(3.0)));
    check(knotix::isIntegerValue(json(-2.0)));
    check(knotix::isIntegerValue(json::parse("4611686018427387904")));
}

TEST("isIntegerValue rejects fractional, non-finite and non-numeric values") {
    check(!knotix::isIntegerValue(json(3.5)));
    check(!knotix::isIntegerValue(json(-0.25)));
    check(!knotix::isIntegerValue(json(std::numeric_limits<double>::infinity())));
    check(!knotix::isIntegerValue(json(std::numeric_limits<double>::quiet_NaN())));
    check(!knotix::isIntegerValue(json("3")));
    check(!knotix::isIntegerValue(json(true)));
    check(!knotix::isIntegerValue(json()));
    check(!knotix::isIntegerValue(json::array()));
    check(!knotix::isIntegerValue(json::object()));
}

TEST("dataTypeName maps every DataType to its JSON Schema name") {
    check_eq(knotix::dataTypeName(DataType::Null), "null"sv);
    check_eq(knotix::dataTypeName(DataType::Object), "object"sv);
    check_eq(knotix::dataTypeName(DataType::Number), "number"sv);
    check_eq(knotix::dataTypeName(DataType::Integer), "integer"sv);
    check_eq(knotix::dataTypeName(DataType::String), "string"sv);
    check_eq(knotix::dataTypeName(DataType::Array), "array"sv);
    check_eq(knotix::dataTypeName(DataType::Boolean), "boolean"sv);
}

TEST("dataTypeName agrees with the JSON serialization of DataType") {
    for (auto type : {DataType::Null, DataType::Object, DataType::Number, DataType::Integer,
                      DataType::String, DataType::Array, DataType::Boolean}) {
        check_eq(knotix::dataTypeName(type), json(type).get<std::string>());
    }
}

TEST("typeOf reports the JSON value category") {
    check_eq(knotix::typeOf(json()), "null"sv);
    check_eq(knotix::typeOf(json::object()), "object"sv);
    check_eq(knotix::typeOf(json::array()), "array"sv);
    check_eq(knotix::typeOf(json("text")), "string"sv);
    check_eq(knotix::typeOf(json(true)), "boolean"sv);
    check_eq(knotix::typeOf(json(false)), "boolean"sv);
}

TEST("typeOf distinguishes integer and number") {
    check_eq(knotix::typeOf(json(1)), "integer"sv);
    check_eq(knotix::typeOf(json(1u)), "integer"sv);
    check_eq(knotix::typeOf(json(-1)), "integer"sv);
    // Integral-valued floating point numbers count as integers.
    check_eq(knotix::typeOf(json(1.0)), "integer"sv);
    check_eq(knotix::typeOf(json(0.5)), "number"sv);
    check_eq(knotix::typeOf(json(-2.5)), "number"sv);
}

namespace {

// Minimal save/load inspector pair in the spirit of CAF's inspectors: apply
// receives a getter and a setter for the inspected value.
struct FakeSaveInspector {
    std::string* out;
    template <class Get, class Set>
    bool apply(Get&& get, Set&&) {
        *out = get();
        return true;
    }
};

struct FakeLoadInspector {
    std::string_view in;
    template <class Get, class Set>
    bool apply(Get&&, Set&& set) {
        return set(std::string{in});
    }
};

} // namespace

TEST("inspect dumps the JSON value for a save inspector") {
    auto value = json::object({{"key", "value"}, {"n", 2}});
    std::string saved;
    FakeSaveInspector inspector{&saved};
    check(inspect(inspector, value));
    check_eq(saved, value.dump());
}

TEST("inspect parses valid JSON text for a load inspector") {
    json value;
    FakeLoadInspector inspector{R"({"a":[1,2]})"};
    check(inspect(inspector, value));
    check_json_eq(value, json::object({{"a", json::array({1, 2})}}));
}

TEST("inspect keeps the old value and fails on malformed JSON text") {
    auto value = json(42);
    FakeLoadInspector inspector{"{not json"};
    check(!inspect(inspector, value));
    check_json_eq(value, json(42));
}

} // SUITE("knotix.json")
