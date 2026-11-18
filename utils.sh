#utils.sh
#---鲁班猫开发板ipv4地址----
lbcip=192.168.137.200
# target=demo1
target=mjpeg_preview
gdbserver_port=12345

set -euo pipefail 

#---编译后的软件更新到板子上---
sendsw(){
    rsync -avz build_arm64/$target cat@$lbcip:~/ywj_demo1/$target
}

#---编译指令---
build(){
    # 首次配置，指定交叉编译
    [ -d build_arm64 ] || meson setup build_arm64 --cross-file cross-rk3566.ini
    # 编译
    ninja -C build_arm64 -j$(nproc)

    sendsw
}

pullbd(){
    scp "cat@$lbcip:$1" "$2"
}

pullsysroot(){
    # 从板子拉取 sysroot（头文件+库+动态链接器），-L 解引用软链接
    rsync -aL --exclude='qt-default' cat@$lbcip:/lib/aarch64-linux-gnu sysroot/lib/
    rsync -aL cat@$lbcip:/lib/ld-linux-aarch64.so.1 sysroot/lib/
    rsync -aL cat@$lbcip:/usr/include sysroot/usr/
    rsync -aL --exclude='qt-default' cat@$lbcip:/usr/lib/aarch64-linux-gnu sysroot/usr/lib/
}
#---cli调试---
dbg(){
    gdb-multiarch build_arm64/$target \
      --directory "$PWD" \
      -ex "target remote $lbcip:$gdbserver_port"
}