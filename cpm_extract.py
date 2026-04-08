#!/usr/bin/env python3
"""
cpm.bin から CCP + BDOS を抽出するスクリプト
17セクタスキップ後、指定セクタ分を抽出可能
BIOS部分を含めないオプション付き
"""

import argparse

def main():
    parser = argparse.ArgumentParser(description="cpm.bin から CCP+BDOSを抽出")
    parser.add_argument("-o", "--output", default="ccp_bdos.bin",
                        help="出力ファイル名 (default: ccp_bdos.bin)")
    parser.add_argument("-s", "--sectors", type=int, default=44,
                        help="抽出するセクタ数 (default: 44)")
    parser.add_argument("--include-bios", action="store_true",
                        help="BIOS部分も含める (デフォルトは含めない)")
    
    args = parser.parse_args()

    input_file = "cpm.bin"
    skip_sectors = 17
    sector_size = 128
    skip_bytes = skip_sectors * sector_size

    # BIOSを含めない場合は44セクタ程度が標準的（CCP+BDOS）
    sectors = args.sectors
    if not args.include_bios and args.sectors == 44:
        print("BIOSを除外して44セクタ抽出します (CCP + BDOS のみ)")
    elif args.include_bios:
        print(f"BIOSを含めて {sectors}セクタ抽出します")
    else:
        print(f"{sectors}セクタ抽出します")

    extract_bytes = sectors * sector_size

    try:
        with open(input_file, "rb") as f:
            f.seek(skip_bytes)
            data = f.read(extract_bytes)

        if len(data) < extract_bytes:
            print(f"警告: 要求された {extract_bytes}バイトに対し、{len(data)}バイトしか読み込めませんでした。")

        with open(args.output, "wb") as f:
            f.write(data)

        print(f"✅ 抽出完了！")
        print(f"出力ファイル: {args.output}  ({len(data):,} バイト / {len(data)//128} セクタ)")

    except FileNotFoundError:
        print(f"エラー: {input_file} が見つかりません。")
    except Exception as e:
        print(f"エラー: {e}")

if __name__ == "__main__":
    main()
