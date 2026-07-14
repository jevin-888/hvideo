/**
 * @file StbiImageGuard.h（文件名）
 * @brief 用于 stbi_image 的 RAII 包装，防止内存泄漏
 */

#ifndef HSVJ_STBI_IMAGE_GUARD_H
#define HSVJ_STBI_IMAGE_GUARD_H

#include <stb_image.h>

namespace hsvj {

/**
 * @brief RAII wrapper for stbi_image 数据
 * 
 * Automatically frees stbi_image 数据 when going out of scope,
 * preventing memory leaks in 错误 paths.
 * 
 * 用法：
 * @code（示例代码开始）
 * StbiImageGuard guard(stbi_load(路径.c_str(), &w, &h, &n, 4);
 * if (guard.获取()) {
 *     // 使用 guard.获取() to access the 数据
 *     // 数据 will be automatically freed when guard goes out of scope
 * }
 * @endcode（示例代码结束）
 */
class StbiImageGuard {
public:
    /**
     * @brief Construct guard with image 数据
     * @param 数据 Pointer to stbi_image 数据 (can be nullptr)
     */
    explicit StbiImageGuard(unsigned char* data) : data_(data) {}
    
    /**
     * @brief Destructor - automatically frees image 数据
     */
    ~StbiImageGuard() {
        if (data_) {
            stbi_image_free(data_);
            data_ = nullptr;
        }
    }
    
    // 禁用拷贝
    StbiImageGuard(const StbiImageGuard&) = delete;
    StbiImageGuard& operator=(const StbiImageGuard&) = delete;
    
    // 启用移动
    StbiImageGuard(StbiImageGuard&& other) noexcept : data_(other.data_) {
        other.data_ = nullptr;
    }
    
    StbiImageGuard& operator=(StbiImageGuard&& other) noexcept {
        if (this != &other) {
            if (data_) {
                stbi_image_free(data_);
            }
            data_ = other.data_;
            other.data_ = nullptr;
        }
        return *this;
    }
    
    /**
     * @brief 获取 the raw pointer (does not transfer ownership)
     * @return Pointer to image 数据
     */
    unsigned char* get() const { return data_; }
    
    /**
     * @brief 释放 ownership of the 数据
     * @return Pointer to image 数据 (caller must free manually)
     */
    unsigned char* release() {
        unsigned char* temp = data_;
        data_ = nullptr;
        return temp;
    }
    
    /**
     * @brief 检查 if guard holds valid 数据
     * @return true if 数据 is not nullptr
     */
    explicit operator bool() const { return data_ != nullptr; }

private:
    unsigned char* data_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_STBI_IMAGE_GUARD_H
