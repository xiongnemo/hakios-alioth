# 在 Redmi K40 / POCO F3（alioth）上运行 iOS 16

[English](README.md) · 中文

在 Redmi K40 / POCO F3（`alioth`，骁龙 870）上运行原版 iOS 16.5（20F66）用户空间，底层是开源的 XNU 内核，
配上我们自己的引导程序和驱动。这是 [HakiOS](https://github.com/oosexgang/hakios-os) 在这台手机上的移植，
以同一 SoC 家族的小米 10 移植（[hackios-xnu-kernel-umi](https://github.com/JopangJovan/hackios-xnu-kernel-umi)）为基础。

**不刷写任何东西。** 引导程序通过 `fastboot boot` 进入内存，iOS 在内存里运行一个会话，重启后手机回到它自己的
Android，数据原封不动。iOS 把这台手机当作 iPhone X（`iPhone10,3`）。

## 截图

用手机的侧边键 + 音量上截取。网络名称和手机自己的地址已打码。

### 游戏

OpenGL ES 1/2/3 游戏在自己的进程里用 Adreno 650 渲染，每个进程有独立的 GPU 地址空间。

<p>
  <img src="docs/alioth/screenshots/games/minecraft-title.png" width="420" alt="Minecraft PE 1.2.10：标题画面">
  <img src="docs/alioth/screenshots/games/minecraft-world-1.png" width="420" alt="Minecraft：在世界里">
  <img src="docs/alioth/screenshots/games/minecraft-world-2.png" width="420" alt="Minecraft：在世界里">
</p>

Minecraft PE 1.2.10：标题画面，以及在世界里走动，60 帧。

<p>
  <img src="docs/alioth/screenshots/games/terraria-title.png" width="420" alt="Terraria 1.2：标题画面">
  <img src="docs/alioth/screenshots/games/terraria-world-1.png" width="420" alt="Terraria：在世界里">
  <img src="docs/alioth/screenshots/games/terraria-world-2.png" width="420" alt="Terraria：在世界里">
</p>

Terraria 1.2：标题画面，以及在世界里，60 帧。

<p>
  <img src="docs/alioth/screenshots/games/sims3-title.png" width="420" alt="模拟人生 3 梦想大道：标题画面">
  <img src="docs/alioth/screenshots/games/sims3-create-a-sim.png" width="420" alt="模拟人生 3 梦想大道：创建人物">
  <img src="docs/alioth/screenshots/games/sims3-house.png" width="420" alt="模拟人生 3 梦想大道：在家里">
  <img src="docs/alioth/screenshots/games/sims3-house-2.png" width="420" alt="模拟人生 3 梦想大道：在家里">
  <img src="docs/alioth/screenshots/games/sims3-town.png" width="420" alt="模拟人生 3 梦想大道：小镇">
</p>

The Sims 3 Ambitions（模拟人生 3 梦想大道，OpenGL ES 1，PVRTC 纹理由处理器解码）：标题画面、创建人物、
在家里、小镇，62 帧。

### 图形

<p>
  <img src="docs/alioth/screenshots/graphics/gpu-check-summary.png" width="260" alt="GPU Check：总结">
  <img src="docs/alioth/screenshots/graphics/gpu-check-scenekit.png" width="260" alt="GPU Check：SceneKit 和 SpriteKit">
</p>

[GPU Check](https://github.com/xiongnemo/ios-gpu-check) 逐项测试游戏可能用到的每一种图形能力：83 项通过 81 项，
剩下两项需要 Metal，这个移植还没有。

### 系统与设置

<p>
  <img src="docs/alioth/screenshots/system/settings.png" width="260" alt="设置">
  <img src="docs/alioth/screenshots/system/about.png" width="260" alt="关于本机">
  <img src="docs/alioth/screenshots/system/battery.png" width="260" alt="电池">
  <img src="docs/alioth/screenshots/system/display.png" width="260" alt="显示与亮度">
  <img src="docs/alioth/screenshots/system/wifi.png" width="260" alt="Wi-Fi">
  <img src="docs/alioth/screenshots/system/bluetooth-pairing.png" width="260" alt="蓝牙配对">
  <img src="docs/alioth/screenshots/system/bluetooth-connected.png" width="260" alt="蓝牙已连接">
</p>

设置；关于本机（iOS 16.5，iPhone X）；电池（来自手机的电量计）；显示与亮度；Wi-Fi；
和另一台手机蓝牙配对，然后连接成功。

### 传感器

<p>
  <img src="docs/alioth/screenshots/sensors/sensor-check-light.png" width="260" alt="光线和距离">
  <img src="docs/alioth/screenshots/sensors/sensor-check-motion.png" width="260" alt="运动传感器">
  <img src="docs/alioth/screenshots/sensors/sensor-check-power.png" width="260" alt="电池、温度状态、麦克风、摄像头">
  <img src="docs/alioth/screenshots/sensors/sensor-check-display.png" width="260" alt="方向、音量键、屏幕">
  <img src="docs/alioth/screenshots/sensors/sensor-check-touch.png" width="260" alt="触摸和触感反馈">
</p>

[Sensor Check](https://github.com/xiongnemo/ios-sensor-check) 实时列出 App 能用到的每一个传感器和硬件输入：
环境光、加速度计、陀螺仪、姿态、电池、温度状态、麦克风、摄像头、121 帧的屏幕、触摸和触感反馈。

## 能用的

每一项都在手机上验证过。

| 方面 | |
|---|---|
| 显示 | 120 Hz（面板 TE 每 8.2 ms 一次）；亮度；深色模式；根据环境光自动调节亮度；自动旋转；反转颜色、色彩滤镜、夜览由显示引擎的颜色校正实现，和真 iPhone 一样 |
| 图形 | 系统合成和 App 都通过 Mesa（freedreno）在 Adreno 650 上渲染：OpenGL ES 1、2、3，GLKit、SceneKit、SpriteKit、Core Image；PVRTC 纹理由处理器解码；每个进程独立的 GPU 地址空间 |
| 输入 | 多点触摸、电源键和音量键、振动 |
| 声音 | 扬声器（CS35L41）、麦克风（语音备忘录能录音） |
| Wi-Fi | 自动连接构建时写入的网络，优先 5 GHz |
| 蓝牙 | 可被发现、搜索、配对，蓝牙音频可输出到音箱和耳机 |
| 相机 | 相机 App 预览和拍照：主摄（IMX582，自动对焦）和前置（S5K3T2）；也可以在启动时改用超广角（IMX355）或微距（S5K5E9）当后置 |
| 手电筒 | 从控制中心打开 |
| 电源 | 电量显示和充电 |
| 传感器 | 通过传感器 DSP 提供环境光、加速度计、陀螺仪、姿态、指南针（有定位时为真北） |
| 定位 | 通过苹果定位服务进行 Wi-Fi 定位 |
| USB | 电脑把手机识别为 iPhone：配对（「信任此电脑」）、通过 AFC 传文件 |
| App | 未签名、已解密的 IPA 可以在启动时由电脑安装，或用手机上的安装器安装 |

为这个移植写的 App：[GPU Check](https://github.com/xiongnemo/ios-gpu-check)、
[Sensor Check](https://github.com/xiongnemo/ios-sensor-check)、[Motion Test](https://github.com/xiongnemo/ios-motion-test)
和 [Map](https://github.com/xiongnemo/ios-map)（OpenStreetMap 地图；苹果地图需要 Metal）。

## 还不能用的

- **Metal**：需要 Metal 的 App 会打开就闪退（测距仪、很多较新的 3D 游戏）。OpenGL ES 可以用。
- **数据保存**：iOS 在内存里运行，重启后设置、照片、装的 App 都会消失。
- GPS、蜂窝网络、NFC、指纹、距离传感器。
- GPU 卡死后的恢复：一个 App 让 GPU 卡住，整个屏幕都会停，只能重启手机。
- 功耗：熄屏后屏幕链路和 GPU 仍然开着。
- iOS 运行时拔掉 USB 线：重新插上后电脑认不出手机，要等下次启动（修复正在测试）。
- Safari 不能从相册里选照片。

各部分怎么实现、测到了什么、接下来做什么：[docs/alioth/status.zh.md](docs/alioth/status.zh.md)。

## 怎样得到能启动的包

> 这里说的自动构建和组装脚本正在搭建。在 Releases 页面出现第一个套件之前，只能从源码自己构建全部内容。

能启动的包由两半组成，其中只有一半可以公开发布。

**GitHub Actions 构建的部分**（只用这个仓库的源码构建，以 `alioth-kit-<版本>.zip` 发布在 Releases 页面）：

| 文件夹 | 内容 |
|---|---|
| `kernel/` | 带这台手机驱动的 XNU 内核 |
| `userland/` | 我们为 iOS 写的程序和库（基于 Mesa 的 OpenGL ES、USB 守护进程、IPA 安装器等）、Mesa 的 OSMesa 库，以及为这个移植写的 App |
| `boot/` | 这台手机的 UEFI 阶段（Project Silicium）和启动垫片 |
| `assemble/` | 在你电脑上生成另一半的脚本，以及 `start.bat` |

**在你电脑上生成的部分**，因为不能再分发：系统镜像（苹果的 iOS 文件）、手机固件（厂商的）、引导程序里的 iPhone 身份信息
（取自苹果的设备树），以及你的 Wi-Fi 密码。

你需要：已解锁 Bootloader、打开了 USB 调试的 Redmi K40 / POCO F3；一台装有 WSL 的 Windows 10 或 11 电脑，或者 Linux；
约 15 GB 可用磁盘空间；能上网。

1. 从 Releases 页面下载套件并解压。
2. 手机开机进 Android 并连上电脑，运行 `assemble.bat`（Linux 上运行 `assemble/assemble.sh`）。它会：
   - 从苹果的公开服务器下载 iPhone X（`iPhone10,3`）的 iOS 16.5 IPSW，并用它构建系统镜像，不解密、不绕过任何东西；
   - 从手机自己的 Android 里提取驱动需要的固件（GPU、音频、Wi-Fi、蓝牙、传感器、相机）；
   - 询问 iOS 要连接的 Wi-Fi 网络；
   - 从 IPSW 生成引导程序的身份信息，并构建启动镜像；
   - 生成包文件夹 `ios-alioth-k40/`。
3. 手机在 Android 下，运行这个文件夹里的 `start.bat`。不刷写任何东西，重启就回到 Android。

生成的包请只留给自己用：里面有苹果的文件、厂商的固件和你的 Wi-Fi 密码。

## 这个仓库里没有的

除了上面的截图，没有任何 iOS 文件：系统镜像用你自己的 iOS 16.5 IPSW 构建。也没有厂商固件（从你自己的手机上提取），
没有密钥、证书或激活相关的东西。登录 Apple 账号不可能，也不是目标。

## 致谢

[HakiOS](https://github.com/oosexgang/hakios-os)（本项目所基于的内核、引导程序和用户空间）、
[hackios-xnu-kernel-umi](https://github.com/JopangJovan/hackios-xnu-kernel-umi)（小米 10 移植）、
[Project Silicium](https://github.com/Project-Silicium/Mu-Silicium)（UEFI 阶段）、[ios-alioth](https://github.com/MrMoxing/ios-alioth)（这台手机的第一个启动包，本项目从它起步）、
FFT-infra 的 QcomXnuUsbDebug（USB 设备驱动）和 [Mesa](https://mesa3d.org/)（freedreno）。
