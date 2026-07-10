# 功能与评分对应表

本表用于说明代码版本与实验评分扩展项的对应关系，最终得分以教师验收为准。

## 基础要求

- 数据备份：`backup`
- 数据还原：`restore`
- 基础分对应：40 分

## 扩展功能

| 扩展项 | 程序功能 | 对应命令/文件 | 参考分值 |
| --- | --- | --- | --- |
| 自定义备份-路径 | 按路径关键词筛选 | `--path-contains=docs` | 3 |
| 自定义备份-类型 | 按扩展名筛选 | `--ext=.txt,.cpp` | 3 |
| 自定义备份-名字 | 按文件名关键词筛选 | `--name-contains=report` | 3 |
| 自定义备份-时间 | 按修改时间筛选 | `--modified-after` / `--modified-before` | 3 |
| 自定义备份-尺寸 | 按最大文件大小筛选 | `--max-size=1048576` | 3 |
| 打包/解包 | 单文件 `.sba` 归档 | `pack` / `unpack` | 10 |
| 压缩/解压 | RLE 压缩算法 | `pack --compress=rle` / `unpack` | 10 |
| 加密/解密 | OpenSSL EVP AES-256-GCM，PBKDF2-HMAC-SHA256 密钥派生 | `pack --password=...` / `unpack --password=...` | 10 |
| 元数据支持 | 恢复文件修改时间 | `restore` / `unpack` 自动执行 | 10 |
| 图形界面 | 原生 C++ Win32 操作界面，PowerShell GUI 备用 | `bin/sbm_gui.exe` / `gui.ps1` | 10 |
| 定时备份 | 周期性生成快照并淘汰旧快照 | `schedule --keep=N` | 10 |

## 难度分估算

基础分 40，加上上述扩展项后，理论难度分可超过 110。根据课程规则，项目难度分上限为 110。

## 演示建议

1. 使用 `backup` 完成普通备份。
2. 使用 `verify` 校验备份。
3. 使用 `pack --compress=rle --password=secret123` 生成压缩加密归档。
4. 使用错误密码演示解包失败。
5. 使用正确密码 `unpack`，再 `restore`。
6. 使用 `schedule` 生成两个快照目录。
7. 打开 `bin/sbm_gui.exe` 展示原生 C++ 图形界面，必要时使用 `gui.ps1` 作为备用界面。

也可以直接运行 `demo.ps1` 自动完成上述主要演示流程。
