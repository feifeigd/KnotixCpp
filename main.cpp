#include "function_registry.hpp"

#include <print>

int main() {
    JsonValue value{1};
    FunctionRegistry registry = makeFunctionRegistry();

    std::println("schemas={}", registry.getSchemas().toString());
    return 0;
}
