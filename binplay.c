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
#include <assert.h>

#include <portaudio.h>

#define PROG "binplay"
#define CC "gcc"
#define C_FLAGS "-Wall -O3 -pedantic -lportaudio"

enum Error_code {
  Error = -1,
  NoError = 0,

  MAX_ERROR,
};

typedef double f64;
typedef float f32;
typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;
typedef int8_t i8;
typedef uint8_t u8;

typedef enum Arg_type {
  ArgInt = 0,
  ArgFloat,
  ArgString,
  ArgBuffer,

  MAX_ARG_TYPE,
} Arg_type;

static const char* arg_type_desc[MAX_ARG_TYPE] = {
  "integer",
  "float",
  "string",
  "buffer",
};

#define Help (MAX_ERROR + 1)

typedef struct Parse_arg {
  char flag;  // Single char to identify the argument flag
  const char* long_flag; // Long string to identify the argument flag
  const char* desc; // Description of this flag
  Arg_type type;  // Which type the data argument is to be
  i32 num_args;  // Can be either one or zero for any one flag
  void* data; // Reference to the data which is going to be overwritten by the value of the argument(s)
} Parse_arg;

#define CLAMP(V, MIN, MAX) (V > MAX) ? (MAX) : ((V < MIN) ? (MIN) : (V))
#define ARR_SIZE(ARR) (sizeof(ARR) / sizeof(ARR[0]))
#define MAX_COMMAND_SIZE  512
#define MAX_FILE_SIZE     512

#define FRAMES_PER_BUFFER 512
#define SAMPLE_RATE       44100
#define SAMPLE_SIZE       2
#define CHANNEL_COUNT     2

i32 g_frames_per_buffer = 512;
i32 g_sample_rate = SAMPLE_RATE;
i32 g_sample_size = 2;
f32 g_volume = 1.0f;
i32 g_cursor_speed = 5 * SAMPLE_RATE * SAMPLE_SIZE * CHANNEL_COUNT;
i32 g_loop_after_complete = 1;

typedef struct Binplay {
  FILE* fp;
  const char* file_name;
  u32 file_size;
  i32 file_cursor;
  u8 done;
  u8 play;
  u32 output_size;
  u8* output;
} Binplay;

Binplay binplay = {0};
PaStream* stream = NULL;
PaStreamParameters output_port;

static void args_print_help(FILE* fp, Parse_arg* args, i32 num_args, i32 argc, char** argv);
static i32 parse_args(Parse_arg* args, i32 num_args, i32 argc, char** argv);
static i32 rebuild_program();
static void exec_command(const char* fmt, ...);
static void clear(i32 fp);
static void display_info(Binplay* b);
static i32 binplay_init(Binplay* b, const char* path);
static void binplay_exec(Binplay* b);
static i32 binplay_process_audio(void* output);
static i32 stereo_callback(const void* in_buffer, void* out_buffer, unsigned long frames_per_buffer, const PaStreamCallbackTimeInfo* time_info, PaStreamCallbackFlags flags, void* user_data);
static i32 binplay_open_stream(Binplay* b);
static i32 binplay_start_stream(Binplay* b);
static void binplay_exit(Binplay* b);

