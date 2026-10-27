#ifndef OTIO_SHIM_H
#define OTIO_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct OtioTimeline OtioTimeline;
typedef struct OtioTrack OtioTrack;
typedef struct OtioClip OtioClip;
typedef struct OtioGap OtioGap;
typedef struct OtioExternalRef OtioExternalRef;
typedef struct OtioStack OtioStack;
typedef struct OtioMarker OtioMarker;
typedef struct OtioEffect OtioEffect;
typedef struct OtioTransition OtioTransition;

// Error handling
typedef struct {
    int code;
    char message[256];
} OtioError;

// RationalTime (passed by value)
typedef struct {
    double value;
    double rate;
} OtioRationalTime;

typedef struct {
    OtioRationalTime start_time;
    OtioRationalTime duration;
} OtioTimeRange;

// Timeline
OtioTimeline* otio_timeline_create(const char* name);
void otio_timeline_free(OtioTimeline* tl);
int otio_timeline_set_global_start_time(OtioTimeline* tl, OtioRationalTime time, OtioError* err);

// Tracks (0 = video, 1 = audio)
OtioTrack* otio_timeline_add_video_track(OtioTimeline* tl, const char* name);
OtioTrack* otio_timeline_add_audio_track(OtioTimeline* tl, const char* name);

// Standalone track creation (for use with Stack)
OtioTrack* otio_track_create_video(const char* name);
OtioTrack* otio_track_create_audio(const char* name);
void otio_track_free(OtioTrack* track);

// Clips
OtioClip* otio_clip_create(const char* name, OtioTimeRange source_range);
void otio_clip_free(OtioClip* clip);
int otio_clip_set_media_reference(OtioClip* clip, OtioExternalRef* ref, OtioError* err);
int otio_track_append_clip(OtioTrack* track, OtioClip* clip, OtioError* err);

// Gaps
OtioGap* otio_gap_create(OtioRationalTime duration);
void otio_gap_free(OtioGap* gap);
int otio_track_append_gap(OtioTrack* track, OtioGap* gap, OtioError* err);

// Media references
OtioExternalRef* otio_external_ref_create(const char* target_url);
int otio_external_ref_set_available_range(OtioExternalRef* ref, OtioTimeRange range, OtioError* err);
void otio_external_ref_free(OtioExternalRef* ref);

// Serialization (file-based)
int otio_timeline_write_to_file(OtioTimeline* tl, const char* path, OtioError* err);
OtioTimeline* otio_timeline_read_from_file(const char* path, OtioError* err);

// Serialization (string-based) - caller must free returned string with otio_free_string
char* otio_timeline_to_json_string(OtioTimeline* tl, OtioError* err);
OtioTimeline* otio_timeline_from_json_string(const char* json, OtioError* err);

// Serialization with schema version targeting
// schema_names and schema_versions are parallel arrays of length count
// Pass count=0 for no downgrading (equivalent to functions above)
int otio_timeline_write_to_file_with_schema_versions(
    OtioTimeline* tl,
    const char* path,
    const char** schema_names,
    const int64_t* schema_versions,
    int32_t count,
    OtioError* err
);
char* otio_timeline_to_json_string_with_schema_versions(
    OtioTimeline* tl,
    const char** schema_names,
    const int64_t* schema_versions,
    int32_t count,
    OtioError* err
);

// Metadata (string key-value pairs)
// Getter returns malloc'd string - caller must free with otio_free_string
// Returns NULL if key not found

// String memory management
void otio_free_string(char* str);

// Timeline metadata
void otio_timeline_set_metadata_string(OtioTimeline* tl, const char* key, const char* value);
char* otio_timeline_get_metadata_string(OtioTimeline* tl, const char* key);

// Track metadata
void otio_track_set_metadata_string(OtioTrack* track, const char* key, const char* value);
char* otio_track_get_metadata_string(OtioTrack* track, const char* key);

