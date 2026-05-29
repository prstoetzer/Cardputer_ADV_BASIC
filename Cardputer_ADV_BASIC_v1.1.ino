// ============================================================================
// M5Stack Cardputer ADV BASIC Interpreter v1.1
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
// - Full keyboard REPL with editing
// ============================================================================

#include <M5Unified.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <map>
#include <vector>
#include <string>
#include <cctype>
#include <cmath>
#include <cstdlib>

// --------------------------- Configuration ----------------------------------
#define MAX_LINES        300
#define MAX_GOSUB        10
#define MAX_FOR_NEST     8
#define SD_CS_PIN        SS

// --------------------------- Data Structures --------------------------------
std::map<int, String> programLines;
float vars[26] = {0};

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
String lastError = "";

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

// Keyboard
char lastKey = 0;
unsigned long lastKeyTime = 0;

// --------------------------- Forward Declarations ---------------------------
float evaluateExpression(const String& expr, int& pos);
void  executeLine(String line);
void  printPrompt();
void  showError(const String& msg);
void  checkBreak();
String readKeyboardLine(const String& prompt = "");
void  drawSprite(int id, int x, int y, uint16_t color);
void  playTune(String tune);
bool  initSD();
void  saveProgram(String filename);
void  loadProgram(String filename);
void  rebuildDataStore();
void  defSpriteFromData(int id);

// --------------------------- Helpers ----------------------------------------
int varIndex(char c) {
  c = toupper(c);
  if (c >= 'A' && c <= 'Z') return c - 'A';
  return -1;
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
    if (status.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_FN) || M5Cardputer.Keyboard.isKeyPressed(27)) {
      stopRun = true;
      M5.Display.println("\n*** BREAK ***");
    }
  }
}

void updateLastKey() {
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto status = M5Cardputer.Keyboard.keysState();
    if (!status.word.empty()) { lastKey = status.word[0]; lastKeyTime = millis(); }
    if (status.del) lastKey = 8;
    if (status.enter) lastKey = 13;
  }
}

float getInkey() {
  updateLastKey();
  if (millis() - lastKeyTime < 600) { char k = lastKey; lastKey = 0; return (float)k; }
  return 0;
}

