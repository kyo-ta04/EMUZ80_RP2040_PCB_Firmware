# ユーティリティ コマンド

Z80やCP/Mで遊ぶのに、あると便利な簡単なコマンド類です。
CP/Mでも Windows PC(GCC)でも使えるので重宝します。

## ビルド環境

### CP/M 80環境 (HI-TECH-C Z80)

- コンパイラ: HI-TECH-C Z80
- ターゲット: Z80 CP/M

### Windows環境 (GCC/MinGW64)

- コンパイラ: GCC 15.2.0 (MinGW-Builds project)
- ターゲット: x86_64-win32-seh

## コマンド
- cat - Concatenate and print files ファイルの内容を表示するコマンドです。
- cmp - Compare two files 2つのファイルを比較するコマンドです。
- hd - Hex Dump シンプルな16進ダンプコマンドです。
- head - Display the first lines of files ファイルの先頭部分を表示するコマンドです。
- ihex - Binary to Intel HEX Converter バイナリをIntel HEX形式に変換するコマンドです。
- uuencode - Encode a binary file into ASCII text バイナリをテキスト形式にエンコードします。
- uudecode - Decode a binary file from ASCII text エンコードされたテキストをバイナリに戻します。

