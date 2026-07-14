-- 云端授权系统数据库初始化脚本

-- 创建数据库（如果不存在）
CREATE DATABASE IF NOT EXISTS license_system
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

-- 使用数据库
USE license_system;

-- 创建用户表
CREATE TABLE IF NOT EXISTS `user` (
  `id` varchar(64) NOT NULL,
  `username` varchar(64) NOT NULL,
  `password_hash` varchar(255) NOT NULL,
  `name` varchar(128) DEFAULT NULL,
  `contact` varchar(64) DEFAULT NULL,
  `role` varchar(32) NOT NULL DEFAULT 'agent',
  `status` varchar(32) NOT NULL DEFAULT 'active',
  `created_at` bigint NOT NULL,
  `updated_at` bigint NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_username` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 创建设备表
CREATE TABLE IF NOT EXISTS `device` (
  `id` varchar(64) NOT NULL,
  `serial` varchar(128) NOT NULL,
  `fingerprint` varchar(256) NOT NULL,
  `model` varchar(64) DEFAULT NULL,
  `source` varchar(32) NOT NULL DEFAULT 'manual',
  `agent_id` varchar(64) DEFAULT NULL,
  `bound_at` bigint DEFAULT NULL,
  `remark` varchar(256) DEFAULT NULL,
  `created_at` bigint NOT NULL,
  `updated_at` bigint NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_fingerprint` (`fingerprint`),
  KEY `idx_serial` (`serial`),
  KEY `idx_agent_id` (`agent_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 创建设备分配日志表（可选）
CREATE TABLE IF NOT EXISTS `device_assign_log` (
  `id` varchar(64) NOT NULL,
  `device_id` varchar(64) NOT NULL,
  `agent_id` varchar(64) NOT NULL,
  `assigned_at` bigint NOT NULL,
  `assigned_by` varchar(64) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_device_id` (`device_id`),
  KEY `idx_agent_id` (`agent_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 插入默认管理员账户（密码：admin123，已哈希）
-- 注意：实际部署时请更改默认密码
INSERT IGNORE INTO `user` (
  `id`, 
  `username`, 
  `password_hash`, 
  `name`, 
  `contact`, 
  `role`, 
  `status`, 
  `created_at`, 
  `updated_at`
) VALUES (
  'admin_001',
  'admin',
  '$2b$12$L3Px.7WQ4U0h4rCx.QPyE.4K6Y.E.Y.yFz0N5D1OqZ6z7Y6J2VXQ.',  -- bcrypt hash of 'admin123'
  '系统管理员',
  'admin@company.com',
  'admin',
  'active',
  UNIX_TIMESTAMP(),
  UNIX_TIMESTAMP()
);

-- 创建专用数据库用户（需要root权限执行）
-- 注意：在生产环境中，请确保使用强密码
-- CREATE USER IF NOT EXISTS 'license_app'@'%' IDENTIFIED BY 'your_secure_password_here';
-- GRANT ALL PRIVILEGES ON license_system.* TO 'license_app'@'%';
-- FLUSH PRIVILEGES;