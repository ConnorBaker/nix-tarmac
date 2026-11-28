#!/usr/bin/env bash
# fetch_error.sh PLUGIN_DIR WORK: a tarball download that fails reports
# the download's error, as the builtin fetcher does, not the archive error
# libarchive makes of the failed read ("Truncated tar archive detected
# while reading data"). Offline: a local server sends half of the body
# it announced and closes the connection, without Accept-Ranges, so Nix
# cannot resume the transfer and the download fails.
set -euo pipefail
plugin=$1
work=$2
mkdir -p "$work/src"
# incompressible, so the half that arrives ends inside a file's data
head -c 1048576 /dev/urandom >"$work/src/blob"
tar -C "$work" -czf "$work/src.tar.gz" src

cat >"$work/server.py" <<'EOF'
import http.server, sys

body = open(sys.argv[1], "rb").read()

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.path == "/whole.tar.gz":
            self.wfile.write(body)
        else:
            self.wfile.write(body[: len(body) // 2])
            self.wfile.flush()
            self.close_connection = True

    def log_message(self, *args):
        pass

srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
EOF
coproc SERVER { exec python3 "$work/server.py" "$work/src.tar.gz"; }
read -r port <&"${SERVER[0]}"
trap 'kill $SERVER_PID 2>/dev/null' EXIT

fetch() { echo "(builtins.fetchTree { type = \"tarball\"; url = \"http://127.0.0.1:$port/$1\"; }).narHash"; }
common=(--extra-experimental-features 'nix-command flakes' --impure --raw
  --store "local?root=$work/root")

# the plugin's fetcher is the one fetching: the whole tarball lands in its store
XDG_CACHE_HOME=$work/cache-whole nix eval "${common[@]}" --plugin-files "$plugin" \
  --expr "$(fetch whole.tar.gz)" >/dev/null
[[ -e $work/cache-whole/nix/tarmac/pack.0 ]] || {
  echo "FAIL: the plugin did not fetch the tarball" >&2
  exit 1
}

for fetcher in builtin plugin; do
  plugins=()
  [[ $fetcher == plugin ]] && plugins=(--plugin-files "$plugin")
  if XDG_CACHE_HOME=$work/cache-$fetcher nix eval "${common[@]}" "${plugins[@]}" \
    --expr "$(fetch half.tar.gz)" >/dev/null 2>"$work/$fetcher.log"; then
    echo "FAIL $fetcher: fetching half a tarball succeeded" >&2
    exit 1
  fi
  grep -q "unable to download 'http://127.0.0.1:$port/half.tar.gz'" "$work/$fetcher.log" || {
    echo "FAIL $fetcher: the download's error is not reported:" >&2
    cat "$work/$fetcher.log" >&2
    exit 1
  }
done
echo "OK: a failed download reports the download's error"
