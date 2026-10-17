# 项目简介

基于 Rockchip RK3566（LubanCat 鲁班猫卡片电脑，arm64）的视觉识别项目：目标是把 YOLOv5s 模型部署到板端运行。当前处于工程骨架搭建阶段（helloworld 验证交叉编译链）。

# 目录结构

    AGENT.md          本文件，工程说明
    README.md         标题占位（yolov5_rk3566）
    main.c            入口源码，目前是 helloworld
    meson.build       Meson 构建定义：project('yolov5_rk3566','c')，产物 executable 'demo'，开启 -g
    cross-rk3566.ini   Meson 交叉编译配置：aarch64-linux-gnu-gcc 工具链（系统 /bin 下）
    utils.sh          封装的 shell 函数（构建/传板/拉库/远程调试），内含 WSL 和板子 IP
    include/          头文件（空）
    source/           源码目录（空）
    models/           模型文件，现有 yolov5s.pt
    build_arm64/      Meson/Ninja 构建输出（git 忽略）
    .vscode/          VSCode cppdbg 远程调试配置（demo1，gdb-multiarch → 板子 12345 端口）

# 环境约定

- 开发机：WSL 虚拟机（Ubuntu），用户 jaja
- 目标板：LubanCat（RK3566, aarch64），用户 cat
- IP 地址（见 utils.sh，有变动时改这里）：
  - wslip = 172.29.180.109（WSL 虚拟机）
  - lbcip = 192.168.137.200（板子）

# 构建与部署（utils.sh 函数）

    build    首次 meson setup build_arm64 --cross-file cross-rk3566.ini，然后 ninja -C build_arm64
    pushsw   rsync 把 build_arm64/$target 推到板子 cat@lbcip:~/
    pullbd   scp 从板子拉文件：pullbd <远程路径> <本地路径>
    pullsysroot  从板子拉取完整 sysroot（/lib、/usr/include、/usr/lib，-L 解引用软链接）到 ./sysroot
    dbg      gdb-multiarch 远程调试，连接板子 12345 端口（板端需先起 gdbserver）

使用方式（在仓库根目录）：

    bash -c 'source utils.sh && build'

注意：utils.sh 的函数没有被自动加载，直接 ./utils.sh 不会执行任何东西。

# 远程调试流程

1. 板上启动：gdbserver :12345 ./demo
2. 本地执行 utils.sh 的 dbg()（或用 .vscode/launch.json 的 cppdbg 配置）
3. 源码目录映射：板上路径 /home/jaja/rk3566yolo → 本仓库

# 注意事项（给 AI 助手/后续开发）

- 可执行文件名已统一为 demo1（meson.build / utils.sh 的 target / .vscode/launch.json 三处一致）。
- cross-rk3566.ini 已配置 sysroot = ./sysroot（从板子拉取的板端头文件/库，约 3.9G，git 忽略）。板子 glibc 为 2.31 (Ubuntu)。改动 cross 文件后必须 rm -rf build_arm64 重新 setup。
- SSH 免密已配置（cat@192.168.137.200）。
- 板子上 /lib/aarch64-linux-gnu/qt-default 有失效软链接，rsync 需 --exclude='qt-default'。
- build_arm64/ 已在 .gitignore，勿提交。
- 模型转换链路（.pt → RKNN）尚未开始，models/ 只有原始 yolov5s.pt。
- 已删除的 scripts/（export.py、run.sh、utils.sh）是旧结构，新结构以根目录 utils.sh 为准。
