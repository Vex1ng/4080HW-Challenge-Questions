#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "object.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
    LoxToken current;
    LoxToken previous;
    bool     hadError;
    bool     panicMode;
} LoxParser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_TERNARY,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY,
    PREC_CALL,
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn    prefix;
    ParseFn    infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    LoxToken name;
    int      depth;
    bool     isConst;
    bool     isCaptured;
} Local;

typedef struct {
    uint8_t index;
    bool    isLocal;
} Upvalue;

#define MAX_LOOP_DEPTH 64
typedef struct {
    int loopStart;
    int localCount;
} LoopContext;

typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT,
} FunctionType;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction*     function;
    FunctionType     type;

    Local       locals[UINT8_MAX + 1];
    int         localCount;
    int         scopeDepth;
    bool        globalIsConst[UINT8_MAX + 1];
    Upvalue     upvalues[UINT8_MAX + 1];
    LoopContext loops[MAX_LOOP_DEPTH];
    int         loopDepth;
} Compiler;

LoxParser loxParser;
Compiler* current = NULL;

static Chunk* currentChunk() {
    return &current->function->chunk;
}

static void errorAt(LoxToken* token, const char* message) {
    if (loxParser.panicMode) return;
    loxParser.panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
    loxParser.hadError = true;
}

static void error(const char* message)          { errorAt(&loxParser.previous, message); }
static void errorAtCurrent(const char* message) { errorAt(&loxParser.current,  message); }

static void advance() {
    loxParser.previous = loxParser.current;
    for (;;) {
        loxParser.current = scanToken();
        if (loxParser.current.type != TOKEN_ERROR) break;
        errorAtCurrent(loxParser.current.start);
    }
}

static void consume(LoxTokenType type, const char* message) {
    if (loxParser.current.type == type) { advance(); return; }
    errorAtCurrent(message);
}

static bool check(LoxTokenType type) {
    return loxParser.current.type == type;
}

static bool match(LoxTokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, loxParser.previous.line);
}

static void emitBytes(uint8_t b1, uint8_t b2) {
    emitByte(b1);
    emitByte(b2);
}

static void emitShort(uint8_t op, uint16_t slot) {
    emitByte(op);
    emitByte((slot >> 8) & 0xff);
    emitByte(slot & 0xff);
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->count - 2;
}

static void patchJump(int offset) {
    int jump = currentChunk()->count - offset - 2;
    if (jump > UINT16_MAX) {
        error("Too much code to jump over.");
    }
    currentChunk()->code[offset]     = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);
    int offset = currentChunk()->count - loopStart + 2;
    if (offset > UINT16_MAX) error("Loop body too large.");
    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

static void emitReturn() {
    emitByte(OP_NIL);
    emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) { error("Too many constants in one chunk."); return 0; }
    return (uint8_t)constant;
}

static void emitConstant(Value value) {
    emitBytes(OP_CONSTANT, makeConstant(value));
}

static ObjFunction* endCompiler() {
    emitReturn();
    ObjFunction* function = current->function;

#ifdef DEBUG_PRINT_CODE
    if (!loxParser.hadError) {
        disassembleChunk(currentChunk(),
            function->name != NULL ? function->name->chars : "<script>");
    }
#endif

    current = current->enclosing;
    return function;
}

static void initCompiler(Compiler* compiler, FunctionType type) {
    compiler->enclosing  = current;
    compiler->function   = NULL;
    compiler->type       = type;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->loopDepth  = 0;
    compiler->function   = newFunction();
    current = compiler;

    if (type != TYPE_SCRIPT) {
        current->function->name = copyString(loxParser.previous.start,
                                             loxParser.previous.length);
    }

    Local* local       = &current->locals[current->localCount++];
    local->depth       = 0;
    local->isConst     = false;
    local->isCaptured  = false;
    local->name.start  = "";
    local->name.length = 0;
}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        current->localCount--;
    }
}

static void expression();
static void statement();
static void declaration();
static void varDeclaration();
static ParseRule* getRule(LoxTokenType type);
static void parsePrecedence(Precedence precedence);

static uint8_t identifierConstant(LoxToken* name) {
    for (int i = 0; i < currentChunk()->constants.count; i++) {
        Value existing = currentChunk()->constants.values[i];
        if (!IS_STRING(existing)) continue;
        ObjString* existingStr = AS_STRING(existing);
        if (existingStr->length == name->length &&
            memcmp(existingStr->chars, name->start, name->length) == 0) {
            return (uint8_t)i;
        }
    }
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(LoxToken* a, LoxToken* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, LoxToken* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
    int upvalueCount = compiler->function->upvalueCount;

    for (int i = 0; i < upvalueCount; i++) {
        Upvalue* upvalue = &compiler->upvalues[i];
        if (upvalue->index == index && upvalue->isLocal == isLocal) {
            return i;
        }
    }

    if (upvalueCount == UINT8_MAX + 1) {
        error("Too many closure variables in function.");
        return 0;
    }

    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index   = index;
    return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler* compiler, LoxToken* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolveLocal(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpvalue(compiler, (uint8_t)local, true);
    }

    int upvalue = resolveUpvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false);
    }

    return -1;
}

