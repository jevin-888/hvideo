#ifndef INIT_PROGRESS_H
#define INIT_PROGRESS_H

namespace hsvj {

/**
 * 初始化阶段枚举
 */
enum class InitStage : int {
    STAGE_PRE_CHECK = 0,      // 步骤 1：环境预检
    STAGE_FRAMEWORK = 1,      // 步骤 2：启动引擎框架
    STAGE_MODULES = 2,        // 步骤 3：初始化模块
    STAGE_BUSINESS = 3,       // 步骤 4：加载各模块
    STAGE_COMPLETE = 4        // 完成
};

/**
 * 各阶段内的详细步骤枚举
 */
enum class InitStep : int {
    // Stage 1: 环境预检
    STEP_1_1_PATH_LOG = 0,        // 1.1 初始化路径和日志
    STEP_1_2_DIRECTORIES = 1,     // 1.2 目录治理
    STEP_1_3_CONFIG = 2,          // 1.3 检查配置文件
    STEP_1_4_LICENSE = 3,         // 1.4 授权检查
    
    // Stage 2: 启动引擎框架
    STEP_2_1_LOAD_CONFIG = 0,     // 2.1 加载 config.json
    STEP_2_2_CONSTRUCT = 1,       // 2.2 构造管理器骨架
    
    // Stage 3: 初始化模块
    STEP_3_1_CREATE_LAYERS = 0,   // 3.1 预创建图层
    STEP_3_2_MANAGERS = 1,        // 3.2 管理器初始化
    
    // Stage 4: 加载各模块
    STEP_4_1_HARDWARE = 0,        // 4.1 硬件模块
    STEP_4_2_RENDERER = 1,        // 4.2 渲染模块
    STEP_4_3_BUSINESS = 2,        // 4.3 业务能力
    STEP_4_4_DSP = 3,             // 4.4 DSP 音频
    STEP_4_5_NETWORK_INIT = 4,    // 4.5 网络模块初始化
    STEP_4_6_NETWORK_START = 5,   // 4.6 启用外部端口
    STEP_4_7_DEFAULT_PLAY = 6,    // 4.7 默认播放
    STEP_4_8_CAPTURE = 7,         // 4.8 采集启动
    STEP_4_9_REPORT = 8           // 4.9 设备上报
};

/**
 * 进度百分比映射表
 * 用于将 Stage+Step 转换为总进度百分比
 */
inline int getProgressPercent(InitStage stage, InitStep step) {
    // 基础进度：每个 Stage 分配不同的权重
    // Stage 1 (预检): 0-20%
    // Stage 2 (框架): 20-25%
    // Stage 3 (模块): 25-45%
    // Stage 4 (业务): 45-100%
    
    switch (static_cast<int>(stage)) {
        case 0: // Stage 1: 环境预检
    return 5 + static_cast<int>(step) * 5;  // 5%, 10%, 15%, 20%
        
        case 1: // Stage 2: 启动引擎框架
    return 20 + static_cast<int>(step) * 5;  // 20%, 25%
        
        case 2: // Stage 3: 初始化模块
    return 25 + static_cast<int>(step) * 10;  // 25%, 35%
        
        case 3: // Stage 4: 加载各模块
    return 45 + static_cast<int>(step) * 6;  // 45%, 51%, 57%, 63%, 69%, 75%, 81%, 87%, 93%
        
        case 4: // Stage 5: 完成
    return 100;
        
        default:
            return 0;
    }
}

/**
 * 获取进度的中文描述
 */
inline const char* getProgressMessage(InitStage stage, InitStep step) {
    switch (static_cast<int>(stage)) {
        case 0: // 阶段 1
            switch (static_cast<int>(step)) {
                case 0: return "初始化路径和日志...";
                case 1: return "创建目录结构...";
                case 2: return "加载配置文件...";
                case 3: return "检查授权许可...";
                default: return "环境预检中...";
            }
        
        case 1: // 阶段 2
            switch (static_cast<int>(step)) {
                case 0: return "启动引擎框架...";
                case 1: return "构造管理器骨架...";
                default: return "框架初始化中...";
            }
        
        case 2: // 阶段 3
            switch (static_cast<int>(step)) {
                case 0: return "预创建授权图层...";
                case 1: return "初始化管理器...";
                default: return "模块初始化中...";
            }
        
        case 3: // 阶段 4
            switch (static_cast<int>(step)) {
                case 0: return "启动硬件模块...";
                case 1: return "初始化渲染引擎...";
                case 2: return "配置业务能力...";
                case 3: return "DSP 音频配置...";
                case 4: return "初始化网络模块...";
                case 5: return "启动网络服务...";
                case 6: return "准备就绪...";
                case 7: return "启动采集模块...";
                case 8: return "设备上报...";
                default: return "业务加载中...";
            }
        
        case 4: // Stage 5: 完成
    return "初始化完成";
        
        default:
            return "正在启动...";
    }
}

} // 命名空间 hsvj

#endif // 结束 INIT_PROGRESS_H
