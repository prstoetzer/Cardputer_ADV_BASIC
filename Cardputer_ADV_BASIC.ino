// ============================================================================
// M5Stack Cardputer ADV BASIC Interpreter v1.2
// Complete standalone Arduino sketch for M5Stack Cardputer ADV (ESP32-S3)
// ============================================================================
// Features:
// - Classic line-numbered BASIC (PRINT, INPUT, IF, GOTO, GOSUB, RETURN, END, etc.)
// - FOR / NEXT with STEP and nesting
// - DATA / READ / RESTORE for constants and sprite data
// - Graphics: CLS, PLOT, LINE, CIRCLE, RECT, FILLRECT, LOCATE, COLOR, SPRITE
// - User-definable sprites via DEFSPRITE + DATA
// - Sound: BEEP / TONE + PLAY "notes"
// - GPIO: PINMODE, DWRITE, PWM, DIGITALREAD(), ANALOGREAD(), INKEY()
// - SD card: LOAD / SAVE programs
// - Extended math: PI, SQRT, POW/^, SIN, COS, TAN, ATAN, EXP, LOG, FLOOR, CEIL, ROUND, ABS, RND
// - Logical operators: AND, OR, NOT (for compound IF conditions)
// - String variables A$..Z$ with + concat; CHR$ STR$ LEFT$ RIGHT$ MID$ INKEY$; LEN ASC VAL
// - Full keyboard REPL with editing
// - Break a running program with Ctrl+C (also Enter / Fn / Esc)
// ============================================================================

#include <M5Unified.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <map>
#include <vector>
#include <string>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstdlib>

// --------------------------- Configuration ----------------------------------
#define MAX_LINES        300
#define MAX_GOSUB        10
#define MAX_FOR_NEST     8

// Correct SD card pins for M5Stack Cardputer / Cardputer ADV
#define SD_SPI_SCK_PIN   40
#define SD_SPI_MISO_PIN  39
#define SD_SPI_MOSI_PIN  14
#define SD_SPI_CS_PIN    12

// --------------------------- Data Structures --------------------------------
std::map<int, String> programLines;
float vars[26] = {0};
String strVars[26];      // string variables A$..Z$ (parallel to numeric A..Z)

struct ForContext {
  int   varIdx;
  float endVal;
  float stepVal;
  int   forLineNum;
};
std::vector<ForContext> forStack;

int   gosubStack[MAX_GOSUB];
int   gosubSP = 0;

bool  running = false;
int   currentLine = 0;
bool  stopRun = false;
bool  jumped = false;   // set true when a statement changes currentLine (GOTO/GOSUB/RETURN/IF/FOR-NEXT)
String lastError = "";

// --------------------------- File I/O Support -------------------------------
#define MAX_OPEN_FILES 3
File openFiles[MAX_OPEN_FILES];
bool fileIsOpen[MAX_OPEN_FILES] = {false};

// DATA / READ / RESTORE
std::vector<float> dataStore;
size_t dataIndex = 0;

// User-defined sprites (id -> bitmap data)
struct UserSprite {
  uint8_t w;
  uint8_t h;
  std::vector<uint8_t> pixels; // row-major, 1 bit per pixel (MSB first)
};
std::map<int, UserSprite> userSprites;

// Keyboard input buffer (FIFO of pending key codes for INKEY / INKEY$).
// A small ring buffer avoids the timing races of a single "last key" slot.
#define KEYBUF_SIZE 16
char keyBuf[KEYBUF_SIZE];
int  keyBufHead = 0;   // next position to read
int  keyBufTail = 0;   // next position to write
bool keyWasDown = false; // edge tracking so a held key enqueues once

// --------------------------- Forward Declarations ---------------------------
float evaluateExpression(const String& expr, int& pos);
String evaluateStringExpression(const String& expr, int& pos);
bool  isStringExpr(const String& expr);
String numToBasicStr(float v);
void  executeLine(String line);
void  printPrompt();
void  showError(const String& msg);
void  checkBreak();
String readKeyboardLine(const String& prompt = "");
void  drawSprite(int id, int x, int y, uint16_t color);
void  playTune(String tune);
bool  initSD();
void  ensureBasicDir();
String sandboxPath(String name);
void  saveProgram(String filename);
void  loadProgram(String filename);
void  rebuildDataStore();
void  defSpriteFromData(int id);

#define BASIC_DIR "/BASIC"

// --------------------------- Helpers ----------------------------------------
int varIndex(char c) {
  c = toupper((unsigned char)c);
  if (c >= 'A' && c <= 'Z') return c - 'A';
  return -1;
}

// Uppercase everything outside double-quoted string literals. BASIC keywords
// and variable names are case-insensitive; only the text inside "..." keeps
// its original case. This lets users type in lowercase (the Cardputer's
// default) and still have TO / THEN / STEP / function names recognized.
String normalizeCase(const String& line) {
  String out = "";
  bool inStr = false;
  for (unsigned i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '"') inStr = !inStr;
    out += inStr ? c : (char)toupper((unsigned char)c);
  }
  return out;
}

void showError(const String& msg) {
  lastError = msg;
  M5.Display.setTextColor(TFT_RED, TFT_BLACK);
  M5.Display.println("ERROR: " + msg);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  stopRun = true;
}

void checkBreak() {
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto status = M5Cardputer.Keyboard.keysState();

    // Ctrl+C: Ctrl held while the 'C' key is pressed. Check both the decoded
    // word list and the raw key matrix, since some library versions suppress
    // the character when a modifier is held.
    bool ctrlC = false;
    if (status.ctrl) {
      for (char c : status.word) {
        if (c == 'c' || c == 'C') { ctrlC = true; break; }
      }
      if (!ctrlC && (M5Cardputer.Keyboard.isKeyPressed('c') ||
                     M5Cardputer.Keyboard.isKeyPressed('C'))) {
        ctrlC = true;
      }
    }

    if (ctrlC || status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_FN) ||
        M5Cardputer.Keyboard.isKeyPressed(27)) {
      stopRun = true;
      M5.Display.println("\n*** BREAK ***");
    }
  }
}

// Push a key code into the FIFO (drops it silently if the buffer is full).
void pushKey(char k) {
  int next = (keyBufTail + 1) % KEYBUF_SIZE;
  if (next == keyBufHead) return; // full
  keyBuf[keyBufTail] = k;
  keyBufTail = next;
}

// Poll the keyboard and enqueue any newly pressed keys. Edge-triggered so a
// held key is enqueued once per press, not every poll.
void pollKeyboard() {
  M5Cardputer.update();
  bool down = M5Cardputer.Keyboard.isPressed();
  if (down && M5Cardputer.Keyboard.isChange()) {
    auto status = M5Cardputer.Keyboard.keysState();
    for (char c : status.word) pushKey(c);
    if (status.del)   pushKey(8);
    if (status.enter) pushKey(13);
  }
  keyWasDown = down;
}

// Pop one key code, or 0 if none pending.
char popKey() {
  if (keyBufHead == keyBufTail) return 0; // empty
  char k = keyBuf[keyBufHead];
  keyBufHead = (keyBufHead + 1) % KEYBUF_SIZE;
  return k;
}

float getInkey() {
  pollKeyboard();
  return (float)(unsigned char)popKey();
}

