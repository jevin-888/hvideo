# Hplayer 功能覆盖清单

> 本清单由 tests/catalog/generate_catalog.py 从源码生成。planned 只表示已发现，只有执行过测试用例后才能改为 passed。

## API 模块

| 模块 | 发现动作 | 测试状态 | 来源 |
| --- | --- | --- | --- |
| layers | create_layer, create_runtime_layer, create_slice, getLayerInfo, get_property, get_roamConfig, list, removeLayer, remove_runtime_layer, restartCapture, restartRk628Capture, set_property, show_hint, startCapture, stopCapture, update, update_priority | planned | src/core/CommandRouter_LayerManagement.cpp, src/network/web/modules/pages/layerMatrix/layerManagement.js, src/network/web/modules/pages/layerMatrix/sliceManagement.js |
| lyrics | bind_layer, getStatus, get_bind_layer, get_margin, get_style, list_styles, load, set_margin, set_style, set_visible, unload | planned | src/core/CommandRouter_Lyric.cpp, src/network/web/modules/pages/layerMatrix/layerManagement.js |
| peripheral-events | clear_simulation, set_channel | planned | src/network/web/modules/pages/peripheralManagement.js |
| peripherals | add_template, bluetooth_get_status, bluetooth_set_name, bluetooth_set_power, bluetooth_set_role, clear_simulation, clear_templates, ctrl512, delay, delete_template, dmx512_legacy, execute_function_button, forward_payload, forward_to_serial, get_config, get_functions, get_template_functions, get_templates, import_templates, list_serial_ports, rename_template, save_template_functions, send_test, set_channel, set_config, set_learn_mode, set_master | planned | src/core/PeripheralManager.cpp, src/network/web/mobile/modules/pages/mobileControl.js, src/network/web/modules/pages/peripheralManagement.js, src/network/web/modules/pages/vodForward.js |
| playback | create_slice, detect_capture_devices, getPlaybackLock, getStatus, getSystemVolume, loadVideo, lockPlayback, muteToggle, next, next_audioTrack, pause, play, prepare, prev_audioTrack, previous, replay, restartCapture, restartRk628Capture, resume, seek, setPlaybackRate, setSystemVolume, setVolume, set_audioChannel, startCapture, stop, stopCapture, switch_audioTrack, unlockPlayback, volumeDown, volumeUp | planned | src/core/CommandRouter_VideoPlayback.cpp, src/network/web/mobile/modules/pages/mobileVideoControl.js |
| playlists | addVideoToPlayList, createPlayList, deletePlayList, getPlayMode, playVideo, removeVideoFromPlayList, setPlayMode, setTargetLayer | planned | src/core/CommandRouter_Playlist.cpp |
| regions | auto_recalculate_blend, blend_resize_grid, checkpoint_fusion_state, geometry_move, geometry_resize_grid, get_blend_auto_edges, get_cave_wall_config, get_flexible_mapping, get_fusion_master_enabled, get_geometry_state, get_manager_mode, get_mask_state, get_region_blend, get_region_color, get_region_config, get_region_geometry_correction, manager_move_line, manager_move_point, mask_move, mask_resize_grid, refresh_regions, reset_fusion_config, save_fusion_config, save_geometry, save_mask, save_matrix_command, seed_mask_from_geometry, set_active_region_id, set_audio_output_layer, set_blend_auto_edges, set_cave_wall_config, set_flexible_mapping, set_fusion_master_enabled, set_geometry_display, set_geometry_display_all, set_geometry_grid, set_geometry_point, set_geometry_points, set_geometry_selection, set_grid_visual_style, set_manager_mode, set_mask_state, set_merge_gap_brightness, set_region_blend, set_region_color, set_region_geometry_correction | planned | src/core/CommandRouter_RegionConfig.cpp, src/network/web/modules/pages/flexibleMapping.js, src/network/web/modules/pages/layerMatrix/config.js |
| rendering | add_photo, clear_photos, create_slice, generate_qrcode, loadImage, set_blend_mode, set_effect, update_text | planned | src/core/CommandRouter_LayerRender.cpp, src/network/web/modules/features/materials.js, src/network/web/modules/features/playlist.js, src/network/web/modules/pages/videoControl.js |
| scenes | apply_scene, delete_scene, list_scenes, load_default, load_scene, save_as, save_current, switch_scene | planned | src/core/CommandRouter_Scene.cpp, src/network/web/mobile/modules/pages/mobileScene.js |
| stage-control | arm, disarm, get_config, preview_cue, replace_config, reset_timeline, set_enabled | planned | src/core/StageControlManager.cpp, src/network/web/modules/pages/stageControl.js |
| sync | online_vod_room_sync_start, online_vod_room_sync_stop, sync_device | planned | src/core/CommandRouter_Sync.cpp, src/network/web/modules/pages/systemSettings.js |
| system | reboot, restart_app | planned | src/core/CommandRouter_System.cpp |
| system-config | 动态动作/待补充 | planned | 后端注册 |

