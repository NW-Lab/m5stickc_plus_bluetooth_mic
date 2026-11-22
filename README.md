# M5StickC Plus Bluetooth Mic

M5StickC PlusをWindows用Bluetoothマイクとして使うESP-IDFプロジェクトです。
Bluetooth ClassicのHFP Hands-Free Profileを使い、Windows側では入力デバイスとして選択します。

> XIAO nRF52840はBluetooth LEのみのため、Windowsの標準Bluetoothマイクに必要なHFPには対応しません。

## マイクの選択

`main/mic_input.h` の `MIC_SOURCE` でビルド時に選択します。

```c
#define MIC_SOURCE MIC_SOURCE_ICS43434
```

- `MIC_SOURCE_ICS43434`: Adafruit ICS-43434外付けI2Sマイク（初期設定）
- `MIC_SOURCE_STICKC_PLUS`: M5StickC Plus内蔵SPM1423 PDMマイク

ICS-43434は本体上部のHAT端子に接続します。GroveのGPIO32/33は使いません。

| ICS-43434 | M5StickC PlusのHAT端子 |
| --- | --- |
| BCLK / SCK | G26 |
| WS / LRCLK | G0 |
| DOUT / SD | G36/G25（GPIO36で受信） |
| L/R | GND（左チャンネル） |
| VDD / VIN | 3V3 |
| GND | GND |

G36とG25は同じ端子を共有するため、外付けモードではG25をプルなしの入力に設定します。
G0は起動モードの判定にも使われます。WSはマイク側の入力なので、G0をGNDに固定したり外部から駆動したりしないでください。配線後に起動や書き込みができない場合は、WS線を外して確認してください。

内蔵マイクを選ぶ場合はI2Sの設定に加え、AXP192経由でマイク電源を有効にします。

## 画面表示

内蔵LCDにマイクの種類、Bluetoothの接続状態、音声送信状態、サンプルレート、マイク入力のピークレベルを表示します。

- `PAIR MODE`: Windowsからのペアリング待ち。`WAIT PC`: 保存済みのPCを待機中。
- `CONNECTING` / `CONNECTED`: HFPの接続中 / 接続済み。
- `TX LIVE`: Bluetoothスタックが最近マイクの音声を取得した状態。PC側での再生や録音の成功までは保証しません。`TX WAIT`は音声接続済みで送信待ち、`TX OFF`は音声未接続。
- `MIC ERROR` / `BT ERROR`: マイク読み取り / Bluetooth初期化エラー。詳細はシリアルモニターを確認してください。

レベルバーは音声接続中の入力振幅で、静かな環境では動きが小さくなります。

## ビルドと書き込み

ESP-IDF 5.5以降をセットアップした環境で、プロジェクトのルートから実行します。

```text
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

WindowsのBluetooth設定から「M5 Bluetooth Mic」をペアリングし、サウンド設定の入力デバイスとして選択してください。接続後、HFPのネゴシエーションに合わせて8 kHz（CVSD）または16 kHz（mSBC）でマイク入力を送ります。
