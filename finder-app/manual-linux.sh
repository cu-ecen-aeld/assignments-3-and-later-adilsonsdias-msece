#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
cp arch/${ARCH}/boot/Image ${OUTDIR}/
fi

echo "Adding the Image in outdir"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs
mkdir -p bin dev etc home lib lib64 proc sbin sys usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
else
    cd busybox
fi

make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX+${OUTDIR}/rootfs install
cd ${OUTDIR}/rootfs

echo "Library dependencies"
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

cp -L ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/

if [ -d "${SYSROOT}/lib64" ]; then
    echo "Копируем библиотеки из sysroot/lib64"
    cp -L ${SYSROOT}/lib64/libm.so.6 ${OUTDIR}/rootfs/lib64/
    cp -L ${SYSROOT}/lib64/libc.so.6 ${OUTDIR}/rootfs/lib64/
    cp -L ${SYSROOT}/lib64/libresolv.so.2 ${OUTDIR}/rootfs/lib64/
else
    echo "Папка lib64 не найдена в sysroot. Копируем библиотеки из sysroot/lib"
    cp -L ${SYSROOT}/lib/libm.so.6 ${OUTDIR}/rootfs/lib64/
    cp -L ${SYSROOT}/lib/libc.so.6 ${OUTDIR}/rootfs/lib64/
    cp -L ${SYSROOT}/lib/libresolv.so.2 ${OUTDIR}/rootfs/lib64/
fi

cd ${OUTDIR}/rootfs/dev
    sudo mknod -m 666 null c 1 3
    sudo mknod -m 600 console c 5 1
# TODO: Clean and build the writer utility
cd ${FINDER_APP_DIR}
    make clean
    make CROSS_COMPILE=${CROSS_COMPILE}
# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
# Копируем все исполняемые файлы и скрипты в /home нашей целевой системы
    cp ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/
    cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
    cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
    cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/
    
    
    cp -r ${FINDER_APP_DIR}/conf/ ${OUTDIR}/rootfs/home/

    sed -i 's|#!/bin/bash|#!/bin/sh|g' ${OUTDIR}/rootfs/home/finder.sh
    sed -i 's|#!/bin/bash|#!/bin/sh|g' ${OUTDIR}/rootfs/home/finder-test.sh

    sed -i 's|\.\./conf|conf|g' ${OUTDIR}/rootfs/home/finder-test.sh
# TODO: Chown the root directory
cd ${OUTDIR}/rootfs
    sudo chown -R root:root *
# TODO: Create initramfs.cpio.gz
cd ${OUTDIR}/rootfs
    find . | cpio -H newc -ov --owner root:root | gzip -9 > ${OUTDIR}/initramfs.cpio.gz