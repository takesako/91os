let s, t, p, i;
const names = [], values = [];

const error = n => {
  if (n < 0) n = 0;
  if (n > s.length) n = s.length;
  let start = n, end = n, line = 1;
  let text = '', mark = '';
  while (start > 0 && s[start - 1] != '\n') start--;
  while (end < s.length && s[end] != '\n') end++;
  for (let j = 0; j < start; j++) {
    if (s[j] == '\n') line++;
  }
  for (let j = start; j < end; j++) {
    if (s[j] != '\r') text += s[j];
  }
  for (let j = start; j < n; j++) {
    mark += s[j] == '\t' ? '\t' : ' ';
  }
  throw Error(`\nline ${line}:\n${text}\n${mark}^`);
};

const identifier = n =>
  n != undefined && /^[A-Za-z_][A-Za-z0-9_]*$/.test(n);

const number = n =>
  n != undefined &&
  /^(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?)$/.test(n);

const find = function(name) {
  let found = -1;
  for (let k = 0; k < names.length; k++) {
    if (names[k] == name) found = k;
  }
  return found;
};

const get = function(name, position) {
  let k = find(name);
  if (k < 0) error(position);
  return values[k];
};

const set = function(name, value) {
  let k = find(name);
  if (k < 0) {
    names.push(name);
    values.push(value);
  } else {
    values[k] = value;
  }
  return value;
};

const expect = function(token) {
  if (t[i] != token) error(p[i] ?? s.length);
  i++;
};

const arrayLiteral = function(run) {
  expect('[');
  let array;
  if (run) array = [];
  if (t[i] != ']') {
    let n = logicalOr(run);
    if (run) array.push(n);
    while (t[i] == ',') {
      i++;
      n = logicalOr(run);
      if (run) array.push(n);
    }
  }
  expect(']');
  return run ? array : 0;
};

const factor = function(run) {
  let n;
  if (t[i] == '!' || t[i] == '-' || t[i] == '+') {
    let o = t[i++];
    n = factor(run);
    if (run) n = o == '!' ? !n : o == '-' ? -n : +n;
  } else if (t[i] != undefined && /^"/.test(t[i])) {
    n = JSON.parse(t[i++]);
    if (!run) n = 0;
  } else if (number(t[i])) {
    n = +t[i++];
    if (!run) n = 0;
  } else if (t[i] == 'getchar' || t[i] == 'getchar_nonblock') {
    let name = t[i++];
    if (t[i] == '(') {
      i++;
      expect(')');
    }
    n = run
      ? name == 'getchar' ? getchar() : getchar_nonblock()
      : -1;
  } else if (identifier(t[i])) {
    let name = t[i], position = p[i++];
    n = run ? get(name, position) : 0;
  } else if (t[i] == '(') {
    i++;
    n = logicalOr(run);
    expect(')');
  } else if (t[i] == '[') {
    n = arrayLiteral(run);
  } else {
    return error(p[i] ?? s.length);
  }
  while (t[i] == '[' || t[i] == '.') {
    if (t[i] == '[') {
      i++;
      let index = logicalOr(run);
      expect(']');
      if (run) n = n[index];
    } else {
      i++;
      if (t[i] != 'length') error(p[i] ?? s.length);
      i++;
      if (run) n = n.length;
    }
  }
  return n;
};

const term = function(run) {
  let n = factor(run);
  while (t[i] == '*' || t[i] == '/' || t[i] == '%') {
    let o = t[i++], m = factor(run);
    if (run) {
      n = o == '*' ? n * m :
          o == '/' ? n / m :
          n % m;
    }
  }
  return n;
};

const expression = function(run) {
  let n = term(run);
  while (t[i] == '+' || t[i] == '-') {
    let o = t[i++], m = term(run);
    if (run) n = o == '+' ? n + m : n - m;
  }
  return n;
};

const comparisonOperator = o =>
  o == '<' || o == '<=' || o == '>' || o == '>=' ||
  o == '==' || o == '!=' || o == '===' || o == '!==';

