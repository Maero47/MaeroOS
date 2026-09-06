#!/bin/sh
# Build fbDOOM natively for MaeroOS with the i686-elf toolchain + our libc.
set -e

# work from the project root with RELATIVE paths (the abs path has a space)
cd "$(dirname "$0")/.."
SRC="ports/fbDOOM/fbdoom"
US="userspace"
OBJ="ports/fbDOOM/build-maeros"
OUT="testfiles/doom"

CC=i686-elf-gcc
GCCINC="$(dirname "$(command -v i686-elf-gcc)")/../lib/gcc/i686-elf/$(i686-elf-gcc -dumpversion)/include"
[ -d "$GCCINC" ] || GCCINC=/opt/homebrew/Cellar/i686-elf-gcc/15.2.0/lib/gcc/i686-elf/15.2.0/include

CFLAGS="-std=gnu99 -O2 -g -nostdlib -nostdinc -static -ffreestanding \
 -fno-builtin -fno-pic -fno-pie -mno-sse -mno-mmx -mno-sse2 \
 -fno-omit-frame-pointer -w -DNORMALUNIX -DLINUX \
 -I$US/include -isystem $GCCINC -I$SRC"

# SRC_DOOM from the fbDOOM Makefile, with i_sound → dummy backend and our
# input backend instead of the Linux-console tty one.
SRCS="i_main dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
 d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info \
 i_cdmus i_endoom i_joystick i_scale i_sound_maeros i_system i_timer memio \
 m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random \
 p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj \
 p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick \
 p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 \
 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff \
 w_checksum w_file w_file_stdc_unbuffered w_main w_wad z_zone \
 i_video_fbdev i_input_maeros"

mkdir -p "$OBJ"
for s in $SRCS; do
    if [ "$SRC/$s.c" -nt "$OBJ/$s.o" ] 2>/dev/null || [ ! -f "$OBJ/$s.o" ]; then
        echo "  CC $s.c"
        $CC $CFLAGS -c "$SRC/$s.c" -o "$OBJ/$s.o"
    fi
done

echo "  LD doom"
$CC -nostdlib -static -T "$US/user.ld" -o "$OUT" \
    "$US/libc/crt0.o" "$OBJ"/*.o "$US/libc/libc.a" -lgcc
echo "built: $OUT"
