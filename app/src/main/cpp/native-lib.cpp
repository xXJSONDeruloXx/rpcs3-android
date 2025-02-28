#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/vfs_config.h"
#include "Input/pad_thread.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Utilities/File.h"
#include "Utilities/JIT.h"
#include "Utilities/Thread.h"
#include "rpcs3_version.h"
#include "util/asm.hpp"
#include "util/console.h"
#include "util/fixed_typemap.hpp"
#include "util/logs.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include <Emu/Cell/Modules/cellSaveData.h>
#include <Emu/Cell/Modules/sceNpTrophy.h>
#include <Emu/Io/pad_config.h>
#include <Emu/RSX/GSFrameBase.h>
#include <Emu/System.h>

#include <algorithm>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <jni.h>
#include <string>
#include <sys/resource.h>
#include <thread>

struct AtExit {
  std::function<void()> cb;
  ~AtExit() { cb(); }
};

static bool g_initialized;
static ANativeWindow *g_native_window;

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

LOG_CHANNEL(rpcs3_android, "ANDROID");

struct LogListener : logs::listener {
  LogListener() { logs::listener::add(this); }

  void log(u64 stamp, const logs::message &msg, const std::string &prefix,
           const std::string &text) override {
    int prio = 0;
    switch (static_cast<logs::level>(msg)) {
    case logs::level::always:
    case logs::level::fatal:
      prio = ANDROID_LOG_FATAL;
      break;
    case logs::level::error:
      prio = ANDROID_LOG_ERROR;
      break;
    case logs::level::todo:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::success:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::warning:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::notice:
      prio = ANDROID_LOG_DEBUG;
      break;
    case logs::level::trace:
      prio = ANDROID_LOG_VERBOSE;
      break;
    }

    __android_log_write(prio, "RPCS3", text.c_str());
  }
} static g_androidLogListener;

struct GraphicsFrame : GSFrameBase {
  ANativeWindow *native_window;
  explicit GraphicsFrame(ANativeWindow *native_window)
      : native_window(native_window) {
    ANativeWindow_acquire(native_window);
  }
  ~GraphicsFrame() { ANativeWindow_release(native_window); }
  void close() override {}
  void reset() override {}
  bool shown() override { return true; }
  void hide() override {}
  void show() override {}
  void toggle_fullscreen() override {}

  void delete_context(draw_context_t ctx) override {}
  draw_context_t make_context() override { return nullptr; }
  void set_current(draw_context_t ctx) override {}
  void flip(draw_context_t ctx, bool skip_frame = false) override {}
  int client_width() override { return ANativeWindow_getWidth(native_window); }
  int client_height() override {
    return ANativeWindow_getHeight(native_window);
  }
  f64 client_display_rate() override { return 30.f; }
  bool has_alpha() override {
    return ANativeWindow_getFormat(native_window) == WINDOW_FORMAT_RGBA_8888;
  }

  display_handle_t handle() const override { return native_window; }

  bool can_consume_frame() const override { return false; }

  void present_frame(std::vector<u8> &data, u32 pitch, u32 width, u32 height,
                     bool is_bgra) const override {}

  void take_screenshot(const std::vector<u8> sshot_data, u32 sshot_width,
                       u32 sshot_height, bool is_bgra) override {}
};

std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

void jit_announce(uptr, usz, std::string_view);

[[noreturn]] void report_fatal_error(std::string_view _text,
                                     bool is_html = false,
                                     bool include_help_text = true) {
  std::string buf;

  buf = std::string(_text);

  // Check if thread id is in string
  if (_text.find("\nThread id = "sv) == umax && !thread_ctrl::is_main()) {
    // Append thread id if it isn't already, except on main thread
    fmt::append(buf, "\n\nThread id = %u.", thread_ctrl::get_tid());
  }

  if (!g_tls_serialize_name.empty()) {
    fmt::append(buf, "\nSerialized Object: %s", g_tls_serialize_name);
  }

  const system_state state = Emu.GetStatus(false);

  if (state == system_state::stopped) {
    fmt::append(buf, "\nEmulation is stopped");
  } else {
    const std::string &name = Emu.GetTitleAndTitleID();
    fmt::append(buf, "\nTitle: \"%s\" (emulation is %s)",
                name.empty() ? "N/A" : name.data(),
                state == system_state::stopping ? "stopping" : "running");
  }

  fmt::append(buf, "\nBuild: \"%s\"", rpcs3::get_verbose_version());
  fmt::append(buf, "\nDate: \"%s\"", std::chrono::system_clock::now());

  __android_log_write(ANDROID_LOG_FATAL, "RPCS3", buf.c_str());

  jit_announce(0, 0, "");
  utils::trap();
  std::abort();
  std::terminate();
}