// --------------------------- Expression Parser ------------------------------
float evaluateExpression(const String& expr, int& pos) {
  auto skipWS = [&]() { while (pos < expr.length() && isspace((unsigned char)expr[pos])) pos++; };

  auto parseNumber = [&]() -> float {
    skipWS();
    if (pos >= expr.length()) return 0;
    bool neg = false;
    if (expr[pos] == '-') { neg = true; pos++; }
    float num = 0;
    while (pos < expr.length() && isdigit((unsigned char)expr[pos])) num = num * 10 + (expr[pos++] - '0');
    if (pos < expr.length() && expr[pos] == '.') {
      pos++; float frac = 0.1;
      while (pos < expr.length() && isdigit((unsigned char)expr[pos])) { num += (expr[pos++] - '0') * frac; frac *= 0.1; }
    }
    return neg ? -num : num;
  };

  std::function<float()> parseFactor = [&]() -> float {
    skipWS();
    if (pos >= expr.length()) return 0;
    char c = expr[pos];

    // Unary minus / plus in front of parentheses, variables, or functions.
    if (c == '-' && pos + 1 < expr.length() &&
        (expr[pos+1] == '(' || isalpha((unsigned char)expr[pos+1]))) {
      pos++; return -parseFactor();
    }
    if (c == '+' && pos + 1 < expr.length() &&
        (expr[pos+1] == '(' || isalpha((unsigned char)expr[pos+1]))) {
      pos++; return parseFactor();
    }

    if (c == '(') {
      pos++; float v = evaluateExpression(expr, pos); skipWS();
      if (pos < expr.length() && expr[pos] == ')') pos++;
      return v;
    }

    if (isalpha((unsigned char)c)) {
      String name = "";
      while (pos < expr.length() && isalnum((unsigned char)expr[pos])) name += expr[pos++];
      name.toUpperCase();
      skipWS();

      if (name == "NOT") {
        float v = parseFactor();   // NOT binds to the following factor
        return (v == 0) ? 1 : 0;
      }

      if (name.length() == 1) {
        int idx = varIndex(name[0]);
        if (idx >= 0) return vars[idx];
      }

      if (name == "PI") return 3.1415926535;
      if (name == "RND") {
        if (pos < expr.length() && expr[pos] == '(') { pos++; skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; }
        return (float)random(0, 1000000) / 1000000.0;
      }
      if (name == "ABS") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return abs(v); }
      }
      if (name == "SIN") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return sin(v * PI / 180.0); }
      }
      if (name == "COS") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return cos(v * PI / 180.0); }
      }
      if (name == "TAN") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return tan(v * PI / 180.0); }
      }
      if (name == "ATAN") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return atan(v) * 180.0 / PI; }
      }
      if (name == "SQRT" || name == "SQR") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return sqrt(v); }
      }
      if (name == "POW") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float b = evaluateExpression(expr,pos); skipWS();
          if (pos < expr.length() && expr[pos] == ',') pos++; float e = evaluateExpression(expr,pos); skipWS();
          if (pos < expr.length() && expr[pos] == ')') pos++; return pow(b, e); }
      }
      if (name == "EXP") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return exp(v); }
      }
      if (name == "LOG") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return log(v); }
      }
      if (name == "FLOOR") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return floor(v); }
      }
      if (name == "CEIL") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return ceil(v); }
      }
      if (name == "ROUND") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return round(v); }
      }
      if (name == "INT") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float v = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return floor(v); }
      }
      if (name == "DIGITALREAD" || name == "DREAD") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float pin = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; pinMode((int)pin, INPUT); return digitalRead((int)pin); }
      }
      if (name == "ANALOGREAD" || name == "AREAD") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float pin = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return analogRead((int)pin); }
      }
      if (name == "INKEY") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; }
        return getInkey();
      }
      if (name == "LEN") {        // LEN(string)
        if (pos < expr.length() && expr[pos] == '('){ pos++; String s = evaluateStringExpression(expr, pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return (float)s.length(); }
      }
      if (name == "ASC") {        // ASC(string) -> code of first char
        if (pos < expr.length() && expr[pos] == '('){ pos++; String s = evaluateStringExpression(expr, pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return s.length() ? (float)(unsigned char)s[0] : 0; }
      }
      if (name == "VAL") {        // VAL(string) -> numeric value
        if (pos < expr.length() && expr[pos] == '('){ pos++; String s = evaluateStringExpression(expr, pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; return s.toFloat(); }
      }
      if (name == "EOF") {
        if (pos < expr.length() && expr[pos] == '('){ pos++; float fn = evaluateExpression(expr,pos); skipWS(); if (pos < expr.length() && expr[pos] == ')') pos++; 
          int fnum = (int)fn - 1;
          if (fnum >= 0 && fnum < MAX_OPEN_FILES && fileIsOpen[fnum]) {
            return openFiles[fnum].available() ? 0 : 1;
          }
          return 1; // EOF if not open
        }
      }
    }
    return parseNumber();
  };

  auto parsePower = [&]() -> float {
    float val = parseFactor();
    skipWS();
    if (pos < expr.length() && expr[pos] == '^') { pos++; float rhs = parseFactor(); return pow(val, rhs); }
    return val;
  };

  auto parseTerm = [&]() -> float {
    float val = parsePower();
    while (pos < expr.length()) {
      skipWS();
      char op = expr[pos];
      if (op == '*' || op == '/') {
        pos++;
        float rhs = parsePower();
        if (op == '*') val *= rhs;
        else if (rhs != 0) val /= rhs; else { showError("Div by zero"); return 0; }
      } else break;
    }
    return val;
  };

  // Additive layer: + and -
  std::function<float()> parseAdditive = [&]() -> float {
    float val = parseTerm();
    while (pos < expr.length()) {
      skipWS();
      if (pos >= expr.length()) break;
      char op = expr[pos];
      if (op == '+' || op == '-') {
        pos++; float right = parseTerm(); val = (op == '+') ? val + right : val - right;
      } else break;
    }
    return val;
  };

  // Comparison layer: = == <> < > <= >=  (returns 1 or 0)
  auto parseComparison = [&]() -> float {
    float left = parseAdditive();
    skipWS();
    if (pos < expr.length() && (expr[pos] == '=' || expr[pos] == '<' || expr[pos] == '>')) {
      String cmp = String(expr[pos]); pos++;
      if (pos < expr.length() && (expr[pos] == '=' || expr[pos] == '>' || expr[pos] == '<')) { cmp += expr[pos]; pos++; }
      float right = parseAdditive();
      if (cmp == "=" || cmp == "==") return (left == right) ? 1 : 0;
      if (cmp == "<>") return (left != right) ? 1 : 0;
      if (cmp == "<")  return (left < right) ? 1 : 0;
      if (cmp == ">")  return (left > right) ? 1 : 0;
      if (cmp == "<=") return (left <= right) ? 1 : 0;
      if (cmp == ">=") return (left >= right) ? 1 : 0;
    }
    return left;
  };

  // Helper: peek an uppercase keyword (AND/OR) without consuming if it doesn't match.
  auto peekKeyword = [&](const char* kw) -> bool {
    skipWS();
    int save = pos, k = 0;
    while (kw[k]) {
      if (pos >= expr.length()) { pos = save; return false; }
      if (toupper((unsigned char)expr[pos]) != kw[k]) { pos = save; return false; }
      pos++; k++;
    }
    // Ensure the keyword isn't part of a longer identifier (e.g. ANDREW).
    if (pos < expr.length() && isalnum((unsigned char)expr[pos])) { pos = save; return false; }
    return true; // consumed
  };

  // Logical layer (lowest precedence): AND, OR
  float left = parseComparison();
  while (pos < expr.length()) {
    skipWS();
    if (peekKeyword("AND")) {
      float right = parseComparison();
      left = ((left != 0) && (right != 0)) ? 1 : 0;
    } else if (peekKeyword("OR")) {
      float right = parseComparison();
      left = ((left != 0) || (right != 0)) ? 1 : 0;
    } else break;
  }
  return left;
}

