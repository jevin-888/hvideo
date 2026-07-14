/**
 * @file CommandRouter_LayerRender.cpp（文件名）
 * @brief CommandRouter 图层渲染相关命令处理实现
 *
 * 此文件包含 handleLayerRender 函数的实现
 * 从 CommandRouter.cpp 拆分出来以减小主文件体积
 */

#include "core/CommandRouter.h"
#include "utils/SliceConfigJson.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "layer/LayerImage.h"
#include "layer/LayerText.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

// Base64 解码辅助函数 - 在 CommandRouter.cpp 中定义
static std::string base64_decode(const std::string &in) {
  std::string out;
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++)
    T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] =
        i;

  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (T[c] == -1)
      break;
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

CommandResponse CommandRouter::handleLayerRender(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x03;
  response.timestamp = std::time(nullptr);

  if (!mubu_) {
    response.ok = false;
    response.error = 0x0008;
    response.message = "Mubu not initialized";
    return response;
  }

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  if (!param.isMember("action") || !param["action"].isString()) {
    setParamError(response, "Missing or invalid 'action' parameter");
    return response;
  }

  if (!param.isMember("layerId") || !param["layerId"].isInt()) {
    setParamError(response, "Missing or invalid layerId parameter");
    return response;
  }

  int layerId = param["layerId"].asInt();
  Layer *layer = getLayerWithCheck(layerId, response);
  if (!layer) {
    return response;
  }

  std::string action = param["action"].asString();
  Json::Value data;
  data["layerId"] = layerId;

  if (action == "generate_qrcode") {
    // 生成二维码（Layer71）
    const bool isQrLayer = layer->getType() == LayerType::QRCODE || layerId == 71;
    if (!isQrLayer) {
      response.ok = false;
      response.error = 0x0102; // 图层类型无效
      response.message = "Layer type must be QRCODE for QR code generation";
      return response;
    }

    LayerImage *imageLayer = static_cast<LayerImage *>(layer);
    bool success = false;

    // 优先处理前端生成的图片数据
    if (param.isMember("image_data") && param["image_data"].isString()) {
      std::string base64Data = param["image_data"].asString();
      if (!base64Data.empty()) {
        // 去除可能的 header (data:image/png;base64,)
        size_t commaPos = base64Data.find(",");
        if (commaPos != std::string::npos) {
          base64Data = base64Data.substr(commaPos + 1);
        }

        // 解码 Base64
        std::string binaryData = base64_decode(base64Data);

        // 确保输出目录存在 (PathConfig 已处理兼容性)
        if (!FileUtils::exists(QR_CODE_DIR)) {
          FileUtils::createDirectory(QR_CODE_DIR);
        }

        // 生成保存路径
        std::string filename = "qrcode_" + std::to_string(layerId) + ".png";
        std::string fullPath = FileUtils::joinPath(QR_CODE_DIR, filename);

        // 保存文件
        if (FileUtils::writeTextFile(fullPath, binaryData)) {
          LOG_INFO("Saved QR code image to: %s", fullPath.c_str());

          // 使缓存失效，强制重新加载（因为路径不变但文件内容已更新）
          LOG_INFO("[QRCode] Invalidating cache for layer %d before reload", layerId);
          imageLayer->invalidateCache();
          
          // 加载图片到图层（新保存的图片会覆盖原来的图片）
          LOG_INFO("[QRCode] Loading new QR image: %s", fullPath.c_str());
          LOG_INFO("[QRCode] Layer71状态 - visible=%d, renderer=%p, position=(%d,%d), size=(%dx%d)",
                   layer->isVisible(), imageLayer->getRenderer(),
                   layer->getPosition().x, layer->getPosition().y,
                   layer->getSize().width, layer->getSize().height);
          
          if (imageLayer->loadImage(fullPath)) {
            LOG_INFO("[QRCode] Successfully loaded QR image for layer %d", layerId);
            LOG_INFO("[QRCode] 加载后状态 - textureId=%u, width=%d, height=%d",
                     imageLayer->getTextureId(), imageLayer->getWidth(), imageLayer->getHeight());
            success = true;
            data["generated"] = true;
            data["path"] = fullPath;
            data["source"] = "frontend";

            // 先获取图片的原始尺寸（loadImage后getSize返回的是图片原始尺寸）
            Size originalSize = imageLayer->getSize();

            // 更新图层的位置、尺寸和优先级（使用前端传递的参数）
            // 这样新保存的图片会按照用户设置的位置和尺寸显示
            if (param.isMember("position") && param["position"].isObject()) {
              const Json::Value &pos = param["position"];
              if (pos.isMember("x") && pos["x"].isInt() && pos.isMember("y") &&
                  pos["y"].isInt()) {
                Position newPos(pos["x"].asInt(), pos["y"].asInt());
                layer->setPosition(newPos);
                LOG_INFO("二维码图层 %d 位置已更新: (%d, %d)", layerId,
                         newPos.x, newPos.y);
              }
            }

            // 处理用户设置的尺寸（参数名为 layer_size）
            int userWidth = 0;
            int userHeight = 0;
            if (param.isMember("layer_size") &&
                param["layer_size"].isObject()) {
              const Json::Value &sz = param["layer_size"];
              if (sz.isMember("width") && sz["width"].isInt() &&
                  sz.isMember("height") && sz["height"].isInt()) {
                userWidth = sz["width"].asInt();
                userHeight = sz["height"].asInt();
              }
            }
            
            // 如果用户未设置尺寸或尺寸为0，使用图片原始尺寸
            if (userWidth <= 0 || userHeight <= 0) {
              userWidth = originalSize.width;
              userHeight = originalSize.height;
              LOG_INFO("二维码图层 %d 使用图片原始尺寸: %dx%d", layerId, userWidth, userHeight);
            }
            
            // 应用尺寸
            Size newSize(userWidth, userHeight);
            layer->setSize(newSize);
            LOG_INFO("二维码图层 %d 尺寸已更新: %dx%d", layerId, userWidth,
                     userHeight);

            if (param.isMember("priority") && param["priority"].isInt()) {
              int newPriority = param["priority"].asInt();
              if (newPriority > 0) {
                layer->setPriority(newPriority);
                mubu_->sortLayersByPriority();
              }
            }

            // 确保图层可见，以便新保存的图片能正确显示
            if (!layer->isVisible()) {
              layer->setVisible(true);
              LOG_INFO("二维码图层 %d 已设置为可见", layerId);
            }

            // 返回尺寸：如果用户设置了非0值，返回用户设置的值；如果为0，返回图片原始尺寸
            if (userWidth > 0 && userHeight > 0) {
              // 用户设置了尺寸，返回用户设置的值
              data["width"] = userWidth;
              data["height"] = userHeight;
            } else {
              // 用户设置为0，返回图片的原始尺寸
              data["width"] = originalSize.width;
              data["height"] = originalSize.height;
            }

            // 同步配置保存：只存文件名，不存路径；运行时用 ROOT_PATH + Image/ + 文件名
            updateLayerConfigAndSave(layerId, [fullPath, &param, layer, layerId](LayerConfigData &config) {
              // Layer70 (Logo) 路径由 logo/ 目录硬编码，不使用 imagePath
              // Layer71 (QRCode) 路径由 QRCode/ 目录生成，不使用 imagePath
              if (layerId != 70 && layerId != 71)
                config.imagePath =
                    ROOT_PATH + "Image/" + FileUtils::getFilename(fullPath);

              // 更新位置参数（使用前端传递的值）
              if (param.isMember("position") && param["position"].isObject()) {
                const Json::Value &pos = param["position"];
                if (pos.isMember("x") && pos["x"].isInt()) {
                  config.position.x = pos["x"].asInt();
                }
                if (pos.isMember("y") && pos["y"].isInt()) {
                  config.position.y = pos["y"].asInt();
                }
              } else {
                // 如果参数中没有位置，使用图层当前的位置
                config.position = layer->getPosition();
              }

              // 更新尺寸参数（使用前端传递的值，参数名为 layer_size）
              if (param.isMember("layer_size") &&
                  param["layer_size"].isObject()) {
                const Json::Value &sz = param["layer_size"];
                int cfgWidth = 0;
                int cfgHeight = 0;
                if (sz.isMember("width") && sz["width"].isInt()) {
                  cfgWidth = sz["width"].asInt();
                }
                if (sz.isMember("height") && sz["height"].isInt()) {
                  cfgHeight = sz["height"].asInt();
                }
                // 如果尺寸为0，使用图层当前尺寸（已从图片加载）
                if (cfgWidth <= 0 || cfgHeight <= 0) {
                  config.size = layer->getSize();
                  LOG_INFO("二维码配置保存: 使用图层当前尺寸 %dx%d", config.size.width, config.size.height);
                } else {
                  config.size.width = cfgWidth;
                  config.size.height = cfgHeight;
                }
              } else {
                // 如果参数中没有尺寸，使用图层当前的尺寸
                config.size = layer->getSize();
              }

              // 更新优先级（使用前端传递的值）
              if (param.isMember("priority") && param["priority"].isInt()) {
                config.priority = param["priority"].asInt();
              } else {
                // 如果参数中没有优先级，使用图层当前的优先级
                config.priority = layer->getPriority();
              }

              // 同步图层的其他状态（确保重启后不丢失）
              config.visible = layer->isVisible();
              config.rotation = layer->getRotation();
              // 二维码图层不需要scale（缩放），保持默认值1.0
              // config.scale = layer->getScale(); // 二维码图层不使用scale
              config.alpha = layer->getAlpha();

              // 保存二维码参数，确保重启后不丢失
              if (param.isMember("content") && param["content"].isString()) {
                config.qrContent = param["content"].asString();
              }
              if (param.isMember("qr_text") && param["qr_text"].isString()) {
                config.qrText = param["qr_text"].asString();
              } else if (param.isMember("qrText") && param["qrText"].isString()) {
                config.qrText = param["qrText"].asString();
              }
              if (param.isMember("qr_size") && param["qr_size"].isInt()) {
                config.qrSize = param["qr_size"].asInt();
              } else if (param.isMember("qrSize") && param["qrSize"].isInt()) {
                config.qrSize = param["qrSize"].asInt();
              }
              if (param.isMember("error_correction") &&
                  param["error_correction"].isInt()) {
                config.qrErrorCorrection = param["error_correction"].asInt();
              }
              if (param.isMember("fg_color") && param["fg_color"].isString()) {
                config.qrFgColor = param["fg_color"].asString();
              }
              if (param.isMember("bg_color") && param["bg_color"].isString()) {
                config.qrBgColor = param["bg_color"].asString();
              } else if (param.isMember("bgColor") && param["bgColor"].isString()) {
                config.qrBgColor = param["bgColor"].asString();
              }
            });

            LOG_INFO("[QRCode] QR image updated");
          } else {
            LOG_ERROR("Failed to load saved QR code image: %s",
                      fullPath.c_str());
            response.message = "Failed to load saved QR code image";
          }
        } else {
          LOG_ERROR("Failed to save QR code image to: %s", fullPath.c_str());
          response.message = "Failed to save QR code file";
        }
      }
    }

    if (success) {
      response.ok = true;
      response.error = 0x0000;
      response.message = "二维码生成成功";
    } else {
      response.ok = false;
      response.error = 0x0104; // 图层操作失败
      if (response.message.empty()) {
        response.message = "Failed to save/load frontend generated QR code";
      }
    }

  } else if (action == "update_text") {
    // 更新文本内容
    if (layer->getType() != LayerType::TEXT) {
      response.ok = false;
      response.error = 0x0102; // 图层类型无效
      response.message = "Layer type must be TEXT for text update";
      return response;
    }

    if (!param.isMember("text") || !param["text"].isString()) {
      setParamError(response, "Missing text parameter");
      return response;
    }

    std::string text = param["text"].asString();
    LayerText *textLayer = static_cast<LayerText *>(layer);
    textLayer->setText(text);

    data["text"] = text;
    data["text_length"] = static_cast<int>(text.length());

    response.ok = true;
    response.error = 0x0000;
    response.message = "文本更新成功";

  } else if (action == "set_effect") {
    // 设置特效
    if (!param.isMember("effect_no") || !param["effect_no"].isInt()) {
      setParamError(response, "Missing effect_no parameter");
      return response;
    }

    int effectNo = param["effect_no"].asInt();
    Json::Value effectParams = param.isMember("effect_params")
                                   ? param["effect_params"]
                                   : Json::nullValue;

    layer->setEffect(effectNo, effectParams);

    data["effect_no"] = effectNo;
    data["effect_name"] = "EFFECT_" + std::to_string(effectNo);
    if (!effectParams.isNull()) {
      data["effect_params"] = effectParams;
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = "特效设置成功";

  } else if (action == "set_blend_mode") {
    // 设置混合模式
    if (!param.isMember("blend_mode") || !param["blend_mode"].isInt()) {
      setParamError(response, "Missing blend_mode parameter");
      return response;
    }

    int blendMode = param["blend_mode"].asInt();
    layer->setBlendMode(blendMode);

    data["blend_mode"] = blendMode;
    data["blend_mode_name"] = "BLEND_" + std::to_string(blendMode);

    response.ok = true;
    response.error = 0x0000;
    response.message = "混合模式设置成功";

  } else if (action == "create_slice") {
    // 创建切片
    if (!param.isMember("slice_index") || !param["slice_index"].isInt()) {
      setParamError(response, "Missing slice_index parameter");
      return response;
    }

    int sliceIndex = param["slice_index"].asInt();
    std::string sliceKey = "slice" + std::to_string(sliceIndex);

    // 创建默认切片配置
    Json::Value sliceConfig(Json::objectValue);

    // 如果提供了切片配置参数，使用提供的参数
    if (param.isMember("slice_config") && param["slice_config"].isObject()) {
      sliceConfig = param["slice_config"];
    } else {
      // 否则使用默认配置 - 尺寸必须从config.json配置，不使用硬编码
      sliceConfig["visible"] = true;
      sliceConfig["position"] = "0 0";
      sliceConfig["size"] = "0 0"; // 必须从config.json配置
    }

    sliceConfig = normalizeSliceJson(sliceConfig);
    layer->setSlice(sliceKey, sliceConfig);

    data["slice_key"] = sliceKey;
    data["slice_index"] = sliceIndex;
    data["slice_config"] = sliceConfig;

    response.ok = true;
    response.error = 0x0000;
    response.message = "切片创建成功";

  } else if (action == "loadImage") {
    // 加载图片到图像图层；支持 image_file（相对 Image/）或 path（绝对路径，供 U 盘媒体使用）
    if (layer->getType() != LayerType::IMAGE) {
      response.ok = false;
      response.error = 0x0102; // 图层类型无效
      response.message = "Layer type must be IMAGE for image loading";
      return response;
    }

    bool hasPath = param.isMember("path") && param["path"].isString() &&
                   !param["path"].asString().empty();
    bool hasImageFile = param.isMember("image_file") && param["image_file"].isString() &&
                        !param["image_file"].asString().empty();
    if (!hasPath && !hasImageFile) {
      setParamError(response, "Missing image_file or path parameter");
      return response;
    }

    std::string imageFile = hasImageFile ? param["image_file"].asString() : "";
    // Layer70 (Logo) 始终从 logo/ 目录加载，不使用 Image/ 目录
    std::string imagePath;
    if (hasPath && layerId != 70) {
      imagePath = param["path"].asString();
    } else if (layerId == 70) {
      // 先尝试 Logo/，再尝试 logo/（兼容大小写目录）
      std::string logoPath = ROOT_PATH + "Logo/" + imageFile;
      if (FileUtils::exists(logoPath)) {
        imagePath = logoPath;
      } else {
        std::string altPath = ROOT_PATH + "logo/" + imageFile;
        if (FileUtils::exists(altPath)) {
          imagePath = altPath;
        } else {
          imagePath = logoPath; // 使用默认路径，让后续加载报错
        }
      }
    } else {
      imagePath = ROOT_PATH + "Image/" + imageFile;
    }
    LayerImage *imageLayer = static_cast<LayerImage *>(layer);

    std::string normalizedPath = FileUtils::normalizePath(imagePath);
    if (normalizedPath.find("/../") != std::string::npos ||
        (normalizedPath.length() >= 3 && normalizedPath.substr(0, 3) == "../") ||
        (normalizedPath.length() >= 3 &&
         normalizedPath.substr(normalizedPath.length() - 3) == "/..")) {
      response.ok = false;
      response.error = 0x0104;
      response.message = "Invalid image path";
      return response;
    }
    LOG_INFO("CommandRouter: 图层 %d 加载图片: %s", layerId,
             normalizedPath.c_str());

    if (!FileUtils::exists(normalizedPath)) {
      response.ok = false;
      response.error = 0x0104;
      response.message = "Image file not found: " + normalizedPath;
      LOG_ERROR("CommandRouter: 图层 %d 图片文件不存在: %s", layerId,
                normalizedPath.c_str());
      return response;
    }

    // 加载图片时自动关闭照片墙模式
    imageLayer->setPhotoWallMode(false);
    if (layerId == 60 && !param.isMember("displayDuration")) {
      imageLayer->setDisplayDuration(3600.0f);
    }

    if (imageLayer->loadImage(normalizedPath)) {
      data["image_file"] = imageFile.empty() ? FileUtils::getFilename(normalizedPath) : imageFile;
      data["path"] = normalizedPath;
      data["loaded"] = true;
      response.ok = true;
      response.error = 0x0000;
      response.message = "图片加载成功";
      LOG_INFO("CommandRouter: 图层 %d 加载图片成功", layerId);
    } else {
      response.ok = false;
      response.error = 0x0104; // 图层操作失败
      response.message = "Failed to load image: " + normalizedPath;
      LOG_ERROR("CommandRouter: 图层 %d 加载图片失败: %s", layerId,
                normalizedPath.c_str());
    }

  } else if (action == "add_photo") {
    // 添加照片到照片墙
    if (layer->getType() != LayerType::IMAGE) {
      response.ok = false;
      response.error = 0x0102; // 图层类型无效
      response.message = "Layer type must be IMAGE for photo wall";
      return response;
    }

    if (!param.isMember("path") || !param["path"].isString()) {
      setParamError(response, "Missing path parameter");
      return response;
    }

    std::string imagePath = param["path"].asString();
    LayerImage *imageLayer = static_cast<LayerImage *>(layer);

    // 处理相对路径
    if (imagePath.length() > 0 && imagePath[0] != '/' && imagePath[0] != '.') {
      if (imagePath.find("Image/") == 0 || imagePath.find("image/") == 0) {
        imagePath = ROOT_PATH + imagePath;
      }
    }

    std::string normalizedPath = FileUtils::normalizePath(imagePath);
    LOG_INFO("CommandRouter: 图层 %d 添加照片: %s", layerId,
             normalizedPath.c_str());

    // 添加照片时自动开启照片墙模式
    imageLayer->setPhotoWallMode(true);

    if (imageLayer->addPhoto(normalizedPath)) {
      data["path"] = normalizedPath;
      data["added"] = true;
      response.ok = true;
      response.error = 0x0000;
      response.message = "照片添加成功";
    } else {
      response.ok = false;
      response.error = 0x0104; // 图层操作失败
      response.message = "Failed to add photo: " + normalizedPath;
    }

  } else if (action == "clear_photos") {
    // 清空照片墙
    if (layer->getType() != LayerType::IMAGE) {
      response.ok = false;
      response.error = 0x0102; // 图层类型无效
      response.message = "Layer type must be IMAGE for photo wall";
      return response;
    }

    LayerImage *imageLayer = static_cast<LayerImage *>(layer);
    imageLayer->clearPhotos();

    data["cleared"] = true;
    response.ok = true;
    response.error = 0x0000;
    response.message = "照片墙已清空";

  } else {
    response.ok = false;
    response.error = 0x000A; // 操作不支持
    response.message = "Unsupported action: " + action;
    return response;
  }

  // 生成响应数据JSON
  response.dataJson = jsonToString(data);

  return response;
}

} // 命名空间 hsvj
