# Project Guide for Coding Agents

## Project and build

- This is a C++23 Windows project using CMake and the vcpkg manifest in `vcpkg.json`.
- Dependencies are CAF, CURL, and nlohmann/json. CMake uses `$VCPKG_ROOT` as the vcpkg toolchain when it is set.
- The main targets are `FunctionCalling` and `FunctionCallingCAF`; `knotix` is a static library linked by both.
- Configure with `cmake -S . -B build` when the vcpkg environment is available.
- Build both applications with `cmake --build build --config Debug --target FunctionCalling FunctionCallingCAF`. Use `Release` in place of `Debug` when appropriate.
- The root CMake project currently defines no test targets. Prefer targeted builds; do not make real provider/API calls just to test local changes.

## Source layout

- `knotix/schema/tool.h`: `DataType` and its `NLOHMANN_JSON_SERIALIZE_ENUM` mapping.
- `knotix/json.hpp`: shared JSON alias and helpers, including safe object lookup and JSON type-name helpers.
- `knotix/schema/message.h`: message schema types and `FunctionCall` parsing.
- `function_registry.hpp`: function definitions, JSON Schema generation, argument validation, and handler dispatch.
- `agent_config.*`: provider configuration loading.
- `agent_conversation.*`: request construction and tool-call orchestration.
- `openai_compatible_client.*`: CURL transport and OpenAI-compatible Chat Completions/SSE parsing.
- `caf_main.cpp`: CAF application entry point.

## JSON and tool-call conventions

- Include `knotix/json.hpp` for `JsonValue` (`nlohmann::json`) and shared helpers. Do not add a competing JSON parser or alias.
- Parse with `JsonValue::parse`, serialize with `.dump()`, inspect with `is_*`, and extract values with `.get<T>()`.
- Use `jsonGet(object, key)` for const object reads where the field may be absent. nlohmann's const `operator[]` asserts on a missing key in Debug builds; `jsonGet` preserves the project's null-on-missing behavior.
- Keep `DataType`'s JSON names in sync through the enum serializer in `knotix/schema/tool.h`. Use `knotix::typeOf`, `knotix::dataTypeName`, and `knotix::isIntegerValue` rather than duplicating type-name mappings. Integral-valued floating-point numbers count as integers for validation.
- Function parameters are described with JSON Schema in `FunctionDefinition::toSchema`; required argument names are listed separately in `requiredParams`.
- The client uses Chat Completions tool-call objects. `function.arguments` is a JSON-encoded string; streaming fragments are accumulated before parsing. Tool-call deltas may omit `content`, so treat response fields as optional.
- Strict mode is not currently enabled in generated tool definitions. If adding it, satisfy OpenAI's strict-schema requirements for every object and required property, and represent nullable fields explicitly; confirm compatibility with the configured provider.

## Configuration and secrets

- The agent reads `agent.json` by default; `AGENT_CONFIG` can point to another config file.
- Provider API keys are read from environment variables named by `api_key_env`. Never add key values to source, logs, examples, or this file.

## Change and validation practices

- Follow `.clang-format` (LLVM-based, four-space indentation) and nearby code patterns.
- Keep changes focused, preserve existing error reporting, and avoid silently accepting malformed JSON or tool arguments.
- For SSE changes, cover events containing tool-call deltas without `content`; prefer a local mock server for end-to-end checks.
