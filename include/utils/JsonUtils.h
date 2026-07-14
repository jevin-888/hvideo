/**
 * @file JsonUtils.h（文件名）
 * @brief JSON工具类定义
 * 
 * 本文件定义了JSON工具类，提供：
 * - 统一的JSON解析方法
 * - JSON字符串转换
 * - 错误处理
 */

#ifndef HSVJ_JSON_UTILS_H
#define HSVJ_JSON_UTILS_H

#include <string>
#include <json/json.h>

namespace hsvj {

/**
 * @brief JSON工具类
 * 
 * 提供JSON操作的静态方法，统一JSON解析逻辑
 */
class JsonUtils {
public:
    /**
     * @brief 解析JSON字符串
     * @param json JSON字符串
     * @param root 解析后的JSON值（输出参数）
     * @param error 错误信息（输出参数，如果解析失败）
     * @return 是否解析成功
     */
    static bool parseJson(const std::string& json, Json::Value& root, std::string& error);
    
    /**
     * @brief 解析JSON字符串（不返回错误信息）
     * @param json JSON字符串
     * @param root 解析后的JSON值（输出参数）
     * @return 是否解析成功
     */
    static bool parseJson(const std::string& json, Json::Value& root);
    
    /**
     * @brief 将JSON值转换为字符串
     * @param value JSON值
     * @return JSON字符串
     */
    static std::string toString(const Json::Value& value);
    
    /**
     * @brief 将JSON值转换为格式化的字符串（带缩进）
     * @param value JSON值
     * @param indentSize 缩进大小（默认2）
     * @return 格式化的JSON字符串
     */
    static std::string toFormattedString(const Json::Value& value, int indentSize = 2);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_JSON_UTILS_H

