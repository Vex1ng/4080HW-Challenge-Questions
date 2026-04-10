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
    Token current;
    Token previous;
    bool  hadError;
    bool  panicMode;
} Parser;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_TERNARY,     // ?:
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn    prefix;
    ParseFn    infix;
    Precedence precedence;
} ParseRule;

// ch21 - struct to track a local variable
typedef struct {
    Token name;
    int   depth;  // scope depth where this local was declared
} Local;

// ch21 - compiler state now also tracks locals
typedef struct {
    Local locals[UINT8_MAX + 1];
    int   localCount;
    int   scopeDepth;
} Compiler;

Parser    parser;
Compiler* current = NULL;
Chunk*    compilingChunk;

static Chunk* currentChunk() {
    return compilingChunk;
}

// ─── Error helpers ────────────────────────────────────────────────────────────

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message)          { errorAt(&parser.previous, message); }
static void errorAtCurrent(const char* message) { errorAt(&parser.current,  message); }

// ─── Token helpers ────────────────────────────────────────────────────────────

static void advance() {
    parser.previous = parser.current;
    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;
        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) { advance(); return; }
    errorAtCurrent(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

// ─── Emit helpers ─────────────────────────────────────────────────────────────

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t b1, uint8_t b2) {
    emitByte(b1);
    emitByte(b2);
}

static void emitReturn() {
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

static void endCompiler() {
    emitReturn();
#ifdef DEBUG_PRINT_CODE
    if (!parser.hadError) disassembleChunk(currentChunk(), "code");
#endif
}

// ─── Scope helpers (ch21) ─────────────────────────────────────────────────────

static void initCompiler(Compiler* compiler) {
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    current = compiler;
}

static void beginScope() {
    current->scopeDepth++;
}

static void endScope() {
    current->scopeDepth--;

    // pop all locals that were declared in the scope we just left
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        emitByte(OP_POP);
        current->localCount--;
    }
}

// ─── Forward declarations ─────────────────────────────────────────────────────

static void expression();
static void statement();
static void declaration();
static ParseRule* getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

// ─── Identifier / variable helpers ───────────────────────────────────────────

static uint8_t identifierConstant(Token* name) {
    // check if this variable name is already in the constants table
    for (int i = 0; i < currentChunk()->constants.count; i++) {
        Value existing = currentChunk()->constants.values[i];
        if (!IS_STRING(existing)) continue; // skip non-string constants
        ObjString* existingStr = AS_STRING(existing);

        // check if the lengths and characters match
        if (existingStr->length == name->length &&
            memcmp(existingStr->chars, name->start, name->length) == 0) {
            return (uint8_t)i; // reuse the existing slot
        }
    }

    return makeConstant(OBJ_VAL(copyString(name->start, name->length))); //use new slot
}

// ch21 - check if two identifier tokens have the same name
static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

// ch21 - walk locals from innermost scope outward looking for a match
static int resolveLocal(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error("Can't read local variable in its own initializer.");
            }
            return i;
        }
    }
    return -1;  // not found - must be a global
}