// --------------------------- Number Formatting ------------------------------
// Format a float the way classic BASIC prints it: integers without a decimal
// point, fractions without trailing zeros.
String numToBasicStr(float v) {
  if (isnan(v)) return "NAN";
  if (isinf(v)) return v < 0 ? "-INF" : "INF";
  if (v == (long)v && fabs(v) < 1e9) {
    return String((long)v);
  }
  char buf[24];
  dtostrf(v, 0, 6, buf);
  String s = buf;
  // Trim trailing zeros (and a trailing dot) from the fractional part.
  if (s.indexOf('.') != -1) {
    int end = s.length();
    while (end > 0 && s[end - 1] == '0') end--;
    if (end > 0 && s[end - 1] == '.') end--;
    s = s.substring(0, end);
  }
  return s;
}

// --------------------------- String Expressions -----------------------------
// Returns true if the (already-trimmed) expression should be evaluated as a
// string: it begins with a quote, a string variable (LETTER$), or a string
// function (CHR$, STR$, LEFT$, RIGHT$, MID$, INKEY$).
bool isStringExpr(const String& expr) {
  int p = 0;
  while (p < (int)expr.length() && isspace((unsigned char)expr[p])) p++;
  if (p >= (int)expr.length()) return false;
  if (expr[p] == '"') return true;

  // Read an identifier and see if it's a string token.
  if (isalpha((unsigned char)expr[p])) {
    String name = "";
    int q = p;
    while (q < (int)expr.length() && isalnum((unsigned char)expr[q])) name += expr[q++];
    // String variable: single letter immediately followed by '$'
    if (q < (int)expr.length() && expr[q] == '$') {
      String up = name; up.toUpperCase();
      if (up == "CHR" || up == "STR" || up == "LEFT" || up == "RIGHT" ||
          up == "MID" || up == "INKEY") return true;   // string functions
      if (name.length() == 1) return true;             // A$..Z$
    }
  }
  return false;
}

String evaluateStringExpression(const String& expr, int& pos) {
  auto skipWS = [&]() { while (pos < (int)expr.length() && isspace((unsigned char)expr[pos])) pos++; };

  std::function<String()> parseStrFactor = [&]() -> String {
    skipWS();
    if (pos >= (int)expr.length()) return "";

    // String literal
    if (expr[pos] == '"') {
      pos++; String s = "";
      while (pos < (int)expr.length() && expr[pos] != '"') s += expr[pos++];
      if (pos < (int)expr.length()) pos++; // closing quote
      return s;
    }

    // Parenthesised string expression
    if (expr[pos] == '(') {
      pos++; String s = evaluateStringExpression(expr, pos); skipWS();
      if (pos < (int)expr.length() && expr[pos] == ')') pos++;
      return s;
    }

    if (isalpha((unsigned char)expr[pos])) {
      String name = "";
      while (pos < (int)expr.length() && isalnum((unsigned char)expr[pos])) name += expr[pos++];
      String up = name; up.toUpperCase();
      bool dollar = false;
      if (pos < (int)expr.length() && expr[pos] == '$') { dollar = true; pos++; }
      skipWS();

      // String functions (all spelled with a trailing $)
      if (dollar && up == "CHR") {        // CHR$(n)
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; float n = evaluateExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ')') pos++;
          return String((char)(int)n);
        }
      }
      if (dollar && up == "STR") {        // STR$(n)
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; float n = evaluateExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ')') pos++;
          return numToBasicStr(n);
        }
      }
      if (dollar && up == "INKEY") {      // INKEY$  -> "" or single char
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; skipWS(); if (pos < (int)expr.length() && expr[pos] == ')') pos++;
        }
        float k = getInkey();
        if (k <= 0) return "";
        return String((char)(int)k);
      }
      if (dollar && up == "LEFT") {       // LEFT$(s, n)
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; String s = evaluateStringExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ',') pos++;
          float n = evaluateExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ')') pos++;
          int len = (int)n; if (len < 0) len = 0;
          if (len > (int)s.length()) len = s.length();
          return s.substring(0, len);
        }
      }
      if (dollar && up == "RIGHT") {      // RIGHT$(s, n)
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; String s = evaluateStringExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ',') pos++;
          float n = evaluateExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ')') pos++;
          int len = (int)n; if (len < 0) len = 0;
          if (len > (int)s.length()) len = s.length();
          return s.substring(s.length() - len);
        }
      }
      if (dollar && up == "MID") {        // MID$(s, start[, len])  (1-based start)
        if (pos < (int)expr.length() && expr[pos] == '(') {
          pos++; String s = evaluateStringExpression(expr, pos); skipWS();
          if (pos < (int)expr.length() && expr[pos] == ',') pos++;
          float start = evaluateExpression(expr, pos); skipWS();
          int len = -1;
          if (pos < (int)expr.length() && expr[pos] == ',') {
            pos++; len = (int)evaluateExpression(expr, pos); skipWS();
          }
          if (pos < (int)expr.length() && expr[pos] == ')') pos++;
          int st = (int)start - 1; if (st < 0) st = 0;
          if (st >= (int)s.length()) return "";
          if (len < 0) return s.substring(st);
          int endIdx = st + len; if (endIdx > (int)s.length()) endIdx = s.length();
          return s.substring(st, endIdx);
        }
      }

      // String variable A$..Z$
      if (dollar && name.length() == 1) {
        int idx = varIndex(name[0]);
        if (idx >= 0) return strVars[idx];
      }
    }
    return "";
  };

  // Concatenation with '+'
  String result = parseStrFactor();
  while (true) {
    skipWS();
    if (pos < (int)expr.length() && expr[pos] == '+') {
      pos++; result += parseStrFactor();
    } else break;
  }
  return result;
}


bool isForActive(int varIdx) {
  for (auto& ctx : forStack) if (ctx.varIdx == varIdx) return true;
  return false;
}

void pushForContext(int varIdx, float endV, float stepV, int forLine) {
  if (forStack.size() >= MAX_FOR_NEST) { showError("Too many nested FOR"); return; }
  forStack.push_back({varIdx, endV, stepV, forLine});
}

// --------------------------- DATA / READ / RESTORE --------------------------
void rebuildDataStore() {
  dataStore.clear();
  dataIndex = 0;
  for (auto& pair : programLines) {
    String line = pair.second;
    line.trim();
    if (line.startsWith("DATA") || line.startsWith("data")) {
      int sp = line.indexOf(' ');
      if (sp != -1) {
        String vals = line.substring(sp + 1);
        int p = 0;
        while (p < vals.length()) {
          while (p < vals.length() && (vals[p] == ' ' || vals[p] == ',')) p++;
          if (p >= vals.length()) break;
          int start = p;
          while (p < vals.length() && vals[p] != ',' && vals[p] != ' ') p++;
          String token = vals.substring(start, p);
          float v = token.toFloat();
          dataStore.push_back(v);
        }
      }
    }
  }
}

