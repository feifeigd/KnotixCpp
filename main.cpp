#include "function_registry.hpp"

#include <print>

int main() {
    JsonValue value{1};
    FunctionRegistry registry = makeFunctionRegistry();

    std::println("schemas={}", registry.getSchemas().dump());
    return 0;
}
