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

### host-map 修复后的同参数转码复测

在后续完成 `FFHWAccel.start_frame(buf_ref, buf, size)` 与 `vulkan_prores_raw.c` 的 `host-map slices` 零拷贝回移植后，又用完全相同的转码参数重新对当前 backport 分支补跑了一次实测。

输出文件：`/tmp/a7s_backport_vulkan_qp28_rerun.mov`

复测结果：

- 最终吞吐约 `8.6 fps`
- 实时倍率约 `0.356x`
- 总墙钟耗时约 `40.64s`
- `/usr/bin/time` 输出：`ELAPSED=40.64 USER=258.06 SYS=3.65 CPU=643% MAXRSS=1083728`
- ffmpeg `bench` 输出：`bench: utime=257.902s stime=3.586s rtime=40.406s`
- 峰值内存：`maxrss=1083728KiB`

这次复测说明：

- `buf_ref + host-map slices` 已经显著修复了纯 Vulkan 解码路径的 CPU/吞吐问题
- 但对完整 `Vulkan decode -> hwdownload -> format -> hwupload -> scale_vaapi -> h264_vaapi` 转码链路，没有带来同等幅度的整体加速
- 当前完整转码链路依然明显慢于文档中记录的官方 8.0.1 结果

### 完整转码链继续追踪: 锁定 `bayer -> nv12` 自动 swscale 路径

后续把完整链路拆成若干小段继续测，目标是确认剩余差距到底落在 Vulkan 解码、`hwdownload`，还是软件像素格式转换。

先后做了以下几组对照：

- `hwdownload,format=bayer_rggb16le`
  - backport: `speed=1.25x`，`ELAPSED=11.71 USER=0.33 SYS=1.82`
  - n8.0.1: `speed=1.36x`，`ELAPSED=10.87 USER=0.39 SYS=1.81`
- `hwdownload,format=bayer_rggb16le,format=yuv420p`
  - backport: `speed=0.882x`，`ELAPSED=16.53 USER=61.18 SYS=2.62`
  - n8.0.1: `speed=1.0x`，`ELAPSED=14.65 USER=60.13 SYS=1.97`
- `hwdownload,format=bayer_rggb16le,format=nv12`
  - backport: `speed=0.454x`，`ELAPSED=31.94 USER=259.46 SYS=2.39`
  - n8.0.1: `speed=1.3x`，`ELAPSED=11.36 USER=55.27 SYS=1.54`

这个拆分结果很关键：

- Vulkan 解码本身不是剩余瓶颈
- `hwdownload` 本身也不是剩余瓶颈
- `bayer -> yuv420p` 与 8.0.1 已经比较接近
- 真正爆炸的是 old `libswscale` 自动处理 `bayer_rggb16le -> nv12` 这条路径

为了排除单独的 `rgb48 -> nv12` 或 `yuv420p -> nv12` 转换核本身有问题，又补了纯软件 micro benchmark：

- `lavfi testsrc2,format=rgb48le -> format=nv12`
  - backport: `speed=2.38x`，`ELAPSED=6.15 USER=36.60 SYS=0.24`
  - n8.0.1: `speed=2.32x`，`ELAPSED=6.30 USER=36.86 SYS=0.22`
- `lavfi testsrc2,format=yuv420p -> format=nv12`
  - backport: `speed=5.45x`，`ELAPSED=2.69 USER=7.65 SYS=0.16`
  - n8.0.1: `speed=5.49x`，`ELAPSED=2.67 USER=7.59 SYS=0.18`

这说明：

- 单独的 `rgb48 -> nv12` 没问题
- 单独的 `yuv420p -> nv12` 也没问题
- 剩余异常只出现在旧版 `libswscale` 自动把 `bayer_rggb16le` 直接协商到 `nv12` 时

### 显式插入 `rgb48le` 中间格式的验证

既然自动 `bayer -> nv12` 路径异常，就继续验证手工拆成两步是否可以绕开：

```bash
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -vaapi_device /dev/dri/renderD128 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=rgb48le,format=nv12,hwupload,scale_vaapi=w=4096:h=-2" \
  -c:v h264_vaapi \
  -rc_mode CQP -qp 28 \
  -profile:v high -coder cabac \
  -an \
  -write_tmcd false \
  -y OUTPUT.mov
```

先只看 `hwdownload + format` 这一段：