// Clip metadata
void otio_clip_set_metadata_string(OtioClip* clip, const char* key, const char* value);
char* otio_clip_get_metadata_string(OtioClip* clip, const char* key);

// Gap metadata
void otio_gap_set_metadata_string(OtioGap* gap, const char* key, const char* value);
char* otio_gap_get_metadata_string(OtioGap* gap, const char* key);

// Stack metadata
void otio_stack_set_metadata_string(OtioStack* stack, const char* key, const char* value);
char* otio_stack_get_metadata_string(OtioStack* stack, const char* key);

// ExternalReference metadata
void otio_external_ref_set_metadata_string(OtioExternalRef* ref, const char* key, const char* value);
char* otio_external_ref_get_metadata_string(OtioExternalRef* ref, const char* key);

// Stack (composition for nested structures)
OtioStack* otio_stack_create(const char* name);
void otio_stack_free(OtioStack* stack);
int otio_stack_append_track(OtioStack* stack, OtioTrack* track, OtioError* err);
int otio_stack_append_clip(OtioStack* stack, OtioClip* clip, OtioError* err);
int otio_stack_append_gap(OtioStack* stack, OtioGap* gap, OtioError* err);
int otio_stack_append_stack(OtioStack* stack, OtioStack* child, OtioError* err);

// Timeline stack accessor
OtioStack* otio_timeline_get_tracks(OtioTimeline* tl);

// Track can also contain stacks (for versioning/alternatives)
int otio_track_append_stack(OtioTrack* track, OtioStack* stack, OtioError* err);

// Child type enumeration (returned by child_type functions)
// 0 = Clip, 1 = Gap, 2 = Stack, 3 = Track, -1 = Unknown/Error
#define OTIO_CHILD_TYPE_CLIP  0
#define OTIO_CHILD_TYPE_GAP   1
#define OTIO_CHILD_TYPE_STACK 2
#define OTIO_CHILD_TYPE_TRACK 3

// Track iteration
int32_t otio_track_children_count(OtioTrack* track);
int32_t otio_track_child_type(OtioTrack* track, int32_t index);
void* otio_track_child_at(OtioTrack* track, int32_t index);

// Stack iteration
int32_t otio_stack_children_count(OtioStack* stack);
int32_t otio_stack_child_type(OtioStack* stack, int32_t index);
void* otio_stack_child_at(OtioStack* stack, int32_t index);

// Name accessors (returns malloc'd string - caller must free with otio_free_string)
char* otio_clip_get_name(OtioClip* clip);
char* otio_gap_get_name(OtioGap* gap);
char* otio_track_get_name(OtioTrack* track);
char* otio_stack_get_name(OtioStack* stack);

// Source range accessor
OtioTimeRange otio_clip_get_source_range(OtioClip* clip);

// Available range (from media reference)
OtioTimeRange otio_clip_available_range(OtioClip* clip, OtioError* err);

// ============================================================================
// Clip Multi-Reference Support
// ============================================================================

// String iterator for media reference keys
typedef struct OtioStringIterator OtioStringIterator;

// Get all media reference keys from a clip
OtioStringIterator* otio_clip_media_reference_keys(OtioClip* clip);
int32_t otio_string_iterator_count(OtioStringIterator* iter);
char* otio_string_iterator_next(OtioStringIterator* iter);  // caller must free with otio_free_string
void otio_string_iterator_reset(OtioStringIterator* iter);
void otio_string_iterator_free(OtioStringIterator* iter);

// Get/set active media reference key
char* otio_clip_active_media_reference_key(OtioClip* clip);  // caller must free with otio_free_string
int otio_clip_set_active_media_reference_key(OtioClip* clip, const char* key, OtioError* err);

// Add media reference with key
int otio_clip_add_media_reference(OtioClip* clip, const char* key,
                                   void* ref, int32_t ref_type, OtioError* err);

// Check if clip has a media reference for the given key
int otio_clip_has_media_reference(OtioClip* clip, const char* key);

