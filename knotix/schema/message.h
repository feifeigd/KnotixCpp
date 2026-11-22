#pragma once

#include "../json.hpp"

namespace message {

// RoleType is the type of the role of a message.
enum class RoleType {
    // Assistant is the role of an assistant, means the message is returned by ChatModel.
    Assistant,
    // User is the role of a user, means the message is a user message.
    User,
    // System is the role of a system, means the message is a system message.
    System,
    // Tool is the role of a tool, means the message is a tool call output.
    Tool,
};
NLOHMANN_JSON_SERIALIZE_ENUM(RoleType, {
    {RoleType::Assistant, "assistant"},
    {RoleType::User,      "user"},
    {RoleType::System,    "system"},
    {RoleType::Tool,      "tool"},
})

// FunctionCall is the function call in a message.
// It's used in Assistant Message.
struct FunctionCall {
    // Name is the name of the function to call, it can be used to identify the specific function.
    std::string Name; // name
    // Arguments is the arguments to call the function with, in JSON format.
    std::string Arguments; // arguments
};

inline void from_json(const json& j, FunctionCall& fc) {
    j.at("name").get_to(fc.Name);
    j.at("arguments").get_to(fc.Arguments);
}

struct Message {
    RoleType Role;
    std::string Content;
};

inline void from_json(const json& j, Message& message) {
    j.at("role").get_to(message.Role);
    j.at("content").get_to(message.Content);
}

inline void to_json(json& j, const Message& message) {
    j = json{
        {"role", message.Role},
        {"content", message.Content},
    };
}

} // namespace message