static void addLocal(LoxToken name, bool isConst) {
    if (current->localCount == UINT8_MAX + 1) {
        error("Too many local variables in function.");
        return;
    }
    Local* local      = &current->locals[current->localCount++];
    local->name       = name;
    local->depth      = -1;
    local->isConst    = isConst;
    local->isCaptured = false;
}

static void declareVariable(bool isConst) {
    if (current->scopeDepth == 0) return;

    LoxToken* name = &loxParser.previous;

    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) break;
        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }

    addLocal(*name, isConst);
}

static uint8_t parseVariable(const char* errorMessage, bool isConst) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    declareVariable(isConst);
    if (current->scopeDepth > 0) return 0;

    uint8_t index = identifierConstant(&loxParser.previous);
    current->globalIsConst[index] = isConst;
    return index;
}

static void markInitialized() {
    if (current->scopeDepth == 0) return;
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t global) {
    if (current->scopeDepth > 0) {
        markInitialized();
        return;
    }
    emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList() {
    uint8_t argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            expression();
            if (argCount == 255) {
                error("Can't have more than 255 arguments.");
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return argCount;
}

static void and_(bool canAssign) {
    int endJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    parsePrecedence(PREC_AND);
    patchJump(endJump);
}

static void or_(bool canAssign) {
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump  = emitJump(OP_JUMP);
    patchJump(elseJump);
    emitByte(OP_POP);
    parsePrecedence(PREC_OR);
    patchJump(endJump);
}

static void namedVariable(LoxToken name, bool canAssign) {
    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);

    if (arg != -1) {
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else if ((arg = resolveUpvalue(current, &name)) != -1) {
        getOp = OP_GET_UPVALUE;
        setOp = OP_SET_UPVALUE;
    } else {
        arg   = identifierConstant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        if (arg != -1 && setOp == OP_SET_LOCAL && current->locals[arg].isConst) {
            error("Cannot assign to a const variable.");
        } else if (setOp == OP_SET_GLOBAL && current->globalIsConst[arg]) {
            error("Cannot assign to a const variable.");
        }
        expression();
        if (setOp == OP_SET_LOCAL) {
            emitShort(setOp, (uint16_t)arg);
        } else {
            emitBytes(setOp, (uint8_t)arg);
        }
    } else {
        if (getOp == OP_GET_LOCAL) {
            emitShort(getOp, (uint16_t)arg);
        } else {
            emitBytes(getOp, (uint8_t)arg);
        }
    }
}

static void variable(bool canAssign) {
    namedVariable(loxParser.previous, canAssign);
}

static void call(bool canAssign) {
    uint8_t argCount = argumentList();
    emitBytes(OP_CALL, argCount);
}

static void binary(bool canAssign) {
    LoxTokenType operatorType = loxParser.previous.type;
    ParseRule*   rule         = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType) {
        case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
        case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL);          break;
        case TOKEN_GREATER:       emitByte(OP_GREATER);        break;
        case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT);  break;
        case TOKEN_LESS:          emitByte(OP_LESS);           break;
        case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
        case TOKEN_PLUS:          emitByte(OP_ADD);            break;
        case TOKEN_MINUS:         emitByte(OP_SUBTRACT);       break;
        case TOKEN_STAR:          emitByte(OP_MULTIPLY);       break;
        case TOKEN_SLASH:         emitByte(OP_DIVIDE);         break;
        default: return;
    }
}

static void grouping(bool canAssign) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
    double value = strtod(loxParser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
    emitConstant(OBJ_VAL(copyString(loxParser.previous.start + 1,
                                    loxParser.previous.length - 2)));
}

static void literal(bool canAssign) {
    switch (loxParser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        case TOKEN_NIL:   emitByte(OP_NIL);   break;
        case TOKEN_TRUE:  emitByte(OP_TRUE);  break;
        default: return;
    }
}

static void unary(bool canAssign) {
    LoxTokenType operatorType = loxParser.previous.type;
    parsePrecedence(PREC_UNARY);
    switch (operatorType) {
        case TOKEN_BANG:  emitByte(OP_NOT);    break;
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        default: return;
    }
}