- backport `hwdownload,format=bayer_rggb16le,format=rgb48le,format=nv12`
  - `speed=0.793x`
  - `ELAPSED=18.36 USER=60.04 SYS=2.99`
- n8.0.1 同命令
  - `speed=1.31x`
  - `ELAPSED=11.27 USER=55.75 SYS=1.60`

虽然墙钟时间仍略慢，但用户态 CPU 已经从原来的约 `258s` 降到约 `60s`，说明确实绕开了原先那条病态自动路径。

再跑完整 `Vulkan -> hwdownload -> format -> hwupload -> scale_vaapi -> h264_vaapi` 链。这里连续做了两轮同命令复测：

- 第一轮 backport 显式 `rgb48le` 中间格式
  - `speed=0.655x`
  - `ELAPSED=22.20 USER=61.10 SYS=4.41`
  - `maxrss=356388KiB`
- 第一轮 n8.0.1 显式 `rgb48le` 中间格式
  - `speed=0.811x`
  - `ELAPSED=18.12 USER=60.21 SYS=4.03`
  - `maxrss=448856KiB`
- 第二轮 backport 显式 `rgb48le` 中间格式
  - `speed=0.645x`
  - `ELAPSED=22.59 USER=61.22 SYS=4.38`
  - `maxrss=370452KiB`
- 第二轮 n8.0.1 显式 `rgb48le` 中间格式
  - `speed=0.828x`
  - `ELAPSED=17.75 USER=59.72 SYS=3.64`
  - `maxrss=439808KiB`

这组复测表明：

- 当前 backport 分支的剩余差距，不再是 Vulkan ProRes RAW backport 本身
- 真正的问题仍然指向 `n7.1.3` 时代旧版 `libswscale` 对 `bayer_rggb16le -> nv12` 的自动协商/级联路径
- 对当前分支和当前机器，最直接有效的绕行办法仍然是在滤镜图中显式插入 `format=rgb48le`
- 这样做以后，完整实际转码链已经从原来的约 `0.356x` 显著提升到约 `0.645x~0.655x`
- 但在这两轮最新复测里，8.0.1 仍保持在约 `0.811x~0.828x`，所以当前结果应表述为“差距显著收敛”，而不是“已经完全持平”

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

纯解码场景下，backport 分支最初同样慢于官方 8.0.1：

- backport: `45 fps`
- n8.0.1: `66 fps`

这说明性能差异不是单纯由后面的 `scale_vaapi` / `h264_vaapi` 造成的，解码侧本身也还有差距。

## 后续性能追查

在确认功能已经走通以后，后续又继续对 8.0.1 与当前 backport 的实现做了针对性对比，目标是解释纯解码吞吐和 CPU 时间为什么仍显著落后。

### 第一轮排查: Vulkan 执行池

先对比了 `libavutil/vulkan.c` 的执行池实现，验证了旧式“单 command pool + mutex”是否是主要 CPU 瓶颈。为此专门做了一个最小 backport：

- `ff_vk_exec_get()` 统一为带 `FFVulkanContext *` 参数的新接口
- 执行池改为每个 exec context 各自持有 command pool
- 去掉 fence reset/wait 周围的 mutex 路径

这轮改动可以成功构建，但 benchmark 结论是否定的：

- 旧执行池基线约 `speed=2.03x`
- 新执行池 backport 约 `speed=1.69x`
- 官方 8.0.1 约 `speed=2.60x`

也就是说，“单 pool + mutex”并不是当时主要 CPU 开销来源。

### 第二轮排查: userspace CPU hotspot

由于普通用户权限下 `perf record` 被 `perf_event_paranoid=4` 阻止，后续改用 `valgrind --tool=callgrind` 对当前 backport 做了 userspace CPU 热点分析。

热点结论是：

- CPU 时间主要打在 `libavcodec/prores_raw.c` 的 `get_value` / `decode_comp` / IDCT 软件路径
- 另外有一块一次性开销来自 `glslang` shader 编译初始化
- `libavutil/vulkan.c` 的公用执行池函数本身并不是主要热点

### 第三轮排查: `prores_raw.c` 与 `vulkan_prores_raw.c`

继续直接对比了 `libavcodec/prores_raw.c` 和 `libavcodec/vulkan_prores_raw.c` 在 backport 与 8.0.1 中的差异。

结论分两部分：