// Media reference type constants for multi-reference API
#define OTIO_REF_TYPE_EXTERNAL          0
#define OTIO_REF_TYPE_MISSING           1
#define OTIO_REF_TYPE_GENERATOR         2
#define OTIO_REF_TYPE_IMAGE_SEQUENCE    3

// Track modification operations
int otio_track_remove_child(OtioTrack* track, int32_t index, OtioError* err);
int otio_track_insert_clip(OtioTrack* track, int32_t index, OtioClip* clip, OtioError* err);
int otio_track_insert_gap(OtioTrack* track, int32_t index, OtioGap* gap, OtioError* err);
int otio_track_insert_stack(OtioTrack* track, int32_t index, OtioStack* stack, OtioError* err);
int otio_track_clear_children(OtioTrack* track, OtioError* err);

// NeighborGapPolicy constants
#define OTIO_NEIGHBOR_GAP_NEVER              0
#define OTIO_NEIGHBOR_GAP_AROUND_TRANSITIONS 1

// Neighbor result struct
typedef struct {
    void* left;
    int32_t left_type;
    void* right;
    int32_t right_type;
} OtioNeighbors;

// Get neighbors of a child at the given index
OtioNeighbors otio_track_neighbors_of(OtioTrack* track, int32_t child_index,
                                       int32_t gap_policy, OtioError* err);

// Stack modification operations
int otio_stack_remove_child(OtioStack* stack, int32_t index, OtioError* err);
int otio_stack_insert_track(OtioStack* stack, int32_t index, OtioTrack* track, OtioError* err);
int otio_stack_insert_clip(OtioStack* stack, int32_t index, OtioClip* clip, OtioError* err);
int otio_stack_insert_gap(OtioStack* stack, int32_t index, OtioGap* gap, OtioError* err);
int otio_stack_insert_stack(OtioStack* stack, int32_t index, OtioStack* child, OtioError* err);
int otio_stack_clear_children(OtioStack* stack, OtioError* err);

// ----------------------------------------------------------------------------
// Marker
// ----------------------------------------------------------------------------

OtioMarker* otio_marker_create(const char* name, OtioTimeRange marked_range, const char* color);
void otio_marker_free(OtioMarker* marker);
char* otio_marker_get_name(OtioMarker* marker);
char* otio_marker_get_color(OtioMarker* marker);
void otio_marker_set_color(OtioMarker* marker, const char* color);
OtioTimeRange otio_marker_get_marked_range(OtioMarker* marker);
int otio_marker_set_marked_range(OtioMarker* marker, OtioTimeRange range, OtioError* err);
char* otio_marker_get_comment(OtioMarker* marker);
void otio_marker_set_comment(OtioMarker* marker, const char* comment);
void otio_marker_set_metadata_string(OtioMarker* marker, const char* key, const char* value);
char* otio_marker_get_metadata_string(OtioMarker* marker, const char* key);

// ----------------------------------------------------------------------------
// Effect
// ----------------------------------------------------------------------------

OtioEffect* otio_effect_create(const char* name, const char* effect_name);
void otio_effect_free(OtioEffect* effect);
char* otio_effect_get_name(OtioEffect* effect);
char* otio_effect_get_effect_name(OtioEffect* effect);
void otio_effect_set_effect_name(OtioEffect* effect, const char* effect_name);
void otio_effect_set_metadata_string(OtioEffect* effect, const char* key, const char* value);
char* otio_effect_get_metadata_string(OtioEffect* effect, const char* key);

// ----------------------------------------------------------------------------
// Transition
// ----------------------------------------------------------------------------

OtioTransition* otio_transition_create(const char* name, const char* transition_type,
                                       OtioRationalTime in_offset, OtioRationalTime out_offset);
