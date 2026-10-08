{
  description = "UEFI NVMe boot test package";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    systems = [ "x86_64-linux" "x86_64-darwin" "aarch64-linux" "aarch64-darwin" ];
    forAllSystems = nixpkgs.lib.genAttrs systems;
  in {
    packages = forAllSystems (system: let
      pkgs = import nixpkgs { inherit system; };
      # x86 targets are cross built from other hosts, like the arm ones; same-platform
      # pkgsCross is the native set. ia32 keeps multilib on x86_64-linux.
      x86 = pkgs.pkgsCross.gnu64;
      i686 = if system == "x86_64-linux" then pkgs.pkgsi686Linux else pkgs.pkgsCross.gnu32;
      # grub-mkimage runs here but packs x86 modules, which nixpkgs grub only has on x86_64-linux
      grubTools = target: platform: pkgs.callPackage ./kernel/grub-tools.nix {
        inherit target platform;
        targetCc = x86.stdenv.cc;
      };
      grubPc = if system == "x86_64-linux" then pkgs.grub2 else grubTools "i386" "pc";
      grubEfi = if system == "x86_64-linux" then pkgs.grub2_efi else grubTools "x86_64" "efi";
    in {
      default = x86.callPackage ./. {};
      arm64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "bcm2837";
      };
      arm64-virt = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "qemuvirt";
      };
      # Allwinner H616/H618 - Orange Pi Zero 3.
      arm64-sun50i = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "sun50i";
      };
      arm64-sc8280xp = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./. {
        arch = "aarch64";
        platform = "sc8280xp";
      };
      ia32 = i686.callPackage ./. {
        arch = "x86_64";
        loaderArch = "ia32";
      };

      legacy-boot = x86.callPackage ./legacy { };
      # The loader as an arm64 Linux Image (U-Boot booti, QEMU -kernel).
      kernel-arm64 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix { };
      kernel-arm64-sun50i = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "sun50i";
      };
      kernel-arm64-sg2002 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "sg2002";
      };
      # any board whose device tree describes it, such as the orange pi zero 4 (a733) which marks on uart0
      kernel-arm64-generic = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
      };
      kernel-arm64-a733 = pkgs.pkgsCross.aarch64-multiplatform.callPackage ./kernel/arm64.nix {
        platform = "generic";
        markUart = "0x02500000";
      };
      # the loader as a riscv64 linux Image (opensbi, u-boot booti, qemu -kernel), clang and lld
      kernel-riscv64 = pkgs.callPackage ./kernel/riscv64.nix {
        compilerRt = pkgs.pkgsCross.riscv64.llvmPackages.compiler-rt;
      };
      # xnu arm32 boot shim as a Linux zImage (QEMU -kernel, U-Boot bootz)
      kernel-arm32 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 { };
      # Luckfox Pico (RV1103/RV1106) and QEMU's virt Cortex-A7, fixed at build time
      arm32 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "rv1106";
      };
      arm32-virt = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "qemuvirt";
      };
      # luckfox lyra (rk3506)
      arm32-rk3506 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "rk3506";
      };
      # allwinner a20 (sun7i), e.g. Banana Pi
      arm32-a20 = pkgs.pkgsCross.armv7l-hf-multiplatform.callPackage ./arm32 {
        platform = "a20";
      };
      # The loader as a Multiboot2 ELF kernel (GRUB etc.).
      kernel-multiboot2 = x86.callPackage ./kernel { };
      kernel-grub-bios = pkgs.callPackage ./kernel/grub-bios.nix {
        grub2 = grubPc;
        loaderKernel = x86.callPackage ./kernel { };
      };
      kernel-grub-efi = pkgs.callPackage ./kernel/grub-efi.nix {
        grub2_efi = grubEfi;
        loaderKernel = x86.callPackage ./kernel { };
      };
    });
  };
}
