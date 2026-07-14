/**
 * @file HttpServer_Playlist.cpp（文件名）
 * @brief 播放列表 API 实现
 * 
 * This file contains 播放列表-related API 路由注册s
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/CommandRouter.h"
#include "core/SystemConfig.h"
#include "database/PlaylistManager.h"
#include "database/PlaylistDatabase.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>

void HttpServer::registerPlaylistRoutes() {
  auto forbidVodReservedLayer = [this](int layerId, HttpResponse& response) -> bool {
    if (systemConfig_ && systemConfig_->isVodEnabled()) {
      int vodLayerId = systemConfig_->getVodLayerId();
      if (vodLayerId > 0 && layerId == vodLayerId) {
        Json::Value data;
        data["requestedLayerId"] = layerId;
        data["localVodTargetLayer"] = vodLayerId;
        data["vodMode"] = systemConfig_->getVodMode();
        data["enableVod"] = systemConfig_->isVodEnabled();
        setJsonErrorResponse(response, 409,
                             "Requested layer is reserved for VOD playback");
        return true;
      }
    }
    return false;
  };
  auto isFusionBackgroundImagePath = [](std::string path) -> bool {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return path.find("/image/gb_fusion/") != std::string::npos ||
           path.rfind("image/gb_fusion/", 0) == 0 ||
           path.rfind("gb_fusion/", 0) == 0;
  };

  // Get 播放列表 list
  get("/api/v1/playlists",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 未使用
        if (!checkPlaylistManager(response)) {
          return;
        }

        auto playlists = playlistManager_->listPlaylists();

        Json::Value playlistsJson(Json::arrayValue);
        for (const auto &playlist : playlists) {
          Json::Value playlistJson;
          playlistJson["id"] = playlist.id;
          playlistJson["name"] = playlist.name;
          playlistJson["count"] = playlist.count;
          playlistJson["isDefault"] = playlist.isDefault;
          playlistJson["targetLayerId"] = playlist.targetLayerId;
          playlistJson["dmxId"] = playlist.dmxId;
          playlistsJson.append(playlistJson);
        }

        setJsonDataResponse(response, playlistsJson, "播放列表加载成功");
      });

  // Set default 播放列表
  put("/api/v1/playlists/default", [this](const HttpRequest &request,
                                          HttpResponse &response) {
    if (!checkPlaylistManager(response)) {
      return;
    }

    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    // 播放列表Id can be empty string to clear default
    std::string playlistId = "";
    if (param.isMember("playlistId") && param["playlistId"].isString()) {
      playlistId = param["playlistId"].asString();
    }

    int layerId = -1;
    if (param.isMember("targetLayerId") && param["targetLayerId"].isInt()) {
      layerId = param["targetLayerId"].asInt();
    } else if (param.isMember("layerId") && param["layerId"].isInt()) {
      layerId = param["layerId"].asInt();
    } else if (!playlistId.empty() && playlistManager_) {
      layerId = playlistManager_->getPlaylistTargetLayer(playlistId);
    }

    if (playlistManager_->setDefaultPlaylist(playlistId, layerId)) {
      if (playlistId.empty()) {
        setJsonSuccessResponse(response, "Default playlist cleared");
      } else {
        setJsonSuccessResponse(response, "Default playlist set");
      }
    } else {
      setJsonErrorResponse(response, 500, "Failed to set default playlist");
    }
  });

  get("/api/v1/playlists/{id}/items",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkPlaylistManager(response)) {
          return;
        }

        std::string playlistId = request.getUrlParam("id");
        
        // Get layer ID: prefer query param, otherwise use 播放列表's target layer
        int layerId = 1; // 默认值
        std::string layerIdParam = request.getQueryParam("layerId");
        
        if (!layerIdParam.empty()) {
          // 说明：显式 layerId 参数
          if (!parseOptionalLayerId(layerIdParam, layerId, 1, response)) {
            return;
          }
        } else {
          // No layerId, get from 播放列表 config
          int targetLayerId = playlistManager_->getPlaylistTargetLayer(playlistId);
          if (targetLayerId > 0) {
            layerId = targetLayerId;
          }
        }

        // 说明：分页参数
        int offset = 0;
        int limit = -1;
        std::string offsetParam = request.getQueryParam("offset");
        std::string limitParam = request.getQueryParam("limit");
        if (!offsetParam.empty()) try { offset = std::stoi(offsetParam); } catch(...) {}
        if (!limitParam.empty()) try { limit = std::stoi(limitParam); } catch(...) {}

        LOG_DEBUG("Fetching playlist items: playlist=%s, layer=%d, offset=%d, limit=%d", 
                 playlistId.c_str(), layerId, offset, limit);

        std::vector<hsvj::PlaylistItem> items;
        if (limit > 0) {
          items = playlistManager_->getPlaylistItemsPaged(playlistId, layerId, offset, limit);
        } else {
          items = playlistManager_->getPlaylistItems(playlistId, layerId);
        }

        Json::Value itemsJson(Json::arrayValue);
        for (const auto &item : items) {
          Json::Value itemJson;
          itemJson["itemIndex"] = item.itemIndex;
          itemJson["path"] = item.uri;
          itemJson["title"] = item.title;
          itemJson["duration"] = item.duration;
          itemJson["inPoint"] = item.inPoint;
          itemJson["outPoint"] = item.outPoint;
          itemJson["audioTrack"] = item.audioTrack;
          itemJson["tags"] = item.tags;
          itemsJson.append(itemJson);
        }

        setJsonDataResponse(response, itemsJson, "播放列表项目加载成功");
      });

  // Create 播放列表
  post("/api/v1/playlists", [this, forbidVodReservedLayer, isFusionBackgroundImagePath](const HttpRequest &request,
                                      HttpResponse &response) {
    if (!checkPlaylistManager(response)) {
      return;
    }

    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    if (!param.isMember("name")) {
      setJsonErrorResponse(response, 400, "Missing 'name' field");
      return;
    }

    std::string name = param["name"].asString();
    // Use high-precision 时间stamp + atomic counter as ID for uniqueness
    static std::atomic<uint32_t> playlistCounter{0};
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count();
    uint32_t counter = playlistCounter.fetch_add(1);
    std::string playlistId = "playlist_" + std::to_string(timestamp) + "_" + std::to_string(counter);

    // 检查是否为临时播放列表（U盘播放列表）
    bool isTemporary = param.isMember("is_temporary") && param["is_temporary"].isBool() && param["is_temporary"].asBool();
    std::string usbMountPath = param.isMember("usb_mount_path") && param["usb_mount_path"].isString()
                                 ? param["usb_mount_path"].asString()
                                 : "";

    int targetLayerId = 1;
    if (param.isMember("target_layerId") && param["target_layerId"].isInt()) {
      targetLayerId = param["target_layerId"].asInt();
    }
    if (!((targetLayerId >= 1 && targetLayerId <= 4) || targetLayerId == 60)) {
      setJsonErrorResponse(response, 400, "Invalid target_layerId: must be 1-4 or 60");
      return;
    }
    if (forbidVodReservedLayer(targetLayerId, response)) {
      return;
    }

    bool success = false;
    std::vector<hsvj::PlaylistItem> items;
    bool inputItemsTruncated = false;
    constexpr int kMaxCreateItems = 800;
    if (isTemporary && param.isMember("items")) {
      if (!param["items"].isArray()) {
        setJsonErrorResponse(response, 400, "Invalid 'items' field: must be an array");
        return;
      }
      const Json::Value &inputItems = param["items"];
      const int itemsToRead = std::min(static_cast<int>(inputItems.size()), kMaxCreateItems);
      inputItemsTruncated = static_cast<int>(inputItems.size()) > kMaxCreateItems;
      items.reserve(static_cast<size_t>(itemsToRead));
      for (int i = 0; i < itemsToRead; ++i) {
        const Json::Value &inputItem = inputItems[i];
        if (!inputItem.isObject()) continue;
        std::string filePath;
        if (inputItem.isMember("path") && inputItem["path"].isString()) {
          filePath = inputItem["path"].asString();
        } else if (inputItem.isMember("uri") && inputItem["uri"].isString()) {
          filePath = inputItem["uri"].asString();
        }
        if (filePath.empty()) continue;

        std::string normalizedPath = hsvj::FileUtils::normalizePath(filePath);
        if (normalizedPath.find("/../") != std::string::npos ||
            (normalizedPath.length() >= 3 && normalizedPath.substr(0, 3) == "../") ||
            (normalizedPath.length() >= 3 && normalizedPath.substr(normalizedPath.length() - 3) == "/..")) {
          LOG_WARN("Skipping invalid playlist create item path: %s", filePath.c_str());
          continue;
        }
        if (isFusionBackgroundImagePath(normalizedPath)) {
          LOG_INFO("Skipping fusion background image for playlist create: %s", normalizedPath.c_str());
          continue;
        }

        hsvj::PlaylistItem item;
        item.uri = normalizedPath;
        item.title = inputItem.isMember("title") && inputItem["title"].isString()
                         ? inputItem["title"].asString()
                         : hsvj::FileUtils::getFilename(normalizedPath);
        item.duration = inputItem.isMember("duration") && inputItem["duration"].isNumeric()
                            ? inputItem["duration"].asDouble()
                            : 0.0;
        item.inPoint = inputItem.isMember("inPoint") && inputItem["inPoint"].isNumeric()
                           ? inputItem["inPoint"].asDouble()
                           : 0.0;
        item.outPoint = inputItem.isMember("outPoint") && inputItem["outPoint"].isNumeric()
                            ? inputItem["outPoint"].asDouble()
                            : -1.0;
        item.audioTrack = inputItem.isMember("audioTrack") && inputItem["audioTrack"].isInt()
                              ? inputItem["audioTrack"].asInt()
                              : 0;
        item.tags = inputItem.isMember("tags") && inputItem["tags"].isString()
                        ? inputItem["tags"].asString()
                        : "";
        items.push_back(item);
      }
    }

    if (isTemporary && !usbMountPath.empty()) {
      // 创建临时播放列表（U盘播放列表）
      LOG_INFO("Creating temporary playlist: %s (path: %s, layer=%d, items=%zu)",
               playlistId.c_str(), usbMountPath.c_str(), targetLayerId, items.size());
      success = playlistManager_->getDatabase()->createTemporaryPlaylist(
          playlistId, name, items, usbMountPath, targetLayerId);
    } else {
      // 创建普通播放列表
      success = playlistManager_->createPlayListWithName(playlistId, name, items);
    }

    if (success) {
      if (playlistManager_->setPlaylistTargetLayer(playlistId, targetLayerId)) {
        // 成功标记
      } else {
        LOG_WARN("Failed to set playlist target layer after create: playlist=%s layer=%d",
                 playlistId.c_str(), targetLayerId);
      }
      if (param.isMember("dmxId") && param["dmxId"].isInt()) {
        playlistManager_->setPlaylistDmxId(playlistId, param["dmxId"].asInt());
      }

      int createdDmxId = 0;
      for (const auto &playlist : playlistManager_->listPlaylists()) {
        if (playlist.id == playlistId) {
          createdDmxId = playlist.dmxId;
          break;
        }
      }
      
      Json::Value data;
      data["id"] = playlistId;
      data["name"] = name;
      data["dmxId"] = createdDmxId;
      data["is_temporary"] = isTemporary;
      data["targetLayerId"] = targetLayerId;
      data["item_count"] = static_cast<Json::UInt64>(items.size());
      data["items_truncated"] = inputItemsTruncated;
      if (isTemporary) {
        data["usb_mount_path"] = usbMountPath;
      }
      setJsonSuccessResponse(response, "Playlist created successfully", data);
    } else {
      setJsonErrorResponse(response, 500, "Failed to create playlist");
    }
  });

  // Add item to 播放列表
  post("/api/v1/playlists/{id}/items", [this, forbidVodReservedLayer, isFusionBackgroundImagePath](const HttpRequest &request,
                                        HttpResponse &response) {
    if (!checkPlaylistManager(response)) {
      return;
    }

    std::string playlistId = request.getUrlParam("id");
    
    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    // 说明：优先使用 JSON 请求体中的 layerId，否则使用查询参数
    int layerId = 1;
    if (param.isMember("layerId") && param["layerId"].isInt()) {
      layerId = param["layerId"].asInt();
    } else {
      if (!parseOptionalLayerId(request.getQueryParam("layerId"), layerId, 1,
                                response)) {
        return;
      }
    }

    if (!((layerId >= 1 && layerId <= 4) || layerId == 60)) {
      setJsonErrorResponse(response, 400, "Invalid layerId: must be 1-4 or 60");
      return;
    }

    bool playlistExists = false;
    for (const auto &playlist : playlistManager_->listPlaylists()) {
      if (playlist.id == playlistId) {
        playlistExists = true;
        break;
      }
    }
    if (!playlistExists) {
      setJsonErrorResponse(response, 404, "Playlist not found: " + playlistId);
      return;
    }

    if (forbidVodReservedLayer(layerId, response)) {
      return;
    }

    if (!param.isMember("path") || !param["path"].isString()) {
      setJsonErrorResponse(response, 400, "Missing 'path' field");
      return;
    }

    std::string filePath = param["path"].asString();

    // Normalize 路径
    std::string normalizedPath = hsvj::FileUtils::normalizePath(filePath);

    // 校验路径以防止目录穿越
    // 检查 for /../, leading ../, or trailing /..
    if (normalizedPath.find("/../") != std::string::npos ||
        (normalizedPath.length() >= 3 && normalizedPath.substr(0, 3) == "../") ||
        (normalizedPath.length() >= 3 && normalizedPath.substr(normalizedPath.length() - 3) == "/..")) {
      setJsonErrorResponse(response, 400,
                           "Invalid path: relative path up-level not allowed");
      return;
    }
    if (isFusionBackgroundImagePath(normalizedPath)) {
      setJsonErrorResponse(response, 400,
                           "gb_fusion is reserved for fusion background display");
      return;
    }

    hsvj::PlaylistItem item;
    item.uri = normalizedPath;
    item.title = param.isMember("title") && param["title"].isString()
                     ? param["title"].asString()
                     : "";
    item.duration = param.isMember("duration") && param["duration"].isNumeric()
                        ? param["duration"].asDouble()
                        : 0.0;
    item.inPoint = param.isMember("inPoint") && param["inPoint"].isNumeric()
                       ? param["inPoint"].asDouble()
                       : 0.0;
    item.outPoint =
        param.isMember("outPoint") && param["outPoint"].isNumeric()
            ? param["outPoint"].asDouble()
            : -1.0;
    item.tags = param.isMember("tags") && param["tags"].isString()
                    ? param["tags"].asString()
                    : "";

    int index = param.isMember("index") && param["index"].isInt()
                    ? param["index"].asInt()
                    : -1;

    if (playlistManager_->addVideoToPlayList(playlistId, item, layerId,
                                             index)) {
      setJsonSuccessResponse(response, "Item added successfully");
    } else {
      LOG_ERROR("addVideoToPlayList failed: playlist=%s, layer=%d, uri=%s",
                playlistId.c_str(), layerId, normalizedPath.c_str());
      setJsonErrorResponse(response, 500, 
          "Failed to add item to playlist (check server logs for SQLite error)");
    }
  });

  // Delete item from 播放列表
  del("/api/v1/playlists/{id}/items/{index}", [this](const HttpRequest &request,
                                                HttpResponse &response) {
    if (!checkPlaylistManager(response)) {
      return;
    }

    std::string playlistId = request.getUrlParam("id");
    std::string indexStr = request.getUrlParam("index");
    // 默认值 to layer 1, can be specified via query param
    int layerId;
    if (!parseOptionalLayerId(request.getQueryParam("layerId"), layerId, 1,
                              response)) {
      return;
    }

    int index;
    try {
      index = std::stoi(indexStr);
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 400, "Invalid index parameter");
      return;
    }
    if (index < 0) {
      setJsonErrorResponse(response, 400, "Invalid index parameter");
      return;
    }

    if (playlistManager_->removeVideoFromPlayList(playlistId, layerId, index)) {
      setJsonSuccessResponse(response, "Item deleted successfully");
    } else {
      setJsonErrorResponse(response, 500, "Failed to delete item");
    }
  });

  // Delete 播放列表
  del("/api/v1/playlists/{id}",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkPlaylistManager(response)) {
          return;
        }

        std::string playlistId = request.getUrlParam("id");

        if (playlistManager_->deletePlayList(playlistId)) {
          setJsonSuccessResponse(response, "Playlist deleted successfully");
        } else {
          setJsonErrorResponse(response, 500, "Failed to delete playlist");
        }
      });

  // Get 播放列表 config
  get("/api/v1/playlists/{id}/config",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkPlaylistManager(response)) {
          return;
        }

        std::string playlistId = request.getUrlParam("id");
        hsvj::PlaylistConfig config = playlistManager_->getPlayMode(playlistId);

        Json::Value configJson;
        configJson["mode"] = config.mode;
        configJson["shuffle"] = config.shuffle;
        configJson["loop"] = config.loop;
        configJson["preload_ahead"] = config.preloadAhead;
        configJson["crossfade"] = config.crossfade;
        configJson["displayDuration"] = config.displayDuration;
        configJson["fadeInTime"] = config.fadeInTime;
        configJson["fadeOutTime"] = config.fadeOutTime;

        setJsonDataResponse(response, configJson, "播放列表配置加载成功");
      });

  // Set 播放列表 config
  put("/api/v1/playlists/{id}/config",
       [this, forbidVodReservedLayer](const HttpRequest &request, HttpResponse &response) {
         if (!checkPlaylistManager(response)) {
           return;
         }

         std::string playlistId = request.getUrlParam("id");
         Json::Value param;
         if (!parseJsonBody(request, param, response)) {
           return;
         }

         hsvj::PlaylistConfig config = playlistManager_->getPlayMode(playlistId);
         if (param.isMember("mode") && param["mode"].isString()) {
           config.mode = param["mode"].asString();
         }
         if (param.isMember("shuffle") && param["shuffle"].isBool()) {
           config.shuffle = param["shuffle"].asBool();
         }
        if (param.isMember("loop") && param["loop"].isInt()) {
          config.loop = param["loop"].asInt();
          if (config.loop < 0 || config.loop > 4) {
            setJsonErrorResponse(response, 400, "Invalid loop: must be 0-4");
            return;
          }
        }
         if (param.isMember("preload_ahead") && param["preload_ahead"].isInt()) {
           config.preloadAhead = param["preload_ahead"].asInt();
         }
          if (param.isMember("crossfade") && param["crossfade"].isNumeric()) {
            config.crossfade = param["crossfade"].asDouble();
          }
         if (param.isMember("displayDuration") && param["displayDuration"].isNumeric()) {
           config.displayDuration = param["displayDuration"].asDouble();
         }
         if (param.isMember("fadeInTime") && param["fadeInTime"].isNumeric()) {
           config.fadeInTime = param["fadeInTime"].asDouble();
         }
         if (param.isMember("fadeOutTime") && param["fadeOutTime"].isNumeric()) {
           config.fadeOutTime = param["fadeOutTime"].asDouble();
         }

          bool success = playlistManager_->setPlayMode(playlistId, config);
          
          // 更新 target layer ID
          if (param.isMember("target_layerId") && param["target_layerId"].isInt()) {
            if (forbidVodReservedLayer(param["target_layerId"].asInt(), response)) {
              return;
            }
            if (!playlistManager_->setPlaylistTargetLayer(playlistId, param["target_layerId"].asInt())) {
              success = false;
            }
          }
          if (param.isMember("dmxId") && param["dmxId"].isInt()) {
            if (!playlistManager_->setPlaylistDmxId(playlistId, param["dmxId"].asInt())) {
              success = false;
            }
          }

          if (success) {
            setJsonSuccessResponse(response, "Playlist config updated successfully");
          } else {
            setJsonErrorResponse(response, 500, "Failed to update playlist config");
          }
        });

}
