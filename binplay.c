// binplay.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#define PROG "binplay"
#define CC "gcc"
#define C_FLAGS "-Wall -O3"

typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;
typedef int8_t i8;
typedef uint8_t u8;

#define MAX_COMMAND_SIZE 512
#define MAX_FILE_SIZE 512

static i32 rebuild_program();
static void exec_command(const char* fmt, ...);

i32 main(i32 argc, char** argv) {
  if (rebuild_program()) {
    return 0;
  }
  return 0;
}

// Compare modify dates between executable and source file
// Recompile and run program again if they differ.
i32 rebuild_program() {
  struct stat source_stat;
  struct stat bin_stat;

  char filename[MAX_FILE_SIZE] = {0};
  snprintf(filename, MAX_FILE_SIZE, "%s.c", PROG);
  if (stat(filename, &source_stat) < 0) {
    return 0;
  }
  snprintf(filename, MAX_FILE_SIZE, "%s", PROG);
  if (stat(filename, &bin_stat) < 0) {
    return 0;
  }

  time_t time_diff = source_stat.st_ctime - bin_stat.st_ctime;

  // Negative time diffs means that the executable file is up to date to the source code
  if (time_diff > 0) {
    exec_command("%s %s.c -o %s %s && ./%s", CC, PROG, PROG, C_FLAGS, PROG);
    return 1;
  }
  return 0;
}

void exec_command(const char* fmt, ...) {
  char command[MAX_COMMAND_SIZE] = {0};
  va_list args;
  va_start(args, fmt);
  vsnprintf(command, MAX_COMMAND_SIZE, fmt, args);
  va_end(args);

  FILE* fp = popen(command, "w");
  fclose(fp);
}
