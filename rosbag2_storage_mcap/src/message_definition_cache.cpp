// Copyright 2022, Foxglove Technologies. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rosbag2_storage_mcap/message_definition_cache.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <ament_index_cpp/get_resource.hpp>
#include <ament_index_cpp/get_resources.hpp>

#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace rosbag2_storage_mcap::internal {

// Match datatype names (foo_msgs/Bar or foo_msgs/msg/Bar)
static const std::regex PACKAGE_TYPENAME_REGEX{R"(^([a-zA-Z0-9_]+)/(?:msg/)?([a-zA-Z0-9_]+)$)"};

// Match field types from .msg definitions ("foo_msgs/Bar" in "foo_msgs/Bar[] bar")
static const std::regex MSG_FIELD_TYPE_REGEX{R"((?:^|\n)\s*([a-zA-Z0-9_/]+)(?:\[[^\]]*\])?\s+)"};

// match field types from `.idl` definitions ("foo_msgs/msg/bar" in #include <foo_msgs/msg/Bar.idl>)
static const std::regex IDL_FIELD_TYPE_REGEX{
  R"((?:^|\n)#include\s+(?:"|<)([a-zA-Z0-9_/]+)\.idl(?:"|>))"};

static const std::unordered_set<std::string> PRIMITIVE_TYPES{
  "bool",  "byte",   "char",  "float32", "float64", "int8",   "uint8",
  "int16", "uint16", "int32", "uint32",  "int64",   "uint64", "string"};

static std::set<std::string> parse_msg_dependencies(const std::string& text,
                                                    const std::string& package_context) {
  std::set<std::string> dependencies;

  for (std::sregex_iterator iter(text.begin(), text.end(), MSG_FIELD_TYPE_REGEX);
       iter != std::sregex_iterator(); ++iter) {
    std::string type = (*iter)[1];
    if (PRIMITIVE_TYPES.find(type) != PRIMITIVE_TYPES.end()) {
      continue;
    }
    if (type.find('/') == std::string::npos) {
      dependencies.insert(package_context + '/' + std::move(type));
    } else {
      dependencies.insert(std::move(type));
    }
  }
  return dependencies;
}

static std::set<std::string> parse_idl_dependencies(const std::string& text) {
  std::set<std::string> dependencies;

  for (std::sregex_iterator iter(text.begin(), text.end(), IDL_FIELD_TYPE_REGEX);
       iter != std::sregex_iterator(); ++iter) {
    dependencies.insert(std::move((*iter)[1]));
  }
  return dependencies;
}

std::set<std::string> parse_dependencies(MessageDefinitionFormat format, const std::string& text,
                                         const std::string& package_context) {
  switch (format) {
    case MessageDefinitionFormat::ROS2MSG:
      return parse_msg_dependencies(text, package_context);
    case MessageDefinitionFormat::ROS2IDL:
      return parse_idl_dependencies(text);
    default:
      throw std::runtime_error("switch is not exhaustive");
  }
}

static const char* extension_for_format(MessageDefinitionFormat format) {
  switch (format) {
    case MessageDefinitionFormat::ROS2MSG:
      return ".msg";
    case MessageDefinitionFormat::ROS2IDL:
      return ".idl";
    default:
      throw std::runtime_error("switch is not exhaustive");
  }
}

static bool msg_definition_exists(const std::string& package_resource_name) {
  std::smatch match;
  if (!std::regex_match(package_resource_name, match, PACKAGE_TYPENAME_REGEX)) {
    throw std::invalid_argument("Invalid package resource name: " + package_resource_name);
  }
  std::string package = match[1];
  std::string share_dir = ament_index_cpp::get_package_share_directory(package);
  std::ifstream file{share_dir + "/msg/" + match[2].str() + ".msg"};
  return file.good();
}

MessageSpec::MessageSpec(MessageDefinitionFormat format, std::string text,
                         const std::string& package_context)
    : dependencies(parse_dependencies(format, text, package_context))
    , text(std::move(text))
    , format(format) {}

const MessageSpec& MessageDefinitionCache::load_message_spec(
  const DefinitionIdentifier& definition_identifier) {
  if (auto it = msg_specs_by_definition_identifier_.find(definition_identifier);
      it != msg_specs_by_definition_identifier_.end()) {
    return it->second;
  }
  std::smatch match;
  if (!std::regex_match(definition_identifier.package_resource_name, match,
                        PACKAGE_TYPENAME_REGEX)) {
    throw std::invalid_argument("Invalid package resource name: " +
                                definition_identifier.package_resource_name);
  }
  std::string package = match[1];
  std::string share_dir = ament_index_cpp::get_package_share_directory(package);
  std::ifstream file{share_dir + "/msg/" + match[2].str() +
                     extension_for_format(definition_identifier.format)};

  std::string contents{std::istreambuf_iterator(file), {}};
  const MessageSpec& spec =
    msg_specs_by_definition_identifier_
      .emplace(definition_identifier,
               MessageSpec(definition_identifier.format, std::move(contents), package))
      .first->second;

  // "References and pointers to data stored in the container are only invalidated by erasing that
  // element, even when the corresponding iterator is invalidated."
  return spec;
}

std::pair<MessageDefinitionFormat, std::string> MessageDefinitionCache::get_full_text(
  const std::string& root_package_resource_name) {
  MessageDefinitionFormat format = MessageDefinitionFormat::ROS2MSG;
  if (!msg_definition_exists(root_package_resource_name)) {
    format = MessageDefinitionFormat::ROS2IDL;
  }
  std::string result;
  std::unordered_set<std::string> seen_deps = {root_package_resource_name};
  std::function<void(const std::string&)> append_recursive =
    [&](const std::string& package_resource_name) {
      const MessageSpec& spec =
        load_message_spec(DefinitionIdentifier{format, package_resource_name});
      if (format == MessageDefinitionFormat::ROS2IDL) {
        result +=
          "\n================================================================================\nIDL:"
          " ";
        result += package_resource_name;
        result += '\n';
      } else if (!result.empty()) {
        result +=
          "\n================================================================================\nMSG:"
          " ";
        result += package_resource_name;
        result += '\n';
      }
      result += spec.text;
      for (const auto& dep : spec.dependencies) {
        bool inserted = seen_deps.insert(dep).second;
        if (inserted) {
          append_recursive(dep);
        }
      }
    };
  append_recursive(root_package_resource_name);
  return std::make_pair(format, result);
}

}  // namespace rosbag2_storage_mcap::internal