const comparison = function(run) {
  let n = expression(run);
  while (comparisonOperator(t[i])) {
    let o = t[i++], m = expression(run);
    if (run) {
      n = o == '<' ? n < m :
          o == '<=' ? n <= m :
          o == '>' ? n > m :
          o == '>=' ? n >= m :
          o == '==' ? n == m :
          o == '!=' ? n != m :
          o == '===' ? typeof n == typeof m && n == m :
          typeof n != typeof m || n != m;
    }
  }
  return n;
};

const logicalAnd = function(run) {
  let n = comparison(run);
  while (t[i] == '&&') {
    i++;
    let m = comparison(run && !!n);
    if (run) n = n && m;
  }
  return n;
};

const logicalOr = function(run) {
  let n = logicalAnd(run);
  while (t[i] == '||') {
    i++;
    let m = logicalAnd(run && !n);
    if (run) n = n || m;
  }
  return n;
};

const finish = function() {
  if (t[i] == ';') {
    i++;
  } else if (i < t.length && t[i] != '}') {
    error(p[i]);
  }
};

const block = function(run) {
  expect('{');
  while (i < t.length && t[i] != '}') {
    statement(run);
  }
  expect('}');
};

const assignment = function(run) {
  let name = t[i], position = p[i++];
  let indexed = false, index = 0;
  if (t[i] == '[') {
    indexed = true;
    i++;
    index = logicalOr(run);
    expect(']');
  }
  expect('=');
  let n = logicalOr(run);
  finish();
  if (run && !indexed) {
    set(name, n);
  } else if (run) {
    let array = get(name, position);
    if (
      typeof array != 'object' ||
      typeof index != 'number' ||
      index < 0 ||
      index % 1 != 0 ||
      index > array.length
    ) {
      error(position);
    }
    if (index == array.length) {
      array.push(n);
    } else {
      array[index] = n;
    }
  }
};

const whileStatement = function(run) {
  i++;
  expect('(');
  let conditionStart = i;
  let condition = logicalOr(run);
  expect(')');
  let blockStart = i;
  block(false);
  let end = i;
  while (run && condition) {
    i = blockStart;
    block(true);
    i = conditionStart;
    condition = logicalOr(true);
    expect(')');
  }
  i = end;
};

const sleepStatement = function(run) {
  i++;
  let milliseconds = logicalOr(run);
  finish();
  if (run) msleep(milliseconds);
};

const statement = function(run) {
  if (t[i] == ';') {
    i++;
  } else if (t[i] == 'if') {
    i++;
    expect('(');
    let condition = logicalOr(run);
    expect(')');
    block(run && condition);
    if (t[i] == 'else') {
      i++;
      block(run && !condition);
    }
  } else if (t[i] == 'while') {
    whileStatement(run);
  } else if (t[i] == 'msleep') {
    sleepStatement(run);
  } else if (
    identifier(t[i]) &&
    (t[i + 1] == '=' || t[i + 1] == '[')
  ) {
    assignment(run);
  } else {
    if (t[i] == 'print') i++;
    let n = logicalOr(run);
    finish();
    if (run) print(n);
  }
};

const program = function(source) {
  s = source;
  t = [];
  p = [];
  for (
    let r = /[A-Za-z_][A-Za-z0-9_]*|"(?:\\.|[^"\\])*"|0[xX][0-9A-Fa-f]+|\d+(?:\.\d+)?|===|!==|&&|\|\||==|!=|<=|>=|[-+*/%!=()={};<>\[\],.]|\s+/y,
        m, j = 0;
    j < s.length;
    j = r.lastIndex
  ) {
    r.lastIndex = j;
    m = r.exec(s);
    if (!m) error(j);
    if (!/^\s+$/.test(m[0])) {
      t.push(m[0]);
      p.push(j);
    }
  }
  i = 0;
  while (i < t.length) {
    statement(true);
  }
};
