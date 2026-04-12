# EMUZ80_RP2040_PCB_Firmware(AE-RP2040_CPM Branch)
![img1](./img/img1.jpg)
**本物の Z80 CPU 上で CP/M 2.2 を動作させる、RP2040 ベースの Z80 バスエミュレータ**

## 概要 - CP/Mブランチ
@tendai22plus氏作の [EMUZ80_RP2040_PCB](https://github.com/tendai22/EMUZ80_RP2040_PCB) 基板用のファームウェアです。  
EMUZ80_RP2040は実物の Z80 CPU を RP2040 または RP2350 搭載ボードで動かすための周辺回路・バスエミュレータ。  
Github -> https://github.com/tendai22/EMUZ80_RP2040_PCB

このブランチは、ソフトウェアでCPU自体をエミュレーションするのではなく、RP2040/RP2350 の PIO (プログラマブル I/O) サブシステムを利用してメモリや I/O デバイスをエミュレートし、**本物の Z80 マイクロプロセッサ**上で CP/M 2.2 オペレーティングシステムを動作させます。

- **対応ボード**: 秋月電子 AE-RP2040
- **Z80 動作クロック**: 最大 6MHz
- **RP2040 動作クロック**: 266MHz
- **CP/M メモリ**: 64KB (TPA 62KB)
- **ROM ディスク**: 5 ドライブ (256KBx4 + 650KB、FlashROM 格納)
- **RAM ディスク**: 1 ドライブ (128KBKB, SRAM)
- **ターミナル接続**: USB 仮想 COM ポート (UART-USB)

> [!NOTE]
> 本ブランチは **CP/M 2.2 専用**です。ROM-BASIC (EMUBASIC) は搭載していません。
> EMUBASIC(ROM-BASIC) を使用する場合は [AE-RP2040 ブランチ](https://github.com/kyo-ta04/EMUZ80_RP2040_PCB_Firmware/tree/AE-RP2040) をご利用ください。

詳しくはこちらをどうぞ:
- [第1話　Z80が2026年に突然息を吹き返した日　〜RP2040でCP/Mモンスター爆誕！〜 - note](https://note.com/quiet_duck4046/n/n6ee36d5d51e4?sub_rt=share_sb) 
- [せっかく メモリー64KBになったから CP/M行きたくなってきたw🤔 - X](https://x.com/DragonBallEZ/status/2037853028548808990)

## CP/M ディスク構成

| ドライブ | サイズ | 種類 | 内容 |
|:---:|:---:|:---:|---|
| **A:** | 256KB | ROM | CP/M 2.2 システムディスク + ユーティリティ (ASM, LOAD, PIP, SYSGEN 等) |
| **B:** | 256KB | ROM | MS-BASIC 等 |
| **C:** | 256KB | ROM | Turbo Pascal 3.01a |
| **D:** | 256KB | ROM | Z80 fig-FORTH 1.1g |
| **I:** | 650KB | ROM | HI-TECH C v3.09 コンパイラ |
| **J:** | 128KB | RAM | 読み書き可能な RAM ディスク |

> [!NOTE]
> ROM ディスクは読み取り専用です。ファイルの保存・編集には RAM ディスク (J:) を使用してください。
> RAM ディスクの内容は電源 OFF で消失します。

## ビルド済み UF2 ファイル

すぐに書き込んで試せる UF2 ファイルをプロジェクトルートに用意しています。

- `EMUZ80_RP2040_PCB_Firmware.uf2` — 秋月電子 AE-RP2040 用

## ターミナル接続

AE-RP2040 の USB コネクタを PC に接続すると、**USB 仮想 COM ポート**として認識されます。
TeraTerm や PuTTY 等のターミナルソフトで該当 COM ポートに接続してください。

> [!WARNING]
> ターミナルソフトからテキストをアップロードする際は、**送信遅延（文字遅延・行遅延）を設定**しないと文字の取りこぼしや誤動作が発生する場合があります。

## ビルド要件
- Antigravity IDE (推奨ビルド環境)
- Raspberry Pi Pico C/C++ SDK
- CMake

## ビルド方法

現在、本プロジェクトのビルドおよび動作確認は **Antigravity IDE** 上でのみ行われています。

*(注: 通常の CMake/Ninja を用いたビルドも理論上は可能ですが、公式にはサポート（テスト）していません)*

## 高速動作のための必須設定
  - ビルド構成を **`Release`** に設定する必要があります。(`Debug` では最適化不足で追従不可)。
  - Z80 の高速動作には、`CMakeLists.txt` にて以下の設定が有効である必要があります。
    - **`copy_to_ram` (RAM実行)**:  — Flash 読み出し速度制限の回避。
    - **`-O3` (最大最適化)**: Z80 バスエミュレーションの処理能力確保。

## ⚠ GPIO 5V トレラント接続に関する注意

本プロジェクトでは、RP2040/RP2350 の GPIO ピン (3.3V I/O) を Z80 バス (5V系) に**直接接続**しています。  
RP2040/RP2350 の GPIO は公式には **5V トレラントではありません**。  
Raspberry Pi 公式のデータシートでは、GPIO 入力電圧の絶対最大定格は **IOVDD + 0.5V（= 約 3.8V）** と規定されています。

### リスクと免責

> [!CAUTION]
> **この接続方法はデータシートの定格範囲外の使用です。**  
> - RP2040/RP2350 チップの**長期的な信頼性低下や寿命短縮**の可能性があります
> - 個体差や温度条件によっては **動作不良やチップ破損** が発生する可能性を否定できません

本プロジェクトの回路構成で発生した損害について、開発者は一切の責任を負いません。使用に際しては自己責任でお願いします。

## 謝辞 (Acknowledgments)

本プロジェクトは多くの方々の素晴らしい成果の上に成り立っています。

- **CP/M 2.2 および CBIOS** — Udo Munk氏 ([z80pack](https://github.com/udo-munk/z80pack))
- **ディスクイメージ収録物・CP/M環境** — SuperFabius氏 ([Z80-MBC2](https://github.com/SuperFabius/Z80-MBC2))
- **EMUZ80 プロジェクト** — vintagechips氏 (https://vintagechips.wordpress.com/)
- **EMUZ80_RP2040_PCB 基板** — tendai22plus氏 (https://github.com/tendai22)


## ライセンス (License)

このプロジェクトのオリジナル部分（PIO Z80バスエミュレータ、BIOS実装など）は **MIT License** の下で公開されています。  
詳細は [LICENSE](LICENSE) ファイルを参照してください。

### 含まれるサードパーティの著作物

| コンポーネント | 出典 | ライセンス |
|---|---|---|
| Z80 CBIOS (`bios01.*`) | [z80pack](https://github.com/udo-munk/z80pack) by Udo Munk | MIT License |
| CP/M 2.2 CCP+BDOS (`ccp_bdos.*`) | Digital Research (1979) | DRDOS, Inc. / Bryan Sparks ライセンス、商用利用可 (2022) |
| CP/M システムディスク (`cpm22-1.*`) | z80pack 同梱 | DRDOS, Inc. (CP/M) |
| MS-BASIC等 (`cpm22_disk1.*`) | (C) Microsoft Corporation (1977,1985-1986) / Z80-MBC2同梱 | 非公式アーカイブ。歴史的保存および教育目的でのみ同梱。 |
| Turbo Pascal 3.01a (`cpm22_tp301a.*`) | Borland International (1985-1986) |  非公式アーカイブ。歴史的保存および教育目的でのみ同梱。 |
| Z80 fig-FORTH (`cpm22_z80forth.*`) | FORTH Interest Group (fig) | パブリックドメイン |
| HI-TECH C Z80 v3.09 (`cpm22_htc.*`) | HI-TECH Software (1984-1987) / Microchip | フリーウェア(公式公開:商用利用可) |

> [!IMPORTANT]  
> **CP/M 2.2** バイナリは 2022年の DRDOS, Inc. / Bryan Sparks の許可に基づき再配布されています(詳細：http://www.cpm.z80.de/license.html)。  
> **MS-BASIC** レトロコンピューティングコミュニティにおいて歴史的・教育的資料として広く共有されているものです。著作権は Microsoft Corporation に帰属します。商用利用を避け、個人の学習・研究目的でのみ利用してください。
(These binaries are included for historical and educational purposes only. Copyright belongs to Microsoft Corporation. Not for commercial use.)  
> **Turbo Pascal 3.01a** レトロコンピューティングコミュニティにおいて歴史的・教育的資料として広く共有されているものです。著作権は Embarcadero (旧Borland) に帰属します。**DOS版（3.02など）は公式に "Antique Software" として無料公開されています。**  
> **HI-TECH C Z80 v3.09** は HI-TECH Software が2000年頃にフリーウェアとして公式公開。ライブラリソース含め、私的・商用問わず利用可能です（as-is、無保証）。

## ファイル構成

```
EMUZ80_RP2040_PCB_Firmware/
├── EMUZ80_RP2040_PCB_Firmware.c  # メインファームウェア
├── EMUZ80_RP2040_xxMHz.uf2      # ビルド済み UF2 ファイル
├── AE-RP2040.pio                 # PIO プログラム (Z80 バスエミュレーション)
├── CMakeLists.txt                # ビルド設定
├── rom_data.h                    # ROM ディスクの extern 宣言
│
├── bios01.asm / .c / .h          # Z80 CBIOS (z80pack ベース)
├── ccp_bdos.c / .h               # CP/M 2.2 CCP + BDOS
│
├── cpm22-1.h / cpm22_1.c         # Drive A: システムディスク
├── cpm22_disk1.h / .c            # Drive B: ユーティリティ
├── cpm22_tp301a.h / .c           # Drive C: Turbo Pascal
├── cpm22_z80forth.h / .c         # Drive D: fig-FORTH
├── cpm22_htc.h / .c              # Drive I: HI-TECH C
│
├── bin2c.py                      # .dsk → C ヘッダ変換ツール
├── cpm2c.py / cpm_extract.py     # CP/M ディスクユーティリティ
│
├── img/                          # ドキュメント用画像
└── LICENSE                       # MIT ライセンス
```

## ギャラリー
### 実行結果
![img5](./img/img5.jpg)
![img2](./img/img2.jpg)
### 回路図
![img3](./img/img3.jpg)
### 基板裏面
![img4](./img/img4.jpg)
** BUSRQ,INT,NMI,WAITはプルアップ、RESETの LEDもジャンパで接続 **

