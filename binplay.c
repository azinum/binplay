// binplay.c

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h> // read
#include <string.h> // strlen
#include <assert.h>

#include <portaudio.h>

#include "termgui/termgui.h"
#include "arg_parser/arg_parser.h"

#define PROG "binplay"
#define CC "gcc"
#define C_FLAGS "-O3 -pedantic -lportaudio"

enum Keys {
  KeyNone = 0,
  KeyExit = 4,
  KeyEnd = 'e',
  KeyReset = 'r',
  KeyToggleLoop = 'l',
  KeyTogglePause = 32, // Spacebar
  KeyToggleHelp = '\t',

  MaxKey,
};

static const char* key_code_desc[] = {
  " KEY          DESCRIPTION",
  " [^D]       - exit",
  " [E]        - go to the (e)nd",
  " [R]        - go to the start and (r)eset",
  " [L]        - toggle (l)oop",
  " [SPACEBAR] - toggle pause",
  " [TAB]      - toggle help menu",
};

typedef double f64;
typedef float f32;
typedef int32_t i32;
typedef uint32_t u32;
typedef int16_t i16;
typedef uint16_t u16;
typedef int8_t i8;
typedef uint8_t u8;

#define CLAMP(V, MIN, MAX) (V > MAX) ? (MAX) : ((V < MIN) ? (MIN) : (V))
#define ARR_SIZE(ARR) (sizeof(ARR) / sizeof(ARR[0]))
#define MAX_COMMAND_SIZE  512
#define MAX_FILE_SIZE     512

#define FRAMES_PER_BUFFER 512
#define SAMPLE_RATE       44100
#define SAMPLE_SIZE       2
#define CHANNEL_COUNT     2

#define INFO_BUFFER_SIZE 512

i32 g_frames_per_buffer = 512;
i32 g_sample_rate = SAMPLE_RATE;
i32 g_sample_size = 2;
i32 g_channel_count = CHANNEL_COUNT;
f32 g_volume = 1.0f;
i32 g_cursor_speed = 10 * SAMPLE_RATE * SAMPLE_SIZE * CHANNEL_COUNT;
i32 g_loop_after_complete = 1;

typedef struct Binplay {
  FILE* fp;
  const char* file_name;
  u32 file_size;
  i32 file_cursor;
  u8 done;
  u8 play;
  u8 show_help;
  u32 output_size;
  u8* output;
  char info[INFO_BUFFER_SIZE];
  f64 time_elapsed;
} Binplay;

Binplay binplay = {0};
PaStream* stream = NULL;
PaStreamParameters output_port;

static i32 rebuild_program();
static void exec_command(const char* fmt, ...);
static void display_info(Binplay* b);
static Result binplay_init(Binplay* b, const char* path);
static void binplay_exec(Binplay* b);
static i32 binplay_process_audio(void* output);
static i32 stereo_callback(const void* in_buffer, void* out_buffer, unsigned long frames_per_buffer, const PaStreamCallbackTimeInfo* time_info, PaStreamCallbackFlags flags, void* user_data);
static Result binplay_open_stream(Binplay* b);
static Result binplay_start_stream(Binplay* b);
static void binplay_exit(Binplay* b);

static void on_update_event(Element* e, void* userdata);
static void on_input_event(Element* e, void* userdata, const char* input, u32 size);

