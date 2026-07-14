/**
 * @file SceneTemplateManager.cpp（文件名）
 * @brief 场景模板管理器实现
 *
 * 本文件实现了场景模板管理器类，负责：
 * - 场景模板的保存和加载
 * - 场景模板列表管理
 * - 场景模板文件操作
 * - 场景模板元数据管理
 */

#include "SceneTemplateManager.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <sys/stat.h>

namespace fs = std::filesystem;

SceneTemplateManager::SceneTemplateManager(const std::string& templateDir) 
    : templateDir_(templateDir) {
    // 确保模板目录存在
    if (!dirExists(templateDir_)) {
        createDir(templateDir_);
    }
}

SceneTemplateManager::~SceneTemplateManager() {
}

void SceneTemplateManager::setTemplateDir(const std::string& dir) {
    templateDir_ = dir;
    // 确保模板目录存在
    if (!dirExists(templateDir_)) {
        createDir(templateDir_);
    }
}

const std::string& SceneTemplateManager::getTemplateDir() const {
    return templateDir_;
}

bool SceneTemplateManager::saveScene(const std::string& name, const std::string& content) {
    if (!isValidFileName(name)) {
        return false;
    }
    
    std::string filePath = getSceneFilePath(name);
    return writeFile(filePath, content);
}

std::string SceneTemplateManager::loadScene(const std::string& name) {
    if (!isValidFileName(name)) {
        return "";
    }
    
    std::string filePath = getSceneFilePath(name);
    return readFile(filePath);
}

bool SceneTemplateManager::deleteScene(const std::string& name) {
    if (!isValidFileName(name)) {
        return false;
    }
    
    std::string filePath = getSceneFilePath(name);
    return deleteFile(filePath);
}

std::vector<std::string> SceneTemplateManager::listScenes() {
    return listFiles(templateDir_);
}

bool SceneTemplateManager::sceneExists(const std::string& name) {
    if (!isValidFileName(name)) {
        return false;
    }
    
    std::string filePath = getSceneFilePath(name);
    return fileExists(filePath);
}

std::unordered_map<std::string, std::string> SceneTemplateManager::getSceneInfo(const std::string& name) {
    std::unordered_map<std::string, std::string> info;
    
    if (!isValidFileName(name)) {
        return info;
    }
    
    std::string filePath = getSceneFilePath(name);
    if (!fileExists(filePath)) {
        return info;
    }
    
    // 获取文件大小
    size_t fileSize = fs::file_size(filePath);
    
    // 获取文件修改时间
    time_t modTime = getFileModificationTime(filePath);
    std::string modTimeStr = std::ctime(&modTime);
    if (!modTimeStr.empty() && modTimeStr.back() == '\n') {
        modTimeStr.pop_back();
    }
    
    // 获取文件内容（用于预览）
    std::string content = readFile(filePath);
    std::string preview = content.substr(0, 100) + (content.size() > 100 ? "..." : "");
    
    info["name"] = name;
    info["size"] = std::to_string(fileSize);
    info["modification_time"] = modTimeStr;
    info["preview"] = preview;
    
    return info;
}

std::string SceneTemplateManager::previewScene(const std::string& name) {
    std::string content = loadScene(name);
    if (content.empty()) {
        return "";
    }
    
    // 返回前1000个字符作为预览
    return content.substr(0, 1000) + (content.size() > 1000 ? "..." : "");
}

bool SceneTemplateManager::renameScene(const std::string& oldName, const std::string& newName) {
    if (!isValidFileName(oldName) || !isValidFileName(newName)) {
        return false;
    }
    
    std::string oldPath = getSceneFilePath(oldName);
    std::string newPath = getSceneFilePath(newName);
    
    if (!fileExists(oldPath)) {
        return false;
    }
    
    try {
        fs::rename(oldPath, newPath);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SceneTemplateManager::copyScene(const std::string& sourceName, const std::string& targetName) {
    if (!isValidFileName(sourceName) || !isValidFileName(targetName)) {
        return false;
    }
    
    std::string sourcePath = getSceneFilePath(sourceName);
    std::string targetPath = getSceneFilePath(targetName);
    
    if (!fileExists(sourcePath)) {
        return false;
    }
    
    try {
        fs::copy(sourcePath, targetPath, fs::copy_options::overwrite_existing);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SceneTemplateManager::dirExists(const std::string& path) const {
    return fs::exists(path) && fs::is_directory(path);
}

bool SceneTemplateManager::createDir(const std::string& path) const {
    try {
        return fs::create_directories(path);
    } catch (const std::exception&) {
        return false;
    }
}

std::string SceneTemplateManager::getSceneFilePath(const std::string& name) const {
    std::string fileName = name;
    
    // 确保文件名以.json结尾
    const std::string extension = ".json";
    if (fileName.size() < extension.size() ||
        fileName.compare(fileName.size() - extension.size(), extension.size(), extension) != 0) {
        fileName += ".json";
    }
    
    return templateDir_ + "/" + fileName;
}

bool SceneTemplateManager::isValidFileName(const std::string& name) const {
    if (name.empty()) {
        return false;
    }
    
    // 检查文件名是否包含非法字符
    const std::string illegalChars = "<>:/\\|?*\"'";
    if (name.find_first_of(illegalChars) != std::string::npos) {
        return false;
    }
    
    // 检查文件名长度是否合法
    if (name.size() > 255) {
        return false;
    }
    
    // 检查文件名是否为特殊文件名
    if (name == "." || name == "..") {
        return false;
    }
    
    return true;
}

std::string SceneTemplateManager::readFile(const std::string& path) const {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool SceneTemplateManager::writeFile(const std::string& path, const std::string& content) const {
    std::ofstream file(path, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    file << content;
    return file.good();
}

bool SceneTemplateManager::deleteFile(const std::string& path) const {
    if (!fileExists(path)) {
        return false;
    }
    
    try {
        return fs::remove(path);
    } catch (const std::exception&) {
        return false;
    }
}

bool SceneTemplateManager::fileExists(const std::string& path) const {
    return fs::exists(path) && fs::is_regular_file(path);
}

std::vector<std::string> SceneTemplateManager::listFiles(const std::string& path, const std::string& extension) const {
    std::vector<std::string> files;
    
    if (!dirExists(path)) {
        return files;
    }
    
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                std::string fileName = entry.path().filename().string();
                if (fileName.size() >= extension.size() &&
                    fileName.compare(fileName.size() - extension.size(), extension.size(), extension) == 0) {
                    // 移除文件扩展名
                    if (extension.size() > 0 && fileName.size() > extension.size()) {
                        fileName = fileName.substr(0, fileName.size() - extension.size());
                    }
                    files.push_back(fileName);
                }
            }
        }
    } catch (const std::exception&) {
        // 忽略异常，返回空列表
    }
    
    return files;
}

time_t SceneTemplateManager::getFileModificationTime(const std::string& path) const {
    if (!fileExists(path)) {
        return 0;
    }
    
    fs::file_time_type ftime = fs::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    
    return std::chrono::system_clock::to_time_t(sctp);
}