static void ternary(bool canAssign) {
    parsePrecedence(PREC_TERNARY);
    consume(TOKEN_COLON, "Expect ':' after then-branch.");
    parsePrecedence(PREC_TERNARY);
    emitByte(OP_TERNARY);
}

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, call,    PREC_CALL},
    [TOKEN_RIGHT_PAREN]   = {NULL,     NULL,    PREC_NONE},
    [TOKEN_LEFT_BRACE]    = {NULL,     NULL,    PREC_NONE},
    [TOKEN_RIGHT_BRACE]   = {NULL,     NULL,    PREC_NONE},
    [TOKEN_COMMA]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_DOT]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_MINUS]         = {unary,    binary,  PREC_TERM},
    [TOKEN_PLUS]          = {NULL,     binary,  PREC_TERM},
    [TOKEN_SEMICOLON]     = {NULL,     NULL,    PREC_NONE},
    [TOKEN_SLASH]         = {NULL,     binary,  PREC_FACTOR},
    [TOKEN_STAR]          = {NULL,     binary,  PREC_FACTOR},
    [TOKEN_QUESTION]      = {NULL,     ternary, PREC_TERNARY},
    [TOKEN_COLON]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_BANG]          = {unary,    NULL,    PREC_NONE},
    [TOKEN_BANG_EQUAL]    = {NULL,     binary,  PREC_EQUALITY},
    [TOKEN_EQUAL]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_EQUAL_EQUAL]   = {NULL,     binary,  PREC_EQUALITY},
    [TOKEN_GREATER]       = {NULL,     binary,  PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL] = {NULL,     binary,  PREC_COMPARISON},
    [TOKEN_LESS]          = {NULL,     binary,  PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]    = {NULL,     binary,  PREC_COMPARISON},
    [TOKEN_IDENTIFIER]    = {variable, NULL,    PREC_NONE},
    [TOKEN_STRING]        = {string,   NULL,    PREC_NONE},
    [TOKEN_NUMBER]        = {number,   NULL,    PREC_NONE},
    [TOKEN_AND]           = {NULL,     and_,    PREC_AND},
    [TOKEN_CLASS]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_ELSE]          = {NULL,     NULL,    PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,    PREC_NONE},
    [TOKEN_FOR]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_FUN]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,    PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,    PREC_NONE},
    [TOKEN_OR]            = {NULL,     or_,     PREC_OR},
    [TOKEN_PRINT]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,    PREC_NONE},
    [TOKEN_SUPER]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_THIS]          = {NULL,     NULL,    PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,    PREC_NONE},
    [TOKEN_VAR]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_WHILE]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_CONTINUE]      = {NULL,     NULL,    PREC_NONE},
    [TOKEN_SWITCH]        = {NULL,     NULL,    PREC_NONE},
    [TOKEN_CASE]          = {NULL,     NULL,    PREC_NONE},
    [TOKEN_DEFAULT]       = {NULL,     NULL,    PREC_NONE},
    [TOKEN_CONST]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_ERROR]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_EOF]           = {NULL,     NULL,    PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(loxParser.previous.type)->prefix;
    if (prefixRule == NULL) { error("Expect expression."); return; }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(loxParser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(loxParser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

static ParseRule* getRule(LoxTokenType type) {
    return &rules[type];
}

static void expression() {
    parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void function(FunctionType type) {
    Compiler compiler;
    initCompiler(&compiler, type);
    beginScope();

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            current->function->arity++;
            if (current->function->arity > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            uint8_t constant = parseVariable("Expect parameter name.", false);
            defineVariable(constant);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    block();

    ObjFunction* fn = endCompiler();
    emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(fn)));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
}

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void returnStatement() {
    if (current->type == TYPE_SCRIPT) {
        error("Can't return from top-level code.");
    }

    if (match(TOKEN_SEMICOLON)) {
        emitReturn();
    } else {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitByte(OP_RETURN);
    }
}

static void ifStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();

    int elseJump = emitJump(OP_JUMP);
    patchJump(thenJump);
    emitByte(OP_POP);

    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJump);
}

static void pushLoopContext(int loopStart, int localCount) {
    if (current->loopDepth == MAX_LOOP_DEPTH) {
        error("Too many nested loops.");
        return;
    }
    current->loops[current->loopDepth].loopStart  = loopStart;
    current->loops[current->loopDepth].localCount = localCount;
    current->loopDepth++;
}

static void popLoopContext() {
    current->loopDepth--;
}

static void setContinueTarget(int loopStart) {
    current->loops[current->loopDepth - 1].loopStart = loopStart;
}

static void continueStatement() {
    if (current->loopDepth == 0) {
        error("Can't use 'continue' outside of a loop.");
        return;
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

    LoopContext* ctx = &current->loops[current->loopDepth - 1];
    for (int i = current->localCount; i > ctx->localCount; i--) {
        emitByte(OP_POP);
    }
    emitLoop(ctx->loopStart);
}

static void whileStatement() {
    int loopStart = currentChunk()->count;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    pushLoopContext(loopStart, current->localCount);
    statement();
    popLoopContext();

    emitLoop(loopStart);
    patchJump(exitJump);
    emitByte(OP_POP);
}

static void forStatement() {
    beginScope();
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

    int loopVarSlot = -1;
    if (match(TOKEN_SEMICOLON)) {
    } else if (match(TOKEN_VAR)) {
        loopVarSlot = current->localCount;
        varDeclaration();
    } else {
        expressionStatement();
    }

    int loopStart = currentChunk()->count;

    int exitJump = -1;
    if (!match(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    }

    pushLoopContext(-1, current->localCount);
    if (!match(TOKEN_RIGHT_PAREN)) {
        int bodyJump       = emitJump(OP_JUMP);
        int incrementStart = currentChunk()->count;

        expression();
        emitByte(OP_POP);
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

        emitLoop(loopStart);
        loopStart = incrementStart;
        patchJump(bodyJump);
    }
    setContinueTarget(loopStart);

    int iterVarSlot = -1;
    if (loopVarSlot != -1) {
        beginScope();
        emitBytes(OP_GET_LOCAL, (uint8_t)loopVarSlot);
        Local* loopVar = &current->locals[loopVarSlot];
        addLocal(loopVar->name, loopVar->isConst);
        markInitialized();
        iterVarSlot = current->localCount - 1;
    }

    statement();
    popLoopContext();

    if (iterVarSlot != -1) {
        emitBytes(OP_GET_LOCAL, (uint8_t)iterVarSlot);
        emitBytes(OP_SET_LOCAL, (uint8_t)loopVarSlot);
        emitByte(OP_POP);
        endScope();
    }

    emitLoop(loopStart);

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);
    }

    endScope();
}

#define MAX_CASES 256
static void switchStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'switch'.");
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after switch value.");
    consume(TOKEN_LEFT_BRACE, "Expect '{' before switch cases.");

    uint16_t switchSlot = (uint16_t)current->localCount;

    int endJumps[MAX_CASES];
    int endJumpCount = 0;
    bool hasDefault  = false;

    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_CASE)) {
            emitShort(OP_GET_LOCAL, switchSlot);
            expression();
            consume(TOKEN_COLON, "Expect ':' after case value.");
            emitByte(OP_EQUAL);

            int skipJump = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP);

            while (!check(TOKEN_CASE) && !check(TOKEN_DEFAULT) &&
                   !check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
                statement();
            }

            if (endJumpCount == MAX_CASES) { error("Too many cases in switch."); return; }
            endJumps[endJumpCount++] = emitJump(OP_JUMP);
            patchJump(skipJump);
            emitByte(OP_POP);

        } else if (match(TOKEN_DEFAULT)) {
            if (hasDefault) error("Can't have more than one 'default' clause.");
            hasDefault = true;
            consume(TOKEN_COLON, "Expect ':' after 'default'.");
            while (!check(TOKEN_CASE) && !check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
                statement();
            }
        } else {
            error("Expect 'case' or 'default' in switch body.");
            advance();
        }
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after switch cases.");
    for (int i = 0; i < endJumpCount; i++) patchJump(endJumps[i]);
    emitByte(OP_POP);
}
#undef MAX_CASES