void otio_transition_free(OtioTransition* transition);
char* otio_transition_get_name(OtioTransition* transition);
char* otio_transition_get_transition_type(OtioTransition* transition);
void otio_transition_set_transition_type(OtioTransition* transition, const char* transition_type);
OtioRationalTime otio_transition_get_in_offset(OtioTransition* transition);
void otio_transition_set_in_offset(OtioTransition* transition, OtioRationalTime offset);
OtioRationalTime otio_transition_get_out_offset(OtioTransition* transition);
void otio_transition_set_out_offset(OtioTransition* transition, OtioRationalTime offset);
OtioRationalTime otio_transition_get_duration(OtioTransition* transition);
void otio_transition_set_metadata_string(OtioTransition* transition, const char* key, const char* value);
char* otio_transition_get_metadata_string(OtioTransition* transition, const char* key);

// Track can also contain transitions
int otio_track_append_transition(OtioTrack* track, OtioTransition* transition, OtioError* err);
int otio_track_insert_transition(OtioTrack* track, int32_t index, OtioTransition* transition, OtioError* err);

// Child type for transitions
#define OTIO_CHILD_TYPE_TRANSITION 4

// ----------------------------------------------------------------------------
// MissingReference
// ----------------------------------------------------------------------------

typedef struct OtioMissingRef OtioMissingRef;

OtioMissingRef* otio_missing_ref_create(void);
void otio_missing_ref_free(OtioMissingRef* ref);
void otio_missing_ref_set_metadata_string(OtioMissingRef* ref, const char* key, const char* value);
char* otio_missing_ref_get_metadata_string(OtioMissingRef* ref, const char* key);

// ----------------------------------------------------------------------------
// ImageSequenceReference
// ----------------------------------------------------------------------------

typedef struct OtioImageSeqRef OtioImageSeqRef;

// Missing frame policy constants
#define OTIO_MISSING_FRAME_ERROR 0
#define OTIO_MISSING_FRAME_HOLD  1
#define OTIO_MISSING_FRAME_BLACK 2

// Constructor/destructor
OtioImageSeqRef* otio_image_seq_ref_create(const char* target_url_base,
    const char* name_prefix, const char* name_suffix,
    int32_t start_frame, int32_t frame_step, double rate, int32_t frame_zero_padding);
void otio_image_seq_ref_free(OtioImageSeqRef* ref);

// Getters (strings - caller must free with otio_free_string)
char* otio_image_seq_ref_get_target_url_base(OtioImageSeqRef* ref);
char* otio_image_seq_ref_get_name_prefix(OtioImageSeqRef* ref);
char* otio_image_seq_ref_get_name_suffix(OtioImageSeqRef* ref);

// Getters (integers/doubles)
int32_t otio_image_seq_ref_get_start_frame(OtioImageSeqRef* ref);
int32_t otio_image_seq_ref_get_end_frame(OtioImageSeqRef* ref);
int32_t otio_image_seq_ref_get_frame_step(OtioImageSeqRef* ref);
double otio_image_seq_ref_get_rate(OtioImageSeqRef* ref);
int32_t otio_image_seq_ref_get_frame_zero_padding(OtioImageSeqRef* ref);
int32_t otio_image_seq_ref_get_missing_frame_policy(OtioImageSeqRef* ref);

// Setters
void otio_image_seq_ref_set_target_url_base(OtioImageSeqRef* ref, const char* url_base);
void otio_image_seq_ref_set_name_prefix(OtioImageSeqRef* ref, const char* prefix);
void otio_image_seq_ref_set_name_suffix(OtioImageSeqRef* ref, const char* suffix);
void otio_image_seq_ref_set_start_frame(OtioImageSeqRef* ref, int32_t frame);
void otio_image_seq_ref_set_frame_step(OtioImageSeqRef* ref, int32_t step);
void otio_image_seq_ref_set_rate(OtioImageSeqRef* ref, double rate);
void otio_image_seq_ref_set_frame_zero_padding(OtioImageSeqRef* ref, int32_t padding);
void otio_image_seq_ref_set_missing_frame_policy(OtioImageSeqRef* ref, int32_t policy);

