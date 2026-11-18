# BananaPi BPI-R4: EDK2 (UEFI) + VyOS on SD, SPI-NAND and eMMC

This tree builds Tianocore EDK2 firmware for the BananaPi BPI-R4 (MediaTek
MT7988A, 4/8 GB) and uses it to run VyOS arm64. It covers:

- EDK2 that boots from **SD card, eMMC or SPI-NAND**, with the matching Linux
  device tree.
- **USB mass-storage boot** in EDK2: the VyOS ISO on a USB stick.
- **Permanent MAC addresses**, kept in the on-board EEPROM and patched into the
  device tree by EDK2.
- A **permanent VyOS install on the eMMC**.

This README is the complete record:
how to rebuild everything, how the board was brought to its current state,
and what is still open. The last section lets someone pick up the work.

---

## 1. Boot chain and hardware facts

```
BootROM -> ATF BL2 (DRAM init) -> FIP: BL31 + EDK2 (BL33, FD at 0x41E00000)
        -> GRUB arm64-efi (EFI/BOOT/BOOTAA64.EFI) -> Linux (VyOS)
```

Facts that are easy to get wrong (all verified on the board):

| Topic | Fact |
|---|---|
| **SW3** | Selects the BootROM boot medium, **and also switches the SD/eMMC data lines in hardware**. With SW3 on SD the eMMC does not answer at all (Linux: `CMD1 ... timeout`, `Failed to initialize a non-removable card`), whatever device tree is used. The eMMC is only reachable when booting from SPI-NAND or eMMC. The guide lists SD 0/0, NAND 0/1, eMMC 1/0; another source gives SD as 1/1. Check the silkscreen. |
| MSDC0 | SD and eMMC share controller MSDC0 on different pins: SD on GPIO32-37 (function 5), eMMC on GPIO38-49 (function 1, `emmc_51`). Linux needs the `-sd` or `-emmc` overlay (`mmc0` is disabled in the base DTB). |
| Boot strap | No documented register exposes the latched strap. GPIO38 and GPIO24 are in function 1 **after reset**, so their mux says nothing. |
| SPI-NAND | Winbond, 256 MiB, 2 KiB pages. The DT (and OpenWrt) use mtd0 `bl2` (2 MiB, read-only in the DT) and mtd1 `ubi` (126 MiB). BL2 is stored **4 times at 512 KiB intervals** (BootROM fallbacks); the FIP sits in the UBI volume `fip`. |
| eMMC | 7.28 GiB (`8GTF4R`, manfid 0x15). BL2 lives in **boot0**, which is the boot partition per EXT_CSD[179] PARTITION_CONFIG = 0x48 (boot0 + boot ACK), already set from the factory. BL2 finds the FIP by **GPT partition name `fip`**. |
| EEPROM | Atmel 24c02 (256 B, 8-byte pages) at I2C 0x57, behind a PCA9545 mux (0x70, channel 0) on I2C2 (`0x11005000`, `mediatek,mt7981-i2c`). Ships blank. The mux reset is **GPIO5, active low; it comes out of reset as SMI_0_MDC (function 1)**, which holds the mux in reset. |
| MACs | Nothing on the board provides MACs: the EEPROM is blank, the efuse holds only calibration, and the SD and eMMC `factory` partitions were zeros. Without the EEPROM scheme Linux uses random MACs on every boot. |
| TRNG | BL31 SiP call `0xC2000550` (`MTK_SIP_TRNG_GET_RND`): `x0` = 0 on success, `x1` = 32 random bits. |
| USB | The USB-A port is xHCI `usb@11200000` (IPPC at `0x11203e00`) with T-PHY `0x11c50000` (USB2) and `0x11c50700` (USB3). A VIA VL822 hub sits on the board behind it. VBUS has no switch. |
| UART | 8250-compatible at `0x11000000`, so `console=ttyS0,115200` and `earlycon=uart8250,mmio32,0x11000000`. **Not** ttyAMA0. |

---

## 2. Tree layout