## 页面和功能文件

| 类型 | 名称 | 页面 | 测试状态 |
| --- | --- | --- | --- |
| UI | configBackup | desktop | planned |
| UI | displayOutput | desktop | planned |
| UI | displayResolution | desktop | planned |
| UI | effectControl | desktop | planned |
| UI | factorySerial | desktop | planned |
| UI | flexibleMapping | desktop | planned |
| UI | fusion-background | desktop | planned |
| UI | fusionControl | desktop | planned |
| UI | layerMatrix | desktop | planned |
| UI | login | desktop | planned |
| UI | peripheralManagement | desktop | planned |
| UI | projectionCorrection | desktop | planned |
| UI | sceneTemplates | desktop | planned |
| UI | stageControl | desktop | planned |
| UI | systemSettings | desktop | planned |
| UI | systemStatus | desktop | planned |
| UI | videoControl | desktop | planned |
| UI | vodForward | desktop | planned |
| UI | mobileControl | mobile | planned |
| UI | mobileScene | mobile | planned |
| UI | mobileVideoControl | mobile | planned |
| UI | materials | desktop | planned |
| UI | playlist | desktop | planned |
| UI | videoPreview | desktop | planned |

## 原生功能域

| 功能域 | 测试状态 |
| --- | --- |
| audio | planned |
| capture | planned |
| core | planned |
| database | planned |
| decoder | planned |
| effect | planned |
| foundation | planned |
| fusion | planned |
| layer | planned |
| lyric | planned |
| network | planned |
| platform | planned |
| playcontrol | planned |
| renderer | planned |
| text | planned |
| utils | planned |
| vod | planned |

## 特殊路由

