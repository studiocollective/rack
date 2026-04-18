#ifndef RACK_VST3_H
#define RACK_VST3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Opaque types
typedef struct RackVST3Scanner RackVST3Scanner;
typedef struct RackVST3Plugin RackVST3Plugin;
typedef struct RackVST3Gui RackVST3Gui;

// Plugin type enum
typedef enum {
    RACK_VST3_TYPE_EFFECT = 0,
    RACK_VST3_TYPE_INSTRUMENT = 1,
    RACK_VST3_TYPE_ANALYZER = 2,
    RACK_VST3_TYPE_SPATIAL = 3,
    RACK_VST3_TYPE_OTHER = 4,
} RackVST3PluginType;

// Plugin info struct (passed to Rust)
typedef struct {
    char name[256];
    char manufacturer[256];
    char path[1024];
    char unique_id[64];  // VST3 uses UID (16 bytes as hex string)
    uint32_t version;
    RackVST3PluginType plugin_type;
    char category[128];  // VST3 subcategories (e.g., "Fx|Reverb")
} RackVST3PluginInfo;

// Error codes (0 = success, negative = error)
#define RACK_VST3_OK 0
#define RACK_VST3_ERROR_GENERIC -1
#define RACK_VST3_ERROR_NOT_FOUND -2
#define RACK_VST3_ERROR_INVALID_PARAM -3
#define RACK_VST3_ERROR_NOT_INITIALIZED -4
#define RACK_VST3_ERROR_LOAD_FAILED -5
#define RACK_VST3_ERROR_NOT_SUPPORTED -6  // Feature not supported by this plugin

// ============================================================================
// Scanner API
// ============================================================================

// Create a new scanner
// Returns NULL if allocation fails
RackVST3Scanner* rack_vst3_scanner_new(void);

// Free scanner
void rack_vst3_scanner_free(RackVST3Scanner* scanner);

// Add a search path for VST3 plugins
// Returns 0 on success, negative error code on failure
int rack_vst3_scanner_add_path(RackVST3Scanner* scanner, const char* path);

// Add system default VST3 search paths
// Returns 0 on success, negative error code on failure
int rack_vst3_scanner_add_default_paths(RackVST3Scanner* scanner);

// Scan for plugins
// Returns number of plugins found (or would be found), or negative error code
//
// Two-pass usage pattern (recommended):
//   1. count = rack_vst3_scanner_scan(scanner, NULL, 0);  // Get total count
//   2. rack_vst3_scanner_scan(scanner, array, count);     // Fill array
//
// If plugins is NULL: Only counts plugins, does not extract details
// If plugins is not NULL: Fills array up to max_plugins
//
// IMPORTANT: Return value may exceed max_plugins if more plugins exist.
//            Compare return value with max_plugins to detect truncation.
//
// plugins: output array (allocated by caller), or NULL to get count only
// max_plugins: size of output array (ignored if plugins is NULL)
int rack_vst3_scanner_scan(RackVST3Scanner* scanner, RackVST3PluginInfo* plugins, size_t max_plugins);

// Probe a single .vst3 bundle for metadata only (no plugin instantiation).
// Loads the module, reads the first audio effect class info, then unloads.
// Safe to call from any thread — does not register timers or callbacks.
// Returns 0 on success (info written to out_info), negative error code on failure.
int rack_vst3_probe_bundle(const char* bundle_path, RackVST3PluginInfo* out_info);

// ============================================================================
// Plugin Instance API
// ============================================================================

// Create a new plugin instance from path and UID
// path: path to .vst3 bundle/folder
// uid: plugin UID (from scan result)
// Returns plugin instance or NULL on error
RackVST3Plugin* rack_vst3_plugin_new(const char* path, const char* uid);

// Create a new plugin instance from bundle path alone (no UID needed)
// Loads the module and picks the first audio effect class.
// This is the fast path — no directory scanning.
// path: path to .vst3 bundle (e.g. "/Library/Audio/Plug-Ins/VST3/Vital.vst3")
// out_name: output buffer for the plugin name (optional, can be NULL)
// name_size: size of out_name buffer
// Returns plugin instance or NULL on error
RackVST3Plugin* rack_vst3_plugin_new_from_path(const char* path, char* out_name, size_t name_size);

// Free plugin instance
void rack_vst3_plugin_free(RackVST3Plugin* plugin);

// Check if plugin is an instrument (based on VST3 subcategories)
// Returns 1 if instrument, 0 if effect/other
int rack_vst3_plugin_is_instrument(RackVST3Plugin* plugin);

// Initialize plugin
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_initialize(RackVST3Plugin* plugin, double sample_rate, uint32_t max_block_size);

