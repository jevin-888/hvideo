/**
 * @file HttpServer_Material.cpp（文件名）
 * @brief 素材管理 API 实现
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/PathConfig.h"
#include "database/MaterialIndex.h"
#include "database/PlaylistDatabase.h"
#include "database/PlaylistManager.h"
#include "utils/FileUtils.h"
#include "utils/MediaUtils.h"
#include "utils/VideoTranscoder.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <thread>

namespace fs = std::filesystem;

// 1x1 透明 GIF，用于缩略图未生成时返回 200 占位，避免浏览器控制台刷 404
static const unsigned char kThumbnailPlaceholderGif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b
};
static const size_t kThumbnailPlaceholderGifSize = sizeof(kThumbnailPlaceholderGif);

// 素材基础目录统一使用 PathConfig 的 ROOT_PATH（/huoshan/ 或 /sdcard/huoshan/ 等）
static std::string getMaterialBaseDir() {
  std::string root = hsvj::ROOT_PATH;
  if (!root.empty() && root.back() == '/') root.pop_back();
  return root.empty() ? "/huoshan" : root;
}

void HttpServer::registerMaterialRoutes() {
  // 获取素材列表 API
  get("/api/v1/materials", [this](const HttpRequest &request,
                               HttpResponse &response) {
    std::string type = request.getQueryParam("type"); // 视频, image, 音频, font
    std::string folder = request.getQueryParam("folder");
    if (type.empty()) type = "video";
    if (!folder.empty() &&
        (folder.find("..") != std::string::npos ||
         folder.find('/') != std::string::npos ||
         folder.find('\\') != std::string::npos ||
         folder.find('\0') != std::string::npos)) {
      setJsonErrorResponse(response, 400, "Invalid folder");
      return;
    }

    if (materialIndex_) {
      setJsonDataResponse(response, materialIndex_->listMaterials(type, folder, 500),
                          "素材列表加载成功");
      return;
    }

    setJsonErrorResponse(response, 503, "Material index not initialized");
  });

  // 获取素材文件夹列API
  get("/api/v1/materials/folders", [this](const HttpRequest &request,
                                       HttpResponse &response) {
    std::string type = request.getQueryParam("type");
    if (type.empty()) type = "video";
    if (materialIndex_) {
      setJsonDataResponse(response, materialIndex_->listFolders(type),
                          "素材目录加载成功");
      return;
    }
    
    setJsonErrorResponse(response, 503, "Material index not initialized");
  });

  post("/api/v1/materials/refresh_index", [this](const HttpRequest &request, HttpResponse &response) {
    (void)request;
    if (!materialIndex_) {
      setJsonErrorResponse(response, 503, "Material index not initialized");
      return;
    }
    materialIndex_->requestFullScan();
    Json::Value data;
    data["scanning"] = materialIndex_->isScanning();
    setJsonSuccessResponse(response, "Material index refresh started", data);
  });

  get("/api/v1/materials/index_status", [this](const HttpRequest &request, HttpResponse &response) {
    (void)request;
    if (!materialIndex_) {
      setJsonErrorResponse(response, 503, "Material index not initialized");
      return;
    }
    setJsonDataResponse(response, materialIndex_->getStatus(), "素材索引状态加载成功");
  });

  // 文件删除 API
  post("/api/v1/materials/delete", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;

    std::string filePath = param["path"].asString();
    if (!validateMaterialPath(filePath, response)) return;

    try {
      if (fs::exists(filePath)) {
        fs::remove(filePath);
        if (materialIndex_) materialIndex_->removeFile(filePath);
        // 同步清除所有播放列表中指向该文件的条目
        if (playlistManager_) {
          auto* db = playlistManager_->getDatabase();
          if (db) {
            int removed = db->removeItemsByUri(filePath);
            if (removed > 0) {
              LOG_INFO("[Delete] Removed %d playlist item(s) for: %s", removed, filePath.c_str());
            }
          }
        }
        setJsonSuccessResponse(response, "File deleted successfully");
      } else {
        setJsonErrorResponse(response, 404, "File not found");
      }
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, std::string("Delete failed: ") + e.what());
    }
  });

  // 重命API
  post("/api/v1/materials/rename", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;

    std::string oldPath = param["path"].asString();
    std::string newName = param["newName"].asString();

    // 验证原路径是否在允许的目录内
    if (!validateMaterialPath(oldPath, response)) {
      return;
    }
    
    // 验证新文件名不包含非法字
    if (newName.empty() || newName.find("..") != std::string::npos ||
        newName.find('/') != std::string::npos || newName.find('\\') != std::string::npos ||
        newName.find('\0') != std::string::npos) {
      setJsonErrorResponse(response, 400, "Invalid new name");
      return;
    }

    try {
      fs::path oldP(oldPath);
      fs::path newP = oldP.parent_path() / newName;
      
      // 验证新路径也在允许的目录内（防止通过特殊字符逃逸）
      if (!isPathAllowed(newP.string())) {
        setJsonErrorResponse(response, 403, "Target path not allowed");
        return;
      }
      
      if (fs::exists(newP)) {
        setJsonErrorResponse(response, 400, "Target already exists");
        return;
      }
      fs::rename(oldPath, newP);
      if (materialIndex_) materialIndex_->renameFile(oldPath, newP.string());
      setJsonSuccessResponse(response, "Renamed successfully");
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, std::string("Rename failed: ") + e.what());
    }
  });

  // 文件夹删API
  post("/api/v1/materials/delete_folder", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;

    std::string folderPath = param["folderPath"].asString();
    
    // 使用统一的路径验证函数，支持所有合法的素材路径
    if (!validateMaterialPath(folderPath, response)) {
      return;
    }
    
    // 额外验证：确保是目录而不是文
    if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
      setJsonErrorResponse(response, 404, "Folder not found");
      return;
    }

    try {
      // 删除文件夹前，先清理播放列表中所有指向该文件夹内文件的条目
      if (playlistManager_) {
        auto* db = playlistManager_->getDatabase();
        if (db) {
          int totalRemoved = 0;
          for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (entry.is_regular_file()) {
              int removed = db->removeItemsByUri(entry.path().string());
              totalRemoved += removed;
            }
          }
          if (totalRemoved > 0) {
            LOG_INFO("[DeleteFolder] Removed %d playlist item(s) for folder: %s", totalRemoved, folderPath.c_str());
          }
        }
      }
      fs::remove_all(folderPath);
      if (materialIndex_) materialIndex_->requestFullScan();
      setJsonSuccessResponse(response, "Folder deleted");
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, e.what());
    }
  });

  // 素材下载API - 浏览器下载文件
  get("/api/v1/materials/download_file", [this](const HttpRequest &request, HttpResponse &response) {
    (void)request;
    // 此接口现在由 HttpServer_Connection.cpp 中的 Impl::handleMaterialVideoStream 接管
    // 以实现流式下载，防止大文件导致内存暴涨。
    // 如果执行到这里，说明拦截失败，作为兜底返回 404
    setJsonErrorResponse(response, 404, "Handler intercepted by streaming engine");
  });

  // 缩略图获API - 返回缩略图图
  get("/api/v1/materials/thumbnail", [this](const HttpRequest &request, HttpResponse &response) {
    std::string path = request.getQueryParam("path");
    if (path.empty()) {
      setJsonErrorResponse(response, 400, "Missing path");
      return;
    }

    // 计算缩略图缓存路
    std::string cacheDir = hsvj::MediaUtils::getThumbnailCacheDir(path);
    size_t pathHash = std::hash<std::string>{}(path);
    std::string cacheFilePath = cacheDir + std::to_string(pathHash) + ".jpg";

    // 检查缩略图是否存在
    if (!fs::exists(cacheFilePath)) {
      // 如果是图片类型，直接返回原始图片作为缩略图（浏览器会自动缩放
      std::string ext = fs::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") {
        if (fs::exists(path) && fs::is_regular_file(path)) {
          try {
            std::ifstream file(path, std::ios::binary);
            if (file.is_open()) {
              std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
              file.close();
              
              // 设置正确Content-Type
              std::string contentType = "image/jpeg";
              if (ext == ".png") contentType = "image/png";
              else if (ext == ".bmp") contentType = "image/bmp";
              
              response.setContentType(contentType);
              response.setBody(std::string(buffer.begin(), buffer.end()));
              return;
            }
          } catch (...) {
            // 读取失败，继续返404
          }
        }
      }
      
      // 缩略图不存在
      // 检查是否是视频文件，如果是则触发异步生成
      if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" ||
          ext == ".mov" || ext == ".mpg" || ext == ".mpeg") {
        if (fs::exists(path) && fs::is_regular_file(path)) {
          hsvj::MediaUtils::generateThumbnailAsync(path, [this](const std::string& event, const std::string& data) {
            this->broadcastSSE(event, data);
          });
        }
      }
      // 返回 200 + 占位图，避免浏览器控制台刷 404；前端通过 SSE thumbnail_ready 刷新为真实缩略图
      response.setStatusCode(200);
      response.setContentType("image/gif");
      response.setBody(std::string(reinterpret_cast<const char*>(kThumbnailPlaceholderGif), kThumbnailPlaceholderGifSize));
      return;
    }

    // 读取缩略图文件并返回
    try {
      std::ifstream file(cacheFilePath, std::ios::binary);
      if (!file.is_open()) {
        setJsonErrorResponse(response, 500, "Failed to open thumbnail file");
        return;
      }
      
      std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      file.close();
      
      response.setContentType("image/jpeg");
      response.setBody(std::string(buffer.begin(), buffer.end()));
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, std::string("Failed to read thumbnail: ") + e.what());
    }
  });

  // 缩略图状态查API
  get("/api/v1/materials/thumbnail/status", [this](const HttpRequest &request, HttpResponse &response) {
    std::string path = request.getQueryParam("path");
    if (path.empty()) {
      setJsonErrorResponse(response, 400, "Missing path");
      return;
    }

    std::string cacheDir = hsvj::MediaUtils::getThumbnailCacheDir(path);
    size_t pathHash = std::hash<std::string>{}(path);
    std::string cacheFilePath = cacheDir + std::to_string(pathHash) + ".jpg";

    Json::Value data;
    data["ready"] = fs::exists(cacheFilePath);
    data["path"] = path;
    if (data["ready"].asBool()) {
        data["thumbnail_path"] = cacheFilePath;
    }

    setJsonSuccessResponse(response, "Thumbnail status checked", data);
  });

  // 转码状态快照API：前端轮询兜底，避免 SSE 不可用时没有进度提示
  get("/api/v1/video/transcode_status", [this](const HttpRequest &request, HttpResponse &response) {
    (void)request;
    setJsonSuccessResponse(response, "Transcode status", getTranscodeStatusSnapshot());
  });

  // 素材下载API - 复制文件到指定目
  post("/api/v1/materials/download", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    if (!param.isMember("path")) {
      setJsonErrorResponse(response, 400, "Missing path");
      return;
    }
    std::string sourcePath = param["path"].asString();
    std::string targetDir = param.isMember("targetDir") ? param["targetDir"].asString() : (hsvj::ROOT_PATH + "download");
    
    if (sourcePath.find("..") != std::string::npos) {
      setJsonErrorResponse(response, 400, "Invalid path");
      return;
    }

    try {
      if (!fs::exists(sourcePath)) {
        setJsonErrorResponse(response, 404, "Not found");
        return;
      }
      if (!fs::exists(targetDir)) fs::create_directories(targetDir);
      fs::path src(sourcePath);
      fs::path dst = fs::path(targetDir) / src.filename();
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
      setJsonSuccessResponse(response, "File copied successfully");
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, e.what());
    }
  });

  // 视频重新编码 API（使用 Rockchip MPP H.264 编码）
  post("/api/v1/video/transcode", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    for (const auto& key : param.getMemberNames()) {
      if (key != "path" && key != "force") {
        setJsonErrorResponse(response, 400, "Unsupported parameter: " + key);
        return;
      }
    }
    if (!param.isMember("path") || !param["path"].isString()) {
      setJsonErrorResponse(response, 400, "Missing or invalid path");
      return;
    }
    std::string inputPath = param["path"].asString();
    if (inputPath.empty()) {
      setJsonErrorResponse(response, 400, "Missing path");
      return;
    }
    if (!validateMaterialPath(inputPath, response)) return;
    if (!fs::exists(inputPath) || !fs::is_regular_file(inputPath)) {
      setJsonErrorResponse(response, 404, "File not found");
      return;
    }
    bool force = param.isMember("force") && param["force"].isBool() && param["force"].asBool();
    hsvj::VideoTranscoder::TranscodeOptions opts;
    opts.copyAudio = true;  // 保留原音轨（stream copy）
    // 预先算好输出路径，立即返回给前端
    if (opts.outputPath.empty()) {
      opts.outputPath = hsvj::VideoTranscoder::getTranscodeOutputPath(inputPath);
    }

    if (!force && hsvj::VideoTranscoder::isOptimizedForOptions(inputPath, opts)) {
      Json::Value data;
      data["path"] = inputPath;
      data["skipped"] = true;
      data["reason"] = "already_optimized";
      setJsonSuccessResponse(response, "Video already optimized, skipped", data);
      return;
    }

    // 检查是否正在转码
    if (!beginTranscode(inputPath)) {
      setJsonErrorResponse(response, 403, "File is already being transcoded");
      return;
    }

    // 异步执行，避免阻塞 HTTP 线程（大视频转码可能需要数分钟）
    std::thread([this, inputPath, opts]() {
      hsvj::VideoTranscoder::TranscodeOptions mOpts = opts;
      std::string errMsg;
      
      // 传递进度回调，通过 SSE 推送给前端
      bool ok = hsvj::VideoTranscoder::transcode(inputPath, mOpts, 
        [this, inputPath](float progress, const std::string& status, const std::string& encoder) {
          updateTranscodeProgress(inputPath, progress, status, encoder);
        }, 
        &errMsg);
      
      // 结束转码标记并广播
      endTranscode(inputPath, ok, errMsg);
      
      if (ok) {
        LOG_INFO("[Transcode] Done: %s -> %s", inputPath.c_str(), mOpts.outputPath.c_str());
      } else {
        LOG_ERROR("[Transcode] Failed: %s - %s", inputPath.c_str(), errMsg.c_str());
      }
    }).detach();

    // 不返回缓存路径——编码完成后会直接替换原文件
    // 返回缓存路径会导致客户端在编码未完成时尝试预览不完整文件
    setJsonSuccessResponse(response, "Transcode started, original file will be replaced when done");
    response.setStatusCode(202);
  });

  // 批量转码API（简化版，转码所有视频）
  post("/api/v1/video/transcode_batch", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    for (const auto& key : param.getMemberNames()) {
      if (key != "force") {
        setJsonErrorResponse(response, 400, "Unsupported parameter: " + key);
        return;
      }
    }

    std::string videoDir = "/huoshan/video";
    bool force = param.isMember("force") && param["force"].isBool() && param["force"].asBool();

    std::vector<std::string> videoFiles;
    try {
      for (const auto& entry : fs::directory_iterator(videoDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
          videoFiles.push_back(entry.path().string());
        }
      }
    } catch (const std::exception& e) {
      setJsonErrorResponse(response, 500, std::string("Scan failed: ") + e.what());
      return;
    }

    if (videoFiles.empty()) {
      setJsonErrorResponse(response, 404, "No video files found");
      return;
    }

    std::thread([this, videoFiles, force]() {
      int total = videoFiles.size();
      int completed = 0;
      int failed = 0;
      int skipped = 0;

      LOG_INFO("[BatchTranscode] Start: %d files", total);

      for (const auto& path : videoFiles) {
        Json::Value data;
        data["path"] = path;
        data["status"] = "batch_start";
        data["current"] = completed + 1;
        data["total"] = total;
        updateBatchTranscodeState(data);

        hsvj::VideoTranscoder::TranscodeOptions opts;
        opts.copyAudio = true;
        opts.outputPath = hsvj::VideoTranscoder::getTranscodeOutputPath(path);

        if (!force && hsvj::VideoTranscoder::isOptimizedForOptions(path, opts)) {
          Json::Value d;
          d["path"] = path;
          d["status"] = "batch_progress";
          d["progress"] = 100.0;
          d["message"] = "已优化，跳过";
          d["current"] = completed + 1;
          d["total"] = total;
          updateBatchTranscodeState(d);
          completed++;
          skipped++;
          continue;
        }

        if (!beginTranscode(path)) {
          Json::Value d;
          d["path"] = path;
          d["status"] = "batch_progress";
          d["progress"] = 100.0;
          d["message"] = "正在转码，跳过重复任务";
          d["current"] = completed + 1;
          d["total"] = total;
          updateBatchTranscodeState(d);
          completed++;
          skipped++;
          continue;
        }

        std::string err;
        bool ok = hsvj::VideoTranscoder::transcode(path, opts,
          [this, path, completed, total](float p, const std::string& s, const std::string& e) {
            Json::Value d;
            d["path"] = path;
            d["status"] = "batch_progress";
            d["progress"] = p;
            d["message"] = s;
            d["encoder"] = e;
            d["current"] = completed + 1;
            d["total"] = total;
            updateBatchTranscodeState(d);
          }, &err);

        endTranscode(path, ok, err);
        if (ok) completed++; else { completed++; failed++; }
      }

      Json::Value done;
      done["status"] = "batch_complete";
      done["total"] = total;
      done["completed"] = completed - failed - skipped;
      done["skipped"] = skipped;
      done["failed"] = failed;
      updateBatchTranscodeState(done);

      LOG_INFO("[BatchTranscode] Done: success=%d skipped=%d failed=%d total=%d",
               completed - failed - skipped, skipped, failed, total);
    }).detach();

    Json::Value result;
    result["message"] = "Batch transcode started";
    result["total_files"] = (int)videoFiles.size();
    std::string resultStr = jsonToString(result);
    setJsonSuccessResponse(response, resultStr);
    response.setStatusCode(202);
  });

  // 注意: /api/v1/materials/preview_stream 的处理由 HttpServer::handleMaterialPreviewStream 负责
  // 详见 HttpServer.cpp 中的实现
}
