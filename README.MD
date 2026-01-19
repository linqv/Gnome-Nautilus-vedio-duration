# Nautilus Video Duration (FFmpeg Helper) —— “时长”列扩展

一个用于 **GNOME Files (Nautilus 4 / libnautilus-extension-4)** 的扩展，  
在列表视图中新增一列 **“时长”**，显示本地视频/音频文件的播放时长。

本项目通过独立的 **FFmpeg helper** 进程获取时长（`vd-ffmpeg-helper`），  
Nautilus 扩展本体不直接链接 FFmpeg，并提供：
- ✅ 线程池并行计算（线程数 = CPU 核心数）
- ✅ Helper 进程池 + 超时控制（避免卡住 Nautilus）
- ✅ 缓存 + LRU 淘汰（默认 512 项）
- ✅ 主线程安全更新（避免 Nautilus 崩溃）
- ✅ 安装到 `/usr/lib/nautilus/extensions-4`

---

## 预览

在 Nautilus 列表视图中显示新增列：

- 列名：**时长**
- 格式：`H:MM:SS`（视频/≥1h 音频）或 `MM:SS`（<1h 音频）
- 失败/未知显示：`-`

---

## 依赖

核心依赖（pkg-config）：
- `libnautilus-extension-4`
- `glib-2.0`, `gio-2.0`, `gmodule-2.0`
- `libavformat`, `libavutil`（FFmpeg）

### Arch Linux（推荐安装）
```bash
sudo pacman -S --needed \
  nautilus pkgconf gcc cmake ninja glib2 ffmpeg
```

---

## 构建与安装

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
sudo cmake --install build
nautilus -q
```

默认安装到 `/usr/lib/nautilus/extensions-4`，也可自定义：
```bash
cmake -S . -B build -G Ninja -DNAUTILUS_EXT_DIR=/usr/lib/nautilus/extensions-4
```

---

## 使用

1. 打开 Nautilus，切换到 **列表视图**。
2. 右键列标题 → 勾选 **“时长”**。

---

## 可选配置（环境变量）

- `NAUTILUS_VD_THREADS`：工作线程数（默认 CPU 核心数）
- `NAUTILUS_VD_HELPER_MAX`：helper 进程并发上限（默认 2）
- `NAUTILUS_VD_FF_TIMEOUT_MS`：单次探测超时（默认 2000ms）
- `NAUTILUS_VD_FF_RETRY_TIMEOUT_MS`：重试探测超时（默认 12000ms）
- `NAUTILUS_VD_CACHE`：缓存条目上限（默认 512）
- `NAUTILUS_VD_HELPER`：自定义 helper 路径（默认安装目录下）