// --------------------------- Expression Parser ------------------------------
float evaluateExpression(const String& expr, int& pos) {
  auto skipWS = [&]() { while (pos < expr.length() && isspace(expr[pos])) pos++; };

  auto parseNumber = [&]() -> float {
    skipWS();
    if (pos >= expr.length()) return 0;
    bool neg = false;
    if (expr[pos] == '-') { neg = true; pos++; }
    float num = 0;
    while (pos < expr.length() && isdigit(expr[pos])) num = num * 10 + (expr[pos++] - '0');
    if (pos < expr.length() && expr[pos] == '.') {
      pos++; float frac = 0.1;
      while (pos < expr.length() && isdigit(expr[pos])) { num += (expr[pos++] - '0') * frac; frac *= 0.1; }
    }
    return neg ? -num : num;
  };

  auto parseFactor = [&]() -> float {
    skipWS();
    if (pos >= expr.length()) return 0;
    char c = expr[pos];

    if (c == '(') {
      pos++; float v = evaluateExpression(expr, pos); skipWS();
      if (pos < expr.length() && expr[pos] == ')') pos++;
      return v;
    }

    if (isalpha(c)) {
      String name = "";
      while (pos < expr.length() && isalnum(expr[pos])) name += expr[pos++];
      name.toUpperCase();
      skipWS();

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

  float left = parseTerm();
  while (pos < expr.length()) {
    skipWS();
    char op = expr[pos];
    if (op == '+' || op == '-') {
      pos++; float right = parseTerm(); left = (op == '+') ? left + right : left - right;
    } else if (op == '=' || op == '<' || op == '>') {
      String cmp = String(op); pos++;
      if (pos < expr.length() && (expr[pos] == '=' || expr[pos] == '>' || expr[pos] == '<')) { cmp += expr[pos]; pos++; }
      float right = parseTerm();
      if (cmp == "=" || cmp == "==") return (left == right) ? 1 : 0;
      if (cmp == "<>") return (left != right) ? 1 : 0;
      if (cmp == "<")  return (left < right) ? 1 : 0;
      if (cmp == ">")  return (left > right) ? 1 : 0;
      if (cmp == "<=") return (left <= right) ? 1 : 0;
      if (cmp == ">=") return (left >= right) ? 1 : 0;
    } else break;
  }
  return left;
}

// --------------------------- FOR/NEXT ---------------------------------------
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
  int idx = toupper(note) - 'C'; if (idx < 0) idx += 7;
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
    if (isalpha(c)) {
      bool sharp = false;
      int octave = 4;
      if (i + 1 < tune.length() && tune[i + 1] == '#') { sharp = true; i++; }
      if (i + 1 < tune.length() && isdigit(tune[i + 1])) { octave = tune[i + 1] - '0'; i++; }
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
  SPI.begin();
  if (SD.begin(SD_CS_PIN)) return true;
  if (SD.begin()) return true;
  return false;
}

void saveProgram(String filename) {
  if (!filename.endsWith(".bas")) filename += ".bas";
  File file = SD.open("/" + filename, FILE_WRITE);
  if (!file) { showError("Cannot open file for save"); return; }
  for (auto& p : programLines) {
    file.println(String(p.first) + " " + p.second);
  }
  file.close();
  M5.Display.println("Saved: " + filename);
}

void loadProgram(String filename) {
  if (!filename.endsWith(".bas")) filename += ".bas";
  File file = SD.open("/" + filename);
  if (!file) { showError("File not found: " + filename); return; }

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
  if (line.length() == 0 || line.startsWith("REM")) return;

  int sp = line.indexOf(' ');
  String cmd = (sp == -1) ? line : line.substring(0, sp);
  cmd.toUpperCase();
  String args = (sp == -1) ? "" : line.substring(sp + 1);
  args.trim();

  checkBreak();
  if (stopRun) return;

  if (cmd == "PRINT" || cmd == "?") {
    int p = 0; bool nl = true;
    while (p < args.length()) {
      if (args[p] == '"') {
        p++; String s = "";
        while (p < args.length() && args[p] != '"') s += args[p++];
        if (p < args.length()) p++;
        M5.Display.print(s);
      } else {
        float v = evaluateExpression(args, p);
        M5.Display.print(v);
      }
      while (p < args.length() && (args[p] == ' ' || args[p] == ',' || args[p] == ';')) {
        if (args[p] == ',') M5.Display.print("   ");
        if (args[p] == ';') nl = false;
        p++;
      }
    }
    if (nl) M5.Display.println();
  }
  else if (cmd == "INPUT") {
    String prompt = ""; int semi = args.indexOf(';');
    if (semi != -1) { prompt = args.substring(0, semi); args = args.substring(semi + 1); args.trim(); }
    if (prompt.length()) M5.Display.print(prompt);
    String inp = readKeyboardLine("");
    float val = inp.toFloat();
    if (args.length() > 0) { int idx = varIndex(args[0]); if (idx >= 0) vars[idx] = val; }
    M5.Display.println();
  }
  else if (cmd == "LET" || (cmd.length() == 1 && isalpha(cmd[0]))) {
    String target = (cmd == "LET") ? args.substring(0, args.indexOf('=')) : cmd;
    target.trim();
    int eq = args.indexOf('=');
    if (eq == -1 && cmd != "LET") eq = line.indexOf('=');
    if (eq != -1) {
      String rhs = (cmd == "LET") ? args.substring(eq + 1) : line.substring(line.indexOf('=') + 1);
      rhs.trim(); int p = 0; float v = evaluateExpression(rhs, p);
      int idx = varIndex(target[0]); if (idx >= 0) vars[idx] = v;
    }
  }
  else if (cmd == "GOTO") {
    int target = args.toInt();
    if (programLines.count(target)) currentLine = target;
    else showError("Line not found");
    return;
  }
  else if (cmd == "GOSUB") {
    int target = args.toInt();
    if (gosubSP < MAX_GOSUB && programLines.count(target)) {
      gosubStack[gosubSP++] = currentLine;
      currentLine = target;
    } else showError("GOSUB error");
    return;
  }
  else if (cmd == "RETURN") {
    if (gosubSP > 0) currentLine = gosubStack[--gosubSP];
    else showError("RETURN without GOSUB");
    return;
  }
  else if (cmd == "IF") {
    int thenPos = args.indexOf("THEN");
    if (thenPos != -1) {
      String condStr = args.substring(0, thenPos);
      String thenPart = args.substring(thenPos + 4); thenPart.trim();
      int p = 0; float cond = evaluateExpression(condStr, p);
      if (cond != 0) {
        int target = thenPart.toInt();
        if (target > 0 && programLines.count(target)) { currentLine = target; return; }
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
    int toPos = rest.indexOf("TO");
    if (toPos == -1) { showError("Missing TO"); return; }

    String startStr = rest.substring(0, toPos);
    String endStep = rest.substring(toPos + 2);
    int stepPos = endStep.indexOf("STEP");
    String endStr = (stepPos == -1) ? endStep : endStep.substring(0, stepPos);
    String stepStr = (stepPos == -1) ? "1" : endStep.substring(stepPos + 4);

    int p = 0;
    float startV = evaluateExpression(startStr, p);
    p = 0; float endV = evaluateExpression(endStr, p);
    p = 0; float stepV = evaluateExpression(stepStr, p);

    if (!isForActive(idx)) {
      vars[idx] = startV;
    }
    pushForContext(idx, endV, stepV, currentLine);
  }
  else if (cmd == "NEXT") {
    int idx = (args.length() > 0) ? varIndex(args[0]) : -1;
    if (forStack.empty()) { showError("NEXT without FOR"); return; }

    ForContext ctx = forStack.back();
    if (idx != -1 && ctx.varIdx != idx) { showError("NEXT variable mismatch"); return; }

    vars[ctx.varIdx] += ctx.stepVal;
    bool cont = (ctx.stepVal >= 0) ? (vars[ctx.varIdx] <= ctx.endVal) : (vars[ctx.varIdx] >= ctx.endVal);

    if (cont) {
      currentLine = ctx.forLineNum;
    } else {
      forStack.pop_back();
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
      while (p < args.length() && isalpha(args[p])) varName += args[p++];
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
    M5.Display.setTextColor((uint16_t)fg);
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
    ledcAttach((int)pin, 5000, 8);
    ledcWrite((int)pin, (int)duty);
  }
  else if (cmd == "PAUSE" || cmd == "DELAY" || cmd == "SLEEP") {
    int p = 0; float ms = evaluateExpression(args, p);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)ms) { checkBreak(); if (stopRun) break; delay(5); }
  }
  else if (cmd == "SAVE") {
    String fn = args;
    if (fn.startsWith("\"") && fn.endsWith("\"")) fn = fn.substring(1, fn.length() - 1);
    saveProgram(fn);
  }
  else if (cmd == "LOAD") {
    String fn = args;
    if (fn.startsWith("\"") && fn.endsWith("\"")) fn = fn.substring(1, fn.length() - 1);
    loadProgram(fn);
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
    for (int i = 0; i < 26; i++) vars[i] = 0;
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

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      auto status = M5Cardputer.Keyboard.keysState();
      for (char c : status.word) { input += c; M5.Display.print(c); }
      if (status.del && input.length() > 0) {
        input.remove(input.length() - 1);
        int16_t cx = M5.Display.getCursorX();
        int16_t cy = M5.Display.getCursorY();
        M5.Display.fillRect(cx - 6, cy, 6, 8, TFT_BLACK);
        M5.Display.setCursor(cx - 6, cy);
      }
      if (status.enter) { M5.Display.println(); break; }
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

  if (isdigit(first[0])) {
    int num = first.toInt();
    String code = (firstSpace == -1) ? "" : line.substring(firstSpace + 1);
    code.trim();
    if (code.length() > 0) programLines[num] = code;
    else programLines.erase(num);
    M5.Display.println("OK");
  } else {
    executeLine(line);
  }
}

void printPrompt() {
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.print("> ");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// --------------------------- Setup & Main Loop ------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5Cardputer.begin(cfg, true);

  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.clear();
  M5.Display.setCursor(0, 0);

  M5.Speaker.setVolume(128);
  randomSeed(analogRead(0));

  M5.Display.println("Cardputer ADV BASIC v1.1");
  M5.Display.println("DATA/READ/RESTORE | User Sprites | FOR/NEXT | SD | PLAY");
  M5.Display.println("Graphics, Sound, GPIO, Math. SD card recommended.");

  if (initSD()) {
    M5.Display.println("SD card ready.");
  } else {
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.println("SD init failed (insert formatted microSD).");
    M5.Display.setTextColor(TFT_WHITE);
  }

  M5.Display.println("Ready.");
  printPrompt();
}

void loop() {
  M5Cardputer.update();

  // REPL input
  String line = readKeyboardLine("");
  if (line.length() > 0) {
    processLine(line);
    if (!running) printPrompt();
  }

  // RUN mode
  if (running && !stopRun) {
    auto it = programLines.lower_bound(currentLine);
    if (it == programLines.end()) {
      running = false;
      M5.Display.println("Program finished.");
      printPrompt();
      return;
    }

    currentLine = it->first;
    String stmt = it->second;
    executeLine(stmt);

    if (!running || stopRun) {
      running = false;
      if (stopRun) stopRun = false;
      printPrompt();
      return;
    }

    ++it;
    if (it != programLines.end()) {
      currentLine = it->first;
    } else {
      running = false;
      M5.Display.println("End of program.");
      printPrompt();
    }
  }

  delay(8);
}
