/**
 * @file Layer.cpp（文件名）
 * @brief 图层基类实现
 *
 * 本文件实现了Layer基类，是所有图层类型的基础：
 * - 图层属性管理（位置、大小、旋转、缩放、透明度等）
 * - 效果设置和参数管理
 * - 切片（slice）管理
 * - 图层优先级和混合模式
 * - 基础渲染接口
 */

#include "layer/Layer.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <algorithm>
#include <cmath>

namespace hsvj {

/**
 * @brief 构造函数
 *
 * 初始化图层的基本属性
 *
 * @param layerId 图层ID
 */
Layer::Layer(int layerId)
    : layerId_(layerId), type_(LayerType::VIDEO), visible_(true),
      position_(0, 0), size_(1920, 1080), rotation_(0.0f), scale_(1.0f),
      alpha_(1.0f), priority_(layerId), shapeType_(0), shapeParam_(0.0f),
      renderer_(nullptr), effectNo_(0), blendMode_(0) {}

/**
 * @brief 析构函数
 */
Layer::~Layer() {}

void Layer::setPosition(const Position &pos) {
    position_ = pos;
    roamBasePosition_ = pos;
    roamBasePositionSet_ = true;
    roamTime_ = 0.0f;
}

/**
 * @brief 设置图层效果
 *
 * @param effectNo 效果编号
 * @param effectParams 效果参数
 */
void Layer::setEffect(int effectNo, const Json::Value& effectParams) {
    effectNo_.store(effectNo, std::memory_order_release);
    effectParams_ = effectParams;
    LOG_DEBUG("Layer %d: Effect set to %d", layerId_, effectNo);
}

/**
 * @brief 设置图层切片
 *
 * @param sliceKey 切片键名
 * @param sliceConfig 切片配置
 */
void Layer::setSlice(const std::string& sliceKey, const Json::Value& sliceConfig) {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    if (sliceConfig.isObject()) {
        slices_[sliceKey] = sliceConfig;
        sliceRevision_.fetch_add(1, std::memory_order_release);
        LOG_DEBUG("Layer %d: Slice %s set", layerId_, sliceKey.c_str());
    }
}

/**
 * @brief 获取图层切片
 *
 * @param sliceKey 切片键名
 * @return 切片配置，如果不存在返回nullValue
 */
Json::Value Layer::getSlice(const std::string& sliceKey) const {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    auto it = slices_.find(sliceKey);
    if (it != slices_.end()) {
        return it->second;
    }
    return Json::nullValue;
}

/**
 * @brief 移除图层切片
 *
 * @param sliceKey 切片键名
 * @return 是否移除成功
 */
bool Layer::removeSlice(const std::string& sliceKey) {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    auto it = slices_.find(sliceKey);
    if (it != slices_.end()) {
        slices_.erase(it);
        sliceRevision_.fetch_add(1, std::memory_order_release);
        LOG_DEBUG("Layer %d: Slice %s removed", layerId_, sliceKey.c_str());
        return true;
    }
    return false;
}

Json::Value Layer::getAllSlices() const {
    Json::Value result(Json::objectValue);
    getAllSlices(result);
    return result;
}

bool Layer::getAllSlices(Json::Value &out) const {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    if (slices_.empty()) {
        out = Json::Value(Json::objectValue);
        return false;
    }
    Json::Value result(Json::objectValue);
    for (const auto& pair : slices_) {
        result[pair.first] = pair.second;
    }
    out.swap(result);
    return true;
}

bool Layer::forEachSlice(
    const std::function<void(const std::string &, const Json::Value &)> &visitor) const {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    if (slices_.empty()) {
        return false;
    }
    for (const auto &pair : slices_) {
        visitor(pair.first, pair.second);
    }
    return true;
}

bool Layer::hasSlices() const {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    return !slices_.empty();
}

void Layer::clearAllSlices() {
    std::lock_guard<std::mutex> lock(slicesMutex_);
    if (!slices_.empty()) {
        LOG_DEBUG("Layer %d: Clearing all %zu slices", layerId_, slices_.size());
        slices_.clear();
        sliceRevision_.fetch_add(1, std::memory_order_release);
    }
}

void Layer::setInvert(int mode) {
    invert_ = mode;
}

void Layer::updateRoamConfig(const Json::Value &roamConfigJson) {
    if (!roamConfigJson.isObject()) {
        roamEnabled_ = false;
        roamMode_ = 0;
        if (roamBasePositionSet_) {
            position_ = roamBasePosition_;
        }
        roamTime_ = 0.0f;
        return;
    }

    roamMode_ = roamConfigJson.get("mode", 0).asInt();
    roamEnabled_ = roamConfigJson.get("enabled", true).asBool();
    roamSpeed_ = roamConfigJson.get("speed", 100.0).asFloat();
    roamLoop_ = roamConfigJson.get("loop", true).asBool();
    roamRangeX_ = roamConfigJson.isMember("rangeX") ? roamConfigJson["rangeX"].asInt() : roamConfigJson.get("range_x", 500).asInt();
    roamRangeY_ = roamConfigJson.isMember("rangeY") ? roamConfigJson["rangeY"].asInt() : roamConfigJson.get("range_y", 500).asInt();
    roamRadius_ = roamConfigJson.get("radius", 200.0).asFloat();

    if (!roamBasePositionSet_) {
        roamBasePosition_ = position_;
        roamBasePositionSet_ = true;
    }

    if (!roamEnabled_ || roamMode_ == 0) {
        position_ = roamBasePosition_;
        roamTime_ = 0.0f;
    }
}

Json::Value Layer::getRoamConfig() const {
    Json::Value config(Json::objectValue);
    config["enabled"] = roamEnabled_;
    config["mode"] = roamMode_;
    config["speed"] = roamSpeed_;
    config["loop"] = roamLoop_;
    config["rangeX"] = roamRangeX_;
    config["rangeY"] = roamRangeY_;
    config["radius"] = roamRadius_;
    return config;
}

bool Layer::hasActiveRoam() const {
    return roamEnabled_ && roamMode_ != 0 && roamSpeed_ > 0.0f;
}

void Layer::updateRoam(float deltaTime) {
    if (!roamEnabled_ || roamMode_ == 0 || roamSpeed_ <= 0.0f) {
        return;
    }

    if (!roamBasePositionSet_) {
        roamBasePosition_ = position_;
        roamBasePositionSet_ = true;
    }

    roamTime_ += deltaTime;
    Position newPos = roamBasePosition_;

    if (roamMode_ == 1) {
        // 左右移动：正弦波
        if (roamRangeX_ > 0) {
            float cycleTime = (2.0f * roamRangeX_) / roamSpeed_;
            float normalizedTime = roamTime_;

            if (roamLoop_) {
                normalizedTime = fmod(normalizedTime, cycleTime);
            } else if (normalizedTime > cycleTime) {
                normalizedTime = cycleTime;
            }

            float progress = (normalizedTime / cycleTime) * 2.0f * M_PI;
            float offset = sin(progress) * roamRangeX_;
            newPos.x = roamBasePosition_.x + static_cast<int>(offset);
        }
    } else if (roamMode_ == 2) {
        // 上下移动：正弦波
        if (roamRangeY_ > 0) {
            float cycleTime = (2.0f * roamRangeY_) / roamSpeed_;
            float normalizedTime = roamTime_;

            if (roamLoop_) {
                normalizedTime = fmod(normalizedTime, cycleTime);
            } else if (normalizedTime > cycleTime) {
                normalizedTime = cycleTime;
            }

            float progress = (normalizedTime / cycleTime) * 2.0f * M_PI;
            float offset = sin(progress) * roamRangeY_;
            newPos.y = roamBasePosition_.y + static_cast<int>(offset);
        }
    } else if (roamMode_ == 3) {
        // 圆形路径
        if (roamRadius_ > 0) {
            float circumference = 2.0f * M_PI * roamRadius_;
            float cycleTime = circumference / roamSpeed_;
            float normalizedTime = roamTime_;

            if (roamLoop_) {
                normalizedTime = fmod(normalizedTime, cycleTime);
            } else if (normalizedTime > cycleTime) {
                normalizedTime = cycleTime;
            }

            float angle = (normalizedTime / cycleTime) * 2.0f * M_PI;
            newPos.x = roamBasePosition_.x + static_cast<int>(cos(angle) * roamRadius_);
            newPos.y = roamBasePosition_.y + static_cast<int>(sin(angle) * roamRadius_);
        }
    }

    position_ = newPos;
}

} // 命名空间 hsvj