1. `libavcodec/prores_raw.c` 的 CPU 软件解码路径在两个版本里本质一致，`get_value` / `decode_comp` / `idct_put_bayer` 这条实现并没有被 8.0.1 删除。
2. 真正的关键差异在 hwaccel `start_frame` 接口和 Vulkan slice 数据输入路径，而不在 compute shader 本体。

## 最终性能修复: host-mapped slices 零拷贝回移植

继续对比后确认，8.0.1 的关键优化点是：

- `FFHWAccel.start_frame` 接口已经扩展为接收 `AVBufferRef *buf_ref`
- `libavcodec/vulkan_prores_raw.c` 可利用 `buf_ref` 与 `FF_VK_EXT_EXTERNAL_HOST_MEMORY`
- 在支持时直接把整包输入 host-map 成 `vp->slices_buf`
- 后续 `decode_slice` 只记录各 tile 的 offset，不再逐 slice 调 `ff_vk_decode_add_slice()` 拷贝/封装

基于这条结论，后续在当前 backport 分支上又补了一轮最小回移植：

### 新增/修正内容

- `libavcodec/hwaccel_internal.h`
  - `FFHWAccel.start_frame` 扩展为 `buf_ref + buf + size`
- 调用点同步
  - `prores_raw.c`
  - `proresdec.c`
  - `vp8.c`
  - `vp9.c`
  - `mjpegdec.c`
  - `vc1dec.c`
  - 若干 `FF_HW_CALL(... start_frame, ...)` 调用点
- `libavutil/vulkan.h` / `libavutil/vulkan.c`
  - 为 `FFVkBuffer` 增加 `host_ref` / `virtual_offset`
  - 增加 `ff_vk_host_map_buffer()`
  - 补齐 host-map buffer 生命周期释放逻辑
- `libavcodec/vulkan_prores_raw.c`
  - `start_frame()` 中支持用 `buf_ref` 直接 host-map 整包 slices buffer
  - `decode_slice()` 中若 `slices_buf->host_ref` 有效，则仅记录 offset，不再做逐 slice 拷贝

### 顺带清理

为了让这条接口链在当前构建配置下保持干净，还顺手把当前 Linux 构建中会触发 warning 的若干 VAAPI/Vulkan `start_frame` 实现签名也同步到了新接口，最终把 `start_frame` 相关 warning 收敛到了 0。

## 最新纯 Vulkan 解码 benchmark

在完成 `buf_ref + host-map` 回移植后，用同一条纯 Vulkan 解码命令重新测了一轮：

```bash
./ffmpeg \
  -nostdin -threads:v 1 \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -benchmark \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -an -f null -
```

### backport 分支最新结果

- `speed=2.73x`
- `ELAPSED=5.51`
- `USER=0.34`
- `SYS=0.84`
- ffmpeg `bench`: `utime=0.186s stime=0.780s rtime=5.277s`

### 官方 8.0.1 同批次对照结果

- `speed=2.66x`
- `ELAPSED=5.63`
- `USER=0.34`
- `SYS=0.84`
- ffmpeg `bench`: `utime=0.198s stime=0.776s rtime=5.417s`

### 最新 benchmark 对比结论

完成 `buf_ref + host-map slices` 回移植后，当前 backport 分支的纯 Vulkan ProRes RAW 解码性能已经追平官方 8.0.1，同一台机器、同一样片、同一条命令下：

- 墙钟时间已经处于同一量级
- 用户态 CPU 时间已经与 8.0.1 基本一致
- 输出像素格式仍保持 `vulkan`

这说明此前纯解码性能差距的主要根因，确实是 Vulkan hwaccel 输入 slice 路径缺少 8.0.1 的 `AVBufferRef + host-map` 零拷贝链路，而不是 ProRes RAW compute shader 本体或执行池模型本身。

## 最终结论

1. 当前 `backport-8.0.1` 分支已经严格建立在 `n7.1.3` tag 基线上。
2. ProRes RAW 软件解码、Vulkan compute decode 基础设施、Vulkan hwaccel 本体，以及参考分支上的关键修复都已经回移植并推送。
3. `CONFIG_PRORES_RAW_VULKAN_HWACCEL` 已经从 `0` 变为 `1`，构建通过，运行时也已确认真实输出 `vulkan` hardware frames。
4. 纯 Vulkan ProRes RAW 解码在完成 `buf_ref + host-map slices` 回移植后，性能已经追平官方 8.0.1。
5. 纯 Vulkan ProRes RAW 解码这条关键前端路径已经追平官方 8.0.1，造成此前差距的主要缺口已经定位并修复。
6. 如果直接沿用 `hwdownload,format=bayer_rggb16le,format=nv12` 这条旧滤镜链，当前 backport 分支在最新同参数复测里仍只有约 `8.6 fps / 0.356x`，尚未追平文档中记录的官方 8.0.1 转码结果。
7. 因此当前这条 backport 分支的更准确结论是：

