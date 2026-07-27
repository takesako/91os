# 組み込みJavaScript処理系 仕様書

## 1. 概要

本処理系は、Cesanta Software の小型JavaScriptエンジン **Elk** を基にした、組み込み用途向けのJavaScriptサブセットです。

標準JavaScript（ECMAScript）の全機能を実装するものではありません。小さなメモリ領域で、変数、式、条件分岐、繰り返し、関数、簡易的なオブジェクト・配列・文字列処理を実行することを目的としています。

この仕様書は、添付された `elk(5).c` の実装を基準とします。

## 2. 基本的な特徴

- インタプリタ方式でソースコードを直接実行する
- 数値は `double` 型で保持する
- オブジェクト、文字列、関数などを固定長のメモリ領域に格納する
- ガベージコレクションを備える
- C関数をJavaScript側へ登録できる
- 文末には原則としてセミコロン `;` が必要
- JavaScript標準ライブラリの大部分は存在しない
- ブラウザAPI、Node.js API、DOM、モジュール機能は存在しない

## 3. コメントと空白

次の空白文字を区切りとして扱います。

- 半角スペース
- タブ
- 改行
- CR
- フォームフィード
- 垂直タブ

次のコメントを使用できます。

```javascript
// 1行コメント

/*
  複数行コメント
*/
```

## 4. 識別子

変数名および関数名には、次の文字を使用できます。

- 先頭: `A-Z`、`a-z`、`_`、`$`
- 2文字目以降: 上記に加えて `0-9`

例:

```javascript
let score = 10;
let _count = 3;
let $value = 5;
```

日本語の変数名など、ASCII以外の文字を識別子として使用することはできません。

## 5. データ型

### 5.1 対応する値

| 種類 | 例 | 備考 |
|---|---|---|
| 数値 | `123`, `3.14`, `1e3` | 内部表現は `double` |
| 文字列 | `"hello"`, `'hello'` | バイト列として扱う |
| 真偽値 | `true`, `false` |  |
| null | `null` |  |
| undefined | `undefined` |  |
| オブジェクト | `{name: "A"}` | プロパティ集合 |
| 配列相当 | `[]` | 内部的にはオブジェクト |
| 関数 | `function (x) {...}` | 名前なし関数リテラル |
| C関数 | C側から登録 | 組み込み関数として呼び出せる |

### 5.2 存在しない、または標準と異なる型

次の標準JavaScript型・仕組みは実装されていません。

- `BigInt`
- `Symbol`
- `Map`
- `Set`
- `WeakMap`
- `WeakSet`
- `Date`
- `Promise`
- `ArrayBuffer`
- TypedArray
- クラスインスタンス
- プロトタイプチェーン

## 6. リテラル

### 6.1 数値リテラル

Cの `strtod()` で解釈可能な10進数を使用できます。

```javascript
let a = 123;
let b = 3.14;
let c = 1e3;
```

16進数、2進数、8進数については、本処理系の仕様として保証されません。

### 6.2 文字列リテラル

シングルクォートとダブルクォートを使用できます。

```javascript
let a = "hello";
let b = 'world';
```

通常の文字列リテラルで明示的に対応するエスケープは次のとおりです。

| 記述 | 意味 |
|---|---|
| `\n` | 改行 |
| `\r` | CR |
| `\t` | タブ |
| `\xNN` | 1バイトの16進値 |
| `\"` | ダブルクォート内の `"` |
| `\'` | シングルクォート内の `'` |

通常の文字列リテラルでは `\uXXXX` は処理されません。Unicodeエスケープのデコードには、後述の `JSON.parse()` を使用します。

### 6.3 テンプレートリテラル

バッククォートによるテンプレート文字列を使用できます。

```javascript
let name = "Elk";
let text = `Hello ${name}`;
```

対応するエスケープは主に `\n`、`\r`、`\t` です。

制限事項:

- 出力用の一時バッファは100 KiB固定
- タグ付きテンプレートは使用不可
- 標準JavaScriptの完全なテンプレートリテラル構文ではない
- `${...}` 内の波括弧は数えるが、複雑な字句解析は行わない

### 6.4 オブジェクトリテラル

次の形式を使用できます。

```javascript
let user = {
  name: "Alice",
  age: 20,
  "school": "Kamiyama"
};
```

キーに使用できるもの:

- 識別子
- 文字列リテラル

制限事項:

- メソッド短縮記法は使用不可
- プロパティ短縮記法は使用不可
- 計算プロパティ名は使用不可
- スプレッド構文は使用不可
- getter / setter は使用不可

