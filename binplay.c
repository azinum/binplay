// binplay.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h> // read
#include <string.h> // strlen

#include <portaudio.h>

#define PROG "binplay"
#define CC "gcc"
#define C_FLAGS "-Wall -O3 -pedantic -lportaudio"

#define NoError (0)
#define Error (-1)

typedef double f64;
typedef float f32;
typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;
typedef int8_t i8;
typedef uint8_t u8;

// These will be customizable by the user later
#define MAX_COMMAND_SIZE  512
#define MAX_FILE_SIZE     512

#define FRAMES_PER_BUFFER 512
#define SAMPLE_RATE       44100
#define SAMPLE_SIZE       2
#define CHANNEL_COUNT     2

#define OUTPUT_BUFFER_SIZE (FRAMES_PER_BUFFER * SAMPLE_RATE * SAMPLE_SIZE)

i32 g_frames_per_buffer = 512;
i32 g_sample_rate = SAMPLE_RATE;
i32 g_sample_size = 2;
f32 g_volume = 1.0f;
i32 g_cursor_speed = 5 * SAMPLE_RATE * SAMPLE_SIZE * CHANNEL_COUNT;
i32 g_loop_after_complete = 1;

#define CLAMP(V, MIN, MAX) (V > MAX) ? (MAX) : ((V < MIN) ? (MIN) : (V))

typedef struct Binplay {
  FILE* fp;
  const char* file_name;
  u32 file_size;
  i32 file_cursor;
  u8 done;
  u8 play;
  u8* output_buffer;
  u32 output_buffer_size;
} Binplay;

Binplay binplay = {0};
PaStream* stream = NULL;
PaStreamParameters output_port;

static i32 rebuild_program();
static void exec_command(const char* fmt, ...);
static void clear(i32 fp);
static void display_info(Binplay* b);
static i32 binplay_init(Binplay* b, const char* path);
static i32 binplay_process_audio(void* output);
static i32 stereo_callback(const void* in_buffer, void* out_buffer, unsigned long frames_per_buffer, const PaStreamCallbackTimeInfo* time_info, PaStreamCallbackFlags flags, void* user_data);
static i32 binplay_open_stream(Binplay* b);
static i32 binplay_start_stream(Binplay* b);
static void binplay_exit(Binplay* b);

i32 main(i32 argc, char** argv) {
  if (rebuild_program()) {
    return 0;
  }
  if (argc <= 1) {
    fprintf(stdout, "USAGE:\n  ./%s <filename>\n", PROG);
    return 0;
  }
  Binplay* b = &binplay;
  if (binplay_init(b, argv[1]) == NoError) {
    if (binplay_open_stream(b) == NoError) {
      i32 fd = STDIN_FILENO;
      struct termios term;
      tcgetattr(fd, &term);
      term.c_lflag &= ~(ICANON | ECHO);
      term.c_cc[VMIN] = 0;
      term.c_cc[VTIME] = 2;
      tcsetattr(fd, TCSANOW, &term);
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL));

      clear(STDOUT_FILENO);
      display_info(b);

      binplay_start_stream(b);
      i32 read_size = 0;
      while (!b->done) {
        char input = 0;
        read_size = read(fd, &input, 1);
        if (read_size > 0) {
          switch (input) {
            // Spacebar
            case 32: {
              b->play = !b->play;
              break;
            }
            // Reset to 0
            case 'r': {
              b->file_cursor = 0;
              break;
            }
            // Go to end
            case 'e': {
              b->file_cursor = b->file_size;
              break;
            }
            // Arrow keys
            case 27: {
              char a[2] = {0};
              read_size = read(fd, &a[0], 2);
              if (read_size == 2) {
                clear(STDOUT_FILENO);
                if (a[0] == 91) {
                  // Left
                  if (a[1] == 68) {
                    b->file_cursor -= g_cursor_speed;
                  }
                  // Right
                  else if (a[1] == 67) {
                    b->file_cursor += g_cursor_speed;
                  }
                  b->file_cursor = CLAMP(b->file_cursor, 0, (i32)b->file_size);
                  // Up
                  if (a[1] == 65) {
                    g_volume += 0.05f;
                  }
                  // Down
                  else if (a[1] == 66) {
                    g_volume -= 0.05f;
                  }
                  g_volume = CLAMP(g_volume, 0.0f, 1.0f);
                }
              }
              break;
            }
            // Ctrl+D
            case 4: {
              b->done = 1;
              break;
            }
            default: {
              break;
            }
          }
        }
        clear(STDOUT_FILENO);
        display_info(b);
      }
    }
    binplay_exit(b);
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
  if (time_diff < 0) {
    return 0;
  }
  exec_command("%s %s.c -o %s %s", CC, PROG, PROG, C_FLAGS);
  return 1;
}