- 功能上已经打通 Vulkan ProRes RAW GPU 解码
- 纯 Vulkan 解码关键性能路径已经追平官方 8.0.1
- 完整 Vulkan 到 VAAPI 的实际转码链路剩余问题，已经收敛到旧版 `libswscale` 的自动 `bayer_rggb16le -> nv12` 路径
- 对当前 backport，显式使用 `format=rgb48le` 中间格式后，完整转码链性能会明显改善，但在最新两轮复测里仍未完全追平官方 8.0.1

## 2026-03-17 GTX 1080 Ti 无缩放 H.264 硬件转码复核

新的测试前提如下：

- GPU: `NVIDIA GeForce GTX 1080 Ti`
- Vulkan ICD: `/etc/vulkan/icd.d/nvidia_icd.json`
- 样片仍为 `/home/lean/Desktop/a7s III ProRes RAW HQ.mov`
- 样片分辨率仍为 `4264x2408`

这次不再沿用此前的 `4096` 缩放链路，而是尝试直接对原始 `4264x2408` 帧做 `Vulkan GPU 解码 -> H.264 硬件编码`，目标是消掉缩放变量后，再比较 backport 和官方 `8.0.1` 的速度差异。

### Vulkan 设备选择

在这台机器上，如果不显式指定 NVIDIA 的 Vulkan ICD，`ffmpeg -init_hw_device vulkan:0` 会直接报：

- `No devices found: VK_ERROR_INITIALIZATION_FAILED`

因此 GTX 1080 Ti 下后续 Vulkan 命令都必须显式加：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
```

### 无缩放 `h264_nvenc` 直转命令

本次尝试的核心命令为：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=nv12" \
  -c:v h264_nvenc \
  -preset p5 -tune hq \
  -rc vbr -cq 28 \
  -profile:v high -coder cabac \
  -an \
  -f null -
```

这条链路里已经没有缩放，只有 Vulkan 解码后做必要的 `bayer_rggb16le -> nv12` 转换，再交给 NVIDIA H.264 硬件编码器。

### `h264_nvenc` 实测结果

backport 这边 Vulkan 解码本身可以正常起跑，但编码器在打开阶段直接失败：

- `No capable devices found`
- `Error while opening encoder - maybe incorrect parameters such as bit_rate, rate, width or height`

为了确认不是 Vulkan 解码链路本身的问题，又补了两组纯 `lavfi -> h264_nvenc` 尺寸对照：

- `4096x2160 -> h264_nvenc`: 可以正常编码
- `4264x2408 -> h264_nvenc`: 仍然报 `No capable devices found`

这说明在这张 GTX 1080 Ti 上，`h264_nvenc` 虽然可以正常工作，但对当前样片的原始宽度 `4264` 仍然不能直接开编码器。因此“无缩放 H.264 NVENC”这条测试链路在当前卡上无法真正进入 steady-state 编码阶段，自然也就拿不到可比较的吞吐数据。

### 直接 `h264_vulkan` 编码复测

为了更贴近“Vulkan GPU 解码后直接硬件 H.264 转码”的思路，又补了一条完全不做 `hwdownload` 的命令：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -c:v h264_vulkan \
  -an \
  -f null -
