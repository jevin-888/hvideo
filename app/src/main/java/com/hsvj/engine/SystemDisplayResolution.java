package com.hsvj.engine;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

/** Main display control through the same framework service used by TvSettings. */
final class SystemDisplayResolution {
    private static final int DISPLAY = 0;
    private static final int HDMI = 11;
    private static final int MIPI = 16;
    private static final Class<?>[] DISPLAY_ARGS = {int.class, int.class};

    private SystemDisplayResolution() {}

    static synchronized String request(String requestedMode) {
        JSONObject result = new JSONObject();
        try {
            Object manager = Class.forName("android.os.RkDisplayOutputManager")
                    .getDeclaredConstructor().newInstance();
            int[] interfaces = (int[]) invoke(manager, "getIfaceList",
                    new Class<?>[]{int.class}, DISPLAY);
            if (interfaces == null || interfaces.length == 0) {
                throw new IllegalStateException("系统未检测到显示输出");
            }
            int type = (Integer) invoke(manager, "getCurrentInterface",
                    new Class<?>[]{int.class}, DISPLAY);
            String[] reported = (String[]) invoke(manager, "getModeList", DISPLAY_ARGS, DISPLAY, type);
            if (reported == null || reported.length == 0) {
                throw new IllegalStateException("无法读取系统分辨率列表");
            }
            Set<String> modes = new LinkedHashSet<>();
            if (type == HDMI) {
                // Keep the manual HDMI choices identical to TvSettings/DrmDisplaySetting.
                modes.addAll(Arrays.asList("3840x2160p60.00", "3840x2160p30.00", "1920x1080p60.00"));
            } else {
                for (String mode : reported) {
                    mode = normalize(mode);
                    if (!mode.isEmpty() && !(type == MIPI && "Auto".equals(mode))) modes.add(mode);
                }
            }
            if (modes.isEmpty()) throw new IllegalStateException("系统未提供可用分辨率");
            String current = type == MIPI ? modes.iterator().next() : current(manager, type);
            if (requestedMode != null && !requestedMode.isEmpty()) {
                if (type == MIPI) throw new IllegalArgumentException("MIPI 使用固定分辨率，无法修改");
                if (!modes.contains(requestedMode)) throw new IllegalArgumentException("请选择系统支持的分辨率");
                if (!matches(requestedMode, current)) {
                    String previous = current;
                    try {
                        setMode(manager, type, requestedMode);
                        // setMode is asynchronous and may swallow service errors. Verify before saving.
                        for (int attempt = 0; attempt < 20; attempt++) {
                            current = current(manager, type);
                            if (matches(requestedMode, current)) break;
                            Thread.sleep(100);
                        }
                        if (!matches(requestedMode, current)) {
                            throw new IllegalStateException("系统分辨率切换未生效");
                        }
                        if ((Integer) invoke(manager, "saveConfig", new Class<?>[0]) != 0) {
                            throw new IllegalStateException("系统分辨率保存失败");
                        }
                    } catch (Exception error) {
                        // Match the system settings failure path: restore the previous mode.
                        try {
                            setMode(manager, type, previous);
                            invoke(manager, "saveConfig", new Class<?>[0]);
                        } catch (Exception restoreError) {
                            error.addSuppressed(restoreError);
                        }
                        throw error;
                    }
                }
            }
            for (String mode : modes) {
                if (matches(mode, current)) { current = mode; break; }
            }
            JSONObject state = new JSONObject();
            state.put("mode", current);
            state.put("modes", new JSONArray(modes));
            state.put("read_only", type == MIPI);
            result.put("data", state);
        } catch (Exception error) {
            try {
                result.put("status", error instanceof IllegalArgumentException ? 400 : 503);
                result.put("message", error.getMessage() == null
                        ? "无法访问系统显示服务" : error.getMessage());
            } catch (Exception ignored) { /* JSONObject contains only strings and integers. */ }
        }
        return result.toString();
    }

    private static Object invoke(Object manager, String method, Class<?>[] signature, Object... args)
            throws Exception {
        return manager.getClass().getMethod(method, signature).invoke(manager, args);
    }

    private static String current(Object manager, int type) throws Exception {
        String value = normalize((String) invoke(manager, "getCurrentMode", DISPLAY_ARGS, DISPLAY, type));
        if (value.isEmpty()) throw new IllegalStateException("无法读取当前系统分辨率");
        return value;
    }

    private static void setMode(Object manager, int type, String mode) throws Exception {
        int activeType = (Integer) invoke(manager, "getCurrentInterface", new Class<?>[]{int.class}, DISPLAY);
        if (activeType != type) throw new IllegalStateException("显示输出已变化，请重新读取分辨率");
        invoke(manager, "setMode", new Class<?>[]{int.class, int.class, String.class}, DISPLAY, type, mode);
    }

    private static String normalize(String mode) {
        return mode == null ? "" : mode.replaceFirst("(?<=[0-9])-[0-9]+$", "");
    }

    private static boolean matches(String expected, String actual) {
        if (expected.equals(actual)) return true;
        int rate = Math.max(expected.indexOf('p'), expected.indexOf('i'));
        if (rate <= 0 || !actual.startsWith(expected.substring(0, rate + 1))) return false;
        try {
            return Math.abs(Double.parseDouble(expected.substring(rate + 1))
                    - Double.parseDouble(actual.substring(rate + 1))) < 0.01;
        } catch (NumberFormatException ignored) {
            return false;
        }
    }
}