| Path | Contents |
|---|---|
| `edk2/edk2` | submodule, tianocore/edk2 @ `2970e56` (+ `patches/edk2-*.patch`, applied by `mkedk2.sh`) |
| `edk2/edk2-platforms` | submodule, tianocore/edk2-platforms @ `a46be4f` (unchanged) |
| `edk2/bpi-r4-platform` | **the BPI-R4 port** |
| `arm-trusted-firmware-mtk` | submodule, mt798x-platform/arm-trusted-firmware-mtk @ `e09077068` (unchanged) |
| `mkdtb.sh` | extracts the base DTB and overlays from the ISO's kernel, writes `dtb/*-sd.dtb` and `dtb/*-emmc.dtb` |
| `mkedk2.sh` | builds EDK2 and the three BL2/FIP variants (section 3) |
| `mkgrub.sh` | builds `grub/bootaa64.efi` (standalone arm64-efi, includes the `chain` and `memrw` modules) |
| `mksdimg.sh` | builds a complete SD image (GPT + BL2 + FIP + ESP) |
| `unpack-iso.sh` | unpacks the newest `vyos-*arm64.iso` to `/tmp/vyos-iso` |
| `grub/grub.cfg` | GRUB menu for the SD ESP |
| `usb/grub.cfg` | GRUB menu for the USB stick; the rest of `usb/` is assembled from build output (section 5) |
| `tools/bpi-r4-mac` | show or set the MAC addresses from Linux |
| `tools/flash-nand.sh` | writes BL2 and the FIP to SPI-NAND and verifies both |
| `patches/edk2-xhci-mtk-sch.patch` | EDK2 core patch (MediaTek xHCI) |

Clone with `git clone --recurse-submodules`, or run
`git submodule update --init --recursive` in an existing clone. Build output and
the VyOS ISO are listed in `.gitignore`.

### EDK2 port (`edk2/bpi-r4-platform`)

```
Platform/BananaPi/BpiR4/
  BpiR4.dsc / BpiR4.fdf          platform description; FD = 1.5 MiB
  BpiR4Pkg.dec                   board package: BpiR4MacLib, DTB file GUIDs, formset GUID
  BpiR4-sd.dtb / BpiR4-emmc.dtb  copied in by mkedk2.sh, embedded as FREEFORM files
  Library/BpiR4MacLib            EEPROM MAC record, TRNG generation, port table
  Library/BpiR4DtbLoaderLib      DtPlatformDtbLoaderLib: DTB by boot medium, MAC
                                 fixup, diagnostics under /chosen
  Drivers/BpiR4MacConfigDxe      Setup page "BPI-R4 MAC Addresses" (Device Manager)
Silicon/MediaTek/MT7988Pkg/
  Include/MT7988.h               clock/GPIO/I2C register map (with sources)
  Library/MT7988BootMediumLib    boot medium detection + eMMC pin setup for NAND boot
  Library/MtkI2cLib              polled DMA-mode I2C2 driver (ported from U-Boot mtk_i2c)
  Drivers/MtkMsdcDxe             SD + eMMC (PIO)
  Drivers/MtkXhciDxe             USB clocks, T-PHY, IPPC, non-discoverable xHCI, port power
```

---

## 3. Building

### Prerequisites (Debian 12 host)

```bash
sudo apt install gcc-aarch64-linux-gnu bison flex bc make xxd libssl-dev \
                 libgnutls28-dev uuid-dev device-tree-compiler \
                 python3-pyelftools mtools dosfstools fdisk xz-utils \
                 u-boot-tools
```

`dtc`/`fdtoverlay` come from `device-tree-compiler`, and `mkimage` (which ATF
uses to wrap BL2) from `u-boot-tools`. `mkgrub.sh` also needs
`grub-mkstandalone` (grub-common) and downloads the arm64 GRUB modules from
deb.debian.org.

### Steps

```bash
cd /vyos/bpi-r4
./unpack-iso.sh            # newest vyos-*arm64.iso -> /tmp/vyos-iso
./mkdtb.sh                 # dtb/mt7988a-bananapi-bpi-r4-{sd,emmc}.dtb
./mkedk2.sh                # EDK2 + 3x ATF; VERBOSE=1 for the full UART log
./mkgrub.sh                # grub/bootaa64.efi (only after changing modules)
./mksdimg.sh               # optional: complete SD image bpi-r4-edk2.img
```

