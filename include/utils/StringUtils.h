/**
 * @file StringUtils.h（文件名）
 * @brief 字符串工具类定义
 * 
 * 本文件定义了字符串工具类，提供：
 * - 字符串分割
 * - 字符串修剪
 * - 字符串替换
 */

#ifndef HSVJ_STRING_UTILS_H
#define HSVJ_STRING_UTILS_H

#include <string>
#include <vector>
#include <sstream>

namespace hsvj {

class StringUtils {
public:
    /**
     * @brief 分割字符串
     * @param str 源字符串
     * @param delimiter 分隔符
     * @return 分割后的字符串列表
     */
    static std::vector<std::string> split(const std::string& str, char delimiter);

    /**
     * @brief 分割字符串（忽略空字符串）
     * @param str 源字符串
     * @param delimiter 分隔符
     * @return 分割后的字符串列表
     */
    static std::vector<std::string> splitIgnoreEmpty(const std::string& str, char delimiter);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_STRING_UTILS_H
