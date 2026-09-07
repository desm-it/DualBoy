#!/bin/sh
set -eu

core=${1:?usage: check-libretro-symbols.sh CORE}
required='retro_api_version
retro_cheat_reset
retro_cheat_set
retro_deinit
retro_get_memory_data
retro_get_memory_size
retro_get_region
retro_get_system_av_info
retro_get_system_info
retro_init
retro_load_game
retro_load_game_special
retro_reset
retro_run
retro_serialize
retro_serialize_size
retro_set_audio_sample
retro_set_audio_sample_batch
retro_set_controller_port_device
retro_set_environment
retro_set_input_poll
retro_set_input_state
retro_set_video_refresh
retro_unload_game
retro_unserialize'

actual=$(nm -D --defined-only "$core" | awk '$2 ~ /^[TDBR]$/ { print $3 }' | sort)
missing=
for symbol in $required; do
    if ! printf '%s\n' "$actual" | grep -Fqx "$symbol"; then
        missing="${missing}${missing:+
}${symbol}"
    fi
done

if [ -n "$missing" ]; then
    printf 'missing required exports:\n%s\n' "$missing" >&2
    exit 1
fi

unexpected=$(printf '%s\n' "$actual" | grep -Ev '^retro_' || true)
if [ -n "$unexpected" ]; then
    printf 'unexpected public exports:\n%s\n' "$unexpected" >&2
    exit 1
fi

printf 'Libretro export check passed for %s\n' "$core"
