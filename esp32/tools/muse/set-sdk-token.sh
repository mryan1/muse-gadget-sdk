#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Set the Muse SDK token in one board's generated sdkconfig (gitignored),
# reading it without echo so it never lands in a terminal log or history:
#   tools/muse/set-sdk-token.sh [profile]     # default: m5stack-atom-voices3r
# The profile is the overlay suffix, as in build-muse-<profile>/. Build the
# board once first so that sdkconfig exists, then build again after this.
# The token comes from gadgets.muse.ai > Account > SDK tokens, or from a
# Linux Muse gadget already set up with it (the inky device):
#   sudo cat /var/lib/musegadget/sdk_token
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
profile=${1:-m5stack-atom-voices3r}
cfg="$root/build-muse-$profile/sdkconfig"
[ -f "$cfg" ] || { echo "no $cfg: build the board once first (tools/muse/board.sh build ...)" >&2; exit 1; }
read -rsp 'Paste your Muse SDK token (mgst_...), then Enter: ' tok; echo
case "$tok" in mgst_*) ;; *) echo "That doesn't start with mgst_; nothing written." >&2; exit 1 ;; esac
case "$tok" in *[!A-Za-z0-9_.-]*) echo "Unexpected characters; nothing written." >&2; exit 1 ;; esac
TOK="$tok" python3 - "$cfg" <<'EOF'
import os, re, sys
p = sys.argv[1]
s = open(p).read()
s, n = re.subn(r'^CONFIG_GADGET_SDK_TOKEN=.*$', 'CONFIG_GADGET_SDK_TOKEN="%s"' % os.environ["TOK"], s, flags=re.M)
if n != 1:
    sys.exit("CONFIG_GADGET_SDK_TOKEN line not found in " + p)
open(p, "w").write(s)
EOF
echo "Saved token ${tok:0:9}...${tok: -3} to build-muse-$profile/sdkconfig."
