# Gengo プログラミング言語仕様

## 1. 概要

Gengoは、学習用途を想定した小型のインタプリタ型プログラミング言語です。

JavaScript風の構文を持ち、次の機能を提供します。

- 数値、文字列、真偽値
- 配列
- 変数
- 四則演算と剰余
- 比較演算
- 論理演算と短絡評価
- `if` / `else`
- `while`
- `print`
- 配列要素の参照・代入
- 配列の`length`

Gengoのソースコードは、トークンに分解された後、再帰下降構文解析によって逐次実行されます。

---

## 2. 実行方法

Gengo処理系にソースコード文字列を渡して実行します。

```js
program(`
  a = 10;
  b = 3;
  print a % b;
`);
```

実行結果：

```text
1
```

各文の末尾には、原則としてセミコロン`;`を付けます。

```gengo
print 1;
print 2;
```

ブロックの閉じ括弧`}`の直前では、構文上セミコロンを省略できる場合がありますが、常に付けることを推奨します。

---

## 3. 字句仕様

### 3.1 空白

空白、タブ、改行はトークンの区切りとして扱われます。

```gengo
print 1 + 2;
```

次のように改行しても同じ意味です。

```gengo
print
  1
  +
  2
;
```

コメント構文はありません。

---

### 3.2 識別子

変数名などの識別子は、英字またはアンダースコア`_`で始まり、その後に英数字またはアンダースコアを続けられます。

```text
[A-Za-z_][A-Za-z0-9_]*
```

有効な例：

```gengo
x = 1;
score = 10;
user_name = "Taro";
_value2 = 3;
```

無効な例：

```gengo
2value = 3;
```

予約語も字句上は識別子として認識されますが、文脈に応じて特別な意味を持ちます。

- `print`
- `if`
- `else`
- `while`

---

## 4. データ型

Gengoは、内部的にJavaScriptの値を利用します。

主に次の型を扱います。

| 種類 | 例 |
|---|---|
| 整数 | `0`, `10`, `-1` |
| 小数 | `1.1`, `3.14`, `-0.5` |
| 16進整数 | `0x11`, `0XFF` |
| 文字列 | `"hello"` |
| 真偽値 | 比較や論理演算の結果 |
| 配列 | `[1, 2, 3]` |

`true`と`false`はリテラルとしては定義されていません。

真偽値は、比較演算や論理否定などによって生成します。

```gengo
print 1 < 2;
print !0;
```

---

## 5. 数値リテラル

### 5.1 10進整数

```gengo
print 0;
print 123;
```

### 5.2 小数

小数点の前後に、少なくとも1桁ずつ必要です。

```gengo
print 1.1;
print 0.5;
print 12.34;
```

次の省略表記には対応しません。

```gengo
print .5;
print 5.;
```

### 5.3 16進整数

`0x`または`0X`に続けて、16進数字を記述します。

```gengo
print 0x11;
print 0XFF;
```

実行結果：

```text
17
255
```

### 5.4 対応しない数値表現

次の表現には対応しません。

```gengo
1e3
1.0e3
0b1010
0o77
.5
5.
```

---

## 6. 文字列

文字列はダブルクォート`"`で囲みます。

```gengo
print "hello";
```

文字列の解釈にはJSON形式の文字列処理を使用します。

そのため、次のようなエスケープが利用できます。

```gengo
print "hello\nworld";
print "\"Gengo\"";
print "\\";
```

シングルクォート文字列には対応しません。

```gengo
print 'hello';
```

---

## 7. 変数

変数は代入時に作成されます。

宣言キーワードはありません。

```gengo
x = 10;
name = "Gengo";
```

既存の変数へ再代入できます。

```gengo
x = 1;
x = 2;
print x;
```

未定義変数を参照するとエラーになります。

```gengo
print unknown;
```

ただし、短絡評価によって実行されない式の中では、未定義変数を参照してもエラーになりません。

```gengo
print 0 && unknown;
print 1 || unknown;
```

---

## 8. 配列

### 8.1 配列リテラル

角括弧`[]`で配列を作成します。

