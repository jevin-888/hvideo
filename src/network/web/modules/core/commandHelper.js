/**
 * @file commandHelper.js
 * @brief 模块动作辅助函数
 */

import { apiAction } from './api.js';
import { ApiModule } from '../../shared/moduleActions.js';

/**
 * 通用模块动作调用。模块和动作分别进入 URL，params 仅包含业务参数。
 */
export async function sendModuleAction(moduleName, action, params = {}) {
    return await apiAction(moduleName, action, params);
}

export async function sendSceneCommand(action, params = {}) {
    return await sendModuleAction(ApiModule.SCENES, action, params);
}

export async function sendPeripheralCommand(action, params = {}) {
    return await sendModuleAction(ApiModule.PERIPHERALS, action, params);
}

export async function sendPeripheralEventCommand(action, params = {}) {
    return await sendModuleAction(ApiModule.PERIPHERAL_EVENTS, action, params);
}

export async function sendRegionCommand(action, params = {}) {
    return await sendModuleAction(ApiModule.REGIONS, action, params);
}