void defSpriteFromData(int id) {
  if (dataIndex + 1 >= dataStore.size()) { showError("Not enough DATA for sprite"); return; }
  uint8_t w = (uint8_t)dataStore[dataIndex++];
  uint8_t h = (uint8_t)dataStore[dataIndex++];
  int bytesPerRow = (w + 7) / 8;
  int totalBytes = bytesPerRow * h;
  if (dataIndex + totalBytes > dataStore.size()) { showError("Sprite data incomplete"); return; }

  UserSprite spr;
  spr.w = w;
  spr.h = h;
  spr.pixels.resize(totalBytes);
  for (int i = 0; i < totalBytes; i++) {
    spr.pixels[i] = (uint8_t)dataStore[dataIndex++];
  }
  userSprites[id] = spr;
  M5.Display.println("Sprite " + String(id) + " defined (" + String(w) + "x" + String(h) + ")");
}

// --------------------------- Sprite Drawing ---------------------------------
const uint8_t spriteBall[8]   = {0x3C, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C};
const uint8_t spriteShip[16]  = {0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
const uint8_t spriteAlien[8]  = {0x3C,0x7E,0xDB,0xFF,0xFF,0xDB,0x7E,0x3C};

void drawSprite(int id, int x, int y, uint16_t color) {
  // Check user-defined first
  if (userSprites.count(id)) {
    UserSprite& spr = userSprites[id];
    int bytesPerRow = (spr.w + 7) / 8;
    for (int row = 0; row < spr.h; row++) {
      for (int col = 0; col < spr.w; col++) {
        int byteIdx = row * bytesPerRow + (col / 8);
        int bit = 7 - (col % 8);
        if (spr.pixels[byteIdx] & (1 << bit)) {
          M5.Display.drawPixel(x + col, y + row, color);
        }
      }
    }
    return;
  }

  // Built-in sprites
  if (id == 0) { // Ball 8x8
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++)
        if (spriteBall[row] & (1 << (7 - col))) M5.Display.drawPixel(x + col, y + row, color);
  } else if (id == 1) { // Ship ~16x8
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 16; col++)
        if (spriteShip[row*2 + (col/8)] & (1 << (7 - (col % 8)))) M5.Display.drawPixel(x + col, y + row, color);
  } else if (id == 2) { // Alien 8x8
    for (int row = 0; row < 8; row++)
      for (int col = 0; col < 8; col++)
        if (spriteAlien[row] & (1 << (7 - col))) M5.Display.drawPixel(x + col, y + row, color);
  } else {
    M5.Display.print("Unknown sprite id: "); M5.Display.println(id);
  }
}

// --------------------------- Music ------------------------------------------
float noteFreq(char note, int octave, bool sharp) {
  const float base[7] = {261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88};
  int idx = toupper((unsigned char)note) - 'C'; if (idx < 0) idx += 7;
  float f = base[idx];
  if (sharp) f *= 1.059463;
  for (int i = 4; i < octave; i++) f *= 2.0;
  for (int i = octave; i < 4; i++) f /= 2.0;
  return f;
}

void playTune(String tune) {
  tune.toUpperCase();
  int i = 0;
  while (i < tune.length()) {
    checkBreak();
    if (stopRun) break;
    char c = tune[i];
    if (isalpha((unsigned char)c)) {
      bool sharp = false;
      int octave = 4;
      if (i + 1 < tune.length() && tune[i + 1] == '#') { sharp = true; i++; }
      if (i + 1 < tune.length() && isdigit((unsigned char)tune[i + 1])) { octave = tune[i + 1] - '0'; i++; }
      float freq = noteFreq(c, octave, sharp);
      M5.Speaker.tone((uint32_t)freq, 180);
      delay(220);
    } else if (c == ' ' || c == ',') {
      delay(60);
    }
    i++;
  }
  M5.Speaker.stop();
}

// --------------------------- SD Card ----------------------------------------
bool initSD() {
  // Match the official M5Cardputer SD example: one SPI.begin with all four
  // pins, then SD.begin(CS, SPI, speed). The key fix vs. earlier versions is
  // calling SD.end() between retries -- a failed SD.begin() leaves the driver
  // half-initialized, so a second SD.begin() without end() will keep failing.
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  delay(10);

  const uint32_t speeds[] = { 25000000, 10000000, 4000000, 1000000 };
  for (int i = 0; i < 4; i++) {
    if (SD.begin(SD_SPI_CS_PIN, SPI, speeds[i])) {
      // Confirm a real card is actually present and usable.
      if (SD.cardType() != CARD_NONE) return true;
      SD.end();
      return false;
    }
    SD.end();      // reset driver state before the next attempt
    delay(20);
  }
  return false;
}

// Create the /BASIC sandbox folder if it doesn't already exist. All file
// access is confined to this folder.
void ensureBasicDir() {
  if (!SD.exists(BASIC_DIR)) SD.mkdir(BASIC_DIR);
}

// Map a user-supplied file name to a safe path inside /BASIC. Any directory
// components the user tries to include (leading slashes, "..", embedded path
// separators) are stripped so file access can't escape the sandbox.
String sandboxPath(String name) {
  name.trim();
  // Strip surrounding quotes if present.
  if (name.length() >= 2 && name.startsWith("\"") && name.endsWith("\"")) {
    name = name.substring(1, name.length() - 1);
  }
  // Keep only the final path component: drop everything up to the last
  // '/' or '\\'. This neutralizes "..", absolute paths, and subdirectories.
  int slash = -1;
  for (int i = 0; i < (int)name.length(); i++) {
    if (name[i] == '/' || name[i] == '\\') slash = i;
  }
  if (slash >= 0) name = name.substring(slash + 1);
  // Reject any residual ".." just in case.
  while (name.indexOf("..") != -1) name.replace("..", "");
  name.trim();
  ensureBasicDir();
  return String(BASIC_DIR) + "/" + name;
}

void saveProgram(String filename) {
  filename = sandboxPath(filename);   // confine to /BASIC, strip path parts
  if (!filename.endsWith(".bas")) filename += ".bas";

  File file = SD.open(filename, FILE_WRITE);
  if (!file) { showError("Cannot open file for save"); return; }

  for (auto& p : programLines) {
    file.println(String(p.first) + " " + p.second);
  }
  file.close();
  M5.Display.println("Saved: " + filename);
}

void loadProgram(String filename) {
  String orig = filename;
  filename = sandboxPath(filename);
  if (!filename.endsWith(".bas")) filename += ".bas";

  File file = SD.open(filename);
  if (!file) {
    showError("File not found: " + orig);
    return;
  }

  programLines.clear();
  userSprites.clear();
  forStack.clear();
  gosubSP = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int sp = line.indexOf(' ');
    if (sp > 0) {
      int num = line.substring(0, sp).toInt();
      String code = line.substring(sp + 1);
      if (num > 0) programLines[num] = code;
    }
  }
  file.close();
  M5.Display.println("Loaded: " + filename + " (" + String(programLines.size()) + " lines)");
  rebuildDataStore();
}

