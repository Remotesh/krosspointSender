// Internal JSON helpers for the storage files of src/net (storage.cpp,
// packs.cpp). Not part of the public API.
#pragma once

#include "net/net_internal.h"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace ks::net::detail {

using nlohmann::json;

// The one mutex of all storage functions. It is recursive: an API function
// can call another API function.
std::recursive_mutex& storeMutex();

// Reads a JSON file. A missing file or bad JSON gives a null value.
json readJson(const std::string& path);
// Writes a JSON file atomically. Throws std::runtime_error on failure.
void writeJson(const std::string& path, const json& j);

// Reads one field. A missing field or a wrong type gives def.
template <typename T>
T get(const json& j, const char* key, T def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

std::vector<std::string> getStrings(const json& j, const char* key, std::vector<std::string> def = {});

json articleToJson(const Article& a);
Article articleFromJson(const json& j);
json transferToJson(const TransferRecord& r);
TransferRecord transferFromJson(const json& j);

// PackLimits as JSON. Out-of-range values are clamped to the UI ranges.
json limitsToJson(const PackLimits& l);
PackLimits packLimitsFromJson(const json& j);

// Removes every stored pack (packs.cpp). clearLibrary() calls it.
void clearPacks();

// The id is a folder name. Accept only letters, digits, '-' and '_'.
bool safeId(const std::string& id);

// Image folders: <dir>/NNN.bin plus <dir>/index.json (src -> file name).
// writeImageDir replaces the folder. An empty map removes it.
void writeImageDir(const std::string& dir, const ImageMap& images);
ImageMap readImageDir(const std::string& dir);

} // namespace ks::net::detail