// ch21 - register a new local variable in the current scope
static void addLocal(Token name) {
    if (current->localCount == UINT8_MAX + 1) {
        error("Too many local variables in function.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name  = name;
    local->depth = -1;  // -1 means declared but not yet initialized
}

// ch21 - declare a local variable (just adds it to the locals array)
static void declareVariable() {
    if (current->scopeDepth == 0) return;  // globals are handled differently

    Token* name = &parser.previous;

    // check for duplicate names in the same scope
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) break;
        if (identifiersEqual(name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }

    addLocal(*name);
}

static uint8_t parseVariable(const char* errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    declareVariable();
    if (current->scopeDepth > 0) return 0;  // locals don't need a constant index

    return identifierConstant(&parser.previous);
}

static void markInitialized() {
    current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(uint8_t global) {
    if (current->scopeDepth > 0) {
        // local variable - it's already on the stack, just mark it initialized
        markInitialized();
        return;
    }
    emitBytes(OP_DEFINE_GLOBAL, global);
}

// ─── Parse functions ──────────────────────────────────────────────────────────

static void namedVariable(Token name, bool canAssign) {
    uint8_t getOp, setOp;
    int arg = resolveLocal(current, &name);

    if (arg != -1) {
        // found a local
        getOp = OP_GET_LOCAL;
        setOp = OP_SET_LOCAL;
    } else {
        // must be a global
        arg   = identifierConstant(&name);
        getOp = OP_GET_GLOBAL;
        setOp = OP_SET_GLOBAL;
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        expression();
        emitBytes(setOp, (uint8_t)arg);
    } else {
        emitBytes(getOp, (uint8_t)arg);
    }
}

static void variable(bool canAssign) {
    namedVariable(parser.previous, canAssign);
}

static void binary(bool canAssign) {
    TokenType  operatorType = parser.previous.type;
    ParseRule* rule         = getRule(operatorType);
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
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
    emitConstant(OBJ_VAL(copyString(parser.previous.start + 1,
                                    parser.previous.length - 2)));
}

static void literal(bool canAssign) {
    switch (parser.previous.type) {
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        case TOKEN_NIL:   emitByte(OP_NIL);   break;
        case TOKEN_TRUE:  emitByte(OP_TRUE);  break;
        default: return;
    }
}

static void unary(bool canAssign) {
    TokenType operatorType = parser.previous.type;
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

// ─── Parse rule table ─────────────────────────────────────────────────────────

ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]    = {grouping, NULL,    PREC_NONE},
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
    [TOKEN_AND]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_CLASS]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_ELSE]          = {NULL,     NULL,    PREC_NONE},
    [TOKEN_FALSE]         = {literal,  NULL,    PREC_NONE},
    [TOKEN_FOR]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_FUN]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_IF]            = {NULL,     NULL,    PREC_NONE},
    [TOKEN_NIL]           = {literal,  NULL,    PREC_NONE},
    [TOKEN_OR]            = {NULL,     NULL,    PREC_NONE},
    [TOKEN_PRINT]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_RETURN]        = {NULL,     NULL,    PREC_NONE},
    [TOKEN_SUPER]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_THIS]          = {NULL,     NULL,    PREC_NONE},
    [TOKEN_TRUE]          = {literal,  NULL,    PREC_NONE},
    [TOKEN_VAR]           = {NULL,     NULL,    PREC_NONE},
    [TOKEN_WHILE]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_ERROR]         = {NULL,     NULL,    PREC_NONE},
    [TOKEN_EOF]           = {NULL,     NULL,    PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) { error("Expect expression."); return; }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }
}

static ParseRule* getRule(TokenType type) {
    return &rules[type];
}

static void expression() {
    parsePrecedence(PREC_ASSIGNMENT);
}

// ch21 - block statement: compile declarations until we hit a closing brace
static void block() {
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

// ─── Statements (ch18) ────────────────────────────────────────────────────────

static void printStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after value.");
    emitByte(OP_PRINT);
}

static void expressionStatement() {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);  // discard the result since nobody uses it
}

static void synchronize() {
    parser.panicMode = false;
    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:;
        }
        advance();
    }
}

static void statement() {
    if (match(TOKEN_PRINT)) {
        printStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        // ch21 - block statement
        beginScope();
        block();
        endScope();
    } else {
        expressionStatement();
    }
}

// ch19 - variable declaration
static void varDeclaration() {
    uint8_t global = parseVariable("Expect variable name.");

    if (match(TOKEN_EQUAL)) {
        expression();  // compile the initializer
    } else {
        emitByte(OP_NIL);  // default to nil if no initializer
    }

    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    defineVariable(global);
}

static void declaration() {
    if (match(TOKEN_VAR)) {
        varDeclaration();
    } else {
        statement();
    }

    if (parser.panicMode) synchronize();
}

// ─── Entry point ──────────────────────────────────────────────────────────────

bool compile(const char* source, Chunk* chunk) {
    initScanner(source);

    Compiler compiler;
    initCompiler(&compiler);

    compilingChunk = chunk;

    parser.hadError  = false;
    parser.panicMode = false;

    advance();

    while (!match(TOKEN_EOF)) {
        declaration();
    }

    endCompiler();
    return !parser.hadError;
}