// Computed properties
int32_t otio_image_seq_ref_number_of_images(OtioImageSeqRef* ref);
int32_t otio_image_seq_ref_frame_for_time(OtioImageSeqRef* ref, OtioRationalTime time, OtioError* err);
char* otio_image_seq_ref_target_url_for_image_number(OtioImageSeqRef* ref, int32_t image_number, OtioError* err);

// Available range
int otio_image_seq_ref_set_available_range(OtioImageSeqRef* ref, OtioTimeRange range, OtioError* err);
OtioTimeRange otio_image_seq_ref_get_available_range(OtioImageSeqRef* ref);

// Clip integration
int otio_clip_set_image_sequence_reference(OtioClip* clip, OtioImageSeqRef* ref, OtioError* err);

// Metadata
void otio_image_seq_ref_set_metadata_string(OtioImageSeqRef* ref, const char* key, const char* value);
char* otio_image_seq_ref_get_metadata_string(OtioImageSeqRef* ref, const char* key);

// ----------------------------------------------------------------------------
// GeneratorReference
// ----------------------------------------------------------------------------

typedef struct OtioGeneratorRef OtioGeneratorRef;

OtioGeneratorRef* otio_generator_ref_create(const char* name, const char* generator_kind);
void otio_generator_ref_free(OtioGeneratorRef* ref);
char* otio_generator_ref_get_name(OtioGeneratorRef* ref);
char* otio_generator_ref_get_generator_kind(OtioGeneratorRef* ref);
void otio_generator_ref_set_generator_kind(OtioGeneratorRef* ref, const char* kind);
int otio_generator_ref_set_available_range(OtioGeneratorRef* ref, OtioTimeRange range, OtioError* err);
OtioTimeRange otio_generator_ref_get_available_range(OtioGeneratorRef* ref);
void otio_generator_ref_set_metadata_string(OtioGeneratorRef* ref, const char* key, const char* value);
char* otio_generator_ref_get_metadata_string(OtioGeneratorRef* ref, const char* key);

// ----------------------------------------------------------------------------
// LinearTimeWarp (TimeEffect)
// ----------------------------------------------------------------------------

typedef struct OtioLinearTimeWarp OtioLinearTimeWarp;

OtioLinearTimeWarp* otio_linear_time_warp_create(const char* name, double time_scalar);
void otio_linear_time_warp_free(OtioLinearTimeWarp* effect);
char* otio_linear_time_warp_get_name(OtioLinearTimeWarp* effect);
double otio_linear_time_warp_get_time_scalar(OtioLinearTimeWarp* effect);
void otio_linear_time_warp_set_time_scalar(OtioLinearTimeWarp* effect, double scalar);
void otio_linear_time_warp_set_metadata_string(OtioLinearTimeWarp* effect, const char* key, const char* value);
char* otio_linear_time_warp_get_metadata_string(OtioLinearTimeWarp* effect, const char* key);

// ----------------------------------------------------------------------------
// FreezeFrame (TimeEffect with time_scalar = 0)
// ----------------------------------------------------------------------------

typedef struct OtioFreezeFrame OtioFreezeFrame;

OtioFreezeFrame* otio_freeze_frame_create(const char* name);
void otio_freeze_frame_free(OtioFreezeFrame* effect);
char* otio_freeze_frame_get_name(OtioFreezeFrame* effect);
void otio_freeze_frame_set_metadata_string(OtioFreezeFrame* effect, const char* key, const char* value);
char* otio_freeze_frame_get_metadata_string(OtioFreezeFrame* effect, const char* key);

// ----------------------------------------------------------------------------
// Clip Marker/Effect attachment
// ----------------------------------------------------------------------------

int otio_clip_add_marker(OtioClip* clip, OtioMarker* marker, OtioError* err);
int32_t otio_clip_markers_count(OtioClip* clip);
OtioMarker* otio_clip_marker_at(OtioClip* clip, int32_t index);