void qt_events_aware_op(int repeat_duration_ms,
                        std::function<bool()> wrapped_op) {
  /// ?????
}

static std::string unwrap(JNIEnv *env, jstring string) {
  auto resultBuffer = env->GetStringUTFChars(string, nullptr);
  std::string result(resultBuffer);
  env->ReleaseStringUTFChars(string, resultBuffer);
  return result;
}
static jstring wrap(JNIEnv *env, const std::string &string) {
  return env->NewStringUTF(string.c_str());
}
static jstring wrap(JNIEnv *env, const char *string) {
  return env->NewStringUTF(string);
}

static std::string fix_dir_path(std::string string) {
  if (!string.empty() && !string.ends_with('/')) {
    string += '/';
  }

  return string;
}

#define MAKE_STRING(id, x) [int(localized_string_id::id)] = {x, U##x}

static std::pair<std::string, std::u32string> g_strings[] = {
    MAKE_STRING(RSX_OVERLAYS_COMPILING_SHADERS, "Compiling shaders"),
    MAKE_STRING(RSX_OVERLAYS_COMPILING_PPU_MODULES, "Compiling PPU Modules"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_YES, "Yes"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_NO, "No"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_OK, "OK"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_TITLE, "Save Dialog"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_DELETE, "Delete Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_LOAD, "Load Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_SAVE, "Save"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ACCEPT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SPACE, "Space"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_BACKSPACE, "Backspace"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SHIFT, "Shift"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_TEXT, "[Enter Text]"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_PASSWORD, "[Enter Password]"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE, "Select media"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE_PHOTO_IMPORT, "Select photo to import"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_EMPTY, "No media found."),
    MAKE_STRING(RSX_OVERLAYS_LIST_SELECT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_LIST_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_LIST_DENY, "Deny"),
    MAKE_STRING(CELL_OSK_DIALOG_TITLE, "On Screen Keyboard"),
		MAKE_STRING(CELL_OSK_DIALOG_BUSY, "The Home Menu can't be opened while the On Screen Keyboard is busy!"),
		MAKE_STRING(CELL_SAVEDATA_CB_BROKEN, "Error - Save data corrupted"),
		MAKE_STRING(CELL_SAVEDATA_CB_FAILURE, "Error - Failed to save or load"),
		MAKE_STRING(CELL_SAVEDATA_CB_NO_DATA, "Error - Save data cannot be found"),
		MAKE_STRING(CELL_SAVEDATA_NO_DATA, "There is no saved data."),
		MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_TITLE, "New Saved Data"),
		MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_SUB_TITLE, "Select to create a new entry"),
		MAKE_STRING(CELL_SAVEDATA_SAVE_CONFIRMATION, "Do you want to save this data?"),
		MAKE_STRING(CELL_SAVEDATA_AUTOSAVE, "Saving..."),
		MAKE_STRING(CELL_SAVEDATA_AUTOLOAD, "Loading..."),
		MAKE_STRING(CELL_CROSS_CONTROLLER_FW_MSG, "If your system software version on the PS Vita system is earlier than 1.80, you must update the system software to the latest version."),
		MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE, "Select Message"),
		MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_INVITE, "Select Invite"),
		MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
		MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_FROM, "From:"),
		MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_SUBJECT, "Subject:"),
		MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE, "Select Message To Send"),
		MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_INVITE, "Send Invite"),
		MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
		MAKE_STRING(RECORDING_ABORTED, "Recording aborted!"),
		MAKE_STRING(RPCN_NO_ERROR, "RPCN: No Error"),
		MAKE_STRING(RPCN_ERROR_INVALID_INPUT, "RPCN: Invalid Input (Wrong Host/Port)"),
		MAKE_STRING(RPCN_ERROR_WOLFSSL, "RPCN Connection Error: WolfSSL Error"),
		MAKE_STRING(RPCN_ERROR_RESOLVE, "RPCN Connection Error: Resolve Error"),
		MAKE_STRING(RPCN_ERROR_CONNECT, "RPCN Connection Error"),
		MAKE_STRING(RPCN_ERROR_LOGIN_ERROR, "RPCN Login Error: Identification Error"),
		MAKE_STRING(RPCN_ERROR_ALREADY_LOGGED, "RPCN Login Error: User Already Logged In"),
		MAKE_STRING(RPCN_ERROR_INVALID_LOGIN, "RPCN Login Error: Invalid Username"),
		MAKE_STRING(RPCN_ERROR_INVALID_PASSWORD, "RPCN Login Error: Invalid Password"),
		MAKE_STRING(RPCN_ERROR_INVALID_TOKEN, "RPCN Login Error: Invalid Token"),
		MAKE_STRING(RPCN_ERROR_INVALID_PROTOCOL_VERSION, "RPCN Misc Error: Protocol Version Error (outdated RPCS3?)"),
		MAKE_STRING(RPCN_ERROR_UNKNOWN, "RPCN: Unknown Error"),
		MAKE_STRING(RPCN_SUCCESS_LOGGED_ON, "Successfully logged on RPCN!"),
		MAKE_STRING(HOME_MENU_TITLE, "Home Menu"),
		MAKE_STRING(HOME_MENU_EXIT_GAME, "Exit Game"),
		MAKE_STRING(HOME_MENU_RESUME, "Resume Game"),
		MAKE_STRING(HOME_MENU_FRIENDS, "Friends"),
		MAKE_STRING(HOME_MENU_FRIENDS_REQUESTS, "Pending Friend Requests"),
		MAKE_STRING(HOME_MENU_FRIENDS_BLOCKED, "Blocked Users"),
		MAKE_STRING(HOME_MENU_FRIENDS_STATUS_ONLINE, "Online"),
		MAKE_STRING(HOME_MENU_FRIENDS_STATUS_OFFLINE, "Offline"),
		MAKE_STRING(HOME_MENU_FRIENDS_STATUS_BLOCKED, "Blocked"),
		MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_SENT, "You sent a friend request"),
		MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_RECEIVED, "Sent you a friend request"),
		MAKE_STRING(HOME_MENU_FRIENDS_REJECT_REQUEST, "Reject Request"),
		MAKE_STRING(HOME_MENU_FRIENDS_NEXT_LIST, "Next list"),
		MAKE_STRING(HOME_MENU_RESTART, "Restart Game"),
		MAKE_STRING(HOME_MENU_SETTINGS, "Settings"),
		MAKE_STRING(HOME_MENU_SETTINGS_SAVE, "Save custom configuration?"),
		MAKE_STRING(HOME_MENU_SETTINGS_SAVE_BUTTON, "Save"),
		MAKE_STRING(HOME_MENU_SETTINGS_DISCARD, "Discard the current settings' changes?"),
		MAKE_STRING(HOME_MENU_SETTINGS_DISCARD_BUTTON, "Discard"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO, "Audio"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_MASTER_VOLUME, "Master Volume"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BACKEND, "Audio Backend"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFERING, "Enable Buffering"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFER_DURATION, "Desired Audio Buffer Duration"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING, "Enable Time Stretching"),
		MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING_THRESHOLD, "Time Stretching Threshold"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO, "Video"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_FRAME_LIMIT, "Frame Limit"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_ANISOTROPIC_OVERRIDE, "Anisotropic Filter Override"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_OUTPUT_SCALING, "Output Scaling"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_RCAS_SHARPENING, "FidelityFX CAS Sharpening Intensity"),
		MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_STRETCH_TO_DISPLAY, "Stretch To Display Area"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT, "Input"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_BACKGROUND_INPUT, "Background Input Enabled"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_KEEP_PADS_CONNECTED, "Keep Pads Connected"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_SHOW_PS_MOVE_CURSOR, "Show PS Move Cursor"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_CAMERA_FLIP, "Camera Flip"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_MODE, "Pad Handler Mode"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_SLEEP, "Pad Handler Sleep"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_H, "Fake PS Move Rotation Cone (Horizontal)"),
		MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_V, "Fake PS Move Rotation Cone (Vertical)"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED, "Advanced"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_PREFERRED_SPU_THREADS, "Preferred SPU Threads"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_CPU_PREEMPTIONS, "Max Power Saving CPU-Preemptions"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_ACCURATE_RSX_RESERVATION_ACCESS, "Accurate RSX reservation access"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_SLEEP_TIMERS_ACCURACY, "Sleep Timers Accuracy"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_SPURS_THREADS, "Max SPURS Threads"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_DRIVER_WAKE_UP_DELAY, "Driver Wake-Up Delay"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_FREQUENCY, "VBlank Frequency"),
		MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_NTSC, "VBlank NTSC Fixup"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS, "Overlays"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_TROPHY_POPUPS, "Show Trophy Popups"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_RPCN_POPUPS, "Show RPCN Popups"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_SHADER_COMPILATION_HINT, "Show Shader Compilation Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PPU_COMPILATION_HINT, "Show PPU Compilation Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_AUTO_SAVE_LOAD_HINT, "Show Autosave/Autoload Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PRESSURE_INTENSITY_TOGGLE_HINT, "Show Pressure Intensity Toggle Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_ANALOG_LIMITER_TOGGLE_HINT,  "Show Analog Limiter Toggle Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_MOUSE_AND_KB_TOGGLE_HINT, "Show Mouse And Keyboard Toggle Hint"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY, "Performance Overlay"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE, "Enable Performance Overlay"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMERATE_GRAPH, "Enable Framerate Graph"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMETIME_GRAPH, "Enable Frametime Graph"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_DETAIL_LEVEL, "Detail level"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DETAIL_LEVEL, "Framerate Graph Detail Level"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DETAIL_LEVEL, "Frametime Graph Detail Level"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DATAPOINT_COUNT, "Framerate Datapoints"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DATAPOINT_COUNT, "Frametime Datapoints"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_UPDATE_INTERVAL, "Metrics Update Interval"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_POSITION, "Position"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_X, "Center Horizontally"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_Y, "Center Vertically"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_X, "Horizontal Margin"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_Y, "Vertical Margin"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FONT_SIZE, "Font Size"),
		MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_OPACITY, "Opacity"),
		MAKE_STRING(HOME_MENU_SETTINGS_DEBUG, "Debug"),
		MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_OVERLAY, "Debug Overlay"),
		MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_INPUT_OVERLAY, "Input Debug Overlay"),
		MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_DISABLE_VIDEO_OUTPUT, "Disable Video Output"),
		MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_TEXTURE_LOD_BIAS, "Texture LOD Bias Addend"),
		MAKE_STRING(HOME_MENU_SCREENSHOT, "Take Screenshot"),
		MAKE_STRING(HOME_MENU_SAVESTATE, "SaveState"),
		MAKE_STRING(HOME_MENU_SAVESTATE_SAVE, "Save Emulation State"),
		MAKE_STRING(HOME_MENU_SAVESTATE_AND_EXIT, "Save Emulation State And Exit"),
		MAKE_STRING(HOME_MENU_RELOAD_SAVESTATE, "Reload Last Emulation State"),
		MAKE_STRING(HOME_MENU_RECORDING, "Start/Stop Recording"),
		MAKE_STRING(HOME_MENU_TROPHIES, "Trophies"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_TITLE, "Hidden trophy"),
		MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_DESCRIPTION, "This trophy is hidden"),
		MAKE_STRING(HOME_MENU_TROPHY_PLATINUM_RELEVANT, "Platinum relevant"),
		MAKE_STRING(HOME_MENU_TROPHY_GRADE_BRONZE, "Bronze"),
		MAKE_STRING(HOME_MENU_TROPHY_GRADE_SILVER, "Silver"),
		MAKE_STRING(HOME_MENU_TROPHY_GRADE_GOLD, "Gold"),
		MAKE_STRING(HOME_MENU_TROPHY_GRADE_PLATINUM, "Platinum"),
		MAKE_STRING(AUDIO_MUTED, "Audio muted"),
		MAKE_STRING(AUDIO_UNMUTED, "Audio unmuted"),
		MAKE_STRING(PROGRESS_DIALOG_PROGRESS, "Progress:"),
		MAKE_STRING(PROGRESS_DIALOG_PROGRESS_ANALYZING, "Progress: analyzing..."),
		MAKE_STRING(PROGRESS_DIALOG_REMAINING, "remaining"),
		MAKE_STRING(PROGRESS_DIALOG_DONE, "done"),
		MAKE_STRING(PROGRESS_DIALOG_FILE, "file"),
		MAKE_STRING(PROGRESS_DIALOG_MODULE, "module"),
		MAKE_STRING(PROGRESS_DIALOG_OF, "of"),
		MAKE_STRING(PROGRESS_DIALOG_PLEASE_WAIT, "Please wait"),
		MAKE_STRING(PROGRESS_DIALOG_STOPPING_PLEASE_WAIT, "Stopping. Please wait..."),
		MAKE_STRING(PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT, "Creating savestate. Please wait..."),
		MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE, "Scanning PPU Executable..."),
		MAKE_STRING(PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE, "Analyzing PPU Executable..."),
		MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_MODULES, "Scanning PPU Modules..."),
		MAKE_STRING(PROGRESS_DIALOG_LOADING_PPU_MODULES, "Loading PPU Modules..."),
		MAKE_STRING(PROGRESS_DIALOG_COMPILING_PPU_MODULES, "Compiling PPU Modules..."),
		MAKE_STRING(PROGRESS_DIALOG_LINKING_PPU_MODULES, "Linking PPU Modules..."),
		MAKE_STRING(PROGRESS_DIALOG_APPLYING_PPU_CODE, "Applying PPU Code..."),
		MAKE_STRING(PROGRESS_DIALOG_BUILDING_SPU_CACHE, "Building SPU Cache..."),
		MAKE_STRING(EMULATION_PAUSED_RESUME_WITH_START, "Press and hold the START button to resume"),
		MAKE_STRING(EMULATION_RESUMING, "Resuming...!"),
		MAKE_STRING(EMULATION_FROZEN, "The PS3 application has likely crashed, you can close it."),
		MAKE_STRING(SAVESTATE_FAILED_DUE_TO_SAVEDATA, "SaveState failed: Game saving is in progress, wait until finished."),
		MAKE_STRING(SAVESTATE_FAILED_DUE_TO_VDEC, "SaveState failed: VDEC-base video/cutscenes are in order, wait for them to end or enable libvdec.sprx."),
		MAKE_STRING(SAVESTATE_FAILED_DUE_TO_MISSING_SPU_SETTING, "SaveState failed: Failed to lock SPU state, enabling SPU-Compatible mode may fix it."),
		MAKE_STRING(SAVESTATE_FAILED_DUE_TO_SPU, "SaveState failed: Failed to lock SPU state, using SPU ASMJIT will fix it."),
		MAKE_STRING(INVALID, "Invalid"),
};