```gengo
numbers = [10, 20, 30];
empty = [];
```

要素には式を記述できます。

```gengo
a = 2;
numbers = [1, a + 1, 10 % 3];
```

### 8.2 配列要素の参照

```gengo
numbers = [10, 20, 30];
print numbers[0];
```

配列の添字は0から始まります。

### 8.3 配列要素への代入

```gengo
numbers = [10, 20, 30];
numbers[1] = 99;
print numbers[1];
```

### 8.4 配列末尾への追加

添字として現在の配列長を指定すると、要素が末尾に追加されます。

```gengo
numbers = [10, 20];
numbers[2] = 30;
print numbers;
```

内部的には`push`と同等の処理になります。

### 8.5 添字の制約

配列への代入では、添字は次の条件を満たす必要があります。

- 数値である
- 0以上である
- 整数である
- 配列長以下である

配列長より大きい添字には代入できません。

```gengo
numbers = [10, 20];
numbers[5] = 30;
```

### 8.6 配列長

`.length`で配列長を取得できます。

```gengo
numbers = [10, 20, 30];
print numbers.length;
```

実行結果：

```text
3
```

一般的なプロパティ参照には対応していません。

`.length`だけが特別に実装されています。

---

## 9. 演算子

### 9.1 算術演算子

| 演算子 | 意味 |
|---|---|
| `+` | 加算 |
| `-` | 減算 |
| `*` | 乗算 |
| `/` | 除算 |
| `%` | 剰余 |

```gengo
print 1 + 2;
print 10 - 3;
print 4 * 5;
print 10 / 2;
print 10 % 3;
```

`+`は内部的にJavaScriptの加算を使用するため、文字列連結にもなります。

```gengo
print "Hello " + "Gengo";
```

---

### 9.2 単項演算子

| 演算子 | 意味 |
|---|---|
| `+` | 数値への変換 |
| `-` | 符号反転 |
| `!` | 論理否定 |

```gengo
a = -1;
print a;
print +1;
print !0;
print !-1;
```

単項演算子は連続して記述できます。

```gengo
print --1;
print !!1;
```

`--`はデクリメント演算子ではありません。

2つの単項マイナスとして解釈されます。

---

### 9.3 比較演算子

| 演算子 | 意味 |
|---|---|
| `<` | より小さい |
| `<=` | 以下 |
| `>` | より大きい |
| `>=` | 以上 |
| `==` | 等しい |
| `!=` | 等しくない |
| `===` | 型も含めて等しい |
| `!==` | 型または値が異なる |

```gengo
print 1 < 2;
print 1 == 1;
print 1 === 1;
```

`==`と`!=`はJavaScript風の型変換を伴う比較です。

`===`と`!==`は型も比較します。

---

### 9.4 論理演算子

| 演算子 | 意味 |
|---|---|
| `!` | 論理否定 |
| `&&` | 論理積 |
| `||` | 論理和 |

```gengo
print 1 && 2;
print 0 || 5;
print !0;
```

`&&`と`||`は、必ずしも真偽値を返すわけではありません。

JavaScriptと同様に、評価されたオペランドの値を返します。

```gengo
print 1 && 2;
print 0 && 2;
print 0 || 5;
print 3 || 5;
```

実行結果：

```text
2
0
5
3
```

---

## 10. 短絡評価

`&&`と`||`は短絡評価します。

### 10.1 論理積

左辺が偽相当なら、右辺は実行されません。

```gengo
print 0 && unknown;
```

`unknown`は未定義ですが、右辺が評価されないためエラーになりません。

### 10.2 論理和

左辺が真相当なら、右辺は実行されません。

```gengo
print 1 || unknown;
```

この場合も`unknown`は評価されません。

### 10.3 構文解析は行われる

短絡された式も、構文としては正しくなければなりません。

```gengo
print 1 || (1 + );
```

右辺は実行されませんが、構文エラーになります。

---

## 11. 演算子の優先順位

優先順位は上ほど高くなります。