```

结果同样不能成立，编码器直接报：

- `Input of 4272x2416 too large for encoder limits: 48x32 max`

也就是说，当前驱动栈下的 `h264_vulkan` 也不能把这个样片作为“无缩放 H.264 硬件转码”基线跑起来。

### GTX 1080 Ti 新口径结论

这轮复核后的结论很明确：

1. GTX 1080 Ti 可以作为 Vulkan 解码测试设备，但 Vulkan 设备初始化时必须显式指定 NVIDIA ICD。
2. 对当前这段 `4264x2408` ProRes RAW 样片，`h264_nvenc` 在 Pascal 上仍然不能直接做无缩放 H.264 编码；`4096x2160` 可以，`4264x2408` 不行。
3. `h264_vulkan` 直编码在当前驱动/硬件组合下也不能作为这个样片的可行替代链路。
4. 因此“7.1.3 backport vs 官方 8.0.1，均取消缩放，直接 Vulkan 解码后硬件 H.264 转码，再对比速度”这条新 benchmark 思路，在 GTX 1080 Ti + 当前样片条件下暂时无法产出有效吞吐对比数据。
5. 如果后续仍要做两版速度对比，当前可行方案只剩两种：

- 继续保留 `4096` 宽度缩放后再做 `h264_nvenc`
- 或者改测“无缩放 Vulkan 解码 + 其他可接受 `4264` 宽度的硬件编码目标”，例如进一步验证 `hevc_nvenc`

## 2026-03-17 GTX 1080 Ti 改用 4096 宽 NVENC benchmark

在确认“无缩放 Vulkan 解码 + H.264 硬件编码”不可行后，测试口径改为：

- 仍使用 `GTX 1080 Ti`
- 仍使用 `Vulkan ProRes RAW` GPU 解码
- 但在进入 `h264_nvenc` 之前，先把宽度缩到 `4096`

本次实际使用命令：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=nv12,scale=4096:-2" \
  -c:v h264_nvenc \
  -preset p5 -tune hq \
  -rc vbr -cq 28 \
  -profile:v high -coder cabac \
  -an \
  -f null -
```

输出流参数已经确认变为：

- `h264`
- `nv12`
- `4096x2314`

### backport 分支结果

backport 使用二进制：`/home/lean/Desktop/ffmpeg/ffmpeg-n7.1.3-backport-8.0.1/ffmpeg`

实测结果：

- 最终吞吐约 `12 fps`
- 实时倍率约 `0.477x`
- 总墙钟耗时约 `30.99s`
- ffmpeg `bench`：`utime=269.765s stime=6.165s rtime=30.010s`
- `/usr/bin/time`：`ELAPSED=30.99 USER=269.99 SYS=6.73 CPU=892% MAXRSS=2090192`

### 官方 8.0.1 结果

官方 `8.0.1` 使用二进制：`/home/lean/Desktop/ffmpeg/ffmpeg-n8.0.1/ffmpeg`

这次使用的是已经重新链接到本地新版 `libglslang.so.16` 的 `8.0.1` 构建，因此 Vulkan ProRes RAW shader 可以在 NVIDIA Vulkan ICD 下正常初始化。

实测结果：

- 最终吞吐约 `33 fps`
- 实时倍率约 `1.38x`
- 总墙钟耗时约 `11.53s`
- ffmpeg `bench`：`utime=59.738s stime=4.396s rtime=10.333s`
- `/usr/bin/time`：`ELAPSED=11.53 USER=60.12 SYS=5.05 CPU=565% MAXRSS=1392356`

### 4096 宽 benchmark 对比结论

在 `GTX 1080 Ti + 4096 宽缩放 + h264_nvenc` 这条新口径下，两版差距非常明显：

- backport: `12 fps / 0.477x`
- official 8.0.1: `33 fps / 1.38x`

也就是说，把分辨率缩到 `4096` 之后，链路终于可以稳定跑通，但当前 `7.1.3 backport` 的完整转码吞吐仍明显落后于官方 `8.0.1`。

### 更正: 显式 `rgb48le` 中间格式后的同口径复测

上一轮 `4096` 宽 `h264_nvenc` benchmark 直接使用了：

```bash
hwdownload,format=bayer_rggb16le,format=nv12,scale=4096:-2
```

这个口径会把 `n7.1.3` 旧版 `libswscale` 的 `bayer_rggb16le -> nv12` 自动协商差异一并算进来，因此不能单独代表 `scale + h264_nvenc` 的真实差距。

为消掉这部分变量，又补跑了显式 `rgb48le` 中间格式的同参数命令：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=rgb48le,format=nv12,scale=4096:-2" \
  -c:v h264_nvenc \
  -preset p5 -tune hq \
  -rc vbr -cq 28 \
  -profile:v high -coder cabac \
  -an \
  -f null -