// --------------------------- Execution Engine -------------------------------
void executeLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  // REM comments: skip (check before normalizing so we catch any case).
  { String h = line; h.toUpperCase(); if (h.startsWith("REM")) return; }

  // Make keywords/variables case-insensitive while preserving string literals.
  line = normalizeCase(line);

  int sp = line.indexOf(' ');
  String cmd = (sp == -1) ? line : line.substring(0, sp);
  cmd.toUpperCase();
  String args = (sp == -1) ? "" : line.substring(sp + 1);
  args.trim();

  checkBreak();
  if (stopRun) return;

  // ---- Assignment detection (handles LET and implicit, numeric and string) ----
  // Works whether or not there are spaces around '=', e.g.  A=1 / A = 1 / A$="hi" / LET B$ = A$+"x"
  {
    String asn = line;
    asn.trim();
    String head = asn;
    head.toUpperCase();
    bool hasLet = head.startsWith("LET ") || head == "LET";
    if (hasLet) { asn = asn.substring(3); asn.trim(); }

    // Parse a leading variable name: a letter, optional more letters/digits, optional '$'.
    int q = 0;
    if (q < (int)asn.length() && isalpha((unsigned char)asn[q])) {
      int nameStart = q;
      while (q < (int)asn.length() && isalnum((unsigned char)asn[q])) q++;
      String vname = asn.substring(nameStart, q);
      bool isStr = false;
      if (q < (int)asn.length() && asn[q] == '$') { isStr = true; q++; }
      // Skip spaces, then require a single '=' that is not part of ==, <=, >=, <>.
      int after = q;
      while (after < (int)asn.length() && asn[after] == ' ') after++;
      bool isAssign = (after < (int)asn.length() && asn[after] == '=' &&
                       (after + 1 >= (int)asn.length() || asn[after + 1] != '='));
      // Single-letter variable names only (A..Z / A$..Z$).
      if (isAssign && vname.length() == 1) {
        int idx = varIndex(vname[0]);
        if (idx >= 0) {
          String rhs = asn.substring(after + 1);
          rhs.trim();
          if (isStr) {
            int p = 0;
            strVars[idx] = evaluateStringExpression(rhs, p);
          } else {
            int p = 0;
            vars[idx] = evaluateExpression(rhs, p);
          }
          return;
        }
      }
    }
  }

  if (cmd == "PRINT" || cmd == "?") {
    // Check for PRINT #n,  (file output)
    args.trim();
    if (args.startsWith("#")) {
      int commaPos = args.indexOf(',');
      if (commaPos != -1) {
        int fnum = args.substring(1, commaPos).toInt() - 1;
        String rest = args.substring(commaPos + 1);
        rest.trim();

        if (fnum >= 0 && fnum < MAX_OPEN_FILES && fileIsOpen[fnum]) {
          int p = 0;
          while (p < rest.length()) {
            String remaining = rest.substring(p);
            if (isStringExpr(remaining)) {
              int sp2 = 0;
              String s = evaluateStringExpression(remaining, sp2);
              p += sp2;
              openFiles[fnum].print(s);
            } else {
              float v = evaluateExpression(rest, p);
              openFiles[fnum].print(numToBasicStr(v));
            }
            while (p < rest.length() && (rest[p] == ' ' || rest[p] == ',' || rest[p] == ';')) p++;
          }
          openFiles[fnum].println(); // auto newline for simplicity
          return;
        } else {
          showError("File not open or invalid #: " + String(fnum+1));
          return;
        }
      }
    }

    // Normal screen PRINT
    scrollIfNeeded();
    int p = 0; bool nl = true;
    while (p < args.length()) {
      nl = true; // reset; a trailing separator at the very end will clear it
      // Decide string vs numeric for the item starting here.
      String remaining = args.substring(p);
      if (isStringExpr(remaining)) {
        int sp2 = 0;
        String s = evaluateStringExpression(remaining, sp2);
        p += sp2;
        M5.Display.print(s);
      } else {
        float v = evaluateExpression(args, p);
        M5.Display.print(numToBasicStr(v));
      }
      bool sawSep = false;
      while (p < args.length() && (args[p] == ' ' || args[p] == ',' || args[p] == ';')) {
        if (args[p] == ',') { M5.Display.print("   "); sawSep = true; }
        if (args[p] == ';') { sawSep = true; }
        p++;
      }
      // If a separator was the last meaningful token, suppress the trailing newline.
      if (sawSep && p >= args.length()) nl = false;
    }
    if (nl) M5.Display.println();
  }
  else if (cmd == "INPUT") {
    args.trim();
    if (args.startsWith("#")) {
      // INPUT #n, var   (read from file)
      int comma = args.indexOf(',');
      if (comma != -1) {
        int fnum = args.substring(1, comma).toInt() - 1;
        String varPart = args.substring(comma + 1);
        varPart.trim();

        if (fnum >= 0 && fnum < MAX_OPEN_FILES && fileIsOpen[fnum]) {
          if (openFiles[fnum].available()) {
            String valStr = openFiles[fnum].readStringUntil('\n');
            valStr.trim();
            int idx = varIndex(varPart[0]);
            bool wantStr = (varPart.length() >= 2 && varPart[1] == '$');
            if (idx >= 0) {
              if (wantStr) strVars[idx] = valStr;
              else vars[idx] = valStr.toFloat();
            }
          } else {
            showError("End of file on #" + String(fnum+1));
          }
        } else {
          showError("File #" + String(fnum+1) + " not open");
        }
        return;
      }
    }

    // Normal keyboard INPUT
    String prompt = ""; int semi = args.indexOf(';');
    if (semi != -1) { prompt = args.substring(0, semi); args = args.substring(semi + 1); args.trim(); }
    // Strip surrounding quotes from a prompt literal, if present.
    if (prompt.length() >= 2 && prompt.startsWith("\"") && prompt.endsWith("\"")) {
      prompt = prompt.substring(1, prompt.length() - 1);
    }
    if (prompt.length()) M5.Display.print(prompt);
    String inp = readKeyboardLine("");
    if (args.length() > 0) {
      int idx = varIndex(args[0]);
      bool wantStr = (args.length() >= 2 && args[1] == '$');
      if (idx >= 0) {
        if (wantStr) strVars[idx] = inp;
        else vars[idx] = inp.toFloat();
      }
    }
    M5.Display.println();
  }
  else if (cmd == "GOTO") {
    int target = args.toInt();
    if (programLines.count(target)) { currentLine = target; jumped = true; }
    else showError("Line not found");
    return;
  }
  else if (cmd == "GOSUB") {
    int target = args.toInt();
    if (gosubSP < MAX_GOSUB && programLines.count(target)) {
      gosubStack[gosubSP++] = currentLine;
      currentLine = target;
      jumped = true;
    } else showError("GOSUB error");
    return;
  }
  else if (cmd == "RETURN") {
    // Restore the GOSUB line; leaving jumped=false makes the loop advance to the NEXT line.
    if (gosubSP > 0) currentLine = gosubStack[--gosubSP];
    else showError("RETURN without GOSUB");
    return;
  }
  else if (cmd == "IF") {
    int thenPos = args.indexOf("THEN");
    if (thenPos != -1) {
      String condStr = args.substring(0, thenPos);
      String thenPart = args.substring(thenPos + 4); thenPart.trim();
      condStr.trim();

      float cond = 0;
      if (isStringExpr(condStr)) {
        // String comparison:  <strexpr> <op> <strexpr>
        int p = 0;
        String lhs = evaluateStringExpression(condStr, p);
        while (p < (int)condStr.length() && condStr[p] == ' ') p++;
        String op = "";
        if (p < (int)condStr.length() && (condStr[p] == '=' || condStr[p] == '<' || condStr[p] == '>')) {
          op += condStr[p++];
          if (p < (int)condStr.length() && (condStr[p] == '=' || condStr[p] == '>' || condStr[p] == '<')) op += condStr[p++];
        }
        String rhs = evaluateStringExpression(condStr, p);
        int c = lhs.compareTo(rhs);
        if (op == "=" || op == "==") cond = (c == 0) ? 1 : 0;
        else if (op == "<>")        cond = (c != 0) ? 1 : 0;
        else if (op == "<")         cond = (c < 0) ? 1 : 0;
        else if (op == ">")         cond = (c > 0) ? 1 : 0;
        else if (op == "<=")        cond = (c <= 0) ? 1 : 0;
        else if (op == ">=")        cond = (c >= 0) ? 1 : 0;
        else cond = (lhs.length() > 0) ? 1 : 0; // bare string is "true" if non-empty
      } else {
        int p = 0; cond = evaluateExpression(condStr, p);
      }

      if (cond != 0) {
        int target = thenPart.toInt();
        if (target > 0 && programLines.count(target)) { currentLine = target; jumped = true; return; }
        else executeLine(thenPart);
      }
    }
  }
  else if (cmd == "FOR") {
    int eqPos = args.indexOf('=');
    if (eqPos == -1) { showError("FOR syntax"); return; }
    String varPart = args.substring(0, eqPos); varPart.trim();
    int idx = varIndex(varPart[0]);
    if (idx < 0) { showError("Bad FOR variable"); return; }

    String rest = args.substring(eqPos + 1);

    // Keyword detection is case-insensitive: search an uppercased copy but
    // slice the original. FOR operands are numeric, so case never matters there.
    String restU = rest; restU.toUpperCase();
    int toPos = restU.indexOf(" TO ");
    int toLen = 4;
    if (toPos == -1) {                 // also accept "TO" at the very start, e.g. "=1TO5" is rare; be lenient
      toPos = restU.indexOf("TO");
      toLen = 2;
    }
    if (toPos == -1) { showError("Missing TO"); return; }

    String startStr = rest.substring(0, toPos);
    String endStep  = rest.substring(toPos + toLen);
    String endStepU = endStep; endStepU.toUpperCase();
    int stepPos = endStepU.indexOf("STEP");
    String endStr  = (stepPos == -1) ? endStep : endStep.substring(0, stepPos);
    String stepStr = (stepPos == -1) ? "1"     : endStep.substring(stepPos + 4);

    int p = 0;
    float startV = evaluateExpression(startStr, p);
    p = 0; float endV = evaluateExpression(endStr, p);
    p = 0; float stepV = evaluateExpression(stepStr, p);

    if (stepV == 0) { showError("FOR STEP cannot be 0"); return; }

    if (!isForActive(idx)) {
      vars[idx] = startV;
      pushForContext(idx, endV, stepV, currentLine);
    }
  }
  else if (cmd == "NEXT") {
    if (forStack.empty()) { showError("NEXT without FOR"); return; }

    // Build the list of variable indices named on this NEXT (may be empty,
    // a single var, or a comma list like NEXT J,I).
    std::vector<int> wanted;
    {
      int p = 0;
      while (p < (int)args.length()) {
        while (p < (int)args.length() && (args[p] == ' ' || args[p] == ',')) p++;
        if (p >= (int)args.length()) break;
        if (isalpha((unsigned char)args[p])) {
          int id = varIndex(args[p]);
          wanted.push_back(id);
          p++;
          while (p < (int)args.length() && isalnum((unsigned char)args[p])) p++; // skip rest of name
        } else p++;
      }
    }

    // Process each named loop (or just the innermost if none named).
    int count = wanted.empty() ? 1 : (int)wanted.size();
    for (int k = 0; k < count; k++) {
      if (forStack.empty()) { showError("NEXT without FOR"); return; }

      int want = wanted.empty() ? -1 : wanted[k];

      // If a specific variable is named but isn't the innermost loop, the
      // inner loops were left open (e.g. an early exit) - discard them.
      if (want != -1) {
        while (!forStack.empty() && forStack.back().varIdx != want) {
          forStack.pop_back();
        }
        if (forStack.empty()) { showError("NEXT variable mismatch"); return; }
      }

      ForContext& ctx = forStack.back();
      vars[ctx.varIdx] += ctx.stepVal;
      bool cont = (ctx.stepVal >= 0) ? (vars[ctx.varIdx] <= ctx.endVal)
                                     : (vars[ctx.varIdx] >= ctx.endVal);

      if (cont) {
        currentLine = ctx.forLineNum;
        jumped = true;
        return;               // loop back; later vars in a NEXT list aren't reached
      } else {
        forStack.pop_back();  // this loop is done; continue to the next named var
      }
    }
    return;
  }
  else if (cmd == "DATA") {
    // DATA is collected at RUN start via rebuildDataStore()
    return;
  }
  else if (cmd == "READ") {
    // READ var1, var2, ...
    int p = 0;
    while (p < args.length()) {
      while (p < args.length() && (args[p] == ' ' || args[p] == ',')) p++;
      if (p >= args.length()) break;
      String varName = "";
      while (p < args.length() && isalpha((unsigned char)args[p])) varName += args[p++];
      int idx = varIndex(varName[0]);
      if (idx >= 0 && dataIndex < dataStore.size()) {
        vars[idx] = dataStore[dataIndex++];
      } else if (idx >= 0) {
        showError("Out of DATA");
        return;
      }
    }
  }
  else if (cmd == "RESTORE") {
    if (args.length() == 0) {
      dataIndex = 0;
    } else {
      // Simple version: RESTORE resets to 0. Advanced line-specific can be added later.
      dataIndex = 0;
      M5.Display.println("RESTORE (full reset)");
    }
  }
  else if (cmd == "DEFSPRITE") {
    int p = 0;
    float id = evaluateExpression(args, p);
    defSpriteFromData((int)id);
  }
  else if (cmd == "END" || cmd == "STOP") {
    running = false; M5.Display.println("Program ended.");
  }
  else if (cmd == "CLS") {
    M5.Display.clear(); M5.Display.setCursor(0, 0);
  }
  else if (cmd == "COLOR") {
    int p = 0; float fg = evaluateExpression(args, p);
    M5.Display.setTextColor((uint16_t)fg, TFT_BLACK);
  }
  else if (cmd == "PLOT") {
    int p = 0; float x = evaluateExpression(args, p); float y = evaluateExpression(args, p);
    M5.Display.drawPixel((int)x, (int)y, TFT_WHITE);
  }
  else if (cmd == "LINE") {
    int p = 0; float x1 = evaluateExpression(args, p); float y1 = evaluateExpression(args, p);
    float x2 = evaluateExpression(args, p); float y2 = evaluateExpression(args, p);
    M5.Display.drawLine((int)x1, (int)y1, (int)x2, (int)y2, TFT_WHITE);
  }
  else if (cmd == "CIRCLE") {
    int p = 0; float x = evaluateExpression(args, p); float y = evaluateExpression(args, p); float r = evaluateExpression(args, p);
    M5.Display.drawCircle((int)x, (int)y, (int)r, TFT_WHITE);
  }
  else if (cmd == "RECT" || cmd == "BOX") {
    int p = 0; float x = evaluateExpression(args, p); float y = evaluateExpression(args, p);
    float w = evaluateExpression(args, p); float h = evaluateExpression(args, p);
    M5.Display.drawRect((int)x, (int)y, (int)w, (int)h, TFT_WHITE);
  }
  else if (cmd == "FILLRECT" || cmd == "FILL") {
    int p = 0; float x = evaluateExpression(args, p); float y = evaluateExpression(args, p);
    float w = evaluateExpression(args, p); float h = evaluateExpression(args, p);
    M5.Display.fillRect((int)x, (int)y, (int)w, (int)h, TFT_WHITE);
  }
  else if (cmd == "LOCATE") {
    int p = 0; float x = evaluateExpression(args, p); float y = evaluateExpression(args, p);
    M5.Display.setCursor((int)x, (int)y);
  }
  else if (cmd == "SPRITE") {
    int p = 0; float id = evaluateExpression(args, p); float x = evaluateExpression(args, p); float y = evaluateExpression(args, p);
    uint16_t col = TFT_WHITE;
    if (p < args.length()) { float c = evaluateExpression(args, p); col = (uint16_t)c; }
    drawSprite((int)id, (int)x, (int)y, col);
  }
  else if (cmd == "BEEP" || cmd == "TONE") {
    int p = 0; float f = evaluateExpression(args, p); float d = evaluateExpression(args, p);
    if (d < 10) d = 100;
    M5.Speaker.tone((uint32_t)f, (uint32_t)d);
  }
  else if (cmd == "PLAY") {
    String tune = args;
    if (tune.startsWith("\"") && tune.endsWith("\"")) tune = tune.substring(1, tune.length() - 1);
    playTune(tune);
  }
  else if (cmd == "PINMODE") {
    int p = 0; float pin = evaluateExpression(args, p); float mode = evaluateExpression(args, p);
    if (mode == 0) pinMode((int)pin, INPUT);
    else if (mode == 1) pinMode((int)pin, OUTPUT);
    else if (mode == 2) pinMode((int)pin, INPUT_PULLUP);
    else if (mode == 3) pinMode((int)pin, INPUT_PULLDOWN);
  }
  else if (cmd == "DWRITE" || cmd == "DIGITALWRITE") {
    int p = 0; float pin = evaluateExpression(args, p); float val = evaluateExpression(args, p);
    digitalWrite((int)pin, val > 0 ? HIGH : LOW);
  }
  else if (cmd == "PWM") {
    int p = 0; float pin = evaluateExpression(args, p); float duty = evaluateExpression(args, p);
    static bool pwmAttached[49] = {false};   // ESP32-S3 GPIO range
    int pinNum = (int)pin;
    if (pinNum >= 0 && pinNum < 49 && !pwmAttached[pinNum]) {
      ledcAttach(pinNum, 5000, 8);
      pwmAttached[pinNum] = true;
    }
    ledcWrite(pinNum, (int)duty);
  }
  else if (cmd == "PAUSE" || cmd == "DELAY" || cmd == "SLEEP") {
    int p = 0; float ms = evaluateExpression(args, p);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)ms) { checkBreak(); if (stopRun) break; delay(5); }
  }
  else if (cmd == "SAVE") {
    saveProgram(args);   // sandboxPath handles quote-stripping and confinement
  }
  else if (cmd == "LOAD") {
    loadProgram(args);
  }
  else if (cmd == "DELETE") {
    String orig = args; orig.trim();
    String path = sandboxPath(args);
    if (!path.endsWith(".bas")) path += ".bas";

    if (SD.exists(path) && SD.remove(path)) {
      M5.Display.println("Deleted: " + orig);
    } else {
      showError("File not found or could not delete: " + orig);
    }
  }
  else if (cmd == "RENAME") {
    // RENAME "oldname", "newname"
    int comma = args.indexOf(',');
    if (comma == -1) { showError("RENAME needs oldname, newname"); return; }

    String oldPath = sandboxPath(args.substring(0, comma));
    String newPath = sandboxPath(args.substring(comma + 1));
    if (!oldPath.endsWith(".bas")) oldPath += ".bas";
    if (!newPath.endsWith(".bas")) newPath += ".bas";

    if (SD.exists(oldPath) && SD.rename(oldPath, newPath)) {
      M5.Display.println("Renamed.");
    } else {
      showError("Rename failed (source not found?)");
    }
  }
  else if (cmd == "CAT") {
    String orig = args; orig.trim();
    String path = sandboxPath(args);
    if (!path.endsWith(".bas")) path += ".bas";

    File f = SD.open(path);
    if (!f) { showError("File not found: " + orig); return; }

    M5.Display.println("--- " + orig + " ---");
    while (f.available()) {
      scrollIfNeeded();
      M5.Display.write(f.read());
    }
    f.close();
    M5.Display.println("\n--- End of file ---");
  }
  else if (cmd == "OPEN") {
    // Basic support: OPEN "file" FOR OUTPUT AS #1   or FOR INPUT
    String argsUpper = args;
    argsUpper.toUpperCase();

    int forPos = argsUpper.indexOf("FOR");
    int asPos  = argsUpper.indexOf("AS");
    if (forPos == -1 || asPos == -1) { showError("OPEN syntax: OPEN \"file\" FOR INPUT/OUTPUT AS #n"); return; }

    String fname = args.substring(0, forPos);
    fname.trim();
    String origName = fname;
    if (origName.startsWith("\"") && origName.endsWith("\"") && origName.length() >= 2)
      origName = origName.substring(1, origName.length() - 1);
    // Note: data files keep whatever extension the user gives (do NOT force .bas).

    String modeStr = argsUpper.substring(forPos + 3, asPos);
    modeStr.trim();

    String fileNumStr = args.substring(asPos + 2);
    fileNumStr.trim();
    if (fileNumStr.startsWith("#")) fileNumStr = fileNumStr.substring(1);
    int fnum = fileNumStr.toInt() - 1; // 0-based

    if (fnum < 0 || fnum >= MAX_OPEN_FILES) { showError("File number must be 1-3"); return; }
    if (fileIsOpen[fnum]) { showError("File already open"); return; }

    // Use string modes for ESP32 FS/SD library (M5Stack core).
    const char* openMode = "r"; // default = INPUT / read
    if (modeStr.indexOf("APPEND") != -1) {
        openMode = "a"; // append (creates if needed, keeps contents)
    } else if (modeStr.indexOf("OUTPUT") != -1) {
        openMode = "w"; // write (creates/truncates)
    }

    // All data files live inside the /BASIC sandbox, same as programs.
    String fullPath = sandboxPath(origName);

    openFiles[fnum] = SD.open(fullPath, openMode);
    if (!openFiles[fnum]) {
      showError("Failed to open file: " + origName);
      return;
    }
    fileIsOpen[fnum] = true;
    M5.Display.println("File #" + String(fnum+1) + " opened: " + origName);
  }
  else if (cmd == "CLOSE") {
    if (args.length() == 0) {
      // CLOSE all
      for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fileIsOpen[i]) {
          openFiles[i].close();
          fileIsOpen[i] = false;
        }
      }
      M5.Display.println("All files closed.");
    } else {
      int fnum = args.toInt() - 1;
      if (fnum >= 0 && fnum < MAX_OPEN_FILES && fileIsOpen[fnum]) {
        openFiles[fnum].close();
        fileIsOpen[fnum] = false;
        M5.Display.println("File #" + String(fnum+1) + " closed.");
      } else {
        showError("Invalid or not open file number");
      }
    }
  }
  else if (cmd == "DIR" || cmd == "FILES" || cmd == "LS") {
    // List BASIC programs in /BASIC folder + show free space
    ensureBasicDir();   // create the sandbox folder if it doesn't exist yet

    File dir = SD.open(BASIC_DIR);
    if (!dir || !dir.isDirectory()) {
      M5.Display.println("SD card not ready.");
      return;
    }

    M5.Display.println("Files in /BASIC:");
    M5.Display.println("---------------------");

    int count = 0;
    while (true) {
      File entry = dir.openNextFile();
      if (!entry) break;
      if (!entry.isDirectory()) {
        String name = entry.name();
        scrollIfNeeded();
        M5.Display.print("  ");
        M5.Display.print(name);
        for (int i = name.length(); i < 18; i++) M5.Display.print(" ");
        M5.Display.print(entry.size());
        M5.Display.println(" bytes");
        count++;
      }
      entry.close();
    }
    dir.close();

    if (count == 0) M5.Display.println("  (no files found)");

    // Show storage info
    uint64_t total = SD.cardSize();
    uint64_t used  = SD.usedBytes();
    uint64_t freeB = total - used;

    M5.Display.println("---------------------");
    M5.Display.print("Free: ");
    M5.Display.print(freeB / 1024);
    M5.Display.print(" KB   |  Total: ");
    M5.Display.print(total / (1024 * 1024));
    M5.Display.println(" MB");
  }
  else if (cmd == "RUN") {
    running = true;
    currentLine = 0;
    gosubSP = 0;
    forStack.clear();
    userSprites.clear();          // optional: keep or clear on RUN
    stopRun = false;
    rebuildDataStore();
    dataIndex = 0;
    M5.Display.println("Running...");
  }
  else if (cmd == "LIST") {
    for (auto& pr : programLines) {
      scrollIfNeeded();
      M5.Display.println(String(pr.first) + " " + pr.second);
      checkBreak();
      if (stopRun) break;
    }
  }
  else if (cmd == "NEW") {
    programLines.clear();
    userSprites.clear();
    forStack.clear();
    gosubSP = 0;
    dataStore.clear();
    dataIndex = 0;
    for (int i = 0; i < 26; i++) { vars[i] = 0; strVars[i] = ""; }
    M5.Display.println("Program cleared.");
  }
  else if (cmd == "CONT") {
    if (!running && currentLine > 0) { running = true; stopRun = false; }
  }
  else {
    showError("Unknown command: " + cmd);
  }
}