| 優先順位 | 演算子・構文 |
|---:|---|
| 1 | 配列参照`[]`、`.length` |
| 2 | 単項`!`、単項`+`、単項`-` |
| 3 | `*`、`/`、`%` |
| 4 | `+`、`-` |
| 5 | `<`、`<=`、`>`、`>=`、`==`、`!=`、`===`、`!==` |
| 6 | `&&` |
| 7 | `||` |

例：

```gengo
print 1 + 2 * 3;
```

これは次のように解釈されます。

```gengo
print 1 + (2 * 3);
```

論理積は論理和より優先されます。

```gengo
print 1 || 0 && 0;
```

これは次のように解釈されます。

```gengo
print 1 || (0 && 0);
```

括弧で評価順序を明示できます。

```gengo
print (1 + 2) * 3;
```

---

## 12. print文

式の値を出力します。

```gengo
print 123;
print "hello";
print 1 + 2;
```

`print`を付けずに式文を記述した場合も、現在の実装では値が出力されます。

```gengo
1 + 2;
```

ただし、可読性のため`print`を付けることを推奨します。

---

## 13. if文

条件が真相当の場合にブロックを実行します。

```gengo
x = 10;

if (x > 0) {
  print "positive";
}
```

`else`を記述できます。

```gengo
x = -1;

if (x >= 0) {
  print "zero or positive";
} else {
  print "negative";
}
```

波括弧`{}`は必須です。

次のような1文形式には対応しません。

```gengo
if (x > 0) print x;
```

---

## 14. while文

条件が真相当である間、ブロックを繰り返します。

```gengo
i = 0;

while (i < 3) {
  print i;
  i = i + 1;
}
```

実行結果：

```text
0
1
2
```

`break`と`continue`には対応しません。

---

## 15. 真偽判定

条件式では、JavaScriptと同様の真偽判定を使用します。

主な偽相当の値：

- `0`
- 空文字列`""`
- 比較結果の`false`

主な真相当の値：

- 0以外の数値
- 空でない文字列
- 配列
- 比較結果の`true`

```gengo
if (0) {
  print "not executed";
}

if (1) {
  print "executed";
}
```

---

## 16. BNF

以下はGengoの構文を表す簡略BNFです。

字句解析上の空白は省略しています。

```bnf
<program> ::= { <statement> }

<statement> ::= ";"
              | <assignment> ";"
              | <print-statement> ";"
              | <expression-statement> ";"
              | <if-statement>
              | <while-statement>

<assignment> ::= <identifier> "=" <expression>
               | <identifier> "[" <expression> "]" "=" <expression>

<print-statement> ::= "print" <expression>

<expression-statement> ::= <expression>

<if-statement> ::= "if" "(" <expression> ")" <block>
                   [ "else" <block> ]

<while-statement> ::= "while" "(" <expression> ")" <block>

<block> ::= "{" { <statement> } "}"

<expression> ::= <logical-or>

<logical-or> ::= <logical-and>
                 { "||" <logical-and> }

<logical-and> ::= <comparison>
                  { "&&" <comparison> }

<comparison> ::= <additive>
                 { <comparison-operator> <additive> }

<comparison-operator> ::= "<"
                        | "<="
                        | ">"
                        | ">="
                        | "=="
                        | "!="
                        | "==="
                        | "!=="

<additive> ::= <multiplicative>
               { ( "+" | "-" ) <multiplicative> }

<multiplicative> ::= <unary>
                     { ( "*" | "/" | "%" ) <unary> }

<unary> ::= ( "!" | "+" | "-" ) <unary>
          | <postfix>

<postfix> ::= <primary>
              { "[" <expression> "]" | "." "length" }

<primary> ::= <number>
            | <string>
            | <identifier>
            | <array-literal>
            | "(" <expression> ")"

<array-literal> ::= "[" [ <expression>
                          { "," <expression> } ] "]"

<number> ::= <decimal-integer>
           | <decimal-fraction>
           | <hexadecimal-integer>

<decimal-integer> ::= <digit> { <digit> }

<decimal-fraction> ::= <digit> { <digit> }
                       "."
                       <digit> { <digit> }

<hexadecimal-integer> ::= "0" ( "x" | "X" )
                          <hex-digit> { <hex-digit> }

<string> ::= "\"" { <string-character> | <escape-sequence> } "\""

<identifier> ::= <identifier-start> { <identifier-part> }

<identifier-start> ::= <letter> | "_"

<identifier-part> ::= <letter> | <digit> | "_"

<digit> ::= "0" | "1" | "2" | "3" | "4"
          | "5" | "6" | "7" | "8" | "9"

<hex-digit> ::= <digit>
              | "a" | "b" | "c" | "d" | "e" | "f"
              | "A" | "B" | "C" | "D" | "E" | "F"

<letter> ::= "A" | ... | "Z" | "a" | ... | "z"
```

