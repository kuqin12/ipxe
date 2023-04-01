#!/bin/bash
#
# Copyright (c) Microsoft Corporation. All rights reserved
#
# Bash script to setup CC arm-tf development environment
#

cd src
make veryclean
# make bin/8086100e.rom -j 4 CONFIG=qemu
make bin-x86_64-efi/8086100e.efidrv -j 4 CONFIG=qemu
/home/test/mu_tiano_platforms/MU_BASECORE/BaseTools/Bin/Mu-Basetools_extdep/Linux-x86/EfiRom -f "0x8086" -i "0x100e" -l 0x02 -ec bin-x86_64-efi/8086100e.efidrv -o bin-x86_64-efi/8086100e.efirom
objdump -d bin-x86_64-efi/8086100e.efidrv.tmp > ../out_dism.log
