#include "dptx/Utils.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <openssl/evp.h>
#include <regex>
#include <sstream>

namespace dptx {

std::string trim(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !is_space(c); }).base(), value.end());
    return value;
}

std::string strip_quotes(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
        }
    }
    return out.str();
}

std::string md5_hex(const std::string& value) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, value.data(), value.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream out;
    for (unsigned int i = 0; i < digest_len; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return out.str();
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string item;
    while (std::getline(ss, item, '/')) {
        if (!item.empty()) {
            parts.push_back(item);
        }
    }
    return parts;
}

std::string get_header(const std::map<std::string, std::string>& headers, const std::string& name) {
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto iter = headers.find(lower);
    return iter == headers.end() ? "" : iter->second;
}

std::string extract_json_string(const std::string& body, const std::string& key, const std::string& fallback) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(body, match, pattern) && match.size() > 1) {
        return match[1].str();
    }
    return fallback;
}

int extract_json_int(const std::string& body, const std::string& key, int fallback) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (std::regex_search(body, match, pattern) && match.size() > 1) {
        return std::stoi(match[1].str());
    }
    return fallback;
}

bool extract_json_bool(const std::string& body, const std::string& key, bool fallback) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false|1|0|\"true\"|\"false\"|\"1\"|\"0\")", std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(body, match, pattern) || match.size() <= 1) {
        return fallback;
    }
    std::string value = match[1].str();
    value = strip_quotes(value);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value == "true" || value == "1";
}

std::vector<int> extract_json_int_array(const std::string& body, const std::string& key) {
    std::vector<int> values;
    const std::regex array_pattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch array_match;
    if (!std::regex_search(body, array_match, array_pattern) || array_match.size() <= 1) {
        return values;
    }

    const std::string array_body = array_match[1].str();
    const std::regex number_pattern("-?[0-9]+");
    for (std::sregex_iterator it(array_body.begin(), array_body.end(), number_pattern), end; it != end; ++it) {
        values.push_back(std::stoi((*it).str()));
    }
    return values;
}

} // namespace dptx