static void setupCallbacks() {
  Emu.SetCallbacks({
      .call_from_main_thread =
          [](std::function<void()> cb, atomic_t<u32> *wake_up) {
            cb();
            if (wake_up) {
              *wake_up = true;
            }
          },
      .on_run = [](auto...) {},
      .on_pause = [](auto...) {},
      .on_resume = [](auto...) {},
      .on_stop = [](auto...) {},
      .on_ready = [](auto...) {},
      .on_missing_fw = [](auto...) {},
      .on_emulation_stop_no_response = [](auto...) {},
      .on_save_state_progress = [](auto...) {},
      .enable_disc_eject = [](auto...) {},
      .enable_disc_insert = [](auto...) {},
      .try_to_quit = [](auto...) { return true; },
      .handle_taskbar_progress = [](auto...) {},
      .init_kb_handler =
          [](auto...) {
            ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(
                Emu.DeserialManager()));
          },
      .init_mouse_handler =
          [](auto...) {
            ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(
                Emu.DeserialManager()));
          },
      .init_pad_handler =
          [](auto...) {
            ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, ""));
          },
      .update_emu_settings = [](auto...) {},
      .save_emu_settings =
          [](auto...) {
            Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
          },
      .close_gs_frame = [](auto...) {},
      .get_gs_frame =
          [] { return std::make_unique<GraphicsFrame>(g_native_window); },
      .get_camera_handler =
          [](auto...) { return std::make_shared<null_camera_handler>(); },
      .get_music_handler =
          [](auto...) { return std::make_shared<null_music_handler>(); },
      .init_gs_render =
          [](utils::serial *ar) {
            switch (g_cfg.video.renderer.get()) {
            case video_renderer::null:
              g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
              break;
            case video_renderer::vulkan:
              g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
              break;

            default:
              break;
            }
          },
      .get_audio =
          [](auto...) {
            std::shared_ptr<AudioBackend> result =
                std::make_shared<CubebBackend>();
            if (!result->Initialized()) {
              rpcs3_android.error(
                  "Audio renderer %s could not be initialized, using a Null "
                  "renderer instead. Make sure that no other application is "
                  "running that might block audio access (e.g. Netflix).",
                  result->GetName());
              result = std::make_shared<NullAudioBackend>();
            }
            return result;
          },
      .get_audio_enumerator = [](auto...) { return nullptr; },
      .get_msg_dialog = [](auto...) { return nullptr; },
      .get_osk_dialog = [](auto...) { return nullptr; },
      .get_save_dialog = [](auto...) { return nullptr; },
      .get_sendmessage_dialog = [](auto...) { return nullptr; },
      .get_recvmessage_dialog = [](auto...) { return nullptr; },
      .get_trophy_notification_dialog = [](auto...) { return nullptr; },
      .get_localized_string = [](localized_string_id id,
                                 const char *) -> std::string {
        if (int(id) < std::size(g_strings)) {
          return g_strings[int(id)].first;
        }
        return "";
      },
      .get_localized_u32string = [](localized_string_id id,
                                    const char *) -> std::u32string {
        if (int(id) < std::size(g_strings)) {
          return g_strings[int(id)].second;
        }
        return U"";
      },
      .get_localized_setting = [](auto...) { return ""; },
      .play_sound = [](auto...) {},
      .get_image_info = [](auto...) { return false; },
      .get_scaled_image = [](auto...) { return false; },
      .get_font_dirs = [](auto...) { return std::vector<std::string>(); },
      .on_install_pkgs = [](auto...) { return false; },
      .add_breakpoint = [](auto...) {},
      .display_sleep_control_supported = [](auto...) { return false; },
      .enable_display_sleep = [](auto...) {},
      .check_microphone_permissions = [](auto...) {},
  });
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_initialize(
    JNIEnv *env, jobject, jstring apkDir, jstring dataDir, jstring storageDir,
    jstring externalStorageDir, jstring rootDir) {
  auto apkDirStr = fix_dir_path(unwrap(env, apkDir));
  auto dataDirStr = fix_dir_path(unwrap(env, dataDir));
  auto storageDirStr = fix_dir_path(unwrap(env, storageDir));
  auto externalStorageDirStr = fix_dir_path(unwrap(env, externalStorageDir));
  auto rootDirStr = fix_dir_path(unwrap(env, rootDir));

  g_android_executable_dir = apkDirStr;
  g_android_config_dir = apkDirStr + "config/";
  g_android_cache_dir = apkDirStr + "cache/";

  if (!g_initialized) {
    g_initialized = true;
    logs::stored_message ver{rpcs3_android.always()};
    ver.text = fmt::format("RPCS3 v%s", rpcs3::get_verbose_version());

    // Write System information
    logs::stored_message sys{rpcs3_android.always()};
    sys.text = utils::get_system_info();

    // Write OS version
    logs::stored_message os{rpcs3_android.always()};
    os.text = utils::get_OS_version_string();

    // Write current time
    logs::stored_message time{rpcs3_android.always()};
    time.text =
        fmt::format("Current Time: %s", std::chrono::system_clock::now());

    logs::set_init(
        {std::move(ver), std::move(sys), std::move(os), std::move(time)});

    auto set_rlim = [](int resource, std::uint64_t limit) {
      rlimit64 rlim{};
      if (getrlimit64(resource, &rlim) != 0) {
        rpcs3_android.error("failed to get rlimit for %d", resource);
        return;
      }

      rlim.rlim_cur = std::min<std::size_t>(rlim.rlim_max, limit);
      rpcs3_android.error("rlimit[%d] = %u (requested %u, max %u)", resource,
                          rlim.rlim_cur, limit, rlim.rlim_max);

      if (setrlimit64(resource, &rlim) != 0) {
        rpcs3_android.error("failed to set rlimit for %d", resource);
        return;
      }
    };

    set_rlim(RLIMIT_MEMLOCK, 0x80000000);
    set_rlim(RLIMIT_NOFILE, 0x10000);
    set_rlim(RLIMIT_STACK, 128 * 1024 * 1024);

    setupCallbacks();
    Emu.SetHasGui(false);
    Emu.Init();

    g_cfg.video.resolution.set(video_resolution::_480p);
    g_cfg.video.renderer.set(video_renderer::vulkan);
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
    g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
    g_cfg.core.llvm_cpu.from_string(fallback_cpu_detection());
    Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
  }

  std::filesystem::create_directories(g_android_config_dir);
  std::error_code ec;
  // std::filesystem::remove_all(g_android_cache_dir, ec);
  std::filesystem::create_directories(g_android_cache_dir);

  Emu.Kill();
  return true;
}