| 路由 | 测试状态 |
| --- | --- |
| /api/v1/audio-effect/disable | planned |
| /api/v1/audio-effect/enable | planned |
| /api/v1/audio-effect/preview | planned |
| /api/v1/audio-effect/spectrum | planned |
| /api/v1/audio-reactive/config | planned |
| /api/v1/audio-reactive/engine | planned |
| /api/v1/audio-reactive/learn | planned |
| /api/v1/audio-reactive/spectrum | planned |
| /api/v1/audio-reactive/state | planned |
| /api/v1/auth/config | planned |
| /api/v1/auth/login | planned |
| /api/v1/auth/logout | planned |
| /api/v1/auth/validate | planned |
| /api/v1/config.json | planned |
| /api/v1/config/layers | planned |
| /api/v1/config/layers/{id} | planned |
| /api/v1/config/save | planned |
| /api/v1/dmx/channels | planned |
| /api/v1/dmx/config | planned |
| /api/v1/dmx/status | planned |
| /api/v1/effect-config | planned |
| /api/v1/effect/apply | planned |
| /api/v1/filesystem/debug/list | planned |
| /api/v1/filesystem/list | planned |
| /api/v1/filesystem/mkdir | planned |
| /api/v1/filesystem/usb/media | planned |
| /api/v1/fusion/checkpoint | planned |
| /api/v1/fusion/reset | planned |
| /api/v1/fusion/save | planned |
| /api/v1/heartbeat | planned |
| /api/v1/layers | planned |
| /api/v1/layers/authorized | planned |
| /api/v1/layers/templates | planned |
| /api/v1/layers/templates/{name} | planned |
| /api/v1/layers/templates/{name}/add | planned |
| /api/v1/layers/{id} | planned |
| /api/v1/layers/{id}/roam | planned |
| /api/v1/layers/{id}/roam/reset | planned |
| /api/v1/logs | planned |
| /api/v1/logs/date | planned |
| /api/v1/logs/download | planned |
| /api/v1/logs/file | planned |
| /api/v1/materials | planned |
| /api/v1/materials/categories | planned |
| /api/v1/materials/delete | planned |
| /api/v1/materials/delete_folder | planned |
| /api/v1/materials/download | planned |
| /api/v1/materials/download_file | planned |
| /api/v1/materials/folders | planned |
| /api/v1/materials/index_status | planned |
| /api/v1/materials/refresh_index | planned |
| /api/v1/materials/rename | planned |
| /api/v1/materials/thumbnail | planned |
| /api/v1/materials/thumbnail/status | planned |
| /api/v1/mirror/actions/{action} | planned |
| /api/v1/orders | planned |
| /api/v1/orders/items | planned |
| /api/v1/peripheral/presets | planned |
| /api/v1/peripherals/command-lists | planned |
| /api/v1/peripherals/command-lists/check | planned |
| /api/v1/peripherals/command-lists/read | planned |
| /api/v1/playlists | planned |
| /api/v1/playlists/default | planned |
| /api/v1/playlists/{id} | planned |
| /api/v1/playlists/{id}/config | planned |
| /api/v1/playlists/{id}/items | planned |
| /api/v1/playlists/{id}/items/{index} | planned |
| /api/v1/products | planned |
| /api/v1/products/categories | planned |
| /api/v1/rooms/{id}/actions/{action} | planned |
| /api/v1/rooms/{id}/clear | planned |
| /api/v1/rooms/{id}/materials/play | planned |
| /api/v1/rooms/{id}/peripheral/ac | planned |
| /api/v1/rooms/{id}/peripheral/ambiance | planned |
| /api/v1/rooms/{id}/peripheral/button | planned |
| /api/v1/rooms/{id}/peripheral/call | planned |
| /api/v1/rooms/{id}/peripheral/effect | planned |
| /api/v1/rooms/{id}/peripheral/light | planned |
| /api/v1/rooms/{id}/peripheral/voice | planned |
| /api/v1/rooms/{id}/queue | planned |
| /api/v1/rooms/{id}/queue/played | planned |
| /api/v1/rooms/{id}/queue/prioritize | planned |
| /api/v1/rooms/{id}/queue/shuffle | planned |
| /api/v1/rooms/{id}/queue/{songId} | planned |
| /api/v1/rooms/{id}/state | planned |
| /api/v1/rooms/{id}/youtube/queue | planned |
| /api/v1/runtime/layers | planned |
| /api/v1/runtime/layers/{id} | planned |
| /api/v1/scenes | planned |
| /api/v1/scenes/current | planned |
| /api/v1/scenes/{name} | planned |
| /api/v1/scenes/{name}/load | planned |
| /api/v1/service-types | planned |
| /api/v1/settings/screen_rotate | planned |
| /api/v1/singerimg | planned |
| /api/v1/songdb/singers | planned |
| /api/v1/songdb/songs | planned |
| /api/v1/songdb/warmup | planned |
| /api/v1/streams | planned |
| /api/v1/sync/push-to | planned |
| /api/v1/sync/receive | planned |
| /api/v1/system/boot-animation/install | planned |
| /api/v1/system/boot-animation/list | planned |
| /api/v1/system/capture-edid | planned |
| /api/v1/system/config | planned |
| /api/v1/system/config-backup | planned |
| /api/v1/system/config-backup/status | planned |
| /api/v1/system/data-root | planned |
| /api/v1/system/device-alias | planned |
| /api/v1/system/device-info | planned |
| /api/v1/system/device-info/export | planned |
| /api/v1/system/dicts | planned |
| /api/v1/system/discovery | planned |
| /api/v1/system/display-output | planned |
| /api/v1/system/display-resolution | planned |
| /api/v1/system/factory-serial | planned |
| /api/v1/system/lan-devices | planned |
| /api/v1/system/license | planned |
| /api/v1/system/license/import | planned |
| /api/v1/system/network/current-ip-config | planned |
| /api/v1/system/network/info | planned |
| /api/v1/system/ping | planned |
| /api/v1/system/reboot | planned |
| /api/v1/system/resources | planned |
| /api/v1/system/settings | planned |
| /api/v1/system/status | planned |
| /api/v1/system/storage-space | planned |
| /api/v1/video/dsp/audio-route | planned |
| /api/v1/video/status | planned |
| /api/v1/video/transcode | planned |
| /api/v1/video/transcode_batch | planned |
| /api/v1/video/transcode_status | planned |
| /api/v1/youtube/hot | planned |
| /api/v1/youtube/parse | planned |
| /api/v1/youtube/search | planned |

## 覆盖边界

- Actions built dynamically at runtime need explicit cases or a backend action manifest.
- HTTP routes outside /api/v1 and WebSocket/event payloads need protocol-specific cases.
- Hardware-only behavior needs a real hw81/hw82 device profile and evidence collector.
- Server and Flutter Pad flows need external project adapters and environment fixtures.
