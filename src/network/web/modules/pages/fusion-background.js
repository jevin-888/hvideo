/**
 * 几何背景图片控制模块
 * 控制几何调试背景图片显示/隐藏。
 */

(function () {
    'use strict';

    var BACKGROUND_LAYER_ID = 9060;
    var BACKGROUND_LAYER_PRIORITY = 0;
    var DEFAULT_CANVAS_WIDTH = 1920;
    var DEFAULT_CANVAS_HEIGHT = 1080;
    var VIDEO_LAYER_IDS = [1, 2, 3, 4];
    var backgroundState = {
        show: false,
        currentIndex: 0,
        items: [],
        loading: false,
        rectSyncSeq: 0,
        operationSeq: 0,
        indexRefreshTried: false,
        layerAvailable: null,
        performancePauseEnabled: false,
        pausedVideoLayerIds: [],
        debugModeActive: false,
        autoDebugVisible: false,
        userVisible: false,
        debugOperation: Promise.resolve()
    };

    function postModuleAction(moduleName, action, payload, keepalive) {
        var allowedModules = ['layers', 'playback', 'rendering'];
        if (allowedModules.indexOf(moduleName) === -1 ||
            !/^[A-Za-z0-9_-]+$/.test(String(action || ''))) {
            return Promise.reject(new Error('Invalid background module action'));
        }
        return fetch('/api/v1/' + moduleName + '/actions/' + encodeURIComponent(action), {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload),
            keepalive: !!keepalive
        }).then(readApiData);
    }

    function hasExactKeys(value, expectedKeys) {
        var keys = Object.keys(value);
        return keys.length === expectedKeys.length && expectedKeys.every(function (key) {
            return keys.indexOf(key) !== -1;
        });
    }

    function parseApiEnvelope(value) {
        var validRoot = value && typeof value === 'object' && !Array.isArray(value) &&
            hasExactKeys(value, ['ok', 'data', 'error']) && typeof value.ok === 'boolean';
        if (!validRoot) throw new Error('后端返回不符合统一 API contract');

        if (value.ok) {
            if (value.error !== null) throw new Error('后端返回不符合统一 API contract');
            return value;
        }

        var validError = value.data === null && value.error &&
            typeof value.error === 'object' && !Array.isArray(value.error) &&
            hasExactKeys(value.error, ['code', 'message']) &&
            typeof value.error.code === 'string' && value.error.code.length > 0 &&
            typeof value.error.message === 'string';
        if (!validError) throw new Error('后端返回不符合统一 API contract');
        return value;
    }

    function readApiData(response) {
        return response.text().then(function (text) {
            if (!text) throw new Error('后端返回空响应，不符合统一 API contract');
            var json;
            try {
                json = JSON.parse(text);
            } catch (error) {
                throw new Error('后端返回非 JSON 响应，不符合统一 API contract');
            }
            var envelope = parseApiEnvelope(json);
            if (!response.ok || !envelope.ok) {
                var apiError = new Error(envelope.error ? envelope.error.message : ('HTTP ' + response.status));
                apiError.code = envelope.error ? envelope.error.code : 'HTTP_ERROR';
                throw apiError;
            }
            return envelope.data;
        });
    }

    function positiveNumber(value) {
        var number = Number(value);
        return Number.isFinite(number) && number > 0 ? number : 0;
    }

    function parseResolutionSize(value) {
        var match = String(value || '').match(/(\d+)\D+(\d+)/);
        if (!match) return null;
        var width = positiveNumber(match[1]);
        var height = positiveNumber(match[2]);
        return width > 0 && height > 0 ? { width: width, height: height } : null;
    }

    function initBackgroundUI() {
        var checkInterval = setInterval(function () {
            var geometryTab = document.getElementById('fusion-tab-geometry-fusion');
            if (geometryTab) {
                clearInterval(checkInterval);
                addBackgroundControls(geometryTab);
                loadBackgroundStatus();
            }
        }, 500);

        setTimeout(function () {
            clearInterval(checkInterval);
        }, 10000);
    }

    function addBackgroundControls(container) {
        if (document.getElementById('bg-control-panel')) return;

        var panel = document.createElement('div');
        panel.id = 'bg-control-panel';
        panel.className = 'control-group';
        panel.innerHTML = `
            <div class="fusion-bg-compact-row">
                <span class="fusion-bg-title">1. 背景</span>
                <div class="fusion-bg-picker">
                    <button id="bg-prev-btn" class="btn small fusion-bg-nav">&lt;</button>
                    <span id="bg-info" class="fusion-bg-info">加载中...</span>
                    <button id="bg-next-btn" class="btn small fusion-bg-nav">&gt;</button>
                </div>
                <div class="fusion-bg-toggle">
                    <label class="preview-switch margin-none">
                        <input type="checkbox" id="bg-show-toggle">
                        <span class="switch-slider"></span>
                    </label>
                </div>
                <div class="fusion-bg-perf-toggle" title="调试时暂停正在播放的视频层">
                    <span class="fusion-bg-perf-label">省性能</span>
                    <label class="preview-switch margin-none">
                        <input type="checkbox" id="bg-pause-video-toggle">
                        <span class="switch-slider"></span>
                    </label>
                </div>
            </div>
        `;

        if (container.firstChild) {
            container.insertBefore(panel, container.firstChild);
        } else {
            container.appendChild(panel);
        }

        var toggle = document.getElementById('bg-show-toggle');
        if (toggle) toggle.addEventListener('change', handleShowToggle);
        var prevButton = document.getElementById('bg-prev-btn');
        var nextButton = document.getElementById('bg-next-btn');
        if (prevButton) prevButton.addEventListener('click', handlePrevClick);
        if (nextButton) nextButton.addEventListener('click', handleNextClick);
        var pauseToggle = document.getElementById('bg-pause-video-toggle');
        if (pauseToggle) {
            pauseToggle.checked = backgroundState.performancePauseEnabled;
            pauseToggle.addEventListener('change', handlePerformancePauseToggle);
        }
    }

    function delay(ms) {
        return new Promise(function (resolve) {
            setTimeout(resolve, ms);
        });
    }

    function traceNowMs() {
        return (window.performance && typeof window.performance.now === 'function')
            ? window.performance.now()
            : Date.now();
    }

    function nextOperationSeq() {
        backgroundState.operationSeq += 1;
        return backgroundState.operationSeq;
    }

    function isCurrentOperation(operationSeq) {
        return !operationSeq || operationSeq === backgroundState.operationSeq;
    }

    function logTrace(traceId, stage, details) {
        if (!traceId) return;
        console.info('[FusionICloseTrace] trace=' + traceId + ' stage=' + stage, details || {});
    }

    function warnTrace(traceId, stage, details) {
        if (!traceId) return;
        console.warn('[FusionICloseTrace] trace=' + traceId + ' stage=' + stage, details || {});
    }

    function isGbFusionMaterial(item) {
        var text = String((item && item.path) || (item && item.name) || '').replace(/\\/g, '/').toLowerCase();
        return text.indexOf('/gb_fusion/') !== -1 || text.indexOf('gb_fusion/') === 0;
    }

    function normalizeMaterialList(materials) {
        return Array.isArray(materials) ? materials : [];
    }

    function listGbFusionMaterials() {
        return requestJson('/api/v1/materials?type=image&folder=gb_fusion')
            .then(function (materials) {
                materials = normalizeMaterialList(materials);
                if (materials.length > 0) return materials;
                return requestJson('/api/v1/materials?type=image')
                    .then(function (allMaterials) {
                        return normalizeMaterialList(allMaterials).filter(isGbFusionMaterial);
                    })
                    .catch(function () { return []; });
            });
    }

    function waitForMaterialIndexReady() {
        var attempts = 0;
        var poll = function () {
            attempts += 1;
            return requestJson('/api/v1/materials/index_status')
                .then(function (status) {
                    if (!status || status.scanning === false || attempts >= 20) return status;
                    return delay(250).then(poll);
                })
                .catch(function () { return delay(500); });
        };
        return poll();
    }

    function requestJson(url, options) {
        return fetch(url, options).then(readApiData);
    }

    function createBackgroundRuntimeLayer() {
        return postModuleAction('layers', 'create_runtime_layer', {
            layerId: BACKGROUND_LAYER_ID,
            layer_type: 'image',
            visible: false,
            priority: BACKGROUND_LAYER_PRIORITY
        }, false);
    }

    function removeBackgroundRuntimeLayer(keepalive) {
        var promise = postModuleAction('layers', 'remove_runtime_layer', {
            layerId: BACKGROUND_LAYER_ID
        }, keepalive);
        return promise
            .then(function (response) {
                backgroundState.layerAvailable = false;
                return response;
            })
            .catch(function (error) {
                console.warn('[FusionBackground] Failed to remove runtime background layer:', error);
                return false;
            });
    }

    function ensureBackgroundRuntimeLayer() {
        return requestJson('/api/v1/runtime/layers/' + BACKGROUND_LAYER_ID)
            .then(function (data) {
                if (data && Number(data.id) === BACKGROUND_LAYER_ID) return data;
                return createBackgroundRuntimeLayer();
            })
            .catch(function () {
                return createBackgroundRuntimeLayer();
            })
            .then(function () {
                backgroundState.layerAvailable = true;
                return true;
            });
    }

    function getBackgroundCanvasSize() {
        return requestJson('/api/v1/system/status')
            .then(function (data) {
                data = data || {};
                var width = positiveNumber(data.canvas_in_width) ||
                    positiveNumber(data.canvas_width) ||
                    positiveNumber(data.width);
                var height = positiveNumber(data.canvas_in_height) ||
                    positiveNumber(data.canvas_height) ||
                    positiveNumber(data.height);

                if ((!width || !height) && data.resolution) {
                    var resolutionSize = parseResolutionSize(data.resolution);
                    if (resolutionSize) {
                        width = resolutionSize.width;
                        height = resolutionSize.height;
                    }
                }

                return {
                    width: width || DEFAULT_CANVAS_WIDTH,
                    height: height || DEFAULT_CANVAS_HEIGHT
                };
            })
            .catch(function (error) {
                console.warn('[FusionBackground] Failed to read canvas size:', error);
                return { width: DEFAULT_CANVAS_WIDTH, height: DEFAULT_CANVAS_HEIGHT };
            });
    }

    function syncBackgroundLayerRect(visible, traceId, reason, operationSeq) {
        var startedAt = traceNowMs();
        var rectSyncSeq = ++backgroundState.rectSyncSeq;
        logTrace(traceId, 'background.rect.begin', {
            visible: !!visible,
            reason: reason || '',
            seq: rectSyncSeq,
            operationSeq: operationSeq
        });
        return getBackgroundCanvasSize()
            .then(function (size) {
                if (!isCurrentOperation(operationSeq)) {
                    logTrace(traceId, 'background.rect.skippedStale', {
                        visible: !!visible,
                        seq: rectSyncSeq,
                        operationSeq: operationSeq,
                        currentSeq: backgroundState.operationSeq
                    });
                    return false;
                }
                logTrace(traceId, 'background.rect.size', {
                    visible: !!visible,
                    seq: rectSyncSeq,
                    width: Math.round(size.width),
                    height: Math.round(size.height)
                });
                return requestJson('/api/v1/runtime/layers/' + BACKGROUND_LAYER_ID, {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        _rectSyncSeq: rectSyncSeq,
                        visible: !!visible,
                        priority: BACKGROUND_LAYER_PRIORITY,
                        position: { x: 0, y: 0 },
                        size: {
                            width: Math.round(size.width),
                            height: Math.round(size.height)
                        },
                        rotation: 0,
                        scale: 1
                    })
                });
            })
            .then(function (response) {
                logTrace(traceId, 'background.rect.end', {
                    visible: !!visible,
                    seq: rectSyncSeq,
                    cost_ms: Math.round(traceNowMs() - startedAt),
                    completed: true
                });
                return response;
            })
            .catch(function (error) {
                warnTrace(traceId, 'background.rect.error', {
                    visible: !!visible,
                    seq: rectSyncSeq,
                    cost_ms: Math.round(traceNowMs() - startedAt),
                    message: error && error.message ? error.message : String(error)
                });
                throw error;
            });
    }

    function loadBackgroundItems() {
        return listGbFusionMaterials()
            .then(function (materials) {
                if (Array.isArray(materials) && materials.length > 0) return materials;
                if (backgroundState.indexRefreshTried || !window.apiPost) return [];
                backgroundState.indexRefreshTried = true;
                return window.apiPost('/materials/refresh_index', {})
                    .then(function () {
                        return waitForMaterialIndexReady();
                    })
                    .then(function () {
                        return listGbFusionMaterials();
                    })
                    .catch(function () { return []; });
            })
            .then(function (materials) {
                if (!Array.isArray(materials)) return [];
                return materials.sort(function (a, b) {
                    return String(a.name || a.path || '').localeCompare(String(b.name || b.path || ''), undefined, { numeric: true });
                });
            });
    }

    function ensureBackgroundItemsLoaded() {
        if (backgroundState.items.length > 0) {
            return Promise.resolve(backgroundState.items);
        }
        return loadBackgroundItems().then(function (items) {
            updateBackgroundState({
                items: items,
                currentIndex: Math.min(backgroundState.currentIndex, Math.max(0, items.length - 1))
            });
            return backgroundState.items;
        });
    }

    function imageFileFromPath(filePath) {
        var imageFile = String(filePath || '').split(/[\\/]/).pop();
        var normalized = String(filePath || '').replace(/\\/g, '/');
        var lower = normalized.toLowerCase();
        var imageDir = '/image/';
        var imageDirIdx = lower.indexOf(imageDir);
        if (imageDirIdx !== -1) {
            imageFile = normalized.substring(imageDirIdx + imageDir.length);
        }
        return imageFile;
    }

    function loadImageToLayer(item) {
        if (!item || !item.path) return Promise.resolve(false);
        return postModuleAction('rendering', 'loadImage', {
            layerId: BACKGROUND_LAYER_ID,
            image_file: imageFileFromPath(item.path)
        }, false);
    }

    function postVideoCommand(action, layerId, traceId) {
        var payload = {
            layerId: layerId,
            suppress_hint: true
        };
        if (traceId) payload.trace_id = traceId;
        return postModuleAction('playback', action, payload, false);
    }

    function getVideoLayerStatus(layerId, traceId) {
        return postVideoCommand('getStatus', layerId, traceId)
            .then(function (data) {
                return data && typeof data === 'object' && !Array.isArray(data) ? data : null;
            });
    }

    function pauseSingleVideoLayerForBackground(layerId, traceId, operationSeq) {
        return getVideoLayerStatus(layerId, traceId)
            .then(function (status) {
                var state = String((status && status.state) || '').toLowerCase();
                var captureMode = !!(status && status.is_capture_mode);
                if (captureMode || state !== 'playing') return false;
                if (!backgroundState.performancePauseEnabled || !isCurrentOperation(operationSeq)) {
                    return false;
                }
                return postVideoCommand('pause', layerId, traceId)
                    .then(function () {
                        if (backgroundState.pausedVideoLayerIds.indexOf(layerId) === -1) {
                            backgroundState.pausedVideoLayerIds.push(layerId);
                        }
                        if (!backgroundState.performancePauseEnabled || !isCurrentOperation(operationSeq)) {
                            return postVideoCommand('resume', layerId, traceId)
                                .then(function () {
                                    backgroundState.pausedVideoLayerIds =
                                        backgroundState.pausedVideoLayerIds.filter(function (id) {
                                            return id !== layerId;
                                        });
                                    return false;
                                });
                        }
                        return layerId;
                    });
            })
            .catch(function (error) {
                warnTrace(traceId, 'video.pause.layer.error', {
                    layerId: layerId,
                    message: error && error.message ? error.message : String(error)
                });
                return false;
            });
    }

    function pauseVideoForBackground(traceId, operationSeq) {
        if (!backgroundState.performancePauseEnabled) {
            logTrace(traceId, 'video.pause.skipped', {
                reason: 'performance-pause-disabled'
            });
            return Promise.resolve(false);
        }
        if (!isCurrentOperation(operationSeq)) {
            logTrace(traceId, 'video.pause.skipped', {
                reason: 'stale-operation',
                operationSeq: operationSeq,
                currentSeq: backgroundState.operationSeq
            });
            return Promise.resolve(false);
        }
        return Promise.all(VIDEO_LAYER_IDS.map(function (layerId) {
            return pauseSingleVideoLayerForBackground(layerId, traceId, operationSeq);
        }))
            .then(function (results) {
                var paused = results.filter(function (id) { return Number(id) > 0; });
                logTrace(traceId, 'video.pause.complete', { pausedLayerIds: paused });
                return paused;
            });
    }

    function resumeVideoAfterBackground(traceId) {
        var layerIds = Array.from(new Set(backgroundState.pausedVideoLayerIds));
        backgroundState.pausedVideoLayerIds = [];
        if (!layerIds.length) {
            logTrace(traceId, 'video.resume.skipped', {
                reason: 'no-background-paused-layers'
            });
            return Promise.resolve(false);
        }
        logTrace(traceId, 'video.resume.list', { layerIds: layerIds });
        return Promise.all(layerIds.map(function (layerId) {
            return postVideoCommand('resume', layerId, traceId)
                .catch(function (error) {
                    warnTrace(traceId, 'video.resume.layer.error', {
                        layerId: layerId,
                        message: error && error.message ? error.message : String(error)
                    });
                    return false;
                });
        })).then(function () {
            logTrace(traceId, 'video.resume.complete', { layerIds: layerIds });
            return true;
        });
    }

    function loadBackgroundStatus() {
        if (backgroundState.loading) return;
        backgroundState.loading = true;

        loadBackgroundItems()
            .then(function (items) {
                backgroundState.loading = false;
                updateBackgroundState({
                    show: false,
                    items: items
                });
            })
            .catch(function (error) {
                backgroundState.loading = false;
                console.error('[FusionBackground] Failed to load status:', error);
                updateBackgroundState({ show: false });
            });
    }

    function updateBackgroundState(data) {
        if (typeof data.show === 'boolean') backgroundState.show = data.show;
        if (Array.isArray(data.items)) backgroundState.items = data.items;
        if (typeof data.currentIndex === 'number') backgroundState.currentIndex = data.currentIndex;

        var showToggle = document.getElementById('bg-show-toggle');
        if (showToggle) {
            showToggle.checked = backgroundState.show;
        }

        var info = document.getElementById('bg-info');
        if (info) {
            info.textContent = backgroundState.items.length > 0
                ? (backgroundState.currentIndex + 1) + ' / ' + backgroundState.items.length
                : '无图片';
        }

        var pauseToggle = document.getElementById('bg-pause-video-toggle');
        if (pauseToggle) {
            pauseToggle.checked = backgroundState.performancePauseEnabled;
        }
    }

    function handlePerformancePauseToggle(event) {
        var enabled = !!event.target.checked;
        var operationSeq = nextOperationSeq();
        backgroundState.performancePauseEnabled = enabled;
        if (!enabled) {
            void resumeVideoAfterBackground();
            return;
        }
        if (backgroundState.show) {
            void pauseVideoForBackground('', operationSeq);
        }
    }

    function handleShowToggle(event) {
        if (event.target.disabled) return;
        var operationSeq = nextOperationSeq();
        var show = event.target.checked;
        backgroundState.userVisible = show;
        if (!show) {
            backgroundState.autoDebugVisible = false;
            backgroundState.debugModeActive = false;
        }
        updateBackgroundState({ show: show });

        (show ? ensureBackgroundItemsLoaded() : Promise.resolve(backgroundState.items))
            .then(function () {
                return ensureBackgroundRuntimeLayer();
            })
            .then(function (available) {
                if (!available) return Promise.reject(new Error('Fusion background layer is unavailable'));
                return syncBackgroundLayerRect(show, '', 'toggleBackground', operationSeq);
            })
            .then(function () {
                if (!isCurrentOperation(operationSeq)) return false;
                updateBackgroundState({ show: show });
                if (show) {
                    return pauseVideoForBackground('', operationSeq)
                        .then(function () {
                            if (!isCurrentOperation(operationSeq)) return false;
                            if (backgroundState.items.length > 0) {
                                return loadImageToLayer(backgroundState.items[backgroundState.currentIndex]);
                            }
                        });
                }
                if (!show) {
                    return resumeVideoAfterBackground()
                        .then(function () {
                            if (!isCurrentOperation(operationSeq)) return false;
                            return removeBackgroundRuntimeLayer();
                        });
                }
            })
            .catch(function (error) {
                console.error('[FusionBackground] Failed to toggle layer:', error);
                updateBackgroundState({ show: !show });
            });
    }

    function switchBackground(delta) {
        var operationSeq = nextOperationSeq();
        var prevButton = document.getElementById('bg-prev-btn');
        var nextButton = document.getElementById('bg-next-btn');
        if ((prevButton && prevButton.disabled) || (nextButton && nextButton.disabled)) return;
        if (!backgroundState.items.length) return;
        var nextIndex = (backgroundState.currentIndex + delta + backgroundState.items.length) % backgroundState.items.length;
        if (!backgroundState.autoDebugVisible) {
            backgroundState.userVisible = true;
        }
        updateBackgroundState({ currentIndex: nextIndex, show: true });
        ensureBackgroundRuntimeLayer()
            .then(function (available) {
                if (!available) return Promise.reject(new Error('Fusion background layer is unavailable'));
                return syncBackgroundLayerRect(true, '', 'switchBackground', operationSeq);
            })
            .then(function () {
                if (!isCurrentOperation(operationSeq)) return false;
                return pauseVideoForBackground('', operationSeq)
                    .then(function () {
                        if (!isCurrentOperation(operationSeq)) return false;
                        return loadImageToLayer(backgroundState.items[nextIndex]);
                    });
            })
            .catch(function (error) {
                console.error('[FusionBackground] Failed to switch image:', error);
            });
    }

    function handlePrevClick() {
        switchBackground(-1);
    }

    function handleNextClick() {
        switchBackground(1);
    }

    function queueDebugBackgroundOperation(task, traceId) {
        var queuedAt = traceNowMs();
        logTrace(traceId, 'debugBackground.queue');
        backgroundState.debugOperation = backgroundState.debugOperation
            .catch(function () { return null; })
            .then(function () {
                logTrace(traceId, 'debugBackground.queue.enter', {
                    wait_ms: Math.round(traceNowMs() - queuedAt)
                });
                return task();
            })
            .catch(function (error) {
                warnTrace(traceId, 'debugBackground.queue.error', {
                    message: error && error.message ? error.message : String(error)
                });
                console.warn('[FusionBackground] Debug background operation failed:', error);
                return null;
            });
        return backgroundState.debugOperation;
    }

    function enableDebugBackground(traceId, operationSeq) {
        backgroundState.debugModeActive = true;
        return queueDebugBackgroundOperation(function () {
            logTrace(traceId, 'debugBackground.enable.begin', {
                autoDebugVisible: backgroundState.autoDebugVisible,
                userVisible: backgroundState.userVisible,
                show: backgroundState.show,
                operationSeq: operationSeq
            });
            return ensureBackgroundItemsLoaded()
                .then(function (items) {
                    if (!backgroundState.debugModeActive || !isCurrentOperation(operationSeq)) return false;
                    if (!items.length) {
                        updateBackgroundState({ show: false });
                        return false;
                    }
                    if (!backgroundState.show) {
                        backgroundState.autoDebugVisible = true;
                    }
                    return ensureBackgroundRuntimeLayer()
                        .then(function (available) {
                            if (!available) throw new Error('Fusion background layer is unavailable');
                            if (!backgroundState.debugModeActive || !isCurrentOperation(operationSeq)) return false;
                            return syncBackgroundLayerRect(true, traceId, 'enableDebugBackground', operationSeq);
                        })
                        .then(function () {
                            if (!backgroundState.debugModeActive || !isCurrentOperation(operationSeq)) return false;
                            updateBackgroundState({ show: true });
                            return pauseVideoForBackground(traceId, operationSeq);
                        })
                        .then(function () {
                            if (!backgroundState.debugModeActive || !isCurrentOperation(operationSeq)) return false;
                            return loadImageToLayer(backgroundState.items[backgroundState.currentIndex]);
                        })
                        .then(function () {
                            logTrace(traceId, 'debugBackground.enable.end');
                            return true;
                        });
                });
        }, traceId);
    }

    function disableDebugBackground(traceId, operationSeq) {
        backgroundState.debugModeActive = false;
        return queueDebugBackgroundOperation(function () {
            var startedAt = traceNowMs();
            var shouldHide = backgroundState.autoDebugVisible && !backgroundState.userVisible;
            logTrace(traceId, 'debugBackground.disable.begin', {
                shouldHide: shouldHide,
                autoDebugVisible: backgroundState.autoDebugVisible,
                userVisible: backgroundState.userVisible,
                show: backgroundState.show,
                operationSeq: operationSeq
            });
            backgroundState.autoDebugVisible = false;
            return resumeVideoAfterBackground(traceId)
                .then(function () {
                    if (!shouldHide) return false;
                    return syncBackgroundLayerRect(false, traceId, 'disableDebugBackground', operationSeq);
                })
                .then(function () {
                    if (shouldHide) {
                        updateBackgroundState({ show: false });
                    }
                    if (shouldHide && isCurrentOperation(operationSeq)) {
                        return removeBackgroundRuntimeLayer()
                            .then(function () {
                                logTrace(traceId, 'debugBackground.disable.end', {
                                    cost_ms: Math.round(traceNowMs() - startedAt),
                                    hidden: shouldHide,
                                    performancePauseEnabled: backgroundState.performancePauseEnabled
                                });
                                return true;
                            });
                    }
                    logTrace(traceId, 'debugBackground.disable.end', {
                        cost_ms: Math.round(traceNowMs() - startedAt),
                        hidden: shouldHide,
                        performancePauseEnabled: backgroundState.performancePauseEnabled
                    });
                    return true;
                });
        }, traceId);
    }

    function hideBackground(traceId) {
        var operationSeq = nextOperationSeq();
        backgroundState.debugModeActive = false;
        backgroundState.autoDebugVisible = false;
        backgroundState.userVisible = false;
        return queueDebugBackgroundOperation(function () {
            logTrace(traceId, 'debugBackground.hide.begin', { operationSeq: operationSeq });
            return ensureBackgroundRuntimeLayer()
                .then(function (available) {
                    if (!available) return Promise.reject(new Error('Fusion background layer is unavailable'));
                    return syncBackgroundLayerRect(false, traceId, 'hideBackground', operationSeq);
                })
                .then(function () {
                    updateBackgroundState({ show: false });
                    return resumeVideoAfterBackground(traceId);
                })
                .then(function () {
                    if (!isCurrentOperation(operationSeq)) return false;
                    return removeBackgroundRuntimeLayer();
                })
                .then(function () {
                    return true;
                });
        }, traceId);
    }

    function setDebugModeActive(active, traceId) {
        var operationSeq = nextOperationSeq();
        logTrace(traceId, 'debugBackground.setActive', {
            active: !!active,
            operationSeq: operationSeq
        });
        return active ? enableDebugBackground(traceId, operationSeq) : disableDebugBackground(traceId, operationSeq);
    }

    window.FusionBackground = {
        init: initBackgroundUI,
        loadStatus: loadBackgroundStatus,
        updateState: updateBackgroundState,
        enableDebugBackground: enableDebugBackground,
        disableDebugBackground: disableDebugBackground,
        hideBackground: hideBackground,
        setDebugModeActive: setDebugModeActive
    };

    window.addEventListener('pagehide', function () {
        removeBackgroundRuntimeLayer(true);
    });

})();