### 6.5 配列リテラル

**空配列 `[]` だけを作成できます。**

```javascript
let a = [];
a.push(10);
a.push(20);
```

次のような要素入り配列リテラルは使用できません。

```javascript
let a = [1, 2, 3];  // エラー
```

配列は内部的には、`"0"`、`"1"` などのプロパティと `length` プロパティを持つオブジェクトとして実装されています。

### 6.6 正規表現リテラル

次の形式を認識します。

```javascript
let pattern = /abc/;
```

通常の `/abc/` は、実質的に正規表現パターン文字列として扱われます。

`y` フラグを付けた場合だけ、`lastIndex` を持つ正規表現オブジェクトになります。

```javascript
let pattern = /abc/y;
```

制限事項:

- 正規表現エンジンは `miniregex` に依存する簡易実装
- ECMAScript正規表現の完全互換ではない
- `g`、`i`、`m`、`s`、`u` などのフラグ動作は実装されていない
- フラグ文字は読み飛ばされるが、実際に意味を持つのは `y` のみ
- キャプチャグループなどの標準的な結果形式は保証されない

## 7. 変数宣言

### 7.1 let

```javascript
let x;
let y = 10;
let a = 1, b = 2;
```

特徴:

- ブロックスコープ
- 初期値を省略すると `undefined`
- 同一スコープでの再宣言はエラー
- 標準JavaScriptのTDZ（Temporal Dead Zone）は実装されていない
- 宣言の巻き上げはない

### 7.2 const

```javascript
const x = 10;
const a = 1, b = 2;
```

特徴:

- ブロックスコープ
- 初期値が必須
- 代入し直すとエラー
- 同一スコープでの再宣言はエラー
- TDZは実装されていない

```javascript
const x = 10;
x = 20;  // ERROR: assignment to const
```

`const` が保護するのは変数への再代入です。オブジェクト内部のプロパティ変更まで不変にするものではありません。

### 7.3 var

```javascript
var x;
var y = 10;
var a = 1, b = 2;
```

特徴:

- 関数スコープ、またはグローバルスコープ
- 同じ名前の再宣言を許可
- 宣言の巻き上げは実装されていない
- 初期化式は宣言位置で実行される

## 8. 演算子

### 8.1 算術演算子

| 演算子 | 意味 |
|---|---|
| `+` | 加算、または文字列連結 |
| `-` | 減算 |
| `*` | 乗算 |
| `/` | 除算 |
| `%` | 剰余 |
| 単項 `+` | 数値化、または数値をそのまま返す |
| 単項 `-` | 符号反転 |
| `++` | 後置インクリメントのみ |
| `--` | 後置デクリメントのみ |

```javascript
let x = 3;
x++;
x--;
```

前置形式は使用できません。

```javascript
++x;  // 非対応
--x;  // 非対応
```

`**` はトークンとして認識されますが、累乗演算は実装されていません。

除数が0の場合、JavaScript標準の `Infinity` ではなくエラーになります。

```javascript
let x = 10 / 0;  // ERROR: div by zero
```

### 8.2 比較演算子

| 演算子 | 対応 |
|---|---|
| `<` | 対応 |
| `<=` | 対応 |
| `>` | 対応 |
| `>=` | 対応 |
| `===` | 対応 |
| `!==` | 対応 |
| `==` | 限定対応 |
| `!=` | 限定対応 |

重要な相違点:

- 数値の `===` と `!==` は内部で整数型へ変換して比較するため、標準JavaScriptと異なる結果になる可能性がある
- `==` と `!=` は一般的な型変換を実装していない
- `null == undefined` は `true`
- 同じ型同士は値または内部表現を比較する
- `"1" == 1` のような一般的な暗黙変換は行わない

### 8.3 論理演算子

| 演算子 | 対応 |
|---|---|
| `!` | 対応 |
| `&&` | 対応、短絡評価あり |
| `||` | 対応、短絡評価あり |
| `??` | 対応、短絡評価あり |

```javascript
let x = value ?? 0;
```

### 8.4 ビット演算子

| 演算子 | 対応 |
|---|---|
| `&` | 対応 |
| `|` | 対応 |
| `^` | 対応 |
| `~` | 対応 |
| `<<` | 対応 |
| `>>` | 対応 |
| `>>>` | 字句解析されるが実行処理は未実装 |

ビット演算では数値をCの `long` に変換します。このため、JavaScript標準の32ビット整数演算とは異なる場合があります。