extern "C" JNIEXPORT void JNICALL Java_net_rpcs3_RPCS3_shutdown(JNIEnv *env,
                                                                jobject) {
  Emu.GracefulShutdown(true, true, false);
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_boot(JNIEnv *env,
                                                                jobject,
                                                                jobject surface,
                                                                jstring path) {

  g_native_window = ANativeWindow_fromSurface(env, surface);
  if (g_native_window == nullptr) {
    rpcs3_android.fatal("returned native window is null, surface %p", surface);
    return false;
  }
  Emu.SetForceBoot(true);
  Emu.BootGame(unwrap(env, path), "", false, cfg_mode::global);
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_installFw(
    JNIEnv *env, jobject, jint fd, jobject eventHandler, jlong requestId) {
  auto eventHandlerClass = env->GetObjectClass(eventHandler);
  auto progressEventMethodId =
      env->GetMethodID(eventHandlerClass, "onProgressEvent", "(JJJ)Z");

  if (progressEventMethodId == nullptr) {
    rpcs3_android.fatal("installFw: failed to find onProgressEvent method");
    return false;
  }

  try {
    if (!env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                                jlong(0), jlong(0))) {
      return false;
    }
  } catch (...) {
    return false;
  }

  JavaVM *jvm;
  env->GetJavaVM(&jvm);

  eventHandler = env->NewGlobalRef(eventHandler);

  std::thread([jvm, eventHandler, fd, eventHandlerClass, progressEventMethodId,
               requestId]() mutable {
    JNIEnv *env;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = "Firmware Install Thread";
    args.group = nullptr;
    jvm->AttachCurrentThreadAsDaemon(&env, &args);

    AtExit atExit([=] {
      env->DeleteGlobalRef(eventHandler);
      jvm->DetachCurrentThread();
    });

    auto eventHandlerClass = env->GetObjectClass(eventHandler);
    auto progressEventMethodId =
        env->GetMethodID(eventHandlerClass, "onProgressEvent", "(JJJ)Z");

    auto pup_f = fs::file::from_native_handle(fd);

    if (!pup_f) {
      rpcs3_android.fatal("installFw: failed to open PUP");
      env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                             jlong(-1), jlong(0));
      return;
    }

    pup_object pup(std::move(pup_f));

    if (static_cast<pup_error>(pup) != pup_error::ok) {
      rpcs3_android.fatal("installFw: invalid PUP");
      env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                             jlong(-1), jlong(0));
      return;
    }

    fs::file update_files_f = pup.get_file(0x300);

    const usz update_files_size = update_files_f ? update_files_f.size() : 0;

    if (!update_files_size) {
      rpcs3_android.fatal("installFw: invalid PUP");
      env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                             jlong(-1), jlong(0));
      return;
    }

    tar_object update_files(update_files_f);

    auto update_filenames = update_files.get_filenames();
    update_filenames.erase(std::remove_if(update_filenames.begin(),
                                          update_filenames.end(),
                                          [](const std::string &s) {
                                            return !s.starts_with("dev_flash_");
                                          }),
                           update_filenames.end());

    if (update_filenames.empty()) {
      rpcs3_android.fatal("installFw: invalid PUP");
      env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                             jlong(-1), jlong(0));
      return;
    }

    std::string version_string;

    if (fs::file version = pup.get_file(0x100)) {
      version_string = version.to_string();
    }

    if (const usz version_pos = version_string.find('\n');
        version_pos != std::string::npos) {
      version_string.erase(version_pos);
    }

    if (version_string.empty()) {
      rpcs3_android.fatal("installFw: invalid PUP");
      env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                             jlong(-1), jlong(0));
      return;
    }

    jlong progress = 0;
    for (const auto &update_filename : update_filenames) {
      auto update_file_stream = update_files.get_file(update_filename);

      if (update_file_stream->m_file_handler) {
        // Forcefully read all the data
        update_file_stream->m_file_handler->handle_file_op(
            *update_file_stream, 0, update_file_stream->get_size(umax),
            nullptr);
      }

      fs::file update_file =
          fs::make_stream(std::move(update_file_stream->data));

      SCEDecrypter self_dec(update_file);
      self_dec.LoadHeaders();
      self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
      self_dec.DecryptData();

      auto dev_flash_tar_f = self_dec.MakeFile();

      if (dev_flash_tar_f.size() < 3) {
        rpcs3_android.error(
            "Firmware installation failed: Firmware could not be decompressed");

        env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                               jlong(-1), jlong(update_filenames.size()));
        return;
      }

      tar_object dev_flash_tar(dev_flash_tar_f[2]);

      if (!dev_flash_tar.extract()) {

        rpcs3_android.error("Error while installing firmware: TAR contents are "
                            "invalid. (package=%s)",
                            update_filename);

        env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                               jlong(-1), jlong(update_filenames.size()));
        return;
      }

      if (!env->CallBooleanMethod(eventHandler, progressEventMethodId,
                                  requestId, progress++,
                                  jlong(update_filenames.size()))) {
        // Installation was cancelled
        return;
      }
    }

    env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                           jlong(update_filenames.size()),
                           jlong(update_filenames.size()));
  }).detach();

  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_installPkgFile(
    JNIEnv *env, jobject, jint fd, jobject eventHandler, jlong requestId) {
  auto eventHandlerClass = env->GetObjectClass(eventHandler);
  auto progressEventMethodId =
      env->GetMethodID(eventHandlerClass, "onProgressEvent", "(JJJ)Z");

  if (progressEventMethodId == nullptr) {
    rpcs3_android.fatal("installPkg: failed to find onProgressEvent method");
    return false;
  }

  try {
    if (!env->CallBooleanMethod(eventHandler, progressEventMethodId, requestId,
                                jlong(0), jlong(0))) {
      return false;
    }
  } catch (...) {
    return false;
  }

  JavaVM *jvm;
  env->GetJavaVM(&jvm);

  eventHandler = env->NewGlobalRef(eventHandler);

  std::thread([jvm, eventHandler, fd, eventHandlerClass, progressEventMethodId,
               requestId]() mutable {
    JNIEnv *env;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = "Pkg Install Thread";
    args.group = nullptr;
    jvm->AttachCurrentThreadAsDaemon(&env, &args);

    struct AtExit {
      std::function<void()> cb;
      ~AtExit() { cb(); }
    };

    AtExit atExit([=] {
      env->DeleteGlobalRef(eventHandler);
      jvm->DetachCurrentThread();
    });

    auto eventHandlerClass = env->GetObjectClass(eventHandler);
    auto progressEventMethodId =
        env->GetMethodID(eventHandlerClass, "onProgressEvent", "(JJJ)Z");

    std::deque<package_reader> readers;
    std::deque<std::string> bootable_paths;
    readers.emplace_back("dummy.pkg", fs::file::from_native_handle(fd));

    package_install_result result = {};
    named_thread worker("PKG Installer", [&readers, &result, &bootable_paths] {
      result = package_reader::extract_data(readers, bootable_paths);
      return result.error == package_install_result::error_type::no_error;
    });

    const jlong maxProgress = 10000;

    for (std::size_t index = 0; auto &reader : readers) {
      while (true) {
        auto progress = reader.get_progress(maxProgress);

        progress += index * maxProgress;
        progress /= readers.size();

        if (!env->CallBooleanMethod(eventHandler, progressEventMethodId,
                                    requestId, jlong(progress),
                                    jlong(maxProgress))) {
          for (package_reader &reader : readers) {
            reader.abort_extract();
          }

          return;
        }

        if (progress == maxProgress) {
          break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
      }

      ++index;
    }
  }).detach();

  return true;
}