i32 main(i32 argc, char** argv) {
  if (rebuild_program()) {
    return 0;
  }
  char* filename = NULL;
  Parse_arg args[] = {
    {0, NULL, "filename", ArgString, 0, &filename},
    {'f', "frames-per-buffer", "number of frames to handle per buffer", ArgInt, 1, &g_frames_per_buffer},
    {'s', "sample-size", "size of each sample in the data buffer", ArgInt, 1, &g_sample_size},
    {'c', "channel-count", "how many audio channels to use", ArgInt, 1, &g_channel_count},
    {'r', "sample-rate", "number of samples per second", ArgInt, 1, &g_sample_rate},
    {'v', "volume", "startup volume (values between 0.0 and 1.0 give optimal results)", ArgFloat, 1, &g_volume},
  };
  arg_parser_init(0, 4, 4);
  ParseResult result = parse_args(args, ARR_SIZE(args), argc, argv);
  // TODO(lucas): Try to read from pipe if no filename was specified,
  // and if that fails we exit with a failure code.
  if (!filename) {
    fprintf(stderr, "Expected filename, but none was specified\n");
    args_print_help(stderr, args, ARR_SIZE(args), argv);
    return EXIT_FAILURE;
  }
  if (result == ArgParseOk) {
    Binplay* b = &binplay;
    if (binplay_init(b, filename) == NoError) {
      if (binplay_open_stream(b) == NoError) {
        binplay_exec(b);
      }
      binplay_exit(b);
    }
  }
  else if (result == ArgParseError) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
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

void display_info(Binplay* b) {
  const char* play_status[2] = {
    "",
    "[paused]",
  };
  const char* loop_status[2] = {
    "",
    "[looping]",
  };

  char* buffer = &b->info[0];

  snprintf(
    buffer,
    INFO_BUFFER_SIZE,
    "Currently playing: %s %s\n"
    "Progress: [%d/%d] (%d%%) %s\n"
    "\n"
    "Volume: %d%%\n"
    "Channel count: %d\n"
    "Sample rate: %d\n"
    "Sample size: %d\n"
    "Frames per buffer: %d\n"
    ,
    b->file_name,
    play_status[b->play == 0],
    b->file_cursor,
    b->file_size,
    (u32)(100 * (f32)b->file_cursor / b->file_size),
    loop_status[g_loop_after_complete != 0],
    (u32)(100 * g_volume),
    g_channel_count,
    g_sample_rate,
    g_sample_size,
    g_frames_per_buffer
  );
}

Result binplay_init(Binplay* b, const char* path) {
  Result result = NoError;
  if (!(b->fp = fopen(path, "r"))) {
    fprintf(stderr, "Failed to open '%s'\n", path);
    return_defer(Error);
  }
  b->file_name = path;
  fseek(b->fp, 0, SEEK_END);
  b->file_size = ftell(b->fp);
  fseek(b->fp, 0, SEEK_SET);
  b->file_cursor = 0;
  b->done = 0;
  b->play = 1;
  b->show_help = 0;
  b->output_size = g_frames_per_buffer * g_sample_size * g_channel_count;
  b->output = malloc(b->output_size);
  memset(b->info, 0, sizeof(b->info));
  b->time_elapsed = 0.0f;

  if (!b->output) {
    b->output_size = 0;
    return_defer(Error);
  }
  if (!Ok(tg_init())) {
    fprintf(stderr, "Failed to initialize termgui: %s\n", tg_err_string());
    return_defer(Error);
  }
defer:
  return result;
}

void binplay_exec(Binplay* b) {
  binplay_start_stream(b);

  Element* container = NULL;
  Element* info_text_container = NULL;
  Element* info_text = NULL;

  Element container_element;
  tg_container_init(&container_element, false);

  container = tg_attach_element(NULL, &container_element);
  container->padding = 2;
  container->focusable = false;
  container->update_callback = on_update_event;
  container->input_callback = on_input_event;
  container->userdata = b;

  Element info_text_container_element;
  tg_container_init(&info_text_container_element, true);

  info_text_container = tg_attach_element(container, &info_text_container_element);
  info_text_container->padding = 1;
  info_text_container->focusable = false;

  Element info_text_element;
  tg_text_init(&info_text_element, &b->info[0]);

  info_text = tg_attach_element(info_text_container, &info_text_element);
  info_text->border = false;
  info_text->focusable = false;

  display_info(b);
  while (!b->done) {
    TIMER_START();
    if (!Ok(tg_update())) {
      break;
    }
    tg_render();
    TIMER_END(
      b->time_elapsed += _dt;
    );
  }
}

i32 stereo_callback(const void* in_buffer, void* out_buffer, unsigned long frames_per_buffer, const PaStreamCallbackTimeInfo* time_info, PaStreamCallbackFlags flags, void* user_data) {
  if (binplay_process_audio(out_buffer) == NoError) {
    return paContinue;
  }
  return paComplete;
}

Result binplay_open_stream(Binplay* b) {
  Result result = NoError;
  PaError err = Pa_Initialize();
  if (err != paNoError) {
    Pa_Terminate();
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return_defer(Error);
  }

  i32 output_device = Pa_GetDefaultOutputDevice();
  output_port.device = output_device;
  output_port.channelCount = g_channel_count;
  output_port.sampleFormat = paInt16;
  output_port.suggestedLatency = Pa_GetDeviceInfo(output_port.device)->defaultHighOutputLatency;
  output_port.hostApiSpecificStreamInfo = NULL;

  if ((err = Pa_IsFormatSupported(NULL, &output_port, g_sample_rate)) != paFormatIsSupported) {
    fprintf(stderr, "PortAudio Error: %s\n", Pa_GetErrorText(err));
    return_defer(Error);
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
    return_defer(Error);
  }
defer:
  return result;
}

Result binplay_start_stream(Binplay* b) {
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
    const u32 bytes_to_read = g_frames_per_buffer * g_sample_size * g_channel_count;
    fseek(b->fp, b->file_cursor, SEEK_SET);
    u32 bytes_read = fread(file_buffer, 1, bytes_to_read, b->fp);
    for (u32 i = 0; i < g_frames_per_buffer * g_channel_count; ++i) {
      *buffer++ = (i16)(g_volume * file_buffer[i]);
    }
    b->file_cursor += g_frames_per_buffer * g_sample_size * g_channel_count;
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
    for (u32 i = 0; i < g_frames_per_buffer * g_channel_count; ++i) {
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
  tg_free();
  tg_print_error();
}

void on_update_event(Element* e, void* userdata) {
  if (!userdata) {
    return;
  }
  Binplay* b = (Binplay*)userdata;
  if (b->time_elapsed >= 1.0f) {
    b->time_elapsed = 0.0;
    display_info(b);
    tg_refresh();
  }
}

void on_input_event(Element* e, void* userdata, const char* input, u32 size) {
  if (!userdata) {
    return;
  }
  Binplay* b = (Binplay*)userdata;
  switch (*input) {
    case KeyTogglePause: {
      b->play = !b->play;
      break;
    }
    case KeyToggleLoop: {
      g_loop_after_complete = !g_loop_after_complete;
      break;
    }
    case KeyReset: {
      b->file_cursor = 0;
      break;
    }
    case KeyEnd: {
      b->file_cursor = b->file_size;
      break;
    }
    case KeyToggleHelp: {
      b->show_help = !b->show_help;
      break;
    }
    case 27: {
      if (size == 3) {
        ++input;
        if (input[0] == 91) {
          ++input;
          // Left arrow
          if (*input == 68) {
            b->file_cursor -= g_cursor_speed;
          }
          // Right arrow
          else if (*input == 67) {
            b->file_cursor += g_cursor_speed;
          }
          b->file_cursor = CLAMP(b->file_cursor, 0, (i32)b->file_size);

          // Down arrow
          if (*input == 65) {
            g_volume += 0.05f;
          }
          // Up arrow
          else if (*input == 66) {
            g_volume -= 0.05f;
          }
          g_volume = CLAMP(g_volume, 0.0f, 1.0f);
        }
      }
      break;
    }
    default:
      break;
  }
  display_info(b);
}