### 8.5 代入演算子

次の演算子を使用できます。

```text
=  +=  -=  *=  /=  %=  <<=  >>=  &=  ^=  |=
```

`>>>=` は字句解析されますが、実行処理は未実装です。

### 8.6 その他の演算子

| 演算子 | 対応 |
|---|---|
| `typeof` | 対応 |
| `?:` | 三項演算子に対応 |
| `,` | 式文および丸括弧内で限定対応 |
| `?.` | オプショナルチェーンに限定対応 |
| `.` | プロパティ参照 |
| `[]` | インデックス・プロパティ参照 |

`typeof` の返り値は標準JavaScriptと完全には同じではありません。内部型名を返すため、C関数や内部値では独自の文字列になる場合があります。

## 9. 真偽値への変換

偽として扱われる主な値:

- `false`
- 数値 `0`
- 空文字列 `""`
- `null`
- `undefined`

真として扱われる主な値:

- `true`
- 0以外の数値
- 1文字以上の文字列
- オブジェクト
- 関数

`NaN` の扱いは標準JavaScriptと完全一致するとは限りません。

## 10. 制御構文

### 10.1 if / else

```javascript
if (score >= 10) {
  result = "pass";
} else {
  result = "fail";
}
```

条件式の丸括弧は必須です。

波括弧を省略して1文だけ記述することもできます。

```javascript
if (x) y = 1;
```

### 10.2 while

```javascript
let i = 0;
while (i < 10) {
  i++;
}
```

条件式の丸括弧は必須です。

### 10.3 for

C形式の `for` 文を使用できます。

```javascript
for (let i = 0; i < 10; i++) {
  total += i;
}
```

初期化部分では次を使用できます。

- `let`
- `const`
- `var`
- 通常の式
- 空欄

条件式、更新式も省略できます。

```javascript
for (;;) {
  break;
}
```

未対応:

- `for...in`
- `for...of`
- `for await...of`

### 10.4 break

`for` または `while` の内側で使用できます。

```javascript
while (true) {
  break;
}
```

ループ外で使用するとエラーになります。

### 10.5 continue

`for` または `while` の内側で使用できます。

```javascript
for (let i = 0; i < 10; i++) {
  if (i == 5) continue;
}
```

ループ外で使用するとエラーになります。

実装は実行抑制フラグを利用した簡易方式です。複雑な入れ子や関数呼び出しを含む場合は、標準JavaScriptと同じ制御になることを十分にテストする必要があります。

### 10.6 return

関数内で使用できます。

```javascript
let add = function (a, b) {
  return a + b;
};
```

関数外で使用するとエラーになります。

`return;` は `undefined` を返します。

### 10.7 throw

次の限定形式だけ使用できます。

```javascript
throw Error("message");
```

制限事項:

- 投げられるのは `Error(文字列)` 形式だけ
- `throw "message"` は不可
- `new Error(...)` は不可
- `try` / `catch` / `finally` は未実装
- 例外をJavaScript側で捕捉することはできない

## 11. ブロックとセミコロン

ブロックは `{` と `}` で記述します。

```javascript
{
  let x = 1;
  let y = 2;
}
```

`let` と `const` はブロックスコープです。

原則として文末のセミコロンが必要です。

```javascript
let x = 1;
x = x + 1;
```

自動セミコロン挿入（ASI）は実装されていません。

ただし、次の構文の直後は構文解析上セミコロンなしで通る場合があります。

- ブロック
- `if`
- `while`

可搬性と誤動作防止のため、通常の文には必ずセミコロンを書くことを推奨します。

## 12. 関数

### 12.1 function式

名前なしの関数式を作成できます。

```javascript
let add = function (a, b) {
  return a + b;
};
```

制限事項:

- `function add(a, b) {...}` のような名前付き関数宣言は保証されない
- デフォルト引数は使用不可
- 残余引数 `...args` は使用不可
- 分割代入引数は使用不可
- `arguments` オブジェクトは存在しない
- クロージャの完全な字句環境保持は保証されない
- `this` は未実装
- コンストラクタ呼び出しは不可

### 12.2 アロー関数

単一引数または引数なしのアロー関数を使用できます。

```javascript
let twice = x => x * 2;
let hello = () => "hello";
let square = x => {
  return x * x;
};
```

制限事項:

- `(a, b) => a + b` の複数引数形式は実装上正しく認識されない
- `x => ...` または `() => ...` が中心
- アロー関数固有の `this`、`arguments`、`super` などの仕様は存在しない
- オブジェクトリテラルを式本体として直接返す構文は保証されない