What `mkedk2.sh` does:
- applies `patches/edk2-*.patch` to `edk2/edk2` unless already applied
- copies both DTBs into the platform directory
- builds `Platform/BananaPi/BpiR4/BpiR4.dsc` (DEBUG)
- builds ATF three times, all with `DRAM_USE_COMB=1 DDR4_4BG_MODE=1`, each in
  its own `BUILD_BASE`:

| Medium | `BOOT_DEVICE` | Output (`arm-trusted-firmware-mtk/.../mt7988/release/`) |
|---|---|---|
| SD | `sdmmc` | `build-edk2/{bl2.img,fip.bin}` (BL2 header `SDMMC_BOOT`) |
| eMMC | `emmc` | `build-edk2-emmc/{bl2.img,fip.bin}` (`EMMC_BOOT`) |
| SPI-NAND | `spim-nand UBI=1` | `build-edk2-nand/{bl2.img,fip.bin}` (`SPINAND!`) |

`spim-nand` leaves UBI off unless `UBI=1` is given. The script checks for
DDR4 4BG mode in BL2 and for FIP <= 2 MiB.

Builds use `TARGET=DEBUG` (errors still reach the UART). `VERBOSE=1` prints
the whole DXE/BDS log; it was needed for bring-up but is too noisy for
everyday use.

---

## 4. Installing the firmware per boot medium

### SD card

GPT layout (see `mksdimg.sh`): `bl2` (LBA 34, attribute LegacyBIOSBootable),
`u-boot-env`, `factory`, `fip` (LBA 13312, 2 MiB) and `ESP` (FAT32, GRUB +
live VyOS). Updating only the FIP on a running system:

```bash
dd if=fip.bin of=/dev/mmcblk0p4 bs=512 conv=fsync     # p4 = "fip"
```

### SPI-NAND (OpenWrt layout, other UBI volumes are kept)