---

## 17. 字句解析用正規表現

現在の実装では、おおむね次の正規表現でトークンを取り出します。

```js
/[A-Za-z_][A-Za-z0-9_]*|"(?:\\.|[^"\\])*"|0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?|===|!==|&&|\|\||==|!=|<=|>=|[-+*/%!=()={};<>\[\],.]|\s+/y
```

長い演算子を短い演算子より先に並べる必要があります。

たとえば`!==`は、`!=`や`!`より前に認識されます。

---

## 18. エラー

構文エラーや未定義変数などが発生すると、ソースコードとエラー位置が表示されます。

例：

```gengo
print 1 + ;
```

表示例：

```text
print 1 + ;
          ^
```

主なエラー条件：

- 未定義変数の参照
- 未対応の文字やトークン
- 閉じ括弧の不足
- 配列添字の不正
- 配列長を超える位置への代入
- 式の途中での予期しないトークン

---

## 19. 制限事項

現在のGengoには、次の機能はありません。

- コメント
- 関数定義
- 関数呼び出し
- ローカル変数
- スコープ
- `for`
- `break`
- `continue`
- `return`
- オブジェクトリテラル
- 一般的なプロパティ参照
- 複合代入演算子
- インクリメント・デクリメント
- 三項演算子
- ビット演算
- `true`、`false`、`null`リテラル
- 指数表記
- 2進数・8進数リテラル
- シングルクォート文字列
- 配列範囲外参照の明示的な検査

また、変数はすべて単一の共有領域に保存されます。

`if`や`while`のブロックによる新しいスコープは作られません。

---

## 20. サンプルプログラム

### 20.1 FizzBuzz

```gengo
i = 1;

while (i <= 20) {
  if (i % 15 == 0) {
    print "FizzBuzz";
  } else {
    if (i % 3 == 0) {
      print "Fizz";
    } else {
      if (i % 5 == 0) {
        print "Buzz";
      } else {
        print i;
      }
    }
  }

  i = i + 1;
}
```

### 20.2 配列の合計

```gengo
numbers = [10, 20, 30, 40];
i = 0;
total = 0;

while (i < numbers.length) {
  total = total + numbers[i];
  i = i + 1;
}

print total;
```

### 20.3 条件範囲の判定

```gengo
x = 5;

if (x >= 1 && x <= 10) {
  print "1から10の範囲です";
} else {
  print "範囲外です";
}
```

### 20.4 短絡評価

```gengo
x = 0;

print x != 0 && 10 / x;
print x == 0 || unknown;
```

---

## 21. 実装上の構文階層

現在の再帰下降構文解析では、式を次の順で処理します。

```text
logicalOr
  logicalAnd
    comparison
      expression
        term
          factor
```

対応関係：

| 関数 | 主な処理 |
|---|---|
| `logicalOr` | `||` |
| `logicalAnd` | `&&` |
| `comparison` | 比較演算子 |
| `expression` | `+`, `-` |
| `term` | `*`, `/`, `%` |
| `factor` | リテラル、変数、配列、括弧、単項演算子 |

短絡評価では、右辺を構文解析しながら、`run=false`を渡して実際の評価だけを停止します。

これにより、右辺の構文チェックを行いつつ、未定義変数参照や計算などの副作用を回避します。
