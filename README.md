# gmod_nx — Garry's Mod on Nintendo Switch homebrew

在 Switch 上原生运行 Garry's Mod 的加载器工程。**不重写引擎**：直接复用
[tinsarfal/GDroid](https://github.com/tinsarfal/GDroid)（nillerusr Android Source 引擎 + GMod ARM64 模块）
编译好的 AArch64 二进制，套 [NaGaa95/hl2_nx](https://github.com/NaGaa95/hl2_nx)（MIT）的
fake-Android so-loader 在 Horizon 上跑起来。

```
gdroid_nx/
├── Makefile                  devkitA64 交叉编译（已改好 GMod 目标）
├── source/
│   ├── main.c                ★ 新写：32 模块加载 + GMod 环境 + argv 启动
│   ├── android_stubs.c       ★ 新写：Vulkan/AAudio/MediaNDK/AAsset/OpenSL stub
│   ├── posix_shim.c          ★ 新写：bionic 的 _l / _FORTIFY 入口
│   ├── extra_imports.inc     ★ 生成：530 条导入符号表项
│   ├── imports.c             复用 hl2_nx（425 条表项）+ include 上面的增量
│   ├── so_util.c/h           复用 hl2_nx（ELF 加载/重定位，MIT）
│   ├── libc_shim.c/h         复用 hl2_nx（newlib ↔ bionic 兼容层）
│   ├── dl_emu.c/h            复用 hl2_nx（dlopen/dlsym 模拟）
│   ├── jni_fake.c/h          复用 hl2_nx（fake JNI 环境）
│   ├── config.c/h            复用 hl2_nx，改 SO_MEMORY_MB / 安装根目录
│   ├── controller.c / gyro.c 复用 hl2_nx（手柄 + 陀螺仪）
│   └── ...
├── tools/prepare.ps1         ★ 从 APK 生 SD 卡布局（已实测跑通）
└── stage/                    生成的部署目录（lib/*.so + config.txt）
```

## 状态

| 项 | 状态 | 证据 |
|---|---|---|
| GDroid APK 拆包 | ✅ 完成 | 35 个 `lib/arm64-v8a/*.so`，全部合法 ELF64 AArch64，SHA256 与 release 一致 |
| ELF / 依赖 / 符号表解析 | ✅ 完成 | `elfinfo.json`、`syms.json`、`missing_syms.txt` |
| 外部符号缺口计算 | ✅ 完成 | 840 个外部符号 → hl2_nx 覆盖 310 → 缺口 530，已全部落表 |
| 模块加载顺序 | ✅ 完成 | `main.c` module_names[]，32 项，按 .init_array 依赖排 |
| 渲染后端选型 | ✅ 完成 | shaderapidx9 → togl → EGL/GLES；Vulkan 后端不加载 |
| Steam 认证 | ✅ 已规避 | `libsteam_api.so` 仅 8,904 B，是 GDroid 自带的假实现（只导入 3 个 C++ 运行时符号），无需处理 |
| so 提取脚本 | ✅ 实测跑通 | 32/32 成功，见 `stage/verify.txt` |
| 源码编译 | ⬜ 待做 | 需要 devkitPro（见下） |
| 真机启动 | ⬜ 待做 | 需要编译产物 + Switch |

**没有在这里编译过**，因为这台机器上没有 devkitPro。所有 C 代码按 hl2_nx 的接口写的，
但第一次 `make` 大概率还要修几处链接错误 —— 见下面「已知会撞的坑」。

## 从零到能跑

### 1. 装工具链

```bash
# Linux / WSL / MSYS2 任一
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x install-devkitpro-pacman && sudo ./install-devkitpro-pacman
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_image switch-mesa \
                  switch-libdrm_nouveau switch-zlib switch-ffmpeg
export DEVKITPRO=/opt/devkitpro
```

### 2. 编译

```bash
cd gmod_nx
make -j$(nproc)
# 产物：gmod_nx.nro（+ gmod_nx.nacp / gmod_nx.elf）
```

### 3. 生成 SD 卡布局

```powershell
.\tools\prepare.ps1 -Apk .\GDroid-0.1.31.apk -Out D:\switch\gmod_nx -Nro .\gmod_nx.nro
```

产出：

```
D:\switch\gmod_nx\
├── gmod_nx.nro
├── config.txt
├── lib\                    32 个 ARM64 模块
│   ├── liblauncher.so      入口（导出 LauncherMain 与 LauncherMainAndroid）
│   ├── libclient.so        GMod 游戏层 16.4 MB
│   ├── libserver.so        GMod 服务端 16.7 MB
│   ├── liblua_shared.so    Lua 运行时
│   ├── libengine.so        nillerusr 引擎
│   ├── libphonon.so        Phonon 音频
│   └── ...
├── garrysmod\              ← 自己放
├── sourceengine\           ← 自己放
└── platform\               ← 自己放
```

### 4. 放游戏数据

从 Steam 的 Garry's Mod 目录复制三个文件夹到 `D:\switch\gmod_nx\`：

```
garrysmod/      garrysmod_dir.vpk + 编号 VPK 分片 + gameinfo.txt + lua/ + maps/ ...
sourceengine/   hl2_misc_dir.vpk、hl2_textures_dir.vpk 等共享内容
platform/       platform_*_dir.vpk
```

- **不要**复制 PC 的 `bin/`（里面有 x86 的 `client.so`，会和 ARM64 的打架）
- 编号分片 VPK 必须和 `*_dir.vpk` 一起拷，只拷 dir 不够
- `/sdcard/srceng/` 里若已有完整的 HL2 包，也可以复用到 `sourceengine/`

### 5. 启动

`.nro` 不能用相册进（applet 模式只有 ~400 MB 堆），必须 title override：

1. 桌面按住 **R**
2. 点任意已装游戏
3. 进 hbmenu 后松手
4. 点 `gmod_nx.nro`

要图标就打成 NSP：

```
Nro2Nsp 3.3.5 / 3.3.6
  Mode        Internal
  NRO Path    /switch/gmod_nx/gmod_nx.nro
  Title Name  Garry's Mod
  Title ID    0100A1B2C3D4F000     （Makefile 里 APP_TITLEID 同值）
  Version     0.1.0
```

## 架构决策记录

### 为什么不用 GDroid 的 libSDL2.so

GDroid 的 `libSDL2.so` 是 Android 版，`NEEDED libOpenSLES.so + libandroid.so + libGLESv1_CM.so`。
hl2_nx 的 `imports.c` 里已经有 **83 个 SDL2 符号** 直接绑到 switch-sdl2，桌面/音频/事件全走原生。
所以 `main.c` 的模块表里剔掉了 `libSDL2.so`，只靠符号表满足 `libengine.so` / `liblauncher.so` 的
`NEEDED`。**这是第一个可能翻车的地方**：GDroid 用到的 SDL 符号可能比 83 个多，缺哪个会
在 `debug.log` 里留 `unresolved import: SDL_xxx`。

### 为什么砍 Vulkan

`libshaderapivulkan.so` 有 **91 MB**，`NEEDED libvulkan.so`，而 Horizon 上没有 Vulkan 驱动。
它不被任何模块 `NEEDED`（是运行时按 `-shaderapi` 参数 dlopen 的），所以直接不加载。
`main.c` 显式传 `-shaderapi shaderapidx9 -dxlevel 90`，走 hl2_nx 已经验证过的
`shaderapidx9 → togl → dlopen("libEGL.so"/"libGLESv3.so") → switch-mesa` 那条路。

`libvrxr.so` 是硬 NEEDED（6 个模块依赖），所以还是加载，但它要的 110 个 `vk*` 符号
全部指向 `gmod_vk_fail()`（返回 `VK_ERROR_INITIALIZATION_FAILED`），VR 探针会失败并关闭，
不影响引擎其余部分。

### Steam 为什么不用管

`libsteam_api.so` 8,904 字节，导入只有 `__cxa_finalize / __cxa_atexit / __register_atfork`
三个 C++ 运行时符号，导出 45 个 `SteamAPI_*` 全空实现。GDroid 已经把它做成了 stub，
Switch 侧原样加载即可。

## 已知会撞的坑

按可能性排序，第一次 `make` / 第一次启动要逐个过：

| # | 症状 | 原因 | 修法 |
|---|---|---|---|
| 1 | 链接报 `undefined reference to glBindFramebufferOES` 等 | switch-mesa 没有 OES 别名 | `extra_imports.inc` 里已把 `*OES` 映射到 core 同名函数；剩下缺的手工补 |
| 2 | `mfence` / `__aarch64_*` 之类的链接错 | GDroid 用新版 NDK 编的，hl2_nx 的 libc_shim 没覆盖 | 在 `posix_shim.c` 里补 |
| 3 | 启动卡在 `Could not load lib/xxx.so (so_load: -N)` | `SO_MEMORY_MB` 不够，或某个 LOAD 段超限 | 把 `config.h` 的 192 调大（上限受 application 池约束） |
| 4 | `debug.log` 里大量 `unresolved import:` | 符号表没覆盖到 | 看缺哪些，加进 `extra_imports.inc` |
| 5 | 黑屏但进程活着 | togl 没拿到 EGL 伪模块 | 确认 `switch-mesa` + `switch-libdrm_nouveau` 已装，`--wrap=eglGetProcAddress` 生效 |
| 6 | 进主菜单但字体全方块 | `garrysmod` 的字体走 AAsset，stub 返回 NULL | `android_stubs.c` 的 AAsset 是**真实现**（stdio 后端）；确认 cwd 已 chdir 到 install_root |
| 7 | 内存耗尽崩在加载 addon | GMod 比 HL2 吃内存 | sys-clk 超频 + 减少 addon |
| 8 | 手柄没反应 | GMod 的 input 走自己的 system，不是 SDL | `controller.c` 的 hook 点要按 `libinputsystem.so` 重新对准 |

## 技术档案（分析产物）

| 文件 | 内容 |
|---|---|
| `elfinfo.json` | 35 个 so 的 arch/type/SONAME/NEEDED/关键符号探针 |
| `syms.json` | 每个 so 的 UNDEF 与 export 全量符号表 |
| `missing_syms.txt` | 530 个 Switch 侧必须提供的符号 |
| `list_gl.txt` (134) | GL/EGL 符号 → switch-mesa |
| `list_android.txt` (120) | Android 平台符号 → `android_stubs.c` |
| `stage/verify.txt` | 32 个 so 的提取结果与 SHA256 |

关键数据：

```
GDroid 0.1.31 ARM64 APK   85,270,687 B
  sha256                  c98c7128102bbd7c3ec8e1cd9b7103d1da3a3ff506c50ea0bae5111549a5d71d
arm64-v8a .so             35 个，177,740,840 B
  实际加载                32 个
  跳过                    3 个（SDL2 / Vulkan / zstd-jni）
外部符号                  840
  hl2_nx 已覆盖           310
  缺口                    530 = 134 GL + 110 Vulkan + 120 Android + 166 libc
```

## 许可

`source/` 下从 hl2_nx 复用的部分（`so_util.*`、`libc_shim.*`、`dl_emu.*`、`jni_fake.*`、
`imports.c` 原有表项）是 MIT，(C) 2021 fgsfds / Andy Nguyen / NaGaa95。
新写的 `main.c`、`android_stubs.c`、`posix_shim.c`、`extra_imports.inc`、`tools/` 同样 MIT。

不包含任何游戏资源，也不包含 GDroid 的二进制 —— 它们由 `tools/prepare.ps1` 从用户自己的
APK 里提取，需要自备 Steam 版 Garry's Mod。
