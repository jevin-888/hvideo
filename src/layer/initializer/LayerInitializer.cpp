/**
 * @file LayerInitializer.cpp（文件名）
 * @brief 图层初始化器基类实现
 */

#include "layer/initializer/LayerInitializer.h"
#include "layer/initializer/LayerConfigHelpers.h"
#include "utils/SliceConfigJson.h"  // 使用项目中已有的sliceConfigToJson
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <algorithm>

namespace hsvj {

// 静态成员初始化
std::set<std::pair<int, std::string>> LayerInitializer::s_fileNotFoundLogged_;
std::set<std::pair<int, std::string>> LayerInitializer::s_fileLoadFailLogged_;

LayerInitResult LayerInitializer::applyCommonProperties(
    Layer* layer, const LayerConfigData& config) {

    if (!layer) {
        return LayerInitResult::Failure("Layer is null");
    }

    const int layerId = layer->getLayerId();  // 修复：getId() -> getLayerId()

    // 设置可见性（图层60特殊处理）
    layer->setVisible(layerId == 60 ? false : config.visible);

    // 设置位置（允许负坐标）
    layer->setPosition(config.position);

    // 设置尺寸
    if (config.size.width > 0 && config.size.height > 0) {
        layer->setSize(config.size);
    } else {
        layer->setSize(Size(0, 0));
        if (layer->getType() == LayerType::VIDEO) {
            LOG_WARN("[LayerInit] Layer %d (VIDEO) has no size configured, "
                     "rendering may fail", layerId);
        }
    }

    // 设置旋转（范围验证）
    if (config.rotation >= -360.0f && config.rotation <= 360.0f) {
        layer->setRotation(config.rotation);
    }

    // 设置缩放（范围验证）
    if (config.scale > 0.0f && config.scale <= 10.0f) {
        layer->setScale(config.scale);
    } else {
        layer->setScale(1.0f);
    }

    // 设置透明度（范围验证）
    if (config.alpha >= 0.0f && config.alpha <= 1.0f) {
        layer->setAlpha(config.alpha);
    } else {
        layer->setAlpha(1.0f);
    }

    // 设置优先级
    layer->setPriority(config.priority);

    // 设置几何遮罩参数
    layer->setShapeType(config.shapeType);
    layer->setShapeParam(config.shapeParam);
    layer->setBlackToTransparent(config.blackToTransparent);
    layer->setEffectLinkedSlices(config.effectLinkedSlices);
    layer->setInvert(config.invert);
    layer->setGaussianBlur(config.gaussianBlur);

    // 加载切片配置
    if (!config.slices.empty()) {
        for (const auto& slicePair : config.slices) {
            const std::string& sliceKey = slicePair.first;
            const SliceConfig& sliceConfig = slicePair.second;

            // 将 SliceConfig 转换为 Json::Value
            layer->setSlice(sliceKey, sliceConfigToJson(sliceConfig));
        }
    }

    return LayerInitResult::Success();
}

std::string LayerInitializer::normalizeFilePath(
    const std::string& path, const std::string& rootPath) const {

    if (path.empty()) {
        return path;
    }

    std::string normalizedPath = path;

    // 处理相对路径
    if (path[0] != '/' && path[0] != '.') {
        // 移除路径中的 huoshan/ 前缀（如果有）
        if (path.find("huoshan/") == 0) {
            normalizedPath = rootPath + path.substr(8);
        }
        // 已经包含常见子目录
        else if (path.find("Logo/") == 0 ||
                 path.find("QRCode/") == 0 ||
                 path.find("qrcode/") == 0 ||
                 path.find("Image/") == 0 ||
                 path.find("image/") == 0) {
            normalizedPath = rootPath + path;
        }
    }

    return FileUtils::normalizePath(normalizedPath);
}

bool LayerInitializer::checkFileExists(
    const std::string& path, int layerId, const std::string& fileType) {

    if (path.empty()) {
        return false;
    }

    if (FileUtils::exists(path)) {
        return true;
    }

    // 记录文件不存在警告（去重）
    auto key = std::make_pair(layerId, path);
    if (s_fileNotFoundLogged_.find(key) == s_fileNotFoundLogged_.end()) {
        s_fileNotFoundLogged_.insert(key);
        LOG_WARN("[LayerInit] Layer %d %s file not found (logged once): %s",
                 layerId, fileType.c_str(), path.c_str());
    }

    return false;
}

} // 命名空间 hsvj
