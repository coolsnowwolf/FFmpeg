# FFmpeg n7.1.3 tag 基线 Vulkan ProRes RAW backport 记录

## 目标

- 源码目录: `/home/lean/Desktop/ffmpeg/ffmpeg-n7.1.3-backport-8.0.1`
- Git 分支: `backport-8.0.1`
- Git 基线: `n7.1.3` tag
- 参考分支: `origin/backport/prores-raw-base-n7.1.3`
- 参考实现: `/home/lean/Desktop/ffmpeg/ffmpeg-n8.0.1`
- GPU: AMD Radeon VII
- 样片: `/home/lean/Desktop/a7s III ProRes RAW HQ.mov`

本次工作的要求不是在已有脏工作树上补丁修修补补，而是必须从 `n7.1.3` tag 的干净基线出发，建立单独的 backport 分支，再把 ProRes RAW Vulkan 解码能力回移植进来。

## 构建参数

本次 backport 分支实际使用的构建参数与 8.0.1 保持一致：

```bash
./configure \
  --prefix="$HOME/ffmpeg-7.1.3-backport-install" \
  --enable-gpl \
  --enable-libx264 \
  --enable-vaapi \
  --enable-vulkan \
  --enable-libglslang \
  --enable-static \
  --disable-shared

make -j"$(nproc)" ffmpeg ffprobe
```

## backport 过程

这次没有直接手工拷一大批文件，而是先对照 `origin/backport/prores-raw-base-n7.1.3`，再把已经拆好的提交链按阶段回放到 `n7.1.3` tag 基线上。

### 第一阶段: ProRes RAW 软件解码基础

已回移植并推送：

- `c0062f2aab avcodec: register ProRes RAW decoder and parser`
- `9b66cdc042 avcodec: add ProRes RAW software decoder`
- `2693f3d6a8 avcodec/x86: link ProRes RAW dsp asm`

这一阶段把 `AV_CODEC_ID_PRORES_RAW`、parser、软件解码器、profile/descriptor/isom tag 注册，以及 x86 DSP 依赖补齐。

### 第二阶段: Vulkan 计算解码基础设施

已回移植并推送：

- `ca3e16c81f avcodec/vulkan: add compute decode infrastructure`
- `f700b80886 avutil/vulkan: backport SPIR-V compiler helpers`
- `aed20abac7 avcodec: add vulkan_glslang wrapper for libavcodec`
- `65c4da5694 avcodec: add vulkan_shaderc wrapper for libavcodec`
- `2f51442eb5 avcodec/Makefile: add vulkan_glslang and vulkan_shaderc objects`

这一阶段补齐了 `libavcodec/vulkan/Makefile`、`common.comp`、`vulkan_decode.*` 所需能力，以及 `libavutil` 里的 SPIR-V 编译辅助层。

### 第三阶段: ProRes RAW Vulkan hwaccel 本体

已回移植并推送：

- `da63e2dcbd avcodec: add ProRes RAW Vulkan hwaccel`

这一阶段引入：

- `libavcodec/vulkan_prores_raw.c`
- `libavcodec/vulkan/prores_raw.comp`
- `configure` / `Makefile` / `hwaccels.h` 中的 `prores_raw_vulkan` 接线

### 第四阶段: 参考分支上的后续修正

已回移植并推送：

- `860ee6e45f vulkan: fix prores raw shader compatibility`
- `c084c77c42 vulkan: fix prores raw decode frame setup`
- `d8d639c45b vulkan: fix prores raw idct scales`
- `62eece1cbb avcodec/vulkan: use SDR float views for ProRes RAW decode`

### 第五阶段: 当前环境额外兼容修正

参考分支在这台机器上还缺一处本地兼容补丁。当前系统头里的 `glslang` C API 版本偏旧，缺少较新的 `glslang_program_SPIRV_generate_with_options()` / `glslang_spv_options_t` 组合，因此补了一条本地修正并已推送：

- `662d75c307 avutil/vulkan_glslang: support older glslang C API`

修正点是在 `libavutil/vulkan_glslang.c` 中加上版本判断：

- `GLSLANG_VERSION_MAJOR >= 12` 时走 `glslang_program_SPIRV_generate_with_options()`
- 更旧版本回退到 `glslang_program_SPIRV_generate()`

## configure 结果

重新跑 `./configure` 后，关键开关已经正确打开：

```c
#define CONFIG_PRORES_RAW_VULKAN_HWACCEL 1
```

对应 `ffbuild/config.mak` 中为：

```make
CONFIG_PRORES_RAW_VULKAN_HWACCEL=yes
```

## 编译结果

本次 `make -j"$(nproc)" ffmpeg ffprobe` 已成功通过。

构建日志中可见 Vulkan shader 生成步骤被实际触发：

```text
VULKAN  libavcodec/vulkan/common.c
VULKAN  libavcodec/vulkan/prores_raw.c
```

## 单帧样片验证

使用命令：

