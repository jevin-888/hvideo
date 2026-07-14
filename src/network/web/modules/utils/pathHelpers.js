/**
 * @file pathHelpers.js（文件名）
 * @brief 路径处理工具函数库
 * 
 * 提供统一的路径处理工具函数，避免重复代码
 */

/**
 * 从文件路径提取文件名
 * @param {string} path - 文件路径
 * @returns {string} 文件名
 */
export function extractFileName(path) {
    if (!path) return '';
    const parts = path.split('/');
    return parts[parts.length - 1] || path;
}

/**
 * 从文件路径提取文件夹名称（从video/image/Music目录后的第一个子目录）
 * @param {string} path - 文件路径
 * @returns {string} 文件夹名称
 */
export function extractFolderName(path) {
    if (!path) return '';
    // 将路径按 / 分割
    const parts = path.split('/');
    // 查找 video、image/Image 或 Music 目录的索引（大小写不敏感）
    let baseTypeIndex = -1;
    for (let i = 0; i < parts.length; i++) {
        const partLower = parts[i].toLowerCase();
        if (partLower === 'video' || partLower === 'image' || parts[i] === 'Music') {
            baseTypeIndex = i;
            break;
        }
    }
    // 如果找到了 video、image/Image 或 Music 目录，且后面还有至少2个部分（文件夹名和文件名）
    if (baseTypeIndex >= 0 && baseTypeIndex + 2 < parts.length) {
        // 返回 video/image/Image/Music 后的第一个子目录名称
        return parts[baseTypeIndex + 1];
    }
    return '';
}

/**
 * 从文件路径提取文件夹名称（只显示直接父文件夹名称）
 * @param {string} path - 文件路径
 * @returns {string} 父文件夹名称
 */
export function getFolderFromPath(path) {
    if (!path) return '';
    const parts = path.split('/');
    // 如果路径中至少有2个部分（文件名和至少一个父目录）
    if (parts.length >= 2) {
        // 获取倒数第二个部分（父目录名称）
        const parentFolder = parts[parts.length - 2];
        // 如果是基础目录（video/image/Image/huoshan），不显示（大小写不敏感）
        const parentFolderLower = parentFolder.toLowerCase();
        if (parentFolderLower === 'video' || parentFolderLower === 'image' || parentFolder === 'huoshan') {
            return '';
        }
        return parentFolder;
    }
    return '';
}

export function normalizePath(path) {
    if (!path) return '/';
    return '/' + path.split('/').filter(p => p).join('/');
}

