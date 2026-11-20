# V60 公开发布清单

这里保存工具原有内嵌 V60 包的六个公开文件，供 `scripts/prepare-signing.py` 构建使用，避免依赖已经清理的外部历史发布目录。文件从原 `signing-runtime.zip` 提取，整体包 SHA256、各文件 SHA256 均与原 `signing-runtime.inputs.json` 一致。

这里只包含清单和公开签名，不包含 ROM 镜像或 ROM 私钥。基础及 hw81/hw82 清单仅适用各自声明的同一个 V60 镜像；运行时仍按镜像大小、SHA256、RSA 签名及完整字段校验，不能用于其他版本。
