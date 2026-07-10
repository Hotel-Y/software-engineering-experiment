# Simple Backup Manager

这是软件开发综合实验的数据备份软件高分版，提供命令行备份、还原、校验、自定义筛选、打包/解包、压缩/解压、OpenSSL AES-256-GCM 加密/解密、定时备份和图形界面。

## 构建

首次构建先安装项目内 OpenSSL 开发依赖：

```powershell
.\setup_openssl.ps1
```

```powershell
.\build.ps1
```

构建原生 C++ 图形界面：

```powershell
.\build_gui.ps1
```

生成程序：

```text
bin\sbm.exe
bin\sbm_gui.exe
```

## 使用

备份目录：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --overwrite
```

按扩展名筛选备份：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --ext=.txt,.cpp --overwrite
```

按文件大小筛选备份：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --max-size=1048576 --overwrite
```

按文件名关键词筛选备份：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --name-contains=report --overwrite
```

按路径关键词筛选备份：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --path-contains=docs --overwrite
```

按修改时间筛选备份：

```powershell
.\bin\sbm.exe backup .\example_source .\example_backup --modified-after=2024-01-01 --overwrite
```

校验备份：

```powershell
.\bin\sbm.exe verify .\example_backup
```

打包备份目录：

```powershell
.\bin\sbm.exe pack .\example_backup .\example_backup.sba
```

压缩并使用 OpenSSL AES-256-GCM 加密打包：

```powershell
.\bin\sbm.exe pack .\example_backup .\secure_backup.sba --compress=rle --password=secret123
```

解包归档文件：

```powershell
.\bin\sbm.exe unpack .\secure_backup.sba .\example_backup_from_archive --password=secret123
```

定时备份，下面命令每 60 秒生成一次快照，共生成 3 次：

```powershell
.\bin\sbm.exe schedule .\example_source .\snapshots 60 3 --ext=.txt
```

定时备份并只保留最近 2 个快照：

```powershell
.\bin\sbm.exe schedule .\example_source .\snapshots 60 3 --keep=2 --ext=.txt
```

启动原生 C++ 图形界面：

```powershell
.\bin\sbm_gui.exe
```

启动 PowerShell 备用图形界面：

```powershell
.\gui.ps1
```

一键演示全部高分功能：

```powershell
.\demo.ps1
```

还原备份：

```powershell
.\bin\sbm.exe restore .\example_backup .\example_restore
```

## 测试

```powershell
.\test.ps1
```

## 当前功能

- 递归备份普通文件和目录结构。
- 根据 `manifest.sbm` 记录相对路径、文件大小、修改时间和 FNV-1a 校验值。
- 根据清单还原文件。
- 校验备份目录完整性。
- 支持将备份目录打包成单个 `.sba` 归档文件。
- 支持将 `.sba` 归档文件解包为可校验、可还原的备份目录。
- 支持按扩展名筛选。
- 支持按最大文件大小筛选。
- 支持按文件名关键词筛选。
- 支持按路径关键词筛选。
- 支持按修改时间筛选。
- 支持备份前覆盖清理。
- 防止备份目录位于源目录内部。
- 输出目录数、文件数、跳过数、失败数和字节数。
- 支持 RLE 压缩/解压。
- 使用 OpenSSL EVP 库函数实现 AES-256-GCM 加密/解密。
- 使用 PBKDF2-HMAC-SHA256 和随机 salt 派生密钥。
- 使用随机 IV 和 GCM 认证 tag 检测错误密码及数据篡改。
- 支持恢复文件修改时间元数据。
- 支持周期性定时备份快照。
- 支持定时备份快照淘汰策略。
- 提供原生 C++ Win32 图形界面。
- 提供 Windows Forms PowerShell 备用图形界面脚本。

## 项目文档

- 代码设计说明：`docs/code_design.md`
- 功能与评分对应表：`docs/scoring_features.md`
