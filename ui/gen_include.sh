#!/bin/sh
# gen_include.sh - CSS/HTML を C文字列リテラルの .h ファイルに変換する
# 使い方: ./gen_include.sh demo.css demo.html

set -e

to_c_string_h() {
    input="$1"
    output="${input}.h"

    # 以前は各行につき sed を2プロセス起動していたため、大きなスキン対応
    # 文書ではプロセス制限に達した。変換順を保ったまま1回で生成する。
    awk '{
        gsub(/\\/, "\\\\")
        gsub(/"/, "\\\"")
        print "    \"" $0 "\\n\""
    }' "$input" > "$output"

    printf 'Generated: %s\n' "$output"
}

if [ $# -eq 0 ]; then
    to_c_string_h demo.css
    to_c_string_h demo.html
else
    for f in "$@"; do
        to_c_string_h "$f"
    done
fi
