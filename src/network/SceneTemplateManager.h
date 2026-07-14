#ifndef SCENETEMPLATEMANAGER_H
#define SCENETEMPLATEMANAGER_H

#include <string>
#include <vector>
#include <unordered_map>
#include "core/PathConfig.h"

class SceneTemplateManager {
public:
    SceneTemplateManager(const std::string& templateDir = hsvj::SCENE_DIR);
    ~SceneTemplateManager();

    // 设置场景模板目录
    void setTemplateDir(const std::string& dir);
    // 获取场景模板目录
    const std::string& getTemplateDir() const;
    
    // 保存场景模板
    bool saveScene(const std::string& name, const std::string& content);
    // 加载场景模板
    std::string loadScene(const std::string& name);
    // 删除场景模板
    bool deleteScene(const std::string& name);
    // 获取场景模板列表
    std::vector<std::string> listScenes();
    // 检查场景模板是否存在
    bool sceneExists(const std::string& name);
    // 获取场景模板信息
    std::unordered_map<std::string, std::string> getSceneInfo(const std::string& name);
    // 预览场景模板
    std::string previewScene(const std::string& name);
    // 重命名场景模板
    bool renameScene(const std::string& oldName, const std::string& newName);
    // 复制场景模板
    bool copyScene(const std::string& sourceName, const std::string& targetName);

private:
    // 检查目录是否存在
    bool dirExists(const std::string& path) const;
    // 检查文件是否存在
    bool fileExists(const std::string& path) const;
    // 创建目录
    bool createDir(const std::string& path) const;
    // 获取场景模板文件路径
    std::string getSceneFilePath(const std::string& name) const;
    // 检查文件名是否合法
    bool isValidFileName(const std::string& name) const;
    // 读取文件内容
    std::string readFile(const std::string& path) const;
    // 写入文件内容
    bool writeFile(const std::string& path, const std::string& content) const;
    // 删除文件
    bool deleteFile(const std::string& path) const;
    // 列出目录中的文件
    std::vector<std::string> listFiles(const std::string& path, const std::string& extension = ".json") const;
    // 获取文件修改时间
    time_t getFileModificationTime(const std::string& path) const;

    std::string templateDir_;  // 场景模板目录
};

#endif // 结束 SCENETEMPLATEMANAGER_H