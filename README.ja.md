# edf-coop-stable

[English](README.md) | [中文](README.zh-CN.md) | **日本語**

EARTH DEFENSE FORCE 6（地球防衛軍6）（PC / Steam）のオンライン協力プレイ安定化プラグイン **EDF6DirectNet** です。[EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) のプラグインとして動作します。

地球防衛軍シリーズ向けのオンライン安定化 Mod です。現在の対応作品：**EDF6**。EDF6 が安定したら EDF5 などほかの作品にも対応する予定です。

> 非公式 Mod です。D3 PUBLISHER / SANDLOT / Epic Games とは一切関係ありません。ゲームプロセス内でネットワーク呼び出しを横取りするだけで、ゲームファイルは一切改変しません。`Enabled=0` にするか DLL を削除すればバニラに戻ります。
>
> **ステータス：実験的。** 900 件以上の自動テストに合格。自動直結は 2 台の PC による実際のオンラインプレイで検証済み（0.3.2）。2 台の PC をまたいだ切断猶予はまだ実機検証していません。問題があればログを添えて [Issue](https://github.com/hajisensai/edf-coop-stable/issues) を立ててください。

## 何を解決するか

| 症状 | 原因 | プラグインの対処 | 導入が必要な人 |
|---|---|---|---|
| オンライン中に敵・位置・体力が食い違う（同期ズレ） | EDF6 はオンラインデータをすべて `UnreliableUnordered` で送信するため、パケットが 1 つ落ちるとその状態が永久に失われる | `ReliableUnordered` で送信するよう変更（落ちたら自動再送、順不同は引き続き許容されるので、ゲームが受け取る配送セマンティクスは変わらない）。EOS の送受信キューを最低 64MB に拡張し、キューが溢れてパケットが落ちるのを防ぐ | **送信側**。自分が入れれば、自分が送るデータは落ちなくなる。全員が入れるのが最も効果的 |
| 回線が一瞬不安定になっただけで部屋から落とされ、ミッションが水の泡 | EOS 接続がタイムアウトやネットワークエラーで閉じられると、ゲームは即座にそのプレイヤーを外す | いったんゲームには知らせず、裏で EOS に再接続させる。30 秒以内に復帰すればゲームは気づかない。復帰しなければバニラ通りに処理 | **双方とも導入**（プラグインが相手を自動判別、後述） |
| Epic リレーの遅延が大きい・NAT 越えに失敗する | すべての通信が EOS P2P / リレーを経由する | ホストは「グローバル直結」を有効にできる：他の人は入室後に自動でホストへ直接つなぐ（スター型、ホストが中継） | ホストが設定、参加者はプラグインを入れるだけ |
| 落ちた理由がわからない | — | 診断ログを出力：NAT タイプ、直結 / リレー、切断理由、ロビーメンバー、1 分ごとの送受信統計 | 自分 |

デフォルトですべて有効です（グローバル直結のみホストが手動で有効化）。

## インストール

1. [Releases](https://github.com/hajisensai/edf-coop-stable/releases/latest) から `EDF6DirectNet-v*.zip` をダウンロードし、任意のフォルダに**すべて展開**します。
2. **`INSTALL.bat`** をダブルクリック：Steam ライブラリから EDF6 を自動で見つけてインストールします。
   - EDFModLoader が入っていない場合は同梱の公式版（[BlueAmulet/EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) v1.0.10、MIT）を導入します。既存の `winmm.dll` は上書きしません。
   - ゲームが見つからない場合はゲームフォルダのパスを貼り付けるよう求められます（Steam ライブラリで EDF6 を右クリック → 管理 → ローカルファイルを閲覧）。
3. いつも通り Steam からゲームを起動します。初回起動後に `Mods\Plugins\EDF6DirectNet.ini`（設定）と `EDF6DirectNet.log`（ログ）が生成されます。

zip には README_EDF6DirectNet.txt（English）、README_EDF6DirectNet_zh.txt（中文）、README_EDF6DirectNet_ja.txt（日本語）が含まれ、デフォルトの設定ファイルのコメントは Windows の表示言語（中国語 / 日本語 / それ以外は英語）で書かれます。

アップデート：新しいバージョンの `INSTALL.bat` を再実行します。設定ファイルはそのまま残ります。
アンインストール：`UNINSTALL.bat` をダブルクリック（EDFModLoader や他の Mod には触れません）。以前ファイアウォール規則を追加していた場合は、アンインストーラーが削除コマンドを表示します。

zip の中身を手動でゲームフォルダ（`EDF6.exe` があるフォルダ）に展開してもかまいません。

## ホスト側でグローバル直結を有効にする（任意）

設定が必要なのはホストだけです。参加者はプラグインを入れて `AutoJoin=1`（デフォルト）であれば、入室後に自動で直結します。

1. `Mods\Plugins\EDF6DirectNet.ini` を開き、`Mode=host` に変更します。
2. ゲームフォルダ内の `EDF6DirectNet_AllowFirewall.bat` を右クリック → **管理者として実行**（初回のみ）。
3. インターネットから自分に接続できるようにします。どちらか一方：
   - **自動**：`PublicAddress` を空のままにします。プラグインがルーターの UPnP で UDP 27015 をポート開放し、この PC のグローバル IPv6 も自動で通知します。
   - **手動**：ルーターで UDP ポートをこの PC に転送し、`PublicAddress=グローバルIP:外部ポート` を設定します（DDNS のホスト名も可。例：`myroom.ddns.net:40000`）。
   - **PC で直接ダイヤルアップ接続している（PPPoE、ルーターなし）**：この種のアダプターはプラグインが認識できず、アドレスを自動で通知しません。`PublicAddress=グローバルIP:27015` を手動で設定してください。
4. ゲームを再起動し、いつも通り部屋を作ります。

成功したかの確認方法（ログ `Mods\Plugins\EDF6DirectNet.log` を見る）：

| ログ | 意味 |
|---|---|
| `DIRECT players who join your room connect to ...` | ホストのアドレスが確定し、部屋情報に書き込まれた |
| `UPNP router now forwards UDP ...` | UPnP によるポート開放に成功 |
| `UPNP no router with UPnP port mapping found` / `UPNP port mapping failed` | ルーターが UPnP 非対応、または UPnP が無効。手動のポート開放に切り替える |
| `UPNP WARNING: the router WAN address ... is private (carrier-grade NAT)` | キャリアグレード NAT（多くのプロバイダーで一般的）の内側にいてグローバル IPv4 がないため、IPv4 直結は不可能。IPv6 に頼るか、プロバイダーにグローバル IP を申請するしかない |
| `UPNP UDP 27015 is already forwarded to ...; left alone` | ルーター上でこのポートが LAN 内の別の機器にすでに転送されている。プラグインはそれを削除しない。`ListenPort` を変えるか、手動で開放する |
| `DIRECT client ... connected from ...`（ホスト） / `DIRECT connected to host ...`（参加者） | 直結が確立した |
| `DIRECT auto-connect stopped (the room host did not answer on any advertised address ...)` | 参加者がホストに接続できない（ファイアウォール / ポート開放 / Key の不一致）。ゲームは通常通り Epic 経由で続行し、60 秒後に再試行する |

参加者は IPv4 → IPv6 の順に各アドレスを 10 秒ずつ試し、どれもつながらなければ EOS のままになります。通常のプレイには影響しません。

**`Key=` について**：任意の合言葉で、あなたのアドレスとプレイヤーの EOS ID を知っている人による直結データの偽造を防ぎます。部屋情報には**書き込まれません**——ホストが Key を設定したら、参加者全員が自分の ini に同じ Key を書く必要があります。そうしないと自動直結に失敗し、EOS にフォールバックします。知り合いとだけ遊ぶなら設定しなくても構いません（ホストのログに `hosting without Key=` という注意が出ます）。

**プライバシー**：ホストのグローバルアドレスはロビーのメンバー属性に書かれるため、その部屋が見える人なら誰でも読み取れます。

## 設定リファレンス（`EDF6DirectNet.ini`、変更後はゲームを再起動すると反映）

| キー | デフォルト | 説明 |
|---|---|---|
| `[DirectNet] Enabled` | `1` | `0` = プラグインはフックを一切入れず、バニラと完全に同じ |
| `Mode` | `off` | `off` 通常プレイヤー / `host` 直結ホストになる / `join` ホストのアドレスを手動指定（自動直結でカバーされるので通常は不要） |
| `ListenPort` | `27015` | host：待ち受ける UDP ポート（ポート開放・ファイアウォール許可の対象はこれ）。join：ローカルポート、空 = 自動 |
| `PublicAddress` | 空 | host：接続先を他の人に伝える。空 = グローバル IPv6 + UPnP で開放した IPv4 |
| `AutoJoin` | `1` | 他人の部屋に入ったとき、ホストが直結を有効にしていれば自動で接続する |
| `HostAddress` | 空 | `Mode=join` のみ：ホストのアドレス。例：`123.45.67.89:27015` / `[2408:8207::5]:27015` |
| `Key` | 空 | 直結の合言葉。全員一致が必要。半角英数字のみ |
| `UPnP` | `1` | host 時にルーターへ自動でポート開放させる |
| `BindPhysicalInterface` | `1` | 直結の通信を物理ネットワークアダプターに固定し、Clash / VPN / 加速ツールの TUN アダプターに横取りされないようにする |
| `LinkTimeoutMs` | `60000` | 相手からデータが届かなくなってから直結を切断とみなすまでの時間（3000–300000） |
| `[EOS] FixedPort` | `0` | EOS が固定 UDP ポート `FixedPort`～`FixedPort+7` を使う。0 = ランダム |
| `Relay` | `default` | EOS リレー：`default` 変更しない / `allow` / `norelay` / `force` |
| `[Sync] ReliableGameTraffic` | `1` | 同期ズレ対策（信頼性のある送信）。`0` = バニラ |
| `[Resilience] HoldDisconnects` | `auto` | 切断猶予：`auto` プラグイン導入者のみ / `off` バニラ / `all` 判別せず全員に猶予（全員が導入済みと確信できる場合のみ） |
| `GraceSeconds` | `30` | 切断を最大何秒隠すか（1–600） |

## 仕組み

### 信頼性のある送信

`EDF.dll+0x12c8bc0` にある 2 つの `EOS_P2P_SendPacket` 呼び出し箇所は、どちらも `Reliability = UnreliableUnordered` を渡しています。プラグインはインポートテーブルのレベルでこれを `ReliableUnordered` に変えます。各パケットはちょうど 1 回、順不同で届く可能性がありますが、これは信頼性のない転送でもそもそも起こり得る配送パターンなので、ゲームロジックには影響せず、単にパケットが落ちなくなるだけです。受信側の協力は不要です。

### 切断猶予とプラグインの判別

相手がプラグインを入れていない場合、相手のゲームは通常通りあなたを対戦から外します。こちら側だけが切断を隠し続けると両者の状態が食い違うため、猶予はプラグインを入れている相手にしか与えられません。

EDF6 は受信した EOS パケットをすべてゲームデータとして解釈する（`ReceivePacket` の `RequestedChannel` が NULL）ため、P2P 上で探査パケットを送ることはできません。そこでプラグインはロビーの**メンバー属性**を使います。入室 / 部屋作成に成功すると自分に `EDF6DN=1` を書き込み（`EDF.dll` はメンバー属性関連の関数を一切インポートしていないので、ゲームからは見えません）、切断時にはローカルのロビーデータから相手がこの属性を持っているかを読み取ります：

- マークあり、かつ以前に接続が確立していた → 猶予。その間 2 秒ごとに `AcceptConnection` を呼んで再接続を要求；
- マークなし → バニラ通りに処理；
- グローバル直結のメンバー → 直結が生きている限りずっと猶予（そのゲームデータはもともと EOS を通らない）；
- 相手が本当にロビーから抜けた → 直ちに切断をゲームに渡す。

### グローバル直結

ホストは別途メンバー属性 `EDF6DN_ADDR`（スペース区切りのアドレス一覧）を書き込みます。他のメンバーは 2 秒ごとにそれを読み、IPv4、IPv6 の順に各アドレスを 10 秒ずつ試し、すべて失敗したら 60 秒後に再試行します。トランスポート層は独自の UDP プロトコルです：選択的確認応答、トークンバケットによる速度制限付き再送、セッション epoch（古いセッションのパケットが再接続後の新しいセッションに混ざらない）、任意の `Key=` 合言葉（切り詰めた HMAC-SHA256 タグ。偽造防止のみで暗号化はしない）。`IP_UNICAST_IF` で物理アダプターにバインドし、TUN による横取りを防ぎます。

## トラブルシューティング

- **まずログを見る**：`Mods\Plugins\EDF6DirectNet.log`（2MB を超えると `.log.1` にローテーション）。先頭に `==== EDF6DirectNet x.y.z starting` の行があればプラグインは読み込まれています。この行がなければ EDFModLoader が正しく入っていません。
- `EOS hooks FAILED`：ゲームのアップデートでインポートテーブルが合わなくなりました。プラグインは自動で直結を停止します。Issue を立ててください。
- `LOBBY plugin detection UNAVAILABLE`：相手がプラグインを入れているか判別できないため、切断猶予は直結メンバーにのみ適用されます。
- `EOS incoming packet queue FULL`：EOS のキューが溢れてパケットが落ち始めています。ログを添えて Issue を立ててください。
- `RESILIENCE ... RECOVERED` は切断の隠蔽に成功したことを、`did not come back within` はタイムアウト後にゲームへ渡したことを示します。
- `STATS last 60s: ...` は 1 分ごとの送受信統計です。`send-failures` が 0 でない場合はログを添えてください。
- `DIRECT ignored hello for ... its link is live`：オンライン中のプレイヤーになりすまして別のアドレスから接続しようとした人がいて、拒否されました。たまに 1 行出る程度なら相手が回線を切り替えただけかもしれません（5 秒後に自動で受け入れます）。頻繁に出る場合は誰かが荒らしている可能性があるので、`Key=` の設定をおすすめします。

## ビルド

Visual Studio 2022（MSVC x64）が必要です。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
# リリースパッケージを手動で作る：公式 EDFModLoader.zip を展開したフォルダが必要（winmm.dll、ModLoader.ini、およびその LICENSE.txt を含む）
powershell -ExecutionPolicy Bypass -File package.ps1 -Version 0.3.3 -ModLoaderDir <フォルダ>
```

成果物：`build\EDF6DirectNet.dll`（静的 CRT、システム DLL のみに依存）、`build\edf6_directnet_tests.exe`（ユニットテスト + ローカルループバックのマルチノードテスト。20%～40% のパケットロス、回線断、再起動のシナリオを含む。`EDF.dll` のインポートテーブルテストはこの PC にゲームがインストールされている必要があり、なければスキップ）、`build\probe_join.exe`（手動での接続確認用の直結プローブ）。

## リリース

リリースは GitHub Actions（`.github/workflows/release.yml`）で自動的に行われます：

1. バージョン番号を変更：`src/plugin.cpp` の `kVersionMajor/Minor/Patch` と `kVersionText`、および同梱説明書 3 つ `dist/README_EDF6DirectNet*.txt` の 1 行目。
2. リリースノート `release-notes/<バージョン>.md` を書く（Release ページの本文になります。これがないとパイプラインが失敗します）。
3. `main` にコミットし、タグをプッシュ：`git tag v0.3.3 && git push origin v0.3.3`。

パイプラインはビルド、テスト実行、公式 EDFModLoader v1.0.10 のダウンロード（SHA-256 で検証）、`EDF6DirectNet-v<バージョン>.zip` のパッケージングを行い、Release を作成します。タグ、ソースのバージョン番号、説明書のバージョン番号の 3 つが一致しない場合、`package.ps1` はパッケージングを拒否します。Actions ページから手動実行した場合はビルドとパッケージングのみ行い（成果物は実行記録の Artifacts にあります）、公開はしません。

## ディレクトリ構成

| パス | 内容 |
|---|---|
| `src/plugin.cpp` | EDFModLoader のエントリポイント。設定を読み込み、直結を起動 |
| `src/config.*` | INI の読み込みとデフォルト設定ファイル（コメントは Windows の表示言語に応じて中国語 / 日本語 / 英語で書かれる） |
| `src/eos_min.h` | 使用する EOS SDK の構造体（公式 1.15.5 のヘッダーに基づく。ゲームは 1.16.1） |
| `src/eos_hooks.cpp`, `src/iat.*` | `EDF.dll` のインポートテーブルを書き換え、EOS P2P / ロビーの呼び出しを横取り |
| `src/hold.*` | 切断猶予 |
| `src/lobby_marker.*` | ロビーのメンバー属性：誰がプラグインを入れているかの判別、ホストアドレスの配布 |
| `src/direct_net.*`, `src/reliable.*`, `src/wire.*`, `src/auth.*` | 直結トランスポート |
| `src/netif.*`, `src/upnp.*` | 物理アダプターの識別、UPnP |
| `src/log.*` | ログ |
| `dist/` | インストールスクリプトと同梱説明書（中 / 英 / 日） |
| `tests/` | テスト |

## 既知の制限

- ミッション途中からの参加には対応していません（ゲーム自体にその機能がありません）。
- 解決するのはパケットロスによる同期ズレだけです。ゲームロジック自体の同期ズレは、具体的な症状をもとに改めて解析する必要があります。
- 切断猶予の間、他のプレイヤーは同期ポイントで待たされることがあります（最大 `GraceSeconds` 秒）。
- 直結はホストが中継します：ある参加者の直結が再接続した瞬間、ホストがその参加者のために中継していてまだ確認応答を受けていない少量のデータは失われます（その後ゲームは EOS にフォールバックします）。
- `Key=` を設定しない場合、直結には認証がありません：ホストのアドレスとあるプレイヤーの EOS ID を知っている人は、そのプレイヤーのデータを偽造できます。Key を設定すれば偽造は防げますが、傍受された `Bye` / メンバー表のリプレイまでは防げません（プロトコルバージョンの更新が必要なため、次のメジャーバージョンに持ち越し）。
- UPnP のポート開放は永続的で、ゲーム終了後も自動では削除されません（ポートで待ち受けているプログラムがなければ無害です）。必要に応じてルーターの管理画面で `EDF6DirectNet` という名前のマッピングを削除してください。
- 切断猶予は実際のマルチプレイではまだ検証されていません。ログ付きの Issue を歓迎します。

## ライセンス

MIT、[LICENSE](LICENSE) を参照。同梱の EDFModLoader は MIT で、ライセンスはパッケージ内の `EDFModLoader\LICENSE.txt` にあります。
