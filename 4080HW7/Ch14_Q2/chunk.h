#ifndef clox_chunk_h
#define clox_chunk_h

#include "common.h"
#include "value.h"

typedef enum {
    OP_RETURN,
    OP_CONSTANT,
    OP_CONSTANT_LONG,
  } OpCode;

typedef struct {
    uint8_t* code;
    int* lines;
    ValueArray constants;
    int count;
    int capacity;
    int lineCount;
    int lineCapacity;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
int addConstant(Chunk* chunk, Value value);
int getLine(Chunk* chunk, int instruction);
void writeConstant(Chunk* chunk, Value value, int line);

#endif