1. Make NAND `bl2` writable: boot a device tree without `read-only` on
   `/soc/spi@11007000/flash@0/partitions/partition@0`
   (`usb/mt7988a-bananapi-bpi-r4-sd-nandrw.dtb`, GRUB entry "NAND bl2
   writable" on the stick).
2. Install `mtd-utils` (Debian's `mtd-utils_2.1.5-1_arm64.deb`). dpkg complains
   about the missing `libiniparser1`, which only `ubinize` needs; `flash_erase`,
   `nandwrite` and `ubiupdatevol` work. They are in `/usr/sbin`, which isn't
   on a non-root user's PATH, so run them with sudo.
3. Build the BL2 image with 4 copies:
   ```bash
   REL=arm-trusted-firmware-mtk/build-edk2-nand/mt7988/release
   for i in 0 1 2 3; do dd if=$REL/bl2.img of=nand-bl2-4x.bin bs=512k seek=$i conv=sync,notrunc; done
   ```
4. `sudo sh tools/flash-nand.sh nand-bl2-4x.bin nand-fip.bin`. It checks that
   `bl2` is writable and all 4 headers are present, writes BL2 (erase +
   nandwrite) and the FIP (`ubiupdatevol` into `fip`), and verifies both by
   reading them back.

At boot, NAND BL2 prints `UBI: Bad EC magic in block 2018..2031`. That is
harmless: the scan runs to the end of the chip (`UBI_END_ADDR = 0`) while
UBI only covers 128 MiB. It then finds `Volume fip (Id #0)`. See section 8.

### eMMC

```bash
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=emmc-bl2.img of=/dev/mmcblk0boot0 conv=fsync
echo 1 > /sys/block/mmcblk0boot0/force_ro
# FIP: GPT partition named "fip" in the user area (section 6, step 5)
```

Check PARTITION_CONFIG: `cat /sys/kernel/debug/mmc0/mmc0:0001/ext_csd | cut -c 359-360`
should read `48` (boot0). Otherwise, `mmc bootpart enable 1 1 /dev/mmcblk0`
(mmc-utils).

---

## 5. USB stick (VyOS ISO, EDK2 USB boot)

FAT partition with:

```
/vyos-2026.11.02-1213-bpi-R4-generic-arm64.iso     the ISO, unchanged, as a file
/EFI/BOOT/BOOTAA64.EFI                              grub/bootaa64.efi
/EFI/BOOT/grub.cfg                                  usb/grub.cfg
/bpi-r4/                                            everything else from usb/
```

`usb/grub.cfg` loop-mounts the ISO (`findiso=`, `toram`) **without a
`devicetree` line**, so EDK2's DTB (medium + MACs) applies. The entry "NAND
bl2 writable" loads the special DTB for flashing NAND (no MACs).

GRUB's embedded config (`grub/grub-embedded.cfg`) uses the first disk that
has `/EFI/BOOT/grub.cfg`. With SD and stick both present that may be either;
in NAND mode only the stick is visible.

---

## 6. Installing VyOS on the eMMC

The eMMC can only be reached in NAND or eMMC mode, so the route goes through
NAND:

1. **NAND firmware** (once, booted from SD): section 4 "SPI-NAND". Back up
   mtd0 and every UBI volume first.
2. **SW3 to NAND**, stick in, power on. EDK2 reports SPI-NAND, sets up the
   eMMC pins itself, installs the `-emmc` DTB with MACs, and boots the stick.
   Linux: `mmcblk0` type MMC, `mmcblk0boot0/1` present.
3. **Back up the eMMC** (boot0/boot1, GPT, occupied area), then write the eMMC
   BL2 into boot0 (section 4 "eMMC").
4. **`install image`** on the console as an admin user (**never as root**),
   console **S**. The cmdline must contain `console=ttyS0,115200`, because the
   installer takes the serial console from there (`vyos.utils.kernel.get_kernel_serial_console`).
5. **FIP partition.** `install image` creates p1 "BIOS boot" (2048-4095,
   EF02), p2 EFI (256 MiB) and p3 root. On aarch64 VyOS never uses p1
   (`grub-install --target=i386-pc` only runs on x86_64), so it becomes the
   FIP partition:
   ```bash
   sudo sgdisk -d 1 /dev/mmcblk0
   sudo sgdisk -a 1 -n 1:34:4095 -t 1:8300 -c 1:fip /dev/mmcblk0
   sudo partx -u --nr 1 /dev/mmcblk0          # BLKRRPART reports "busy"
   sudo dd if=emmc-fip.bin of=/dev/mmcblk0p1 bs=512 conv=fsync
   ```
   34-4095 is 1.98 MiB, and the FIP is 1.54 MiB. Answering "use all free
   space" with "no" in the installer would leave room at the end instead.
6. **SW3 to eMMC**, stick out. The installer writes `EFI/BOOT/BOOTAA64.EFI`
   (`--force-extra-removable`); EDK2 has no NVRAM and boots that path.

Result: `show version` reports `Boot via: installed image`, persistence on
`/dev/mmcblk0p3`, and the eMMC DTB comes from EDK2.

---

## 7. MAC addresses

### EEPROM record (16 bytes at offset 0)

| Offset | Contents |
|---|---|
| 0x00-0x05 | base MAC (raw, so a Linux nvmem `mac-base` cell could read offset 0) |
| 0x06 | layout version = 1 |
| 0x07 | flags, bit 0 = generated by firmware |
| 0x08-0x0B | magic `R4MA` |
| 0x0C-0x0F | CRC32 (little-endian, zlib/EDK2 `CalculateCrc32`) over 0x00-0x0B |

### Boot behaviour (BpiR4MacLib / BpiR4DtbLoaderLib)

- **Valid record:** use it.
- **Blank (all 0xff):** take 6 bytes from the TRNG, set byte0 `|= 0x02`
  (locally administered), `&= ~0x01` (unicast), byte5 `&= ~0x07`, store the
  record and verify it.
- **Unknown or corrupt:** never overwritten. A random MAC is used for that
  boot only.
- **TRNG failure:** nothing is written and no MACs are set, so Linux uses
  random ones.

Port assignment (`mac-address` in the DT):

| Offset | DT node | Port |
|---|---|---|
| +0 | `/soc/ethernet@15100000/mac@0` | gmac0, switch conduit |
| +1..+4 | `/soc/switch@15020000/ports/port@0..3` | wan, lan1, lan2, lan3 |
| +5 | `/soc/ethernet@15100000/mac@1` | gmac1, SFP |
| +6 | `/soc/ethernet@15100000/mac@2` | gmac2, SFP |

Linux `ethN` names move around (seen: mac@0 as eth6 and as eth4), so match by
MAC.

### Changing the MACs

- **Firmware:** EDK2 Setup, Device Manager, "BPI-R4 MAC Addresses". You can
  set a base MAC, generate a random one, or erase the record. Changes apply
  from the next boot.
- **Linux:** `sudo tools/bpi-r4-mac show | set xx:xx:xx:xx:xx:xx | clear`. It
  finds the at24 nvmem device with `of_node` `eeprom@57`.

### Diagnostics without a UART

EDK2 writes into the DT:
- `/proc/device-tree/chosen/bpi-r4,mac-source`: `user`, `generated`,
  `volatile` or `none`
- `/proc/device-tree/chosen/bpi-r4,mac-diag`: failed EEPROM steps with the
  I2C status (empty means OK)

Example from the bring-up: `mux:No Response/3`, i.e. the PCA9545 NACKed
because of GPIO5.

GRUB's `devicetree` command replaces the EDK2 DTB and so drops the MACs. Only
the installer and override entries use it.

### Port topology: DSA switch and its conduit

The four RJ45 ports belong to the SoC's built-in switch (`switch@15020000`,
driver `mt7530-mmio`). The switch has no host interface of its own: it hangs
off gmac0 (`mac@0`, driver `mtk_soc_eth`), which Linux uses as the **DSA
conduit**. The SFP+ cages are wired directly to gmac1/gmac2.

```
mtk_soc_eth  mac@0 (conduit, no jack) <--mtk tag--> switch CPU port
                                                      port@0..3: wan, lan1-3
             mac@1, mac@2: SFP+ (not behind the switch)
```

Verified on the board (Linux 6.18, eMMC boot, conduit seen as eth4):

- `ip -d link` shows the user ports as `ethN@<conduit>` with
  `dsa conduit <conduit>`; `/sys/class/net/<conduit>/dsa/tagging` is `mtk`
  (4-byte MediaTek tag that carries the switch port).
- DSA is switchdev: the user ports have `phys_switch_id` `00000000` and
  `phys_port_name` `p0`-`p3`. The conduit and the SFP MACs have neither.
- Kernel: `CONFIG_NET_SWITCHDEV`, `CONFIG_NET_DSA`, `CONFIG_NET_DSA_MT7530`,
  `CONFIG_NET_DSA_MT7530_MMIO` and `CONFIG_NET_DSA_TAG_MTK` are all `=y`.

Consequences:

- **Leave the conduit up and unconfigured.** A DSA user port cannot be up
  while its conduit is down, so disabling it takes wan and lan1-3 with it.
  Addresses, VLANs and firewall rules go on the user ports.
- Every frame between the CPU and wan/lan1-3 crosses the single conduit link,
  including traffic routed between two switch ports. The SFP+ ports do not
  share it.
- Without a bridge the switch keeps the ports isolated and sends everything
  to the CPU. Bridging user ports should offload L2 forwarding into the
  switch (FDB entries marked `offload` in `bridge fdb show`, conduit counters
  idle for LAN-to-LAN traffic). **Not tested yet.**

---

## 8. Bring-up notes (problems found and their fixes)

1. **eMMC in MtkMsdcDxe.** CMD6/CMD8 mean different things for SD and eMMC.
   MmcDxe sends CMD1 first, so a successful CMD1 switches the driver to eMMC
   semantics until the next CMD0: CMD8 = SEND_EXT_CSD (data), CMD6 = SWITCH
   (R1b). DDR timings are refused, so MmcDxe falls back to SDR HS52. The bus
   is limited to 25 MHz (PcdMsdcMaxBusClockHz).
2. **Boot medium detection.** The first version looked at GPIO38/GPIO24; both
   are function 1 after reset, so it misdetected. Now:
   - GPIO32 = function 5: SD
   - eMMC pad pulls R0/R1 = 0xBFE/0x401 (what eMMC BL2 writes): eMMC
   - anything else: SPI-NAND

   In NAND mode EDK2 sets up the eMMC pins itself (ATF `mmc_gpio_setup`
   values). MSDC clocks stay at their reset defaults (ATF doesn't touch them
   either).
3. **EEPROM unreachable.** The PCA9545 NACKed because GPIO5 (mux reset) is
   SMI_0_MDC after reset. Fix: force it to GPIO mode, output high, wait 1 ms.
4. **USB: ports unpowered.** MT7988 xHCI has PPC=1 (HCCPARAMS1 `0x1403F99`),
   so after HCRST every PORTSC has PP=0. EDK2 XhciDxe ignores
   `EfiUsbPortPower` ("Not supported"). Fix in MtkXhciDxe: after
   `EFI_USB2_HC_PROTOCOL` is installed, set PP on all root ports (write
   mask `0x4E00FFE9`, like Linux `xhci_port_state_to_neutral`).
5. **USB: nothing behind the on-board hub.** MediaTek xHCI reads periodic
   bandwidth parameters from the endpoint-context dwords 5/6 (Linux
   `xhci-mtk-sch.c`). With zeros the hub's interrupt endpoint is never
   serviced. Fix: `patches/edk2-xhci-mtk-sch.patch` sets `RsvdZ5 = 1` (bpkts 1,
   offset 0) for interrupt endpoints, guarded by `-DXHCI_MTK_SCH`, which only
   the BPI-R4 DSC passes to XhciDxe.
6. **GRUB cmdline truncated.** `set console=console=tty0 console=ttyS0,115200`
   without quotes keeps only `console=tty0`. The same went for `common`.
   Without `console=ttyS0,...` `install image` finds no serial console. Fix:
   quote all multi-word values.
7. **GRUB on arm64.** There is no serial terminal (`com0`); the console goes
   through EFI. `chainloader` is in module `chain`, `read_dword` in `memrw`.
   The SD menu has "Debug: USB controller registers" and "UEFI Shell".

Verified on hardware:
- SD boot with EEPROM MACs (stable across several reboots)
- EEPROM generation on a blank EEPROM, plus the Linux tool `show`
- EDK2 USB boot of the stick in SD and NAND mode
- NAND boot (BL2 from UBI) with eMMC detection
- eMMC boot of the installed VyOS (also proves the EDK2 eMMC driver)

**Not yet tested:**
- MAC Setup page (HII)
- `bpi-r4-mac set/clear` on the board (only tested against a test file)
- corrupt-record path
- `mksdimg.sh` with the current changes
- RELEASE build

---

## 9. Open items

- NAND BL2: `OVERRIDE_UBI_END_ADDR=0x8000000` in the NAND variant of
  `mkedk2.sh`, so the scan stops at the UBI end (removes the "Bad EC magic"
  noise and speeds up the boot).
- Test the MAC Setup page and `bpi-r4-mac set/clear` on the board.
- Decide between DEBUG and RELEASE build.
- Optional: USB keyboard (UsbKbDxe) for the EDK2 Setup without a UART.
- MTK xHCI periodic scheduling is minimal (offset 0). That is enough for hub +
  stick, but not a full port of `xhci-mtk-sch.c`.

---

## 10. Picking the work up again

The state of the work is in sections 8 and 9. A fresh session needs:

- **VyOS CLI:** never use `configure`/`commit` as root.
- **Builds:** run section 3 in this directory. After a firmware change:
  - SD: `dd` the FIP to `/dev/mmcblk0p4`, booted from SD
  - eMMC: `dd` the eMMC FIP to `/dev/mmcblk0p1`, booted from eMMC or NAND
  - NAND: `tools/flash-nand.sh` (NAND-writable DT)
- **Bring-up debugging without a UART:** the `/chosen` properties (section 7)
  and the GRUB register entry (`read_dword`).
- **Recovery:** SW3 to SD boots the SD card (unchanged EDK2 SD firmware).
  SW3 to NAND boots EDK2 from NAND and then the USB stick.