### 12.3 関数呼び出し

```javascript
let result = add(1, 2);
```

引数が不足した場合、対応する仮引数には `undefined` が設定されます。

余分な引数は読み取られますが、`arguments` がないため関数側から参照できません。

## 13. オブジェクト

### 13.1 プロパティ参照

```javascript
let user = {name: "Alice"};
let a = user.name;
let b = user["name"];
```

存在しないプロパティは `undefined` を返します。

### 13.2 プロパティ代入

既存プロパティへの代入はできます。

```javascript
let user = {age: 20};
user.age = 21;
```

ただし、実装上、存在しないプロパティに通常のJavaScript式で新規代入する機能は保証されません。プロパティ参照が `undefined` になるため、左辺として有効なプロパティ参照を得られない場合があります。

### 13.3 オプショナルチェーン

限定的に `?.` を使用できます。

```javascript
let value = obj?.name;
```

左辺が `null` または `undefined` の場合は `undefined` を返します。

関数呼び出し部分も読み飛ばす処理があります。

```javascript
let value = obj?.method();
```

ただし、標準JavaScriptのあらゆるオプショナルチェーン構文への完全対応ではありません。

### 13.4 未対応のオブジェクト機能

- プロトタイプ
- `Object` 標準関数
- `Object.keys()`
- `Object.assign()`
- `hasOwnProperty()`
- `delete`
- getter / setter
- `in`
- `instanceof`
- `new`
- クラス

## 14. 配列

配列は空配列から作成します。

```javascript
let numbers = [];
numbers.push(10);
numbers.push(20);
```

### 14.1 対応するプロパティ

```javascript
numbers.length;
numbers[0];
```

### 14.2 push

```javascript
let length = numbers.push(30);
```

- 引数は1つだけ
- 追加後の長さを返す
- 複数要素を一度に追加する標準形式は不可

```javascript
numbers.push(1, 2);  // 非対応
```

### 14.3 map

```javascript
let doubled = numbers.map(x => x * 2);
```

制限事項:

- コールバックに渡されるのは要素値1つだけ
- インデックスや元配列は渡されない
- コールバックは本処理系のJavaScript関数である必要がある
- 空要素や配列らしくないオブジェクトの動作は標準と異なる

### 14.4 filter

```javascript
let selected = numbers.filter(x => x > 10);
```

制限事項は `map()` と同様です。

### 14.5 未対応の配列機能

- 要素入り配列リテラル `[1, 2, 3]`
- `pop()`
- `shift()`
- `unshift()`
- `slice()`
- `splice()`
- `join()`
- `indexOf()`
- `find()`
- `findIndex()`
- `some()`
- `every()`
- `reduce()`
- `forEach()`
- `sort()`
- `reverse()`
- スプレッド構文
- 分割代入

## 15. 文字列

### 15.1 length

```javascript
let n = "hello".length;
```

`length` はUTF-8の文字数ではなく、保存されている**バイト数**です。

```javascript
let n = "🍎".length;
```

上記は標準JavaScriptのUTF-16コード単位数ではなく、UTF-8バイト数になる可能性があります。

### 15.2 インデックス参照

```javascript
let c = "hello"[1];  // "e"
```

1バイトだけを切り出します。したがって、日本語や絵文字など複数バイトUTF-8文字のインデックス参照は正しく1文字を返しません。

### 15.3 文字列連結

```javascript
let text = "hello" + " world";
```

文字列同士の `+` に対応します。

数値と文字列の自動連結は行いません。

```javascript
let text = "value=" + 10;  // 型不一致エラー
```

値を文字列へ埋め込む場合はテンプレートリテラルを使用します。

```javascript
let text = `value=${10}`;
```

### 15.4 対応する文字列メソッド

#### includes

```javascript
"hello".includes("ell");
```

- 引数は文字列1つ
- 開始位置引数には非対応

#### startsWith

```javascript
"hello".startsWith("he");
```

- 引数は文字列1つ
- 開始位置引数には非対応

#### trim

```javascript
"  hello  ".trim();
```

ASCII系の空白判定を使用します。

#### repeat

```javascript
"ab".repeat(3);
```

- 回数は0以上の整数
- 最大4096回

#### match

```javascript
"abc123".match(/[0-9]+/);
```

結果はマッチした部分文字列を並べた簡易配列です。標準JavaScriptの `match()` と同じプロパティやキャプチャ結果は持ちません。

#### split

```javascript
"a,b,c".split(/,/);
```