// Check if plugin is initialized
int rack_vst3_plugin_is_initialized(RackVST3Plugin* plugin);

// Reset plugin state
// Clears all internal buffers, delay lines, and state without changing parameters.
// Useful for clearing reverb tails, delay lines, etc. between songs or after preset changes.
//
// Returns:
//   0 (RACK_VST3_OK) on success
//   RACK_VST3_ERROR_NOT_INITIALIZED if plugin is not initialized
//   negative error code on failure
//
// Thread-safety: Should be called from a non-realtime thread.
int rack_vst3_plugin_reset(RackVST3Plugin* plugin);

// Get input channel count
// Returns number of input channels, or 0 if not initialized or query failed
// Thread-safety: Should be called after initialize()
int rack_vst3_plugin_get_input_channels(RackVST3Plugin* plugin);

// Get output channel count
// Returns number of output channels, or 0 if not initialized or query failed
// Thread-safety: Should be called after initialize()
int rack_vst3_plugin_get_output_channels(RackVST3Plugin* plugin);

// Process audio (planar format - one buffer per channel)
// Uses planar (non-interleaved) audio format matching VST3 internal format.
// This enables zero-copy processing in effect chains.
//
// inputs: array of input channel pointers (e.g., [left_ptr, right_ptr] for stereo)
// num_input_channels: number of input channels
// outputs: array of output channel pointers (e.g., [left_ptr, right_ptr] for stereo)
// num_output_channels: number of output channels
// frames: number of frames to process
//
// Channel Layout Examples:
//   Mono:   inputs = [mono_ptr], num_input_channels = 1
//   Stereo: inputs = [left_ptr, right_ptr], num_input_channels = 2
//   5.1:    inputs = [L, R, C, LFE, SL, SR], num_input_channels = 6
//
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_process(
    RackVST3Plugin* plugin,
    const float* const* inputs,
    uint32_t num_input_channels,
    float* const* outputs,
    uint32_t num_output_channels,
    uint32_t frames
);

// Get parameter count
// Thread-safety: Read-only after initialization. Safe to call from any thread.
int rack_vst3_plugin_parameter_count(RackVST3Plugin* plugin);

// Get parameter value (normalized 0.0 to 1.0)
// Returns 0 on success, negative error code on failure
// Thread-safety: Can be called from any thread, but the same plugin instance
// must not be accessed concurrently.
int rack_vst3_plugin_get_parameter(RackVST3Plugin* plugin, uint32_t index, float* value);

// Set parameter value (normalized 0.0 to 1.0)
// Returns 0 on success, negative error code on failure
// Thread-safety: Can be called from any thread, but the same plugin instance
// must not be accessed concurrently.
// Note: Calling during audio processing may cause clicks/pops.
int rack_vst3_plugin_set_parameter(RackVST3Plugin* plugin, uint32_t index, float value);

// Get parameter info
// name: output buffer for parameter name (allocated by caller)
// name_size: size of name buffer
// unit: output buffer for parameter unit string (allocated by caller, can be NULL)
// unit_size: size of unit buffer (ignored if unit is NULL)
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_parameter_info(
    RackVST3Plugin* plugin,
    uint32_t index,
    char* name,
    size_t name_size,
    float* min,
    float* max,
    float* default_value,
    char* unit,
    size_t unit_size
);

// ============================================================================
// Preset Management API
// ============================================================================

// Preset info struct
typedef struct {
    char name[256];
    int32_t preset_number;
} RackVST3PresetInfo;

// Get factory preset count
// Returns number of factory presets, or 0 if plugin has no presets
// Thread-safety: Read-only after initialization. Safe to call from any thread.
int rack_vst3_plugin_get_preset_count(RackVST3Plugin* plugin);

// Get preset info by index
// index: preset index (0 to preset_count - 1)
// name: output buffer for preset name (allocated by caller)
// name_size: size of name buffer
// preset_number: output parameter for preset number (used with load_preset)
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_get_preset_info(
    RackVST3Plugin* plugin,
    uint32_t index,
    char* name,
    size_t name_size,
    int32_t* preset_number
);

// Load a factory preset by preset number
// preset_number: the preset number from get_preset_info()
// Returns 0 on success, negative error code on failure
// Thread-safety: Should be called from the same thread that owns the plugin instance.
int rack_vst3_plugin_load_preset(RackVST3Plugin* plugin, int32_t preset_number);