i32 main(i32 argc, char** argv) {
  if (rebuild_program()) {
    return 0;
  }
  char* filename = NULL;
  Parse_arg args[] = {
    {0, NULL, "filename", ArgString, 0, &filename},
    {'f', "frames-per-buffer", "number of frames to handle per buffer", ArgInt, 1, &g_frames_per_buffer},
    {'s', "sample-size", "size of each sample in the data buffer", ArgInt, 1, &g_sample_size},
    {'r', "sample-rate", "number of samples per second", ArgInt, 1, &g_sample_rate},
    {'v', "volume", "startup volume (values between 0.0 and 1.0 give optimal results)", ArgFloat, 1, &g_volume},
  };
  i32 result = parse_args(args, ARR_SIZE(args), argc, argv);
  // TODO(lucas): Try to read from pipe if no filename was specified,
  // and if that fails we exit with a failure code.
  if (!filename) {
    fprintf(stderr, "Expected filename, but none was specified\n");
    args_print_help(stderr, args, ARR_SIZE(args), argc, argv);
    return EXIT_FAILURE;
  }
  if (result == NoError) {
    Binplay* b = &binplay;
    if (binplay_init(b, filename) == NoError) {
      if (binplay_open_stream(b) == NoError) {
        binplay_exec(b);
      }
      binplay_exit(b);
    }
  }
  else if (result == Error) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

void args_print_help(FILE* fp, Parse_arg* args, i32 num_args, i32 argc, char** argv) {
  assert(argc != 0);
  i32 longest_arg_length = 0;
  // Find the longest argument (longflag)
  for (u32 arg_index = 0; arg_index < num_args; ++arg_index) {
    Parse_arg* arg = &args[arg_index];
    if (!arg->long_flag) {
      continue;
    }
    u32 argLength = strlen(arg->long_flag);
    if (longest_arg_length < argLength) {
      longest_arg_length = argLength;
    }
  }

  fprintf(fp, "USAGE:\n  %s [options]\n\n", argv[0]);
  fprintf(fp, "FLAGS:\n");
  for (u32 arg_index = 0; arg_index < num_args; ++arg_index) {
    Parse_arg* arg = &args[arg_index];
    fprintf(fp, "  ");
    if (arg->flag) {
      fprintf(fp, "-%c", arg->flag);
    }
    if (arg->flag && arg->long_flag) {
      fprintf(fp, ", --%-*s", longest_arg_length, arg->long_flag);
    }
    else if (!arg->flag && arg->long_flag) {
      fprintf(fp, "--%-*s", longest_arg_length, arg->long_flag);
    }
    if (arg->num_args > 0) {
      fprintf(fp, " ");
      fprintf(fp, "<%s>", arg_type_desc[arg->type]);
    }
    if (arg->desc != NULL) {
      fprintf(fp, " %s", arg->desc);
    }
    fprintf(fp, "\n");
  }
  fprintf(fp, "  -h, --%-*s show help menu\n\n", longest_arg_length, "help");
}

i32 parse_args(Parse_arg* args, i32 num_args, i32 argc, char** argv) {
  if (!argv) {
    return NoError;
  }
  for (i32 index = 1; index < argc; ++index) {
    char* arg = argv[index];
    u8 long_flag = 0;
    u8 found_flag = 0;

    if (*arg == '-') {
      arg++;
      if (*arg == '-') {
        long_flag = 1;
        arg++;
      }
      if (*arg == 'h' && !long_flag) {
        args_print_help(stdout, args, num_args, argc, argv);
        return Help;
      }
      if (long_flag) {
        if (!strcmp(arg, "help")) {
          args_print_help(stdout, args, num_args, argc, argv);
          return Help;
        }
      }
      Parse_arg* parse_arg = NULL;
      // Linear search over the array of user defined arguments
      for (i32 arg_index = 0; arg_index < num_args; ++arg_index) {
        parse_arg = &args[arg_index];
        if (long_flag) {
          if (parse_arg->long_flag) {
            if (!strcmp(parse_arg->long_flag, arg)) {
              found_flag = 1;
              break;
            }
          }
        }
        else {
          if (parse_arg->flag == *arg) {
            // We found the flag
            found_flag = 1;
            break;
          }
        }
      }

      if (found_flag) {
        if (parse_arg->num_args > 0) {
          if (index + 1 < argc) {
            char* buffer = argv[++index];
            assert(buffer != NULL);
            assert(parse_arg);
            switch (parse_arg->type) {
              case ArgInt: {
                sscanf(buffer, "%i", (i32*)parse_arg->data);
                break;
              }
              case ArgFloat: {
                sscanf(buffer, "%f", (float*)parse_arg->data);
                break;
              }
              case ArgString: {
                char** String = parse_arg->data;
                *String = buffer;
                break;
              }
              case ArgBuffer: {
                sscanf(buffer, "%s", (char*)parse_arg->data);
                break;
              }
              default:
                assert("Invalid argument type specified" && 0);
                break;
            }
          }
          else {
            fprintf(stderr, "Missing parameter(s) after flag -%c", *arg);
            if (parse_arg->long_flag) {
              fprintf(stderr, "/--%s", parse_arg->long_flag);
            }
            fprintf(stderr, "\n");
            return Error;
          }
        }
        else {
          switch (parse_arg->type) {
            case ArgInt: {
              *(i32*)parse_arg->data = 1;
              break;
            }
            case ArgFloat: {
              *(float*)parse_arg->data = 1.0f;
              break;
            }
            default:
              break;
          }
        }
      }
      else {
        fprintf(stderr, "Flag '%s' not defined\n", arg);
        return Error;
      }
    }
    else {
      Parse_arg* parse_arg = NULL;
      for (u32 arg_index = 0; arg_index < num_args; ++arg_index) {
        parse_arg = &args[arg_index];
        if (parse_arg->flag == 0 && parse_arg->long_flag == NULL) {
          switch (parse_arg->type) {
            case ArgString: {
              char** String = parse_arg->data;
              *String = argv[index];
              break;
            }
            case ArgBuffer: {
              strcpy((char*)parse_arg->data, argv[index]);
            }
            default:
              break;
          }
          break;
        }
      }
    }
  }
  return NoError;
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
  if (time_diff <= 0) {
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
  b->output_size = g_frames_per_buffer * g_sample_size * CHANNEL_COUNT;
  b->output = malloc(b->output_size);
  if (!b->output) {
    b->output_size = 0;
    return Error;
  }
  return NoError;
}

void binplay_exec(Binplay* b) {
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

  i16* file_buffer = (i16*)b->output;
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
  free(b->output);
  b->output_size = 0;
  Pa_CloseStream(stream);
  Pa_Terminate();
}
