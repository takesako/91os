# 91os

RISC-Vの独自OSの上で小型JavaScript処理系を動かし、そのJavaScript上で独自言語Gengoを実行する学習用プロジェクトです。

最初はC言語の `printf` だけが動く小さなカーネルを作ります。その後、Elk、Gengo処理系、起動スクリプト、REPLの機能を段階的に追加します。

完成版ソースコードはこちらで公開しています。

[https://github.com/takesako/91os](https://github.com/takesako/91os)

## このプロジェクトで作るもの

最終的な実行環境は、次の層で構成されます。

```mermaid
flowchart TD
    A[macOSターミナル] --> B[run.sh]
    B --> C[ClangでRISC-V用os.elfを作成]
    C --> D[QEMU RISC-V仮想マシン]
    D --> E[OpenSBI]
    E --> F[Cで作った最小カーネル]
    F --> G[Elk JavaScript処理系]
    G --> H[gengo.js]
    H --> I[shell.gengo]
    H --> J[Gengo REPL]
```

CPUが直接実行するのは、C言語から生成したRISC-Vの機械語です。

その上でElkがJavaScriptを解釈し、Elk上で動く `gengo.js` がGengoのソースコードを解釈します。

```text
RISC-V機械語
  Cで作った最小カーネル
    Elk
      gengo.js
        shell.gengo
        REPLへ入力したGengoコード
```

## 到達目標

このプロジェクトを通して、次の内容を理解します。

- RISC-V向けの最小カーネルをビルドする方法
- リンカスクリプトと起動処理の役割
- OSのない環境で必要になる小さなCライブラリ
- QEMU、OpenSBI、カーネルの関係
- C関数をJavaScriptへ公開する方法
- JavaScriptで独自言語処理系を作る方法
- ソースコードをC配列へ変換してカーネルへ埋め込む方法
- REPLのRead、Eval、Print、Loopの流れ
- Gitによる段階的な開発とGitHubへのpush

## 動作環境

このREADMEは、次の環境を想定しています。

- macOS
- Homebrew
- VS Code
- Git
- LLVM
- QEMU
- `xxd`

必要なコマンドを確認します。

```bash
brew --version
git --version
code --version
command -v xxd
```

たとえば、このような表示になればokです。
```text
Homebrew 6.0.12-140-g15b4330
git version 2.50.1 (Apple Git-155)
1.129.1
8a7abeba6e03ea3af87bfbce9a1b7e48fed567b8
arm64
/usr/bin/xxd
```

LLVMとQEMUが未導入の場合は、次を実行します。

```bash
brew install llvm qemu
```

`xxd` が見つからない場合は、vimを導入します。

```bash
brew install vim
command -v xxd
```

## 作業フォルダを作る

作業フォルダを作成して、OSの起動や言語処理系の仕組みを理解しながら作ります。

```bash
cd ~
mkdir 91os
cd 91os
code .
```

現在のフォルダ位置をファイル一覧を確認します。

```bash
pwd
ls -la
```

Gitリポジトリを開始します。

```bash
git init
git branch -M main
```

生成物をGitへ登録しないため、`.gitignore` を作ります。

```bash
code .gitignore
```

次を保存します。

```text
os.elf
os.map
gengo.h
shell.h
.DS_Store
```

最初のコミットを作ります。

```bash
git add .gitignore
git commit -m "init"
```

## 演習1 `printf` だけが動く最小OS

最初は次のファイルだけを作ります。

```text
91os-step/
├── .gitignore
├── boot.ld
├── kernel.c
├── stdio.h
├── libc.h
├── libc.c
├── main.c
└── run.sh
```

Elk、正規表現、Gengoはまだ追加しません。

### `boot.ld`

完成版のリンカスクリプトを参照します。

[boot.ld](https://github.com/takesako/91os/blob/main/boot.ld)

```bash
code boot.ld
```

`boot.ld` は、コード、読み取り専用データ、データ、BSS、スタックをメモリのどこへ配置するか指定します。

### `kernel.c`

完成版のカーネルを参照します。

[kernel.c](https://github.com/takesako/91os/blob/main/kernel.c)

```bash
code kernel.c
```

`kernel.c` の主な役割は次のとおりです。

- スタックポインタの設定
- BSS領域の初期化
- `main` 関数の呼び出し
- OpenSBIを利用した文字入出力
- 待機処理
- QEMUの終了

### `stdio.h`

完成版のヘッダを参照します。

[stdio.h](https://github.com/takesako/91os/blob/main/stdio.h)

```bash
code stdio.h
```

### 最小の `libc.h`

```bash
code libc.h
```

次を保存します。

```c
#ifndef LIBC_H
#define LIBC_H

typedef __SIZE_TYPE__ size_t;

#endif
```

この段階では、`size_t` だけを用意します。

### 最小の `libc.c`

```bash
code libc.c
```

次を保存します。

```c
#include "stdio.h"

int printf(const char *format, ...) {
    int count = 0;

    while (*format != '\0') {
        putchar(*format);
        format++;
        count++;
    }

    return count;
}
```

この `printf` は、書式指定を処理しません。

受け取った文字列を1文字ずつ `putchar` へ渡すだけです。

### 最初の `main.c`

```bash
code main.c
```

次を保存します。

```c
#include "stdio.h"

void main(void) {
    printf("Hello my OS!\n");
}
```

### 最初の `run.sh`

```bash
code run.sh
```

次を保存します。

```bash
#!/bin/bash
set -xue

QEMU=qemu-system-riscv64
CC="$(brew --prefix llvm)/bin/clang"

CFLAGS='-std=c11 -O2 -g3 -Wall -Wextra \
--target=riscv64-unknown-elf \
-march=rv64imafd -mabi=lp64d -mcmodel=medany \
-fuse-ld=lld -fno-stack-protector \
-ffreestanding -nostdlib'

$CC $CFLAGS \
-Wl,-Tboot.ld \
-Wl,-Map=os.map \
-o os.elf \
kernel.c libc.c main.c

$QEMU \
-machine virt \
-bios default \
-nographic \
-serial mon:stdio \
--no-reboot \
-kernel os.elf
```

実行権限を付けます。

```bash
chmod +x run.sh
```

実行します。

```bash
./run.sh
```

次が表示されれば成功です。

```text
Hello my OS!
```

### 文字が表示されるまで

```mermaid
flowchart LR
    A[main.cのprintf] --> B[libc.cのprintf]
    B --> C[kernel.cのputchar]
    C --> D[OpenSBI]
    D --> E[QEMU]
    E --> F[macOSターミナル]
```

### わざと失敗する

`main.c` を次のように変更します。

```c
#include "stdio.h"

void main(void) {
    printf("1 + 2 = %d\n", 3);
}
```

再実行します。

```bash
./run.sh
```

最小版の `printf` では、次のように表示されます。

```text
1 + 2 = %d
```

数値の `3` は表示されません。

最小版の `printf` は `%d` を解析せず、文字列をそのまま表示しているためです。

確認後、`main.c` を元へ戻します。

### Gitへ保存する

```bash
git add .
git commit -m "step1: print text on minimal os"
```

## 演習2 Cライブラリを完成版へ置き換える

Elkを動かすには、メモリ操作、文字列操作、数値変換、ヒープ管理、書式付き出力が必要です。

最小版の `libc.h` と `libc.c` を、リポジトリの完成版へ置き換えます。

- [libc.h](https://github.com/takesako/91os/blob/main/libc.h)
- [libc.c](https://github.com/takesako/91os/blob/main/libc.c)

```bash
code libc.h
code libc.c
```

自作のlibcで実装している標準関数は以下のとおりです。

- `memcpy`
- `memset`
- `memcmp`
- `strlen`
- `strchr`
- `strtod`
- `snprintf`
- `printf`
- `malloc`
- `free`
- `heap_init`

`main.c` を次のように変更します。

```c
#include "stdio.h"

void main(void) {
    printf("1 + 2 = %d\n", 3);
}
```

実行します。

```bash
./run.sh
```

次が表示されれば成功です。

```text
1 + 2 = 3
```

Gitへ保存します。

```bash
git add libc.h libc.c main.c
git commit -m "step2: add small C library"
```

## 演習3 ElkでJavaScriptを動かす

### 必要なファイルを追加する

次のファイルをリポジトリから作業フォルダへ保存します。

- [miniregex.h](https://github.com/takesako/91os/blob/main/miniregex.h)
- [miniregex.c](https://github.com/takesako/91os/blob/main/miniregex.c)
- [elk.h](https://github.com/takesako/91os/blob/main/elk.h)
- [elk.c](https://github.com/takesako/91os/blob/main/elk.c)

VS Codeで各ファイルを作ります。

```bash
code miniregex.h
code miniregex.c
code elk.h
code elk.c
```

`elk.c` は、Cesanta Softwareによる小型JavaScript処理系Elkにいくつかの拡張機能を追加したものになっています。

### `run.sh` を変更する

コンパイル対象へ `miniregex.c` と `elk.c` を追加します。

```bash
$CC $CFLAGS \
-Wl,-Tboot.ld \
-Wl,-Map=os.map \
-o os.elf \
kernel.c libc.c miniregex.c elk.c main.c
```

### JavaScriptを評価する

`main.c` では、次の処理を行います。

1. Elk用メモリを用意する
2. `js_create` でJavaScript処理系を作る
3. C関数をJavaScriptの `print` として登録する
4. `js_eval` でJavaScriptを評価する
5. エラーが発生したら表示して終了する

VS Codeで`main.c` ファイルを作ります。

```bash
code main.c
```

以下のコードを書きます。

```c
#include "stdio.h"
#include "libc.h"
#include "elk.h"

static jsval_t js_print(struct js *js, jsval_t *args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (i > 0) {
            putchar(' ');
        }
        if (js_type(args[i]) == JS_STR) {
            size_t len;
            char *str = js_getstr(js, args[i], &len);

            for (size_t j = 0; j < len; j++) {
                putchar(str[j]);
            }
        } else {
            const char *str = js_str(js, args[i]);

            while (*str != '\0') {
                putchar(*str);
                str++;
            }
        }
    }
    putchar('\n');
    return js_mkundef();
}

void main(void) {
    static unsigned char js_memory[4 * 1024 * 1024];
    static unsigned char heap[1024 * 1024];

    heap_init(heap, sizeof(heap));

    struct js *js = js_create(js_memory, sizeof(js_memory));

    js_set(
        js,
        js_glob(js),
        "print",
        js_mkfun(js_print)
    );

    const char *source =
        "let answer = 6 * 7;"
        "print('Hello from Elk');"
        "print(answer);";

    jsval_t result = js_eval(js, source, ~0U);

    if (js_type(result) == JS_ERR) {
        printf("%s\n", js_str(js, result));
        exit(1);
    }

    printf("JavaScript finished.\n");
}
```

最初はGengo部分を使わず、次のようなJavaScriptだけを評価しています。

```javascript
let answer = 6 * 7;
print("Hello from Elk");
print(answer);
```

実行します。

```bash
./run.sh
```

次が表示されれば成功です。

```text
Hello from Elk
42
```

### JavaScript仕様

このプロジェクトのElkは、標準JavaScriptの全機能を実装しているわけではありません。

対応するJavaScriptの構文と制限事項は、次の仕様書を確認してください。

[JavaScript.md](https://github.com/takesako/91os/blob/main/JavaScript.md)

### Gitへ保存する

```bash
git add .
git commit -m "step3: run JavaScript with Elk"
```

## 演習4 Gengo処理系を動かす

### `gengo.js` を追加する

Gengo処理系はJavaScriptで書きます。

[gengo.js](https://github.com/takesako/91os/blob/main/gengo.js)

```bash
code gengo.js
```

`gengo.js` は主に次の処理を行います。

1. Gengoソースをトークンへ分解する
2. トークンの種類を判定する
3. 演算子の優先順位に従って式を解析する
4. 代入、条件分岐、繰り返しを実行する
5. エラー位置を表示する

### `shell.gengo` を追加する

起動時に自動実行するGengoプログラムを作ります。

```bash
code shell.gengo
```

次を保存します。

```text
print "Hello from Gengo";

count = 1;

while (count < 4) {
    print count;
    count = count + 1;
}
```

### ソースコードをC配列へ変換する

このカーネルにはファイルシステムがありません。

そのため、起動後に `gengo.js` と `shell.gengo` をファイルとして開くことはできません。

`xxd -i` でC配列へ変換し、`os.elf` へ埋め込みます。

`run.sh` のコンパイル処理より前へ、次を追加します。

```bash
xxd -i gengo.js > gengo.h
xxd -i shell.gengo > shell.h
```

生成されるファイルは次の2つです。

```text
gengo.h
shell.h
```

これらは自動で生成されるファイルなので直接編集することはしません。

### `main.c` でGengoを起動する

`main.c` では、次の順番で処理します。

```mermaid
flowchart TD
    A[Elkを作成] --> B[C関数をJavaScriptへ登録]
    B --> C[gengo.jsをjs_evalで評価]
    C --> D[program関数が作られる]
    D --> E[shell.gengoをsourceへ設定]
    E --> F[program sourceを評価]
    F --> G[Gengoプログラムが動く]
```

`main.c` ファイルを開きます。

```bash
code main.c
```

以下のコードに書き換えます。

```c
#include "stdio.h"
#include "libc.h"
#include "elk.h"
#include "gengo.h"
#include "shell.h"

static jsval_t js_print(struct js *js,jsval_t *args,int nargs){
  for(int i=0;i<nargs;i++){
    if(i)putchar(' ');
    if(js_type(args[i])==JS_STR){
      size_t len;
      char *str=js_getstr(js,args[i],&len);
      for(size_t j=0;j<len;j++)putchar(str[j]);
    }else{
      const char *str=js_str(js,args[i]);
      while(*str)putchar(*str++);
    }
  }
  putchar('\n');
  return js_mkundef();
}

static int input(char *source,int size){
  int ch,len=0;
  printf("> ");

  while((ch=getchar())!='\r'&&ch!='\n'){
    if((ch==8||ch==127)&&len)
      printf("\b \b"),len--;
    else if(ch>=32&&ch<127&&len<size-1)
      source[len++]=(char)ch,putchar((char)ch);
  }

  putchar('\n');
  source[len]=0;
  return len;
}

void main(void){
  static unsigned char memory[16*1024*1024],heap[1024*1024];
  char source[256*1024];

  heap_init(heap,sizeof(heap));
  struct js *js=js_create(memory,sizeof(memory));

#define SET(name,function) js_set(js,js_glob(js),name,js_mkfun(function))
  SET("print",js_print);
#undef SET

  jsval_t result=js_eval(js,(char *)gengo_js,gengo_js_len);

  if(js_type(result)==JS_ERR){
    printf("%s\n",js_str(js,result));
    exit(1);
  }

  js_set(js,js_glob(js),"source",
         js_mkstr(js,(char *)shell_gengo,shell_gengo_len));

  result=js_eval(js,"program(source);",~0U);

  if(js_type(result)==JS_ERR){
    printf("%s\n",js_str(js,result));
    exit(1);
  }

  printf("\nGengo lang REPL (try 1+2, type exit to quit)\n");

  for(;;){
    int len=input(source,sizeof(source));
    if(!len)continue;

    if(len==4&&!memcmp(source,"exit",4))
      js_exit(js,0,0);

    js_set(js,js_glob(js),"source",js_mkstr(js,source,len));
    result=js_eval(js,"program(source);",~0U);

    if(js_type(result)==JS_ERR)
      printf("%s\n",js_str(js,result));
  }
}
```

実行します。

```bash
./run.sh
```

次のように表示されれば成功です。

```text
Hello from Gengo
1
2
3
```

### Gengo仕様

Gengoの構文、演算子、配列、条件分岐、繰り返し、制限事項は次を参照してください。

[GengoLang.md](https://github.com/takesako/91os/blob/main/GengoLang.md)

### Gitへ保存する

```bash
git add .
git commit -m "step4: execute shell.gengo"
```

## 演習5 Gengo REPLを作る

REPLは、次の処理を繰り返す仕組みです。

```text
Read
  入力を読む

Eval
  入力を評価する

Print
  結果やエラーを表示する

Loop
  次の入力へ戻る
```

完成版のREPLは `main.c` に実装されています。

[main.c](https://github.com/takesako/91os/blob/main/main.c)

主な処理は次のとおりです。

1. `input` 関数で1行を読む
2. 入力文字列をJavaScript側の `source` へ設定する
3. `program(source);` をElkで評価する
4. Gengo処理系が入力を実行する
5. エラーがあれば表示する
6. 次の入力へ戻る

実行します。

```bash
./run.sh
```

プロンプトが表示されたら、次を入力します。

```text
1 + 2;
```

現在の実装では、式文も値を表示します。

```text
3
```

次も試します。

```text
print 1 + 2 * 3;
```

```text
7
```

変数と配列も使用できます。

```text
name = "Gengo";
```

```text
print "Hello, " + name;
```

```text
values = [10, 20, 30];
```

```text
print values[1];
```

終了するときは次を入力します。

```text
exit
```

### REPLの制約

現在の入力処理は、Enterが押されるまでの1行を読みます。

複数行のプログラムは、1行へまとめて入力します。

```text
i = 0; while (i < 3) { print i; i = i + 1; }
```

### Gitへ保存する

```bash
git add .
git commit -m "step5: add Gengo REPL"
```

## ミニプロジェクト

`shell.gengo` を変更し、起動時に動くオリジナル作品を作ります。

次の条件を満たしてください。

- タイトルを表示する
- 変数を2個以上使う
- `while` を1回以上使う
- `if` または `else` を1回以上使う
- 配列を1個以上使う
- `msleep` で表示に変化を付ける
- 最後にREPLへ移動する

### 作品の例

- カウントダウン
- 文字が移動するアニメーション
- 棒グラフ風の進捗表示
- 配列から順番にメッセージを表示する案内板
- 偶数と奇数を分類するデモ
- 簡単な占い
- ミニクイズ

### 起動メッセージの例

```text
print "\n";
frames = [
  "🐱　　",
  "　🐱　",
  "　　🐱",
  "　🐱　"
];
i = 0;
while (i < 5) {
  j = 0;
  while (j < 4) {
    print "\u001b[2K\u001b[1A" + frames[j];
    msleep 50;
    j = j + 1;
  }
  i = i + 1;
}
print "\u001b[2K\u001b[1A✨ WELCOME ✨";
```

文字列、待ち時間、条件分岐、表示順などを変更し、自分オリジナル作品にしてください。

### 動作確認

```bash
./run.sh
```

次の項目を確認します。

- ビルドエラーがない
- QEMUが起動する
- `shell.gengo` が最後まで動く
- REPLのプロンプトが表示される
- Gengoの式を実行できる
- `exit` で終了できる

Gitへ保存します。

```bash
git add shell.gengo
git commit -m "my shell"
```

## ファイル構成

完成時の主なファイルは次のとおりです。

```text
91os/
├── .git/
├── .gitignore
├── boot.ld
├── kernel.c
├── stdio.h
├── libc.h
├── libc.c
├── miniregex.h
├── miniregex.c
├── elk.h
├── elk.c
├── gengo.js
├── shell.gengo
├── main.c
├── run.sh
├── gengo.h
├── shell.h
├── os.elf
└── os.map
```

`gengo.h`、`shell.h`、`os.elf`、`os.map` は生成物です。

削除しても `./run.sh` で作り直せます。

## 各ファイルの役割

| ファイル | 役割 |
|---|---|
| `run.sh` | 埋め込み、ビルド、QEMU起動 |
| `boot.ld` | コード、データ、BSS、スタックの配置 |
| `kernel.c` | 起動処理、文字入出力、待機、終了 |
| `stdio.h` | 入出力関数の宣言 |
| `libc.h` | 型、文字列、メモリ、ヒープ関数の宣言 |
| `libc.c` | Elkに必要な小さなCライブラリ |
| `miniregex.h` | 簡易正規表現関数の宣言 |
| `miniregex.c` | 簡易正規表現の実装 |
| `elk.h` | Elkの公開API |
| `elk.c` | JavaScript処理系本体 |
| `gengo.js` | Gengoの字句解析、構文解析、実行 |
| `shell.gengo` | 起動時に実行するGengoプログラム |
| `main.c` | C、Elk、Gengo、REPLを接続する処理 |
| `gengo.h` | `gengo.js` から自動生成されるC配列 |
| `shell.h` | `shell.gengo` から自動生成されるC配列 |

## 完成版ソースコード

完全版のソースコードはGitHub上で確認できます。

### ビルドとカーネル

- [run.sh](https://github.com/takesako/91os/blob/main/run.sh)
- [boot.ld](https://github.com/takesako/91os/blob/main/boot.ld)
- [kernel.c](https://github.com/takesako/91os/blob/main/kernel.c)
- [main.c](https://github.com/takesako/91os/blob/main/main.c)

### Cライブラリ

- [stdio.h](https://github.com/takesako/91os/blob/main/stdio.h)
- [libc.h](https://github.com/takesako/91os/blob/main/libc.h)
- [libc.c](https://github.com/takesako/91os/blob/main/libc.c)

### 正規表現

- [miniregex.h](https://github.com/takesako/91os/blob/main/miniregex.h)
- [miniregex.c](https://github.com/takesako/91os/blob/main/miniregex.c)

### JavaScript処理系

- [elk.h](https://github.com/takesako/91os/blob/main/elk.h)
- [elk.c](https://github.com/takesako/91os/blob/main/elk.c)
- [JavaScript処理系仕様](https://github.com/takesako/91os/blob/main/JavaScript.md)

### Gengo処理系

- [gengo.js](https://github.com/takesako/91os/blob/main/gengo.js)
- [shell.gengo](https://github.com/takesako/91os/blob/main/shell.gengo)
- [Gengoプログラミング言語仕様](https://github.com/takesako/91os/blob/main/GengoLang.md)

### リポジトリ全体

- [takesako/91os](https://github.com/takesako/91os)

## GitHubへpushする

自分のGitHubアカウントで`91os` という名前の空のリポジトリを作ります。

作成後に表示されるURLを使って、リモートリポジトリを登録します。

```bash
git remote add origin https://github.com/USERNAME/91os.git
```

※ `USERNAME` は自分のGitHubのアカウント名に置き換えてください。

登録内容を確認します。

```bash
git remote -v
```

最初のpushを実行します。

```bash
git push -u origin main
```

2回目以降は次だけで送信できます。

```bash
git push
```

すでに `origin` が登録されている場合は、URLを変更します。

```bash
git remote set-url origin https://github.com/USERNAME/91os.git
```

## トラブルシューティング

### `brew: command not found`

Homebrewを導入したあと、新しいターミナルを開いて確認します。

```bash
brew --version
```

### `qemu-system-riscv64: command not found`

QEMUを導入します。

```bash
brew install qemu
```

### Clangが見つからない

LLVMの場所を確認します。

```bash
brew --prefix llvm
ls "$(brew --prefix llvm)/bin/clang"
```

`run.sh` では次の指定を使います。

```bash
CC="$(brew --prefix llvm)/bin/clang"
```

### `permission denied: ./run.sh`

実行権限を付けます。

```bash
chmod +x run.sh
```

### `xxd: command not found`

Vimを導入します。

```bash
brew install vim
command -v xxd
```

### `gengo.h` または `shell.h` が見つからない

この2ファイルは `run.sh` が自動生成します。

```bash
./run.sh
ls gengo.h shell.h
```

### QEMUを終了できない

`Control + A` を押して離し、その後 `X` を押します。

### Gengoへコメントを書いたらエラーになった

現在のGengoにはコメント構文がありません。

`//` や `/* */` を削除します。

### REPLへ複数行を入力できない

現在のREPLは1行入力です。

プログラムを1行へまとめます。

```text
i = 0; while (i < 3) { print i; i = i + 1; }
```

## 理解確認

### 問1

`printf("Hello\n");` からmacOSのターミナルへ文字が表示されるまでの経路を説明してください。

### 問2

最初に作った `printf` で `%d` が使えなかった理由を説明してください。

### 問3

`xxd -i` が必要な理由を説明してください。

### 問4

`elk.c` と `gengo.js` は、それぞれ何を解釈しますか。

### 問5

REPLへ入力したGengoコードは、最終的にどのJavaScript関数へ渡されますか。

## 解答例

問1は、`main.c` の `printf`、`libc.c` の `printf`、`kernel.c` の `putchar`、OpenSBI、QEMU、macOSターミナルの順です。

問2は、最初の `printf` が書式文字列を解析せず、受け取った文字をそのまま `putchar` へ渡していたためです。

問3は、カーネルにファイルシステムがなく、起動後に `gengo.js` や `shell.gengo` をファイルとして開けないためです。C配列へ変換して `os.elf` へ埋め込みます。

問4は、`elk.c` がJavaScriptを解釈し、`gengo.js` がGengoを解釈します。

問5は、入力文字列をJavaScript側の `source` へ設定し、Elk上で `program(source);` を評価します。