// Get plugin state size (for allocation)
// Returns actual size in bytes needed to store state, or 0 if state cannot be retrieved
//
// NOTE: This function determines the actual state size by serializing the state.
//       It is not a constant or estimate - it queries the plugin's current state.
//       For large plugins (samplers, complex synths), this may take some time.
//       The returned size is accurate for immediate use with get_state().
//
// Thread-safety: Should be called from the same thread that owns the plugin instance.
int rack_vst3_plugin_get_state_size(RackVST3Plugin* plugin);

// Get plugin state (full state including parameters, preset, etc.)
// data: output buffer for state data (allocated by caller)
// size: input/output - buffer size on input, actual size on output
// Returns 0 on success, negative error code on failure
// Thread-safety: Should be called from the same thread that owns the plugin instance.
int rack_vst3_plugin_get_state(RackVST3Plugin* plugin, uint8_t* data, size_t* size);

// Get plugin state in a single call — serializes once and returns a heap-allocated buffer.
// *out_data is set to a malloc'd buffer (caller must free), *out_size to its length.
// Returns 0 on success, negative error code on failure.
// Thread-safety: Should be called from the same thread that owns the plugin instance.
int rack_vst3_plugin_get_state_alloc(RackVST3Plugin* plugin, uint8_t** out_data, size_t* out_size);

// Set plugin state (restore full state including parameters, preset, etc.)
// data: state data (from previous get_state call)
// size: size of state data in bytes
// Returns 0 on success, negative error code on failure
// Thread-safety: Should be called from the same thread that owns the plugin instance.
int rack_vst3_plugin_set_state(RackVST3Plugin* plugin, const uint8_t* data, size_t size);

// ============================================================================
// MIDI API
// ============================================================================

// MIDI event types (matches VST3 event types)
typedef enum {
    RACK_VST3_MIDI_NOTE_ON = 0x90,
    RACK_VST3_MIDI_NOTE_OFF = 0x80,
    RACK_VST3_MIDI_POLYPHONIC_AFTERTOUCH = 0xA0,
    RACK_VST3_MIDI_CONTROL_CHANGE = 0xB0,
    RACK_VST3_MIDI_PROGRAM_CHANGE = 0xC0,
    RACK_VST3_MIDI_CHANNEL_AFTERTOUCH = 0xD0,
    RACK_VST3_MIDI_PITCH_BEND = 0xE0,
} RackVST3MidiEventType;

// MIDI event struct
typedef struct {
    uint32_t sample_offset;  // Sample offset within buffer
    uint8_t status;          // MIDI status byte
    uint8_t data1;           // First data byte (note/CC number)
    uint8_t data2;           // Second data byte (velocity/value)
    uint8_t channel;         // MIDI channel (0-15)
} RackVST3MidiEvent;

// Send MIDI events to plugin
// events: array of MIDI events
// event_count: number of events in array
//
// NOTE: VST3 has native support for Note On/Off, Polyphonic Aftertouch, and Control Change.
//       Program Change, Channel Aftertouch, and Pitch Bend use custom encoding via
//       LegacyMIDICCOutEvent with controlNumber >= 0x80. Not all VST3 plugins support
//       these non-native event types. If a plugin doesn't respond to Program Change,
//       Channel Aftertouch, or Pitch Bend, it's a limitation of the plugin itself.
//
// Returns 0 on success, negative error code on failure
// Thread-safety: Should be called from the same thread that owns the plugin instance.
// Not safe to call concurrently with process() or other plugin operations.
int rack_vst3_plugin_send_midi(
    RackVST3Plugin* plugin,
    const RackVST3MidiEvent* events,
    uint32_t event_count
);

// ============================================================================
// GUI API
// ============================================================================

// Check if the plugin has an editor view
// Returns 1 if editor is available, 0 if not
int rack_vst3_plugin_has_editor(RackVST3Plugin* plugin);

// Check if the plugin's editor supports resizing
// Returns 1 if resizable, 0 if not
int rack_vst3_plugin_can_resize(RackVST3Plugin* plugin);

// Get the editor view size (in logical points)
// width/height are output parameters
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_get_editor_size(RackVST3Plugin* plugin, int32_t* width, int32_t* height);

// Open the editor view and attach to the given NSView parent (macOS)
// parent: an NSView* to attach the plugin editor to
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_open_editor(RackVST3Plugin* plugin, void* parent);

// Set a callback for when the plugin requests a resize
// The callback receives the new width and height
void rack_vst3_plugin_set_resize_callback(
    RackVST3Plugin* plugin,
    void (*callback)(void* context, int32_t width, int32_t height),
    void* context
);

// Notify the plugin that the host window has been resized
// Call this when the NSWindow content view changes size
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_notify_size(RackVST3Plugin* plugin, int32_t width, int32_t height);

