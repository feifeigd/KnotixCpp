#pragma once

#include "knotix/json.hpp"

#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

struct StreamToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

// Incremental parser for the Server-Sent Events stream of an OpenAI-compatible
// Chat Completions response. Text fragments are echoed to `out` as they
// arrive; the assembled assistant message is available via message() once the
// stream finished.
class OpenAICompatibleSseParser {
public:
    explicit OpenAICompatibleSseParser(std::ostream& out = std::cout) : out_{out} {
    }

    void feed(char const* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n')
                processLine();
            else
                lineBuffer_.push_back(data[i]);
        }
    }

    void feed(std::string_view chunk) {
        feed(chunk.data(), chunk.size());
    }

    void finish() {
        if (!lineBuffer_.empty())
            processLine();
        dispatchEvent();
        if (!done_)
            throw std::runtime_error("Provider stream ended before the [DONE] event.");
        if (printedText_)
            out_ << '\n';
    }

    json message() const {
        auto result = json::object({
            {"role", "assistant"},
            {"content", content_},
        });
        if (!toolCalls_.empty()) {
            std::vector<json> calls;
            calls.reserve(toolCalls_.size());
            for (auto const& [index, call] : toolCalls_) {
                if (call.id.empty() || call.name.empty())
                    throw std::runtime_error("Provider returned an incomplete tool call.");
                auto function = json::object({
                    {"name", call.name},
                    {"arguments", call.arguments},
                });
                calls.push_back(json::object({
                    {"id", call.id},
                    {"type", "function"},
                    {"function", std::move(function)},
                }));
            }
            result.emplace("tool_calls", std::move(calls));
        }
        return result;
    }

private:
    void processLine() {
        auto line = std::string_view{lineBuffer_};
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) {
            lineBuffer_.clear();
            dispatchEvent();
            return;
        }
        if (done_ || line.front() == ':' || line.compare(0, 5, "data:") != 0) {
            lineBuffer_.clear();
            return;
        }

        auto data = line.substr(5);
        if (!data.empty() && data.front() == ' ')
            data.remove_prefix(1);
        if (!eventData_.empty())
            eventData_.push_back('\n');
        eventData_.append(data);
        lineBuffer_.clear();
    }

    void dispatchEvent() {
        if (eventData_.empty() || done_)
            return;
        // View the buffered event; json::parse copies what it needs, so the
        // buffer can be reused once the event is parsed.
        auto const data = std::string_view{eventData_};
        if (data == "[DONE]") {
            done_ = true;
            eventData_.clear();
            return;
        }
        auto event = json::parse(data);
        eventData_.clear();
        auto const& choices = jsonGet(event, "choices");
        if (!choices.is_array())
            throw std::runtime_error("Provider stream event has no choices array.");
        if (choices.empty())
            return;

        auto const& delta = jsonGet(choices.front(), "delta");
        auto const& content = jsonGet(delta, "content");
        if (content.is_string())
            appendContent(content.get<std::string>());

        auto const& fragments = jsonGet(delta, "tool_calls");
        if (!fragments.is_array())
            return;
        for (auto const& fragment : fragments) {
            auto const& indexValue = jsonGet(fragment, "index");
            if (!indexValue.is_number() || indexValue.get<double>() < 0
                || indexValue.get<double>()
                       > static_cast<double>(std::numeric_limits<int>::max()))
                throw std::runtime_error("Provider stream returned an invalid tool-call index.");
            auto const index = static_cast<size_t>(indexValue.get<double>());
            auto& toolCall = toolCalls_[index];
            auto const& id = jsonGet(fragment, "id");
            if (id.is_string())
                toolCall.id.append(id.get<std::string>());
            auto const& function = jsonGet(fragment, "function");
            auto const& name = jsonGet(function, "name");
            if (name.is_string())
                toolCall.name.append(name.get<std::string>());
            auto const& arguments = jsonGet(function, "arguments");
            if (arguments.is_string())
                toolCall.arguments.append(arguments.get<std::string>());
        }
    }

    void appendContent(std::string const& fragment) {
        if (fragment.empty())
            return;
        if (!printedText_) {
            out_ << "Assistant: ";
            printedText_ = true;
        }
        content_.append(fragment);
        out_ << fragment << std::flush;
    }

    std::ostream& out_;
    std::string lineBuffer_;
    std::string eventData_;
    std::string content_;
    std::map<size_t, StreamToolCall> toolCalls_;
    bool done_ = false;
    bool printedText_ = false;
};

} // namespace agent
