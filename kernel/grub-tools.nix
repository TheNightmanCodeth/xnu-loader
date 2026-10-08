{ stdenv
, lib
, fetchurl
, bison
, flex
, python3
  # cc wrapper for an x86 ELF target, which compiles grub's modules
, targetCc
  # grub's cpu and firmware: i386 and pc for BIOS, x86_64 and efi for UEFI
, target ? "i386"
, platform ? "pc"
}:

# grub-mkimage and the modules it packs, for building x86 grub images on a
# machine that is not x86 linux. nixpkgs' grub always targets its own host,
# so it only makes these images on x86_64-linux.
let
  prefix = targetCc.targetPrefix;
in
stdenv.mkDerivation {
  pname = "grub-tools-${target}-${platform}";
  version = "2.12";

  src = fetchurl {
    url = "mirror://gnu/grub/grub-2.12.tar.xz";
    hash = "sha256-88lzkffE6qZ3p44JDH6X5txHsW9lXwRoPr03vvf+D6o=";
  };

  nativeBuildInputs = [ bison flex python3 targetCc targetCc.bintools ];

  # missing from the 2.12 release tarball
  postPatch = ''
    echo "depends bli part_gpt" > grub-core/extra_deps.lst
  '';

  hardeningDisable = [ "all" ];

  configureFlags = [
    "--target=${target}"
    "--with-platform=${platform}"
    "--disable-werror"
    "--disable-nls"
    "--disable-grub-mkfont"
    "--disable-grub-themes"
    "--disable-grub-mount"
    "--disable-device-mapper"
    "--disable-libzfs"
    "TARGET_CC=${prefix}cc"
    "TARGET_OBJCOPY=${prefix}objcopy"
    "TARGET_STRIP=${prefix}strip"
    "TARGET_NM=${prefix}nm"
    "TARGET_RANLIB=${prefix}ranlib"
  ];

  enableParallelBuilding = true;

  meta = {
    description = "grub tools and ${target}-${platform} modules";
    license = lib.licenses.gpl3Plus;
  };
}