// Close the editor view
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_close_editor(RackVST3Plugin* plugin);

// ============================================================================
// ARA (Audio Random Access) API
// ============================================================================

// Check if plugin supports ARA (Melodyne, etc.)
// Returns 1 if ARA-capable, 0 if not
int rack_vst3_plugin_has_ara(RackVST3Plugin* plugin);

// Get ARA factory from plugin (opaque pointer)
// Returns pointer to ARAFactory, or NULL if not ARA-capable
const void* rack_vst3_plugin_get_ara_factory(RackVST3Plugin* plugin);

// Get ARA factory info (plugin name, manufacturer, supported API generation)
// Returns 0 on success, negative error code on failure
int rack_vst3_plugin_get_ara_factory_info(
    RackVST3Plugin* plugin,
    char* plugin_name, size_t name_size,
    char* manufacturer, size_t mfr_size,
    int32_t* highest_supported_api
);

// Initialize ARA on the factory (call once before creating document controllers)
// Returns 0 on success, negative error code on failure
int rack_vst3_ara_init(RackVST3Plugin* plugin);

// Shutdown ARA on the factory
void rack_vst3_ara_uninit(RackVST3Plugin* plugin);

// Host callback function pointer types (Rust implements these)
typedef void* (*RackAraCreateAudioReaderFn)(void* ctx, void* source_host_ref, int use_64bit);
typedef int (*RackAraReadAudioSamplesFn)(void* ctx, void* reader_ref, int64_t pos, int64_t count, void** buffers);
typedef void (*RackAraDestroyAudioReaderFn)(void* ctx, void* reader_ref);
typedef size_t (*RackAraGetArchiveSizeFn)(void* ctx, void* archive_ref);
typedef int (*RackAraReadArchiveFn)(void* ctx, void* archive_ref, size_t pos, size_t len, uint8_t* buf);
typedef int (*RackAraWriteArchiveFn)(void* ctx, void* archive_ref, size_t pos, size_t len, const uint8_t* buf);

// Host callbacks struct (Rust fills this in)
typedef struct {
    void* context;  // Rust-side AraHost pointer
    RackAraCreateAudioReaderFn create_audio_reader;
    RackAraReadAudioSamplesFn read_audio_samples;
    RackAraDestroyAudioReaderFn destroy_audio_reader;
    RackAraGetArchiveSizeFn get_archive_size;
    RackAraReadArchiveFn read_archive;
    RackAraWriteArchiveFn write_archive;
} RackAraHostCallbacks;

// Create ARA document controller with host callbacks
// Returns opaque document controller handle, or NULL on failure
void* rack_vst3_ara_create_document_controller(
    RackVST3Plugin* plugin,
    const RackAraHostCallbacks* callbacks,
    const char* document_name
);

// Destroy document controller
void rack_vst3_ara_destroy_document_controller(void* controller);

// Bind plugin instance to document controller with roles
// roles: bitmask (1=PlaybackRenderer, 2=EditorRenderer, 4=EditorView)
// Returns 0 on success, negative error code on failure
int rack_vst3_ara_bind_to_document(
    RackVST3Plugin* plugin,
    void* controller,
    uint32_t roles
);

// === Document Model Operations ===

// Create audio source (tells ARA about an audio file)
void* rack_vst3_ara_create_audio_source(
    void* controller,
    void* host_ref,
    const char* name,
    const char* persistent_id,
    int64_t sample_count,
    double sample_rate,
    int32_t channel_count
);

// Enable/disable sample access on audio source
int rack_vst3_ara_enable_audio_source_access(void* controller, void* source, int enable);

// Create audio modification (an edit of an audio source)
void* rack_vst3_ara_create_audio_modification(
    void* controller,
    void* audio_source,
    void* host_ref,
    const char* name,
    const char* persistent_id
);

// Create playback region (a time range that plays a modification)
void* rack_vst3_ara_create_playback_region(
    void* controller,
    void* audio_modification,
    void* host_ref,
    double start_in_mod,
    double duration_in_mod,
    double start_in_playback,
    double duration_in_playback
);

// Destroy model objects
void rack_vst3_ara_destroy_playback_region(void* controller, void* region);
void rack_vst3_ara_destroy_audio_modification(void* controller, void* modification);
void rack_vst3_ara_destroy_audio_source(void* controller, void* source);

// Begin/end editing (bracket model changes)
void rack_vst3_ara_begin_editing(void* controller);
void rack_vst3_ara_end_editing(void* controller);

// Notify model updates (call after changes)
void rack_vst3_ara_notify_model_updates(void* controller);

#ifdef __cplusplus
}
#endif

#endif // RACK_VST3_H
