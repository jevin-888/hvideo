/**
 * @file HttpServer_Effect.cpp（文件名）
 * @brief 通用特效命令转发 API
 *
 * 注：原本承载消原唱（VocalRemover RKNN）所有 HTTP 端点，2026-05 重构时
 *     一并删除。本文件现在只保留 /api/effect/apply（特效命令路由）以及
 *     license 校验骨架，作为后续 DJ 音频反应系统挂接点。
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/Engine.h"
#include "core/LicenseManager.h"
#include "utils/Logger.h"
#include <json/json.h>

void HttpServer::initializeEffectServices() {
  // 之前在此处会做 RK3588 RKNN 消原唱模型预热；RKNN 子系统已整体下线，
  // 当前阶段无需任何启动期资源准备。保留函数符号以维持调用链兼容。
  bool effectsLicensed = false;
  if (engine_) {
    const hsvj::LicenseManager *lm = engine_->getLicenseManager();
    effectsLicensed = lm && lm->isLicensed() && lm->isModuleEnabled("effects");
    LOG_INFO("[Effect] license check: licensed=%d", effectsLicensed ? 1 : 0);
  }
}

void HttpServer::registerEffectRoutes() {
  auto rejectIfEffectsUnlicensed = [this](HttpResponse &response) -> bool {
    bool licensed = false;
    if (engine_) {
      const hsvj::LicenseManager *lm = engine_->getLicenseManager();
      licensed = lm && lm->isLicensed() && lm->isModuleEnabled("effects");
    }
    if (!licensed) {
      setJsonErrorResponse(response, 403, "effects module not licensed");
      return true;
    }
    return false;
  };

  // 通用特效命令转发：type=0, code=11
  // 由 CommandRouter 解析具体特效（不涉及音频检测）。
  post("/api/v1/effect/apply",
       [this, rejectIfEffectsUnlicensed](const HttpRequest &request,
                                          HttpResponse &response) {
         if (rejectIfEffectsUnlicensed(response))
           return;
         if (!checkCommandRouter(response))
           return;

         Json::Value param;
         if (!parseJsonBody(request, param, response))
           return;

         std::string cmd = buildCommandJson(0, 11, param);
         executeCommandAndRespond(cmd, response);
       });
}
