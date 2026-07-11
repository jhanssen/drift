#!/usr/bin/env bash
# Rebuilds netprobe.wasm from netprobe.wat with wasmtime's wat2wasm —
# the same pinned C API artifact the native build embeds (no wabt
# dependency). Usage: build.sh [path-to-native-build-dir]
set -eu
cd "$(dirname "$0")"
build=${1:-../../../build}
api=$(echo "$build"/3rdparty/wasmtime/wasmtime-*-c-api)
[ -d "$api" ] || { echo "wasmtime C API not found under $build (build the native tree first)" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/w2w.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <wasm.h>
#include <wasmtime.h>
int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: w2w in.wat out.wasm\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* wat = malloc(n);
    if (fread(wat, 1, n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);
    wasm_byte_vec_t out;
    wasmtime_error_t* err = wasmtime_wat2wasm(wat, n, &out);
    if (err) {
        wasm_name_t msg; wasmtime_error_message(err, &msg);
        fprintf(stderr, "%.*s\n", (int)msg.size, msg.data);
        return 1;
    }
    FILE* o = fopen(argv[2], "wb");
    fwrite(out.data, 1, out.size, o);
    fclose(o);
    printf("%s: %zu bytes\n", argv[2], out.size);
    return 0;
}
EOF
cc -o "$tmp/w2w" "$tmp/w2w.c" -I"$api/include" -L"$api/lib" -lwasmtime \
    -Wl,-rpath,"$api/lib"
"$tmp/w2w" netprobe.wat netprobe.wasm
