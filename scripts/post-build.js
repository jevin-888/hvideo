/**
 * 构建后处理脚本
 * 执行构建完成后的清理和优化工作
 */

const fs = require('fs');
const path = require('path');

// 配置
const config = {
  distDir: path.resolve(__dirname, '../dist'),
  manifestFile: 'manifest.json',
  statsFile: 'build-stats.json'
};

/**
 * 生成构建清单文件
 */
function generateManifest() {
  console.log('生成构建清单...');
  
  const manifest = {
    version: '2.0.0',
    buildTime: new Date().toISOString(),
    files: {}
  };
  
  // 读取dist目录中的文件
  if (fs.existsSync(config.distDir)) {
    const files = fs.readdirSync(config.distDir);
    
    files.forEach(file => {
      const filePath = path.join(config.distDir, file);
      const stat = fs.statSync(filePath);
      
      if (stat.isFile()) {
        manifest.files[file] = {
          size: stat.size,
          modified: stat.mtime.toISOString()
        };
      }
    });
  }
  
  // 写入清单文件
  const manifestPath = path.join(config.distDir, config.manifestFile);
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  
  console.log(`清单文件已生成: ${manifestPath}`);
}

/**
 * 生成构建统计信息
 */
function generateStats() {
  console.log('生成构建统计信息...');
  
  const stats = {
    timestamp: Date.now(),
    totalFiles: 0,
    totalSize: 0,
    fileTypes: {},
    chunks: {}
  };
  
  if (fs.existsSync(config.distDir)) {
    const files = fs.readdirSync(config.distDir);
    stats.totalFiles = files.length;
    
    files.forEach(file => {
      const filePath = path.join(config.distDir, file);
      const stat = fs.statSync(filePath);
      
      if (stat.isFile()) {
        const ext = path.extname(file).toLowerCase();
        stats.totalSize += stat.size;
        
        // 统计文件类型
        if (!stats.fileTypes[ext]) {
          stats.fileTypes[ext] = { count: 0, size: 0 };
        }
        stats.fileTypes[ext].count++;
        stats.fileTypes[ext].size += stat.size;
        
        // 统计chunks
        if (file.includes('.chunk.')) {
          const chunkName = file.split('.')[0];
          if (!stats.chunks[chunkName]) {
            stats.chunks[chunkName] = [];
          }
          stats.chunks[chunkName].push({
            name: file,
            size: stat.size
          });
        }
      }
    });
  }
  
  // 格式化大小
  stats.totalSizeFormatted = formatBytes(stats.totalSize);
  Object.keys(stats.fileTypes).forEach(ext => {
    stats.fileTypes[ext].sizeFormatted = formatBytes(stats.fileTypes[ext].size);
  });
  
  // 写入统计文件
  const statsPath = path.join(config.distDir, config.statsFile);
  fs.writeFileSync(statsPath, JSON.stringify(stats, null, 2));
  
  console.log(`统计文件已生成: ${statsPath}`);
  console.log(`总文件数: ${stats.totalFiles}`);
  console.log(`总大小: ${stats.totalSizeFormatted}`);
}

/**
 * 格式化字节大小
 */
function formatBytes(bytes) {
  if (bytes === 0) return '0 Bytes';
  
  const k = 1024;
  const sizes = ['Bytes', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

/**
 * 清理临时文件
 */
function cleanTempFiles() {
  console.log('清理临时文件...');
  
  const tempFiles = [
    '.DS_Store',
    'Thumbs.db',
    '*.tmp',
    '*.log'
  ];
  
  if (fs.existsSync(config.distDir)) {
    const files = fs.readdirSync(config.distDir);
    
    files.forEach(file => {
      if (tempFiles.some(pattern => {
        if (pattern.startsWith('*')) {
          return file.endsWith(pattern.slice(1));
        }
        return file === pattern;
      })) {
        const filePath = path.join(config.distDir, file);
        fs.unlinkSync(filePath);
        console.log(`已删除临时文件: ${file}`);
      }
    });
  }
}

/**
 * 验证构建结果
 */
function validateBuild() {
  console.log('验证构建结果...');
  
  const requiredFiles = [
    'ktv.html',
    'vod.html',
    'ktv-main.js',
    'vod-main.js',
    'shared.js'
  ];
  
  const missingFiles = [];
  
  requiredFiles.forEach(file => {
    const filePath = path.join(config.distDir, file);
    if (!fs.existsSync(filePath)) {
      missingFiles.push(file);
    }
  });
  
  if (missingFiles.length > 0) {
    console.error('❌ 构建验证失败，缺少以下文件:');
    missingFiles.forEach(file => console.error(`  - ${file}`));
    process.exit(1);
  }
  
  console.log('✅ 构建验证通过');
}

/**
 * 生成部署配置
 */
function generateDeployConfig() {
  console.log('生成部署配置...');
  
  const deployConfig = {
    version: '2.0.0',
    buildTime: new Date().toISOString(),
    deployPath: '/var/www/huoshan',
    backup: true,
    restartServices: ['nginx'],
    postDeploy: [
      'chmod -R 755 /var/www/huoshan',
      'chown -R www-data:www-data /var/www/huoshan'
    ]
  };
  
  const configPath = path.join(config.distDir, 'deploy-config.json');
  fs.writeFileSync(configPath, JSON.stringify(deployConfig, null, 2));
  
  console.log(`部署配置已生成: ${configPath}`);
}

/**
 * 主函数
 */
function main() {
  console.log('开始构建后处理...');
  
  try {
    // 生成清单文件
    generateManifest();
    
    // 生成统计信息
    generateStats();
    
    // 清理临时文件
    cleanTempFiles();
    
    // 验证构建结果
    validateBuild();
    
    // 生成部署配置
    generateDeployConfig();
    
    console.log('✅ 构建后处理完成');
  } catch (error) {
    console.error('❌ 构建后处理失败:', error);
    process.exit(1);
  }
}

// 执行主函数
if (require.main === module) {
  main();
}

module.exports = {
  generateManifest,
  generateStats,
  cleanTempFiles,
  validateBuild,
  generateDeployConfig
};