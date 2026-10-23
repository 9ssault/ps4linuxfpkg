#!/bin/sh
# Sourced by init via run_hooks. Keep this tiny: the real work lives in
# /bin/ps4li-auto.sh, which exits early unless a ps4li.conf job is present.

if [ -f /bin/ps4li-auto.sh ]; then
	sh /bin/ps4li-auto.sh
fi

return 0