```

backport 复测结果：

- 最终吞吐约 `36 fps`
- 实时倍率约 `1.50x`
- ffmpeg `bench`：`utime=58.057s stime=4.396s rtime=9.508s`
- `/usr/bin/time`：`ELAPSED=10.36 USER=58.26 SYS=4.95 CPU=610% MAXRSS=1358212`

官方 `8.0.1` 同命令结果：

- 最终吞吐约 `36 fps`
- 实时倍率约 `1.51x`
- ffmpeg `bench`：`utime=58.749s stime=4.369s rtime=9.503s`
- `/usr/bin/time`：`ELAPSED=10.22 USER=58.94 SYS=4.82 CPU=623% MAXRSS=1408264`

这一轮复测说明：

- 之前 `12 fps` 对 `33 fps` 的巨大差距，主要并不落在 `scale=4096:-2 + h264_nvenc`
- 真正的主要差异，仍然是 `bayer_rggb16le -> nv12` 自动路径
- 一旦显式插入 `format=rgb48le`，`7.1.3 backport` 与 `8.0.1` 的完整 `4096` 宽 NVENC 链路已经基本持平

### workaround: 在 `libavfilter scale` 内部自动走 `Bayer16 -> RGB48 -> NV12/NV21`

为了避免后续每次都手工写：

```bash
hwdownload,format=bayer_rggb16le,format=rgb48le,format=nv12,...
```

当前 backport 已把 workaround 下沉到 `libavfilter/vf_scale.c`：

- 当 libavfilter 自动插入 `auto_scale_*`，并遇到 `bayer_*16* -> nv12|nv21`
- `scale` 不再直接走旧版 `libswscale` 的慢路径
- 而是在过滤器内部拆成 `Bayer16 -> RGB48 -> NV12/NV21` 两段转换

也就是说，对当前这类 Vulkan ProRes RAW 转码命令，用户已经不需要再手工写 `format=rgb48le`，而且这条行为不再依赖 `ffmpeg` CLI 前端改写 filtergraph。

### workaround 生效复测

workaround 落地后，又直接用原来那条“不显式写 `rgb48le`”的命令复测：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=nv12,scale=4096:-2" \
  -c:v h264_nvenc \
  -preset p5 -tune hq \
  -rc vbr -cq 28 \
  -profile:v high -coder cabac \
  -an \
  -f null -
```

backport 最新结果：

- 最终吞吐约 `36 fps`
- 实时倍率约 `1.54x`
- `/usr/bin/time`：`ELAPSED=10.15 USER=58.34 SYS=4.43 CPU=618% MAXRSS=1373908`

verbose 日志里还能直接看到：

- `auto_scale_0` 被自动插入在两个 `format` 之间
- `auto_scale_0` 输出 `Using internal Bayer RGB48 workaround for bayer_rggb16le -> nv12`

这说明当前 workaround 已经达到预期目标：

- 原来需要手写 `format=rgb48le` 才能绕开的慢路径
- 现在即使用户直接写 `format=nv12`
- 也会自动收敛到和显式 `rgb48le` 版本同一量级的吞吐表现
- 并且通过 `libavfilter` 的 `scale` 路径对库层调用者同样生效

### 与官方 8.0.1 的最终同参数复核

在库层 workaround 落地后，又重新按完全相同参数对 backport 与官方 `8.0.1` 各跑了一次：

```bash
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json \
./ffmpeg \
  -benchmark -stats -nostdin \
  -init_hw_device vulkan:0 \
  -hwaccel vulkan \
  -hwaccel_output_format vulkan \
  -i "/home/lean/Desktop/a7s III ProRes RAW HQ.mov" \
  -map 0:v:0 \
  -vf "hwdownload,format=bayer_rggb16le,format=nv12,scale=4096:-2" \
  -c:v h264_nvenc \
  -preset p5 -tune hq \
  -rc vbr -cq 28 \
  -profile:v high -coder cabac \
  -an \
  -f null -
```

本次复核结果：

- backport: `ELAPSED=10.13 USER=58.46 SYS=4.44 CPU=620% MAXRSS=1378104`，最终约 `37 fps / 1.54x`
- official 8.0.1: `ELAPSED=10.43 USER=59.22 SYS=5.10 CPU=616% MAXRSS=1384128`，最终约 `36 fps / 1.5x`

这说明当前 backport 在这条 GTX 1080 Ti + Vulkan decode + 4096 宽 `h264_nvenc` 口径下，已经与官方 `8.0.1` 基本一致。