// --------------------------- Keyboard Input ---------------------------------
String readKeyboardLine(const String& prompt) {
  String input = "";
  if (prompt.length() > 0) M5.Display.print(prompt);

  int16_t cursorX = M5.Display.getCursorX();
  int16_t cursorY = M5.Display.getCursorY();
  unsigned long lastBlink = 0;
  bool cursorVisible = true;

  while (true) {
    M5Cardputer.update();

    // Blinking cursor
    if (millis() - lastBlink > 400) {
      lastBlink = millis();
      cursorVisible = !cursorVisible;
      if (cursorVisible) {
        M5.Display.fillRect(cursorX, cursorY + 6, 6, 2, TFT_WHITE); // underline cursor
      } else {
        M5.Display.fillRect(cursorX, cursorY + 6, 6, 2, TFT_BLACK);
      }
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      // Hide cursor before processing input
      M5.Display.fillRect(cursorX, cursorY + 6, 6, 2, TFT_BLACK);

      auto status = M5Cardputer.Keyboard.keysState();

      // Ctrl+C cancels the current input line (and breaks a running program).
      if (status.ctrl) {
        bool isC = M5Cardputer.Keyboard.isKeyPressed('c') ||
                   M5Cardputer.Keyboard.isKeyPressed('C');
        for (char c : status.word) if (c == 'c' || c == 'C') isC = true;
        if (isC) {
          stopRun = true;
          M5.Display.println("\n*** BREAK ***");
          input = "";
          break;
        }
        // While Ctrl is held, don't type characters into the line.
      } else {
        for (char c : status.word) {
          input += c;
          M5.Display.print(c);
          cursorX = M5.Display.getCursorX();
          cursorY = M5.Display.getCursorY();
        }
      }
      if (status.del && input.length() > 0) {
        input.remove(input.length() - 1);
        int16_t cx = M5.Display.getCursorX();
        int16_t cy = M5.Display.getCursorY();
        M5.Display.fillRect(cx - 6, cy, 6, 8, TFT_BLACK);
        M5.Display.setCursor(cx - 6, cy);
        cursorX = M5.Display.getCursorX();
        cursorY = M5.Display.getCursorY();
      }
      if (status.enter) {
        M5.Display.fillRect(cursorX, cursorY + 6, 6, 2, TFT_BLACK); // hide cursor
        M5.Display.println();
        break;
      }
    }
    delay(5);
  }
  return input;
}

void processLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  int firstSpace = line.indexOf(' ');
  String first = (firstSpace == -1) ? line : line.substring(0, firstSpace);

  if (isdigit((unsigned char)first[0])) {
    int num = first.toInt();
    String code = (firstSpace == -1) ? "" : line.substring(firstSpace + 1);
    code.trim();
    if (code.length() > 0) {
      // Enforce the program-size limit, but always allow editing an existing line.
      if (programLines.find(num) == programLines.end() && (int)programLines.size() >= MAX_LINES) {
        showError("Program full (max " + String(MAX_LINES) + " lines)");
        return;
      }
      programLines[num] = code;
    } else {
      programLines.erase(num);
    }
    M5.Display.println("OK");
  } else {
    executeLine(line);
  }
}

// Pixel-smooth scrolling for the 240x135 screen (with margins)
void scrollIfNeeded() {
  // Scroll earlier to leave bottom margin and avoid cutoff
  while (M5.Display.getCursorY() > M5.Display.height() - 16) {
    M5.Display.scroll(0, -1);
    M5.Display.setCursor(0, M5.Display.getCursorY() - 1);
  }
}

void printPrompt() {
  scrollIfNeeded();
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.print("> ");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// --------------------------- Setup & Main Loop ------------------------------
void setup() {
  auto cfg = M5.config();
  // M5Cardputer.begin initializes M5Unified internally; a single call (matching
  // the official examples) avoids a redundant double-begin of the same hardware.
  M5Cardputer.begin(cfg, true);

  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.clear();
  M5.Display.setCursor(0, 4);  // small top margin to avoid cutoff

  M5.Speaker.setVolume(128);
  randomSeed(analogRead(0));

  M5.Display.println("Cardputer ADV BASIC v1.2");

  if (initSD()) {
    ensureBasicDir();   // create the /BASIC sandbox folder if it's not there
    M5.Display.println("SD card ready. Files are stored in /BASIC.");
  } else {
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.println("SD init failed. Try: reinsert card, format FAT32, or smaller card (<32GB).");
    M5.Display.setTextColor(TFT_WHITE);
  }

  M5.Display.println("Ready.");
  printPrompt();
}

void loop() {
  M5Cardputer.update();

  if (!running) {
    // At the REPL: clear any stray break flag so a prompt-level Ctrl+C
    // doesn't carry over and block the next RUN.
    stopRun = false;

    // Blocking line read for the interactive prompt.
    String line = readKeyboardLine("");
    if (line.length() > 0) {
      processLine(line);
      scrollIfNeeded();
      if (!running) printPrompt();
    }
    return;
  }

  // ---- Program is running ----
  // Non-blocking break check every iteration so Ctrl+C (or Enter/Fn/Esc)
  // interrupts even tight loops.
  checkBreak();
  if (stopRun) {
    running = false;
    stopRun = false;
    printPrompt();
    return;
  }

  // RUN mode
  if (running) {
    auto it = programLines.lower_bound(currentLine);
    if (it == programLines.end()) {
      running = false;
      M5.Display.println("Program finished.");
      printPrompt();
      return;
    }

    currentLine = it->first;
    String stmt = it->second;

    jumped = false;
    executeLine(stmt);

    if (!running || stopRun) {
      running = false;
      if (stopRun) stopRun = false;
      printPrompt();
      return;
    }

    if (jumped) {
      // A branch set currentLine directly; resume from it next iteration.
      return;
    }

    // Sequential advance to the next program line.
    auto next = programLines.upper_bound(currentLine);
    if (next != programLines.end()) {
      currentLine = next->first;
    } else {
      running = false;
      M5.Display.println("End of program.");
      printPrompt();
    }
  }

  delay(1);  // brief yield to the system between statements
}
