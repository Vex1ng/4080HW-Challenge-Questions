#include <stdlib.h>

#include "chunk.h"
#include "memory.h"
#include "value.h"

void initChunk(Chunk* chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  chunk->lineCount = 0;
  chunk->lineCapacity = 0;
  initValueArray(&chunk->constants);
}

void freeChunk(Chunk* chunk) {
  FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
  FREE_ARRAY(int, chunk->lines, chunk->lineCapacity);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = GROW_CAPACITY(oldCapacity);
    chunk->code = GROW_ARRAY(uint8_t, chunk->code,
                             oldCapacity, chunk->capacity);
  }

  if (chunk->lineCount > 0 && chunk->lines[chunk->lineCount - 2] == line) {
    chunk->lines[chunk->lineCount - 1]++;
  } else {
    if (chunk->lineCapacity < chunk->lineCount + 2) {
      int oldCapacity = chunk->lineCapacity;
      chunk->lineCapacity = GROW_CAPACITY(oldCapacity);
      chunk->lines = GROW_ARRAY(int, chunk->lines,
                                oldCapacity, chunk->lineCapacity);
    }

    chunk->lines[chunk->lineCount] = line;
    chunk->lines[chunk->lineCount + 1] = 1;
    chunk->lineCount += 2;
  }

  chunk->code[chunk->count] = byte;
  chunk->count++;
}

int addConstant(Chunk* chunk, Value value) {
  writeValueArray(&chunk->constants, value);
  return chunk->constants.count - 1;
}

int getLine(Chunk* chunk, int instruction) {
  int offset = 0;
  for (int i = 0; i < chunk->lineCount; i += 2) {
    int line = chunk->lines[i];
    int runLength = chunk->lines[i + 1];
    offset += runLength;
    if (instruction < offset) {
      return line;
    }
  }
  return 0;
}

void writeConstant(Chunk* chunk, Value value, int line) {
  int constant = addConstant(chunk, value);

  if (constant <= 0xFF) {
    writeChunk(chunk, OP_CONSTANT, line);
    writeChunk(chunk, (uint8_t)constant, line);
  } else {
    writeChunk(chunk, OP_CONSTANT_LONG, line);
    writeChunk(chunk, (uint8_t)(constant & 0xFF), line);
    writeChunk(chunk, (uint8_t)((constant >> 8) & 0xFF), line);
    writeChunk(chunk, (uint8_t)((constant >> 16) & 0xFF), line);
  }
}
