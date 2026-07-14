/**
 * @file JsonUtils.cpp（文件名）
 * @brief JSON工具类实现
 * 
 * 本文件实现了JSON工具类，提供统一的JSON解析和转换功能
 */

#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <sstream>

namespace hsvj {

bool JsonUtils::parseJson(const std::string& json, Json::Value& root, std::string& error) {
    if (json.empty()) {
        error = "Empty JSON string";
        return false;
    }
    
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    JSONCPP_STRING errors;
    
    const char* begin = json.c_str();
    const char* end = begin + json.length();
    
    if (!reader->parse(begin, end, &root, &errors)) {
        error = errors;
        return false;
    }
    
    return true;
}

bool JsonUtils::parseJson(const std::string& json, Json::Value& root) {
    std::string error;
    return parseJson(json, root, error);
}

std::string JsonUtils::toString(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = ""; // 不格式化，紧凑输出
    builder["precision"] = 6;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    std::ostringstream oss;
    writer->write(value, &oss);
    return oss.str();
}

std::string JsonUtils::toFormattedString(const Json::Value& value, int indentSize) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = std::string(indentSize, ' ');
    builder["precision"] = 6;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    std::ostringstream oss;
    writer->write(value, &oss);
    return oss.str();
}

} // 命名空间 hsvj

