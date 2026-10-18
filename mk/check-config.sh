#!/bin/sh
# Verify that every option requested by the config fragments survived
# olddefconfig (Kconfig silently drops values whose dependencies are unmet).
# Usage: check-config.sh <.config> <fragment>...
set -eu

config=$1
shift
status=0

for frag in "$@"; do
	while IFS= read -r line; do
		case $line in
		CONFIG_*=*)
			if ! grep -qxF -- "$line" "$config"; then
				sym=${line%%=*}
				actual=$(grep -E "^(# )?$sym[= ]" "$config" || echo "$sym undefined")
				echo "$frag: wanted '$line', got '$actual'" >&2
				status=1
			fi
			;;
		"# CONFIG_"*" is not set")
			sym=${line#\# }
			sym=${sym%% *}
			if grep -q "^$sym=" "$config"; then
				echo "$frag: wanted '$sym' off, got '$(grep "^$sym=" "$config")'" >&2
				status=1
			fi
			;;
		esac
	done < "$frag"
done

exit $status