区切りは文字列または正規表現相当のパターンとして処理されます。

制限事項:

- 第2引数 `limit` は使用不可
- キャプチャグループを結果へ含める標準動作は保証されない

#### test

実装上、文字列メソッドとしても呼び出せます。

```javascript
"pattern".test("target");
```

通常のJavaScriptで一般的な `/pattern/.test("target")` とは内部的な扱いが異なるため、正規表現オブジェクトを使う場合は `y` フラグの有無に注意が必要です。

### 15.5 未対応の文字列機能

- `charAt()`
- `charCodeAt()`
- `codePointAt()`
- `endsWith()`
- `indexOf()`
- `lastIndexOf()`
- `substring()`
- `substr()`
- `slice()`
- `replace()`
- `replaceAll()`
- `toUpperCase()`
- `toLowerCase()`
- `padStart()`
- `padEnd()`
- `String` コンストラクタ

## 16. JSON

### 16.1 JSON.parse

`JSON.parse()` という名前ですが、完全なJSONパーサーではありません。

対応するのは、**引用符を含んだJSON文字列リテラル1個のデコード**です。

```javascript
let s = JSON.parse('"hello\\nworld"');
```

対応するエスケープ:

- `\"`
- `\\`
- `\/`
- `\b`
- `\f`
- `\n`
- `\r`
- `\t`
- `\xNN`
- `\uXXXX`
- UTF-16サロゲートペア

次のようなJSON全体の解析はできません。

```javascript
JSON.parse('{"name":"Alice"}');  // 非対応
JSON.parse('[1,2,3]');             // 非対応
JSON.parse('123');                 // 非対応
```

### 16.2 JSON.stringify

実装されていません。

## 17. 正規表現

簡易正規表現機能は `miniregex` によって提供されます。

使用可能な主な呼び出し:

```javascript
"abc123".match(/[0-9]+/);
"a,b,c".split(/,/);
"abc".test("target");

let re = /abc/y;
re.exec("abcdef");
```

### 17.1 exec

`exec()` は、主に `y` フラグ付き正規表現オブジェクトで使用します。

```javascript
let re = /abc/y;
let result = re.exec("abcdef");
```

- マッチ成功時は、マッチ文字列1個を含む簡易配列を返す
- 失敗時は `null`
- `lastIndex` を更新する
- 標準の `index`、`input`、`groups` などはない
- キャプチャグループ結果は返さない

## 18. 組み込み関数

### 18.1 getchar

```javascript
let ch = getchar();
```

- 引数なし
- C標準入力から1文字を読む
- 返り値は文字コードの数値
- EOFまたは読み取り失敗時は `-1`
- 文字列ではなく数値を返す
- 端末設定によってはEnterを押すまで返らない
- 非カノニカル入力や非同期入力は提供しない

### 18.2 msleep

```javascript
msleep(1000);
```

- 引数はミリ秒を表す数値1個
- 0以上の整数のみ
- 上限は `4294967295`
- 実行スレッドを同期的に停止する
- 非同期タイマーではない

### 18.3 C側から追加する関数

C側では `js_mkfun()` と `js_set()` を使って独自関数を登録できます。

JavaScript標準の `console.log()` や `print()` は、このファイル単体では定義されていません。ホスト側がC関数として登録した場合だけ利用できます。

## 19. 未実装の予約語・構文

次の語はトークンとして認識されますが、文として使用すると `not implemented` エラーになります。

| 未実装 |
|---|
| `case` |
| `catch` |
| `class` |
| `default` |
| `delete` |
| `do` |
| `finally` |
| `in` |
| `instanceof` |
| `new` |
| `switch` |
| `this` |
| `try` |
| `void` |
| `with` |
| `yield` |

このほか、次も使用できません。

- `async`
- `await`
- `import`
- `export`
- `super`
- `extends`
- `debugger`
- ラベル文
- ジェネレーター
- モジュール

## 20. 標準JavaScriptとの主な相違点

### 20.1 標準ライブラリがほぼない

次は標準では使用できません。

- `console`
- `Math`
- `Number`
- `String`
- `Boolean`
- `Object`
- `Array`
- `RegExp`
- `Date`
- `JSON.stringify`
- `parseInt`
- `parseFloat`
- `isNaN`
- `setTimeout`
- `setInterval`

### 20.2 暗黙の型変換が限定的

```javascript
"1" + 2;
"1" == 1;
true + 1;
```

これらは標準JavaScriptと同じ結果になりません。多くの場合、型不一致エラーになるか、限定された比較になります。

