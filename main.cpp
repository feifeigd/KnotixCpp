#include "function_registry.hpp"

#include <print>

int main() {
    json value{1};
    FunctionRegistry registry = makeFunctionRegistry();

    std::println("schemas={}", registry.getSchemas().dump());
    return 0;
}