```bash
./ffmpeg \
  -loglevel debug \
  -threads:v 1 \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -frames:v 1 -an \
  -f null -
```

实际日志已确认以下关键点：

- `Selecting decoder 'prores_raw' because of requested hwaccel method vulkan`
- `Format vulkan chosen by get_format().`
- `Format vulkan requires hwaccel prores_raw_vulkan initialisation.`
- `w:4264 h:2408 pixfmt:vulkan`
- `Video: wrapped_avframe, ... vulkan(progressive), 4264x2408`

因此可以确认：

1. 这条 backport 分支没有回退到软件输出 `bayer_rggb16le`。
2. 解码器真实输出的是 Vulkan hardware frames。
3. ProRes RAW Vulkan hwaccel 已经被真正走通。

## Vulkan ProRes RAW 转码 benchmark

为了和 8.0.1 做同口径对比，这里复用了 8.0.1 文档里已经验证过的同一条链路：

```bash
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -vaapi_device /dev/dri/renderD128 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=nv12,hwupload,scale_vaapi=w=4096:h=-2" \
  -c:v h264_vaapi \
  -rc_mode CQP -qp 28 \
  -profile:v high -coder cabac \
  -an \
  -write_tmcd false \
  -y OUTPUT.mov
```

这条链路的含义是：

- 前端用 Vulkan 解码 ProRes RAW
- 然后 `hwdownload`
- 转成 `nv12`
- 再 `hwupload` 到 VAAPI
- 由 `scale_vaapi + h264_vaapi` 完成后半段转码

### backport 分支结果

输出文件：`/tmp/a7s_backport_vulkan_qp28.mov`

实测结果：

- 最终吞吐约 `9.2 fps`
- 实时倍率约 `0.383x`
- 总墙钟耗时约 `37.604s`
- benchmark 输出：`bench: utime=254.687s stime=3.700s rtime=37.604s`
- 峰值内存：`maxrss=1107596KiB`

### 官方 8.0.1 结果

输出文件：`/tmp/a7s_n801_vulkan_qp28.mov`

实测结果：

- 最终吞吐约 `15 fps`
- 实时倍率约 `0.61x`
- 总墙钟耗时约 `23.673s`
- benchmark 输出：`bench: utime=50.311s stime=4.394s rtime=23.673s`
- 峰值内存：`maxrss=489644KiB`

### 转码 benchmark 对比结论

这两者速度不一致，当前 backport 分支明显慢于官方 8.0.1：

- backport: `9.2 fps`
- n8.0.1: `15 fps`

也就是说，当前 tag 基线 backport 虽然已经能正确走到 Vulkan ProRes RAW 解码路径，但整体转码吞吐还没有追平 8.0.1。

## 纯 Vulkan 解码 benchmark

为了区分瓶颈落在转码链路还是解码链路，又补了一轮只解码到 `null` 的测试：

```bash
./ffmpeg \
  -benchmark -stats -nostdin \
  -threads:v 1 \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 -frames:v 346 -an \
  -f null -
```

### backport 分支纯解码结果

- 最终吞吐约 `45 fps`
- 实时倍率约 `1.85x`
- 总墙钟耗时约 `7.773s`
- benchmark 输出：`bench: utime=3.586s stime=0.537s rtime=7.773s`

### 官方 8.0.1 纯解码结果

- 最终吞吐约 `66 fps`
- 实时倍率约 `2.74x`
- 总墙钟耗时约 `5.258s`
- benchmark 输出：`bench: utime=0.207s stime=0.663s rtime=5.258s`

### 纯解码 benchmark 对比结论

纯解码场景下，backport 分支同样慢于官方 8.0.1：

- backport: `45 fps`
- n8.0.1: `66 fps`

这说明性能差异不是单纯由后面的 `scale_vaapi` / `h264_vaapi` 造成的，解码侧本身也还有差距。

## 最终结论

1. 当前 `backport-8.0.1` 分支已经严格建立在 `n7.1.3` tag 基线上。
2. ProRes RAW 软件解码、Vulkan compute decode 基础设施、Vulkan hwaccel 本体，以及参考分支上的关键修复都已经回移植并推送。
3. `CONFIG_PRORES_RAW_VULKAN_HWACCEL` 已经从 `0` 变为 `1`，构建也已通过。
4. 运行时已经明确看到 `pixfmt:vulkan` 与 `wrapped_avframe, vulkan(progressive)`，因此可以确认这条分支确实真正走了 GPU 解码加速，而不是软件回退。
5. 但当前性能还没有追平官方 8.0.1。
6. 在本次同链路测试中：
   - Vulkan ProRes RAW 转码: backport 约 `9.2 fps`，8.0.1 约 `15 fps`
   - 纯 Vulkan 解码: backport 约 `45 fps`，8.0.1 约 `66 fps`

因此，当前状态应定义为：

- 功能上已经打通 Vulkan ProRes RAW GPU 解码
- 性能上仍落后于官方 8.0.1，后续仍值得继续比对 8.0.1 的后续实现差异