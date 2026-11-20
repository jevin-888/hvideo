#ifndef HSVJ_RESOLUTION_H
#define HSVJ_RESOLUTION_H

#include <string>

namespace hsvj {

/**
 * @brief 分辨率结构
 * 
 * 表示宽度和高度，支持字符串转换
 */
struct Resolution {
    int width;   // 宽度（像素）
    int height;  // 高度（像素）
    
    /**
     * @brief 默认构造函数
     * 注意：分辨率必须从config.json加载，不允许硬编码默认值
     */
    Resolution() : width(0), height(0) {}
    
    /**
     * @brief 构造函数
     * @param w 宽度（像素）
     * @param h 高度（像素）
     */
    Resolution(int w, int h) : width(w), height(h) {}
    
    /**
     * @brief 转换为字符串
     * @return 字符串（格式："宽度 高度"）
     */
    std::string toString() const {
        return std::to_string(width) + " " + std::to_string(height);
    }
    
    /**
     * @brief 从字符串解析分辨率
     * @param str 字符串（格式："宽度 高度"）
     * @return 分辨率对象
     */
    static Resolution fromString(const std::string& str) {
        Resolution res;
        size_t pos = str.find(' ');
        if (pos != std::string::npos) {
            res.width = std::stoi(str.substr(0, pos));
            res.height = std::stoi(str.substr(pos + 1));
        }
        return res;
    }
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_RESOLUTION_H