int otio_clip_add_effect(OtioClip* clip, OtioEffect* effect, OtioError* err);
int32_t otio_clip_effects_count(OtioClip* clip);
OtioEffect* otio_clip_effect_at(OtioClip* clip, int32_t index);

// Also support LinearTimeWarp as effect
int otio_clip_add_linear_time_warp(OtioClip* clip, OtioLinearTimeWarp* effect, OtioError* err);

// Set media reference variants
int otio_clip_set_missing_reference(OtioClip* clip, OtioMissingRef* ref, OtioError* err);
int otio_clip_set_generator_reference(OtioClip* clip, OtioGeneratorRef* ref, OtioError* err);

// ----------------------------------------------------------------------------
// Track Marker attachment
// ----------------------------------------------------------------------------

int otio_track_add_marker(OtioTrack* track, OtioMarker* marker, OtioError* err);
int32_t otio_track_markers_count(OtioTrack* track);
OtioMarker* otio_track_marker_at(OtioTrack* track, int32_t index);

// ----------------------------------------------------------------------------
// Track kind
// ----------------------------------------------------------------------------

#define OTIO_TRACK_KIND_VIDEO 0
#define OTIO_TRACK_KIND_AUDIO 1

int32_t otio_track_get_kind(OtioTrack* track);
void otio_track_set_kind(OtioTrack* track, int32_t kind);

// ----------------------------------------------------------------------------
// Time transforms
// ----------------------------------------------------------------------------

// Get the range of a child within its parent track/stack
OtioTimeRange otio_track_range_of_child_at_index(OtioTrack* track, int32_t index, OtioError* err);
OtioTimeRange otio_stack_range_of_child_at_index(OtioStack* stack, int32_t index, OtioError* err);

// Get the trimmed range of a track (computed from children)
OtioTimeRange otio_track_trimmed_range(OtioTrack* track, OtioError* err);
OtioTimeRange otio_stack_trimmed_range(OtioStack* stack, OtioError* err);

// ----------------------------------------------------------------------------
// ExternalReference additional accessors
// ----------------------------------------------------------------------------

char* otio_external_ref_get_target_url(OtioExternalRef* ref);
OtioTimeRange otio_external_ref_get_available_range(OtioExternalRef* ref);
char* otio_external_ref_get_name(OtioExternalRef* ref);
void otio_external_ref_set_name(OtioExternalRef* ref, const char* name);

// ----------------------------------------------------------------------------
// Timeline additional accessors
// ----------------------------------------------------------------------------

char* otio_timeline_get_name(OtioTimeline* tl);
OtioRationalTime otio_timeline_get_global_start_time(OtioTimeline* tl);
OtioTimeRange otio_timeline_get_duration(OtioTimeline* tl, OtioError* err);

// ----------------------------------------------------------------------------
// Edit Algorithms
// ----------------------------------------------------------------------------

// Overwrite: Replace content in track at specified range
// Returns 0 on success, -1 on error
int otio_track_overwrite(OtioTrack* track, OtioClip* clip,
    OtioTimeRange range, int remove_transitions, OtioError* err);

// Insert: Insert item at specific time, shifting subsequent items
int otio_track_insert_at_time(OtioTrack* track, OtioClip* clip,
    OtioRationalTime time, int remove_transitions, OtioError* err);

// Slice: Split composition at time point
int otio_track_slice_at_time(OtioTrack* track, OtioRationalTime time,
    int remove_transitions, OtioError* err);

// Slip: Move media content within item without changing duration/position
int otio_clip_slip(OtioClip* clip, OtioRationalTime delta, OtioError* err);

// Slide: Move item position, adjusting previous item
int otio_clip_slide(OtioClip* clip, OtioRationalTime delta, OtioError* err);

// Trim: Adjust item's in/out points with optional fill
int otio_clip_trim(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err);

// Ripple: Adjust item duration, affecting overall composition length
int otio_clip_ripple(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err);

// Roll: Adjust edit point between adjacent items
int otio_clip_roll(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err);

