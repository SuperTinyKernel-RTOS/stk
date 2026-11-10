#!/usr/bin/env python3

# 
#  SuperTinyKernel(TM) RTOS: Lightweight High-Performance Deterministic C++ RTOS for Embedded Systems.
# 
#  Source: https://github.com/SuperTinyKernel-RTOS
# 
#  Copyright (c) 2022-2026 Neutron Code Limited <stk@neutroncode.com>. All Rights Reserved.
#  License: MIT License, see LICENSE for a full text.
# 

import argparse
import os
import subprocess
import tempfile


def flash_elf(
    elf_path,
    device="STM32F407VG",
    speed=4000,
    jlink_path="JLink.exe",
    objcopy="arm-none-eabi-objcopy",
):
  if not os.path.exists(elf_path):
    print(f"[Error] ELF file not found: '{elf_path}'")
    return False

  # Generate a temporary binary file path
  bin_fd, bin_path = tempfile.mkstemp(suffix=".bin")
  os.close(bin_fd)

  # Normalize paths for J-Link compatibility
  abs_bin_path = os.path.abspath(bin_path).replace("\\", "/")

  try:
    # Step 1: Convert ELF to raw binary using objcopy
    print(f"Converting '{elf_path}' to binary format...")
    subprocess.run([objcopy, "-O", "binary", elf_path, bin_path], check=True)
  except subprocess.CalledProcessError as e:
    print(f"[Error] Failed to convert ELF to binary using '{objcopy}'.")
    print("Ensure arm-none-eabi-toolchain is installed and in your PATH.")
    if os.path.exists(bin_path):
      os.remove(bin_path)
    return False
  except FileNotFoundError:
    print(f"[Error] '{objcopy}' executable not found in system PATH.")
    if os.path.exists(bin_path):
      os.remove(bin_path)
    return False

  # Step 2: Define J-Link Commander script using 'loadbin' at STM32F4 flash base (0x08000000)
  script_content = f"""
    r
    h
    loadbin "{abs_bin_path}", 0x08000000
    r
    g
    qc
    """

  # Create a temporary J-Link script file
  with tempfile.NamedTemporaryFile(
      mode="w", suffix=".jlink", delete=False
  ) as f:
    f.write(script_content)
    script_filename = f.name

  try:
    cmd = [
        jlink_path,
        "-device",
        device,
        "-if",
        "SWD",
        "-speed",
        str(speed),
        "-CommanderScript",
        script_filename,
    ]

    print(f"Flashing binary to {device} at 0x08000000...")
    subprocess.run(cmd, check=True)
    print("\n[Success] Flashing completed successfully!")
    success = True

  except subprocess.CalledProcessError as e:
    print(f"\n[Error] J-Link execution failed with exit code {e.returncode}")
    success = False
  finally:
    # Clean up temporary files
    if os.path.exists(script_filename):
      os.remove(script_filename)
    if os.path.exists(bin_path):
      os.remove(bin_path)

  return success


if __name__ == "__main__":
  parser = argparse.ArgumentParser(
      description=(
          "Flash an ELF file to an STM32 board by converting to binary via"
          " J-Link."
      )
  )
  parser.add_argument("elf", help="Path to the target .elf file")
  parser.add_argument(
      "--device",
      default="STM32F407VG",
      help="Target MCU device (default: STM32F407VG)",
  )
  parser.add_argument(
      "--speed",
      type=int,
      default=4000,
      help="SWD communication speed in kHz (default: 4000)",
  )
  parser.add_argument(
      "--jlink",
      default="JLink.exe",
      help="Path to JLink.exe",
  )
  parser.add_argument(
      "--objcopy",
      default="arm-none-eabi-objcopy",
      help="Path to arm-none-eabi-objcopy executable",
  )

  args = parser.parse_args()
  flash_elf(args.elf, args.device, args.speed, args.jlink, args.objcopy)