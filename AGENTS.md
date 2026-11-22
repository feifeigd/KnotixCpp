# Project Guide for Coding Agents

## Project and build

- This is a C++23 Windows project using CMake and the vcpkg manifest in `vcpkg.json`.
- Dependencies are CAF, CURL, and nlohmann/json. CMake uses `$VCPKG_ROOT` as the vcpkg toolchain when it is set.
- The main targets are `FunctionCalling` and `FunctionCallingCAF`; `knotix` is a static library linked by both.
- Configure with `cmake -S . -B build` when the vcpkg environment is available.
- Build both applications with `cmake --build build --config Debug --target FunctionCalling FunctionCallingCAF`. Use `Release` in place of `Debug` when appropriate.
- Unit tests use CAF's own test framework (`caf/test`). The vcpkg port builds CAF with `CAF_ENABLE_TESTING=OFF`, so the 1.0.2 `libcaf_test` sources are vendored under `third_party/caf-test/` and built as a static `CAF::test` target; the two headers CAF generates at build time (`caf/detail/test_export.hpp`, `caf/log/test.hpp`) are provided there as well.
- Run the whole suite with `run_tests.cmd [Debug|Release]` (configure + build + ctest), or manually: `cmake --build build --config Debug --target knotix_tests`, then `ctest --test-dir build -C Debug --output-on-failure` with `build/vcpkg_installed/x64-windows/debug/bin` on `PATH` for the CAF/CURL DLLs. Disable tests with `-DKNOTIX_BUILD_TESTS=OFF`.
- Do not make real provider/API calls just to test local changes; use `tests/mock_http_server.hpp` (one canned response per connection on 127.0.0.1) for end-to-end checks instead.

## Source layout

- `knotix/schema/tool.h`: `DataType` and its `NLOHMANN_JSON_SERIALIZE_ENUM` mapping.
- `knotix/json.hpp`: shared JSON alias and helpers, including safe object lookup and JSON type-name helpers.
- `knotix/schema/message.h`: message schema types and `FunctionCall` parsing.
- `function_registry.hpp`: function definitions, JSON Schema generation, argument validation, and handler dispatch.
- `agent_config.*`: provider configuration loading.
- `agent_conversation.*`: request construction and tool-call orchestration.
- `openai_compatible_client.*`: CURL transport and Chat Completions request lifecycle; the SSE stream parser lives in `sse_parser.hpp`.
- `caf_main.cpp`: CAF application entry point.
- `tests/`: `caf/test`-based suites per component; `tests/check_json.hpp` compares JSON values safely (reporter stringification throws for non-string JSON, so do not `check_eq` raw `json` values); `tests/mock_http_server.hpp` is the shared local HTTP mock.

## JSON and tool-call conventions

- Include `knotix/json.hpp` for `JsonValue` (`nlohmann::json`) and shared helpers. Do not add a competing JSON parser or alias.
- Parse with `JsonValue::parse`, serialize with `.dump()`, inspect with `is_*`, and extract values with `.get<T>()`.
- Construct scalar values with parentheses: `json{value}` is brace-initialization and creates a one-element array, so use `json(value)` for scalars and enums (`json::object`/`json::array` initializers are unaffected).
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