// Remove: Remove item at time, optionally filling with gap
int otio_track_remove_at_time(OtioTrack* track, OtioRationalTime time,
    int fill_with_gap, OtioError* err);

// ----------------------------------------------------------------------------
// Time coordinate transforms
// ----------------------------------------------------------------------------

// Transform a time from one item's coordinate space to another
// item_type and to_item_type use OTIO_CHILD_TYPE_* constants
OtioRationalTime otio_item_transformed_time(void* item, int32_t item_type,
    OtioRationalTime time, void* to_item, int32_t to_item_type, OtioError* err);

// Transform a time range from one item's coordinate space to another
OtioTimeRange otio_item_transformed_time_range(void* item, int32_t item_type,
    OtioTimeRange range, void* to_item, int32_t to_item_type, OtioError* err);

// Get the range of an item within its parent
OtioTimeRange otio_clip_range_in_parent(OtioClip* clip, OtioError* err);
OtioTimeRange otio_gap_range_in_parent(OtioGap* gap, OtioError* err);

// ----------------------------------------------------------------------------
// Parent navigation
// ----------------------------------------------------------------------------

// Parent type constants
#define OTIO_PARENT_TYPE_NONE     0
#define OTIO_PARENT_TYPE_TRACK    1
#define OTIO_PARENT_TYPE_STACK    2
#define OTIO_PARENT_TYPE_TIMELINE 3

// Get parent type and pointer for clips, gaps, transitions
int32_t otio_clip_get_parent_type(OtioClip* clip);
void* otio_clip_get_parent(OtioClip* clip);

int32_t otio_gap_get_parent_type(OtioGap* gap);
void* otio_gap_get_parent(OtioGap* gap);

int32_t otio_transition_get_parent_type(OtioTransition* transition);
void* otio_transition_get_parent(OtioTransition* transition);

// Get parent for tracks (returns Stack or Timeline via tracks stack)
int32_t otio_track_get_parent_type(OtioTrack* track);
void* otio_track_get_parent(OtioTrack* track);

// Get parent for stacks
int32_t otio_stack_get_parent_type(OtioStack* stack);
void* otio_stack_get_parent(OtioStack* stack);

// ----------------------------------------------------------------------------
// Search algorithms - find_clips
// ----------------------------------------------------------------------------

// Opaque iterator for clip search results
// ----------------------------------------------------------------------------
// Track Iterator (for filtered track lists)
// ----------------------------------------------------------------------------

typedef struct OtioTrackIterator OtioTrackIterator;

// Get filtered tracks from timeline
OtioTrackIterator* otio_timeline_video_tracks(OtioTimeline* tl);
OtioTrackIterator* otio_timeline_audio_tracks(OtioTimeline* tl);

// Iterator operations
int32_t otio_track_iterator_count(OtioTrackIterator* iter);
OtioTrack* otio_track_iterator_next(OtioTrackIterator* iter);
void otio_track_iterator_reset(OtioTrackIterator* iter);
void otio_track_iterator_free(OtioTrackIterator* iter);

// ----------------------------------------------------------------------------
// Clip Iterator (for clip search)
// ----------------------------------------------------------------------------

typedef struct OtioClipIterator OtioClipIterator;

// Find all clips in a track (shallow - direct children only)
OtioClipIterator* otio_track_find_clips(OtioTrack* track);
// Find all clips in a stack (descends into children)
OtioClipIterator* otio_stack_find_clips(OtioStack* stack);
// Find all clips in a timeline
OtioClipIterator* otio_timeline_find_clips(OtioTimeline* timeline);

// Iterator operations
int32_t otio_clip_iterator_count(OtioClipIterator* iter);
OtioClip* otio_clip_iterator_next(OtioClipIterator* iter);
void otio_clip_iterator_reset(OtioClipIterator* iter);
void otio_clip_iterator_free(OtioClipIterator* iter);

#ifdef __cplusplus
}
#endif

#endif