### 20.3 Unicode文字列処理がバイト単位

UTF-8文字列を保存できますが、`length` とインデックス参照はバイト単位です。

日本語・絵文字に対して、標準JavaScriptと同じ文字単位処理は行いません。

### 20.4 エラー処理が簡易的

- エラーメッセージ用バッファは33バイト
- 長いエラーメッセージは切り詰められる
- JavaScriptの `Error` オブジェクトやスタックトレースはない
- `try` / `catch` がない
- エラーが発生するとその評価処理を終了する

### 20.5 メモリ制約

ホストが渡した固定長バッファ内で動作します。

メモリ不足時は次のようなエラーになります。

```text
ERROR: oom
```

関数引数は、JavaScript用メモリ領域の上端を一時スタックとして使用します。大量のオブジェクト、長い文字列、深い関数呼び出しはメモリ不足の原因になります。

### 20.6 Cスタック制限

ホスト側で最大Cスタック使用量を設定できます。上限を超えると次のエラーになります。

```text
ERROR: C stack
```

深い再帰や複雑な式は避ける必要があります。

## 21. 使用できない代表例

### 21.1 要素入り配列

```javascript
let a = [1, 2, 3];
```

代わりに:

```javascript
let a = [];
a.push(1);
a.push(2);
a.push(3);
```

### 21.2 複数引数のアロー関数

```javascript
let add = (a, b) => a + b;
```

代わりに:

```javascript
let add = function (a, b) {
  return a + b;
};
```

### 21.3 文字列と数値の直接連結

```javascript
let text = "score=" + 10;
```

代わりに:

```javascript
let text = `score=${10}`;
```

### 21.4 try / catch

```javascript
try {
  run();
} catch (e) {
  handle(e);
}
```

代替手段はホスト側でエラー結果を確認することです。

### 21.5 非同期処理

```javascript
setTimeout(() => {}, 1000);
await task();
```

使用できません。`msleep()` は同期的に停止するだけです。

## 22. 推奨コーディング規約

この処理系では、標準JavaScriptとの差による問題を避けるため、次の形式を推奨します。

1. 文末には必ず `;` を付ける
2. `if`、`while`、`for` の本体には必ず `{}` を付ける
3. 配列は `[]` で作成し、`push()` で要素を追加する
4. 複数引数の関数は `function` 式を使う
5. 数値と文字列の連結にはテンプレートリテラルを使う
6. 日本語や絵文字を添字で分割しない
7. 標準JavaScriptの組み込みオブジェクトがあると仮定しない
8. 長い文字列、大きな配列、深い再帰を避ける
9. `continue` を含む複雑な入れ子ループは十分にテストする
10. `==` よりも、型をそろえた上で比較する

## 23. 動作例

```javascript
let names = [];
names.push("Alice");
names.push("Bob");
names.push("Carol");

let result = names
  .filter(name => name.startsWith("A"))
  .map(name => `Hello ${name}`);

let i = 0;
while (i < result.length) {
  // print() はホスト側で登録されている場合のみ使用可能
  print(result[i]);
  i++;
}
```

## 24. 実装状況一覧

| 分類 | 対応状況 |
|---|---|
| 数値・文字列・真偽値 | 対応 |
| `null`・`undefined` | 対応 |
| オブジェクトリテラル | 限定対応 |
| 配列リテラル | 空配列のみ |
| `let` | 対応、TDZなし |
| `const` | 限定対応 |
| `var` | 限定対応、巻き上げなし |
| `if` / `else` | 対応 |
| `while` | 対応 |
| C形式 `for` | 対応 |
| `break` / `continue` | 対応 |
| `function` 式 | 対応 |
| アロー関数 | 単一引数・引数なし中心 |
| `return` | 対応 |
| `throw Error(...)` | 限定対応 |
| `try` / `catch` | 未対応 |
| クラス | 未対応 |
| Promise・非同期処理 | 未対応 |
| テンプレート文字列 | 限定対応 |
| オプショナルチェーン | 限定対応 |
| Null合体演算子 | 対応 |
| 正規表現 | miniregexによる限定対応 |
| `JSON.parse()` | 引用文字列のデコードのみ |
| `JSON.stringify()` | 未対応 |
| `getchar()` | 独自組み込み |
| `msleep()` | 独自組み込み |
| Unicode文字単位処理 | 未対応、原則バイト単位 |
| DOM・ブラウザAPI | 未対応 |
| Node.js API | 未対応 |
| ES Modules | 未対応 |
