#utils.sh

wslip=172.29.180.109
lbcip=192.168.137.200
target=demo1

function build(){
    
    # 首次配置，指定交叉编译
    meson setup build_arm64 --cross-file cross-rk3566.ini
    # 编译
    ninja -C build_arm64 -j$(nproc)
}

function pushsw(){
    rsync -avz build_arm64/$target cat@$lbcip:~/$target
}

function pullbd(){
    scp cat@$lbcip:$1 $2
}

function pullsysroot(){
    # 从板子拉取 sysroot（头文件+库+动态链接器），-L 解引用软链接
    rsync -aL --exclude='qt-default' cat@$lbcip:/lib/aarch64-linux-gnu sysroot/lib/
    rsync -aL cat@$lbcip:/lib/ld-linux-aarch64.so.1 sysroot/lib/
    rsync -aL cat@$lbcip:/usr/include sysroot/usr/
    rsync -aL --exclude='qt-default' cat@$lbcip:/usr/lib/aarch64-linux-gnu sysroot/usr/lib/
}

function dbg(){
    gdb-multiarch build_arm64/$target \
      --directory ~/rk3566yolo \
      -ex "target remote $lbcip:12345"

}