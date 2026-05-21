#pragma once

#include <map>
#include <string>
#include <vector>

namespace dptx {

std::string trim(std::string value);
std::string strip_quotes(std::string value);
std::string json_escape(const std::string& value);
std::string md5_hex(const std::string& value);
std::vector<std::string> split_path(const std::string& path);
std::string get_header(const std::map<std::string, std::string>& headers, const std::string& name);
std::string extract_json_string(const std::string& body, const std::string& key, const std::string& fallback = "");
int extract_json_int(const std::string& body, const std::string& key, int fallback = 0);
bool extract_json_bool(const std::string& body, const std::string& key, bool fallback = false);
std::vector<int> extract_json_int_array(const std::string& body, const std::string& key);

} // namespace dptx