void exec_command(const char* fmt, ...) {
  char command[MAX_COMMAND_SIZE] = {0};
  va_list args;
  va_start(args, fmt);
  vsnprintf(command, MAX_COMMAND_SIZE, fmt, args);
  va_end(args);

  fprintf(stdout, "+ %s\n", command); // To simulate 'set -xe'
  FILE* fp = popen(command, "w");
  fclose(fp);
}

void clear(i32 fp) {
  const char* clear_code = "\x1b[2J\x1b[H"; // Clear tty and reset cursor
  i32 write_size = write(STDOUT_FILENO, clear_code, strlen(clear_code));
  (void)write_size;
}

void display_info(Binplay* b) {
  FILE* fp = stdout;
  const char* play_status[2] = {
    "",
    "(PAUSED)",
  };
  fprintf(fp, "Currently playing: %s %s\n", b->file_name, play_status[b->play == 0]);
  fprintf(fp, "Cursor: [%i/%i] (%i%%)\n", binplay.file_cursor, binplay.file_size, (i32)(100 * ((float)binplay.file_cursor / binplay.file_size)));

  fprintf(fp,
    "\n"
    "Volume: %i%%\n"
    "Channel count: %u\n"
    "Sample rate: %u\n"
    "Sample size: %u\n"
    "Frames per buffer: %u\n"
    ,
    (i32)(100 * g_volume),
    CHANNEL_COUNT,
    g_sample_rate,
    SAMPLE_SIZE,
    g_frames_per_buffer
  );
}

i32 binplay_init(Binplay* b, const char* path) {
  if (!(b->fp = fopen(path, "r"))) {
    fprintf(stderr, "Failed to open '%s'\n", path);
    return Error;
  }
  b->file_name = path;
  fseek(b->fp, 0, SEEK_END);
  b->file_size = ftell(b->fp);
  fseek(b->fp, 0, SEEK_SET);
  b->file_cursor = 0;
  b->done = 0;
  b->play = 1;
  b->output_buffer = malloc(OUTPUT_BUFFER_SIZE);
  if (!b->output_buffer) {
    return Error;
  }
  b->output_buffer_size = OUTPUT_BUFFER_SIZE;
  return NoError;
}

i32 stereo_callback(const void* in_buffer, void* out_buffer, unsigned long frames_per_buffer, const PaStreamCallbackTimeInfo* time_info, PaStreamCallbackFlags flags, void* user_data) {
  if (binplay_process_audio(out_buffer) == NoError) {
    return paContinue;
  }
  return paComplete;
}

i32 binplay_open_stream(Binplay* b) {
  PaError err = Pa_Initialize();
  if (err != paNoError) {
    Pa_Terminate();
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return Error;
  }

  i32 output_device = Pa_GetDefaultOutputDevice();
  output_port.device = output_device;
  output_port.channelCount = CHANNEL_COUNT;
  output_port.sampleFormat = paInt16;
  output_port.suggestedLatency = Pa_GetDeviceInfo(output_port.device)->defaultHighOutputLatency;
  output_port.hostApiSpecificStreamInfo = NULL;

  if ((err = Pa_IsFormatSupported(NULL, &output_port, g_sample_rate)) != paFormatIsSupported) {
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return Error;
  }

  err = Pa_OpenStream(
    &stream,
    NULL,
    &output_port,
    g_sample_rate,
    g_frames_per_buffer,
    paNoFlag,
    stereo_callback,
    NULL
  );
  if (err != paNoError) {
    Pa_Terminate();
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return Error;
  }
  return NoError;
}

i32 binplay_start_stream(Binplay* b) {
  PaError err = Pa_StartStream(stream);
  if (err != paNoError) {
    Pa_Terminate();
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return Error;
  }
  return NoError;
}

i32 binplay_process_audio(void* output) {
  Binplay* b = &binplay;
  i16* buffer = (i16*)output;

  i16* file_buffer = (i16*)b->output_buffer;
  if (b->play) {
    const u32 bytes_to_read = g_frames_per_buffer * g_sample_size * CHANNEL_COUNT;
    fseek(b->fp, b->file_cursor, SEEK_SET);
    u32 bytes_read = fread(file_buffer, 1, bytes_to_read, b->fp);
    for (u32 i = 0; i < g_frames_per_buffer * CHANNEL_COUNT; ++i) {
      *buffer++ = (i16)(g_volume * file_buffer[i]);
    }
    b->file_cursor += g_frames_per_buffer * g_sample_size * CHANNEL_COUNT;
    if (bytes_read < bytes_to_read || b->file_cursor >= b->file_size) {
      if (g_loop_after_complete) {
        b->file_cursor = 0;
      }
      else {
        b->file_cursor = b->file_size;
        b->play = 0;
      }
      return NoError;
    }
  }
  else {
    for (u32 i = 0; i < g_frames_per_buffer * CHANNEL_COUNT; ++i) {
      *buffer++ = 0;
    }
  }
  return NoError;
}

void binplay_exit(Binplay* b) {
  fclose(b->fp);
  free(b->output_buffer);
  b->output_buffer_size = 0;
  Pa_CloseStream(stream);
  Pa_Terminate();
}
