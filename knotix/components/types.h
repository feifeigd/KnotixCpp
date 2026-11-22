#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace components {
    struct ITyper{
        virtual std::string GetType() = 0;
    };

    struct IChecker {
        virtual bool IsCallbacksEnabled() = 0;
    };

    enum class Component {
        Prompt,
        AgenticPrompt,
        ChatModel,
        AgenticModel,
        Embedding,
        Indexer,
        Retriever,
        Loader,
        Transformer,
        Tool,
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(Component, {
        {Component::Prompt,         "ChatTemplate"},
        {Component::AgenticPrompt,  "AgenticChatTemplate"},
        {Component::ChatModel,      "ChatModel"},
        {Component::AgenticModel,   "AgenticModel"},
        {Component::Embedding,      "Embedding"},
        {Component::Indexer,        "Indexer"},
        {Component::Retriever,      "Retriever"},
        {Component::Loader,         "Loader"},
        {Component::Transformer,    "DocumentTransformer"},
        {Component::Tool,           "Tool"},
    })
}