static void synchronize() {
    loxParser.panicMode = false;
    while (loxParser.current.type != TOKEN_EOF) {
        if (loxParser.previous.type == TOKEN_SEMICOLON) return;
        switch (loxParser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_SWITCH:
            case TOKEN_WHILE:
            case TOKEN_CONTINUE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:;
        }
        advance();
    }
}

static void varDeclaration() {
    uint8_t global = parseVariable("Expect variable name.", false);

    if (match(TOKEN_EQUAL)) {
        expression();
    } else {
        emitByte(OP_NIL);
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    defineVariable(global);
}

static void constDeclaration() {
    uint8_t global = parseVariable("Expect variable name.", true);

    consume(TOKEN_EQUAL, "Expect '=' after const variable name.");
    expression();

    consume(TOKEN_SEMICOLON, "Expect ';' after const declaration.");
    defineVariable(global);
}

static void funDeclaration() {
    uint8_t global = parseVariable("Expect function name.", false);
    markInitialized();
    function(TYPE_FUNCTION);
    defineVariable(global);
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_SWITCH)) {
        switchStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_CONTINUE)) {
        continueStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();
    }
}

static void declaration() {
    if (match(TOKEN_FUN)) {
        funDeclaration();
    } else if (match(TOKEN_VAR)) {
        varDeclaration();
    } else if (match(TOKEN_CONST)) {
        constDeclaration();
    } else {
        statement();
    }

    if (loxParser.panicMode) synchronize();
}

ObjFunction* compile(const char* source) {
    initScanner(source);

    Compiler compiler;
    initCompiler(&compiler, TYPE_SCRIPT);

    loxParser.hadError  = false;
    loxParser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    ObjFunction* function = endCompiler();
    return loxParser.hadError ? NULL : function;
}