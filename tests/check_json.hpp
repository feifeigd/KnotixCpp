#pragma once

#include "knotix/json.hpp"

#include "caf/detail/source_location.hpp"
#include "caf/test/runnable.hpp"

// Compares two JSON values by their serialized form. Using check_eq on
// nlohmann values directly is unsafe: on failure, the reporter stringifies
// the operands via the implicit conversion to std::string, which throws
// for non-string JSON values.
inline bool check_json_eq(json const& actual, json const& expected,
                          caf::detail::source_location const& location
                          = caf::detail::source_location::current()) {
    return caf::test::runnable::current().check_eq(actual.dump(), expected.dump(), location);
}
