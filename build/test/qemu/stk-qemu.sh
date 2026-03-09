#!/usr/bin/env bash

PACKAGE=$1
ARCH=$2
CPU=$3
BOARD=$4
WORK_DIR=$5
KERNEL=$6

# Initialize FLAGS
FLAGS=""

# Set FLAGS depending on ARCH
case "${ARCH}" in
    "qemu-system-riscv32")
        # Use -device loader to handle the 0x20010000 entry point
        FLAGS="-semihosting -d unimp,guest_errors \
               -bios none -device loader,file=/fw/${KERNEL},cpu-num=0"
        KERNEL_ARG="" 
        ;;
    "qemu-system-gnuarmeclipse")
        # Standard ARM semihosting uses the -kernel flag
        FLAGS="-semihosting-config enable=on,target=native -d guest_errors"
        KERNEL_ARG="-kernel /fw/${KERNEL}"
        ;;
    *)
        echo "Unknown architecture: ${ARCH}"
        exit 1
        ;;
esac

# Run emulator
docker run -v ${WORK_DIR}:/fw ${PACKAGE} ${ARCH} \
    -cpu ${CPU} -machine ${BOARD} -nographic ${FLAGS} \
    ${KERNEL_ARG} > /dev/null & PID=$!

# Check if emulator started by checking its PID
ps --pid "$PID" >/dev/null
if [ "$?" -ne 0 ]; then
    echo "No pid for QEMU instance found! QEMU not started."
    exit 1
fi

# Wait for the output (semihosting)
sleep 10

# Kill emulator
kill ${PID} &> /dev/null
