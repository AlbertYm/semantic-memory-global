# Stage13 跨机器人工验收清单

1. 将 transfer kit 复制到另一台 Windows 机器。
2. 先运行 verify-only 脚本，核对 SHA256、SBOM、license、签名状态和 installer 内容。
3. 若要真实安装，手动指定一次性安装目录，并保留脚本输出。
4. 将验收 JSON 带回本项目后，才能判断真实第二台机器是否通过。

当前本机交付不得声称已经完成真实跨机器验收。
