# Nautilus Video Duration (FFmpeg Helper) —— "时长"列扩展

一个用于 **GNOME Files (Nautilus 4 / libnautilus-extension-4)** 的扩展，  
在列表视图中新增一列 **"时长"**，显示本地视频/音频文件的播放时长。

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

## 系统要求

- Nautilus 4（`libnautilus-extension-4`）
- FFmpeg 开发库（`libavformat`, `libavutil`）

### 支持的发行版（按上游分类）

#### Debian 家族

- **Debian** 13+ (Trixie)
- **Ubuntu** 24.04 LTS+
- **Linux Mint** 22+
- **Elementary OS** 8.0+

#### Red Hat 家族

- **Fedora** 40+
- **RHEL** 9+
- **CentOS Stream** 9+

#### SUSE 家族

- **openSUSE** Leap 15.5+ / Tumbleweed

#### 独立发行版

- **Arch Linux**（滚动更新）

> 说明：需要 Nautilus 4 的发行版版本。较旧版本（如 Ubuntu 22.04）默认
> 仍是 Nautilus 3，需升级到支持 Nautilus 4 的系统版本。

---

## 依赖

### 核心依赖（pkg-config）

- `libnautilus-extension-4`
- `glib-2.0`, `gio-2.0`, `gmodule-2.0`
- `libavformat`, `libavutil`
- `cmake >= 3.10`, `ninja` 或 `make`

### 按上游发行版的安装说明

#### Debian 家族

**Debian 13+ / Ubuntu 24.04 LTS+ / Linux Mint 22+**

```bash
sudo apt update
sudo apt install -y \
  nautilus libnautilus-extension-dev pkg-config build-essential \
  cmake ninja-build libglib2.0-dev libavformat-dev libavutil-dev
```

#### Red Hat 家族

**Fedora 40+**

```bash
sudo dnf install -y \
  nautilus-devel pkgconf-pkg-config gcc cmake ninja-build \
  glib2-devel ffmpeg-devel
```

**RHEL 9+ / CentOS Stream 9+**

```bash
sudo dnf install -y epel-release
sudo dnf install -y \
  nautilus-devel pkgconf-pkg-config gcc cmake ninja-build \
  glib2-devel ffmpeg-devel
```

#### SUSE 家族

**openSUSE Leap 15.5+ / Tumbleweed**

```bash
sudo zypper install -y \
  nautilus-devel pkg-config gcc cmake ninja \
  glib2-devel ffmpeg-devel
```

#### 独立发行版

**Arch Linux**

```bash
sudo pacman -S --needed \
  nautilus pkgconf gcc cmake ninja glib2 ffmpeg
```

---

## 构建与安装

```bash
cmake -S . -B build -G Ninja
cmake --build build -j$(nproc)
sudo cmake --install build
nautilus -q
```

### 自定义安装路径

如需安装到自定义目录（而非默认的 `/usr/lib/nautilus/extensions-4`）：

```bash
cmake -S . -B build -G Ninja \
  -DNAUTILUS_EXT_DIR=/usr/lib/nautilus/extensions-4
cmake --build build -j$(nproc)
sudo cmake --install build
```

### 使用 Makefiles（可选）

```bash
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j$(nproc)
sudo cmake --install build
```

---

## 使用

启用 "时长" 列：

1. 打开 Nautilus 文件管理器
2. 切换到列表视图（若不是默认视图）
3. 右键点击列标题（如 "名称"、"类型"）
4. 勾选 "时长"

---

## 验证安装

检查扩展是否加载成功：

```bash
ls -la /usr/lib/nautilus/extensions-4/ | grep nautilus-video-duration
```

输出应包含：

```
nautilus-video-duration.so
vd-ffmpeg-helper
```

---

## 可选配置（环境变量）

在启动 Nautilus 前设置：

```bash
export NAUTILUS_VD_THREADS=4
export NAUTILUS_VD_HELPER_MAX=3
export NAUTILUS_VD_FF_TIMEOUT_MS=3000
nautilus
```

| 环境变量                          | 说明                 | 默认值     |
| --------------------------------- | -------------------- | ---------- |
| `NAUTILUS_VD_THREADS`             | 工作线程数           | CPU 核心数 |
| `NAUTILUS_VD_HELPER_MAX`          | helper 进程并发上限  | 2          |
| `NAUTILUS_VD_FF_TIMEOUT_MS`       | 单次探测超时（毫秒） | 2000       |
| `NAUTILUS_VD_FF_RETRY_TIMEOUT_MS` | 重试探测超时（毫秒） | 12000      |
| `NAUTILUS_VD_CACHE`               | 缓存条目上限         | 512        |
| `NAUTILUS_VD_HELPER`              | 自定义 helper 路径   | 安装目录下 |

---

## 故障排除

### 安装后 Nautilus 中看不到 "时长" 列

1. 确保安装到正确位置：

```bash
test -f /usr/lib/nautilus/extensions-4/nautilus-video-duration.so && \
  echo "✓ 文件存在" || echo "✗ 文件不存在"
```

2. 重启 Nautilus：

```bash
nautilus -q
sleep 2
nautilus &
```

3. 检查 helper 是否可访问：

```bash
find /usr -name "vd-ffmpeg-helper" 2>/dev/null
```

### 构建失败 / 缺少依赖

- "找不到 libnautilus-extension"
  → 重新检查上方针对发行版的依赖安装命令
- "找不到 FFmpeg / libavformat"
  → 安装 `ffmpeg-devel`（Red Hat/SUSE）或 `libavformat-dev`（Debian）
- CMake 报错 "pkg-config not found"
  → 安装 `pkg-config`（Debian/SUSE）或 `pkgconf-pkg-config`（Red Hat）

---

## 卸载

```bash
sudo rm -f /usr/lib/nautilus/extensions-4/nautilus-video-duration.so
sudo rm -f /usr/lib/nautilus/extensions-4/vd-ffmpeg-helper
nautilus -q
```

---

## 开发与贡献

欢迎提交 Issue 或 Pull Request！

---

## 许可证

本项目采用 MIT 许可证。
