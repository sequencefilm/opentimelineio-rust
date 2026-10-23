#include "otio_shim.h"
#include "otio_shim_macros.h"

// Include individual OTIO headers
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/errorStatus.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/serializableObject.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/transition.h"
#include "opentimelineio/missingReference.h"
#include "opentimelineio/generatorReference.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/freezeFrame.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/algo/editAlgorithm.h"

#include <cstring>
#include <exception>
#include <vector>

namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;

// Use Retainer for reference-counted pointers
template <typename T>
using Retainer = otio::SerializableObject::Retainer<T>;

// ============================================================================
// Handle ownership
// ============================================================================
//
// Every pointer returned by a *_create or JSON load function owns one OTIO
// managed reference, released by the matching *_free. OTIO deletes an object
// whenever its count falls back to 0, and many OTIO calls take a transient
// Retainer on the object they act on (to_json_string wraps `this` in one), so
// a handle at count 0, which is what Retainer::take_value leaves behind, would
// be deleted by the first such call while Rust still holds it.
//
// Functions that hand a child to a parent (append, insert, set_*_reference,
// add_marker, add_effect, ...) release the handle's reference once the parent
// holds its own, because the Rust side forgets the handle on success. On
// failure the handle keeps its reference and the Rust side frees it.

template <typename T>
static T* retain_for_handle(T* so) {
    Retainer<T> retainer(so);
    // detach without releasing, so the reference stays with the handle
    T* ptr = retainer.value;
    retainer.value = nullptr;
    return ptr;
}

static void release_handle(otio::SerializableObject* so) {
    // adopt the handle's reference; the destructor releases it and deletes
    // the object if no parent holds another
    Retainer<otio::SerializableObject> retainer;
    retainer.value = so;
}

// ============================================================================
// Helper Macros - Reduce boilerplate and ensure consistent error handling
// ============================================================================

// Null pointer check - returns failure_val if ptr is null
#define OTIO_NULL_CHECK(ptr, failure_val) \
    do { if (!(ptr)) return (failure_val); } while(0)

// Null check with error message
#define OTIO_NULL_CHECK_ERR(ptr, err, failure_val, msg) \
    do { \
        if (!(ptr)) { \
            set_error(err, 1, msg); \
            return (failure_val); \
        } \
    } while(0)

// Try-catch wrapper for functions returning int (0 = success, -1 = error)
#define OTIO_TRY_INT(err, body) \
    try { \
        body \
        return 0; \
    } catch (const std::exception& e) { \
        set_error(err, 1, e.what()); \
        return -1; \
    } catch (...) { \
        set_error(err, 1, "Unknown exception"); \
        return -1; \
    }

// Try-catch wrapper for functions returning pointers (nullptr on error)
#define OTIO_TRY_PTR(body) \
    try { \
        body \
    } catch (...) { \
        return nullptr; \
    }

// Try-catch wrapper for functions returning int32_t (default_val on error)
#define OTIO_TRY_INT32(default_val, body) \
    try { \
        body \
    } catch (...) { \
        return (default_val); \
    }

// Check OTIO ErrorStatus and return -1 if error
#define OTIO_CHECK_STATUS(status, err) \
    do { \
        if (otio::is_error(status)) { \
            set_error(err, 1, (status).full_description.c_str()); \
            return -1; \
        } \
    } while(0)

// Cast opaque handle to OTIO type
#define OTIO_CAST(Type, var, handle) \
    auto var = reinterpret_cast<otio::Type*>(handle)

// ============================================================================
// Helper Functions
// ============================================================================

static otio::RationalTime to_otio_rt(OtioRationalTime rt) {
    return otio::RationalTime(rt.value, rt.rate);
}

static otio::TimeRange to_otio_tr(OtioTimeRange tr) {
    return otio::TimeRange(to_otio_rt(tr.start_time), to_otio_rt(tr.duration));
}

static void set_error(OtioError* err, int code, const char* msg) {
    if (err) {
        err->code = code;
        strncpy(err->message, msg, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
    }
}

// Safe strdup that returns empty string on failure
static char* safe_strdup(const char* s) {
    if (!s) return strdup("");
    char* result = strdup(s);
    return result ? result : strdup("");
}

static char* safe_strdup(const std::string& s) {
    return safe_strdup(s.c_str());
}

// ============================================================================
// Template helpers for metadata
// ============================================================================

template<typename T>
static char* get_metadata_string_impl(T* obj, const char* key) {
    if (!obj || !key) return nullptr;
    OTIO_TRY_PTR(
        auto& meta = obj->metadata();
        auto it = meta.find(std::string(key));
        if (it != meta.end()) {
            if (it->second.type() == typeid(std::string)) {
                const std::string& value = std::any_cast<const std::string&>(it->second);
                return safe_strdup(value);
            }
        }
        return nullptr;
    )
}

template<typename T>
static void set_metadata_string_impl(T* obj, const char* key, const char* value) {
    if (!obj || !key || !value) return;
    try {
        obj->metadata()[std::string(key)] = std::string(value);
    } catch (...) {
        // Ignore errors in metadata setting
    }
}

// ============================================================================
// Template helpers for child operations
// ============================================================================

template<typename Container>
static int32_t children_count_impl(Container* container) {
    if (!container) return 0;
    try {
        return static_cast<int32_t>(container->children().size());
    } catch (...) {
        return 0;
    }
}

template<typename Container>
static int32_t child_type_impl(Container* container, int32_t index) {
    if (!container) return -1;
    try {
        auto& children = container->children();
        if (index < 0 || static_cast<size_t>(index) >= children.size()) {
            return -1;
        }
        auto child = children[index].value;
        if (dynamic_cast<otio::Clip*>(child)) return OTIO_CHILD_TYPE_CLIP;
        if (dynamic_cast<otio::Gap*>(child)) return OTIO_CHILD_TYPE_GAP;
        if (dynamic_cast<otio::Stack*>(child)) return OTIO_CHILD_TYPE_STACK;
        if (dynamic_cast<otio::Track*>(child)) return OTIO_CHILD_TYPE_TRACK;
        if (dynamic_cast<otio::Transition*>(child)) return OTIO_CHILD_TYPE_TRANSITION;
        return -1;
    } catch (...) {
        return -1;
    }
}

template<typename Container>
static void* child_at_impl(Container* container, int32_t index) {
    if (!container) return nullptr;
    try {
        auto& children = container->children();
        if (index < 0 || static_cast<size_t>(index) >= children.size()) {
            return nullptr;
        }
        return children[index].value;
    } catch (...) {
        return nullptr;
    }
}

template<typename Container, typename Child>
static int append_child_impl(Container* container, Child* child, OtioError* err) {
    OTIO_NULL_CHECK_ERR(container, err, -1, "Container is null");
    OTIO_NULL_CHECK_ERR(child, err, -1, "Child is null");
    try {
        otio::ErrorStatus status;
        container->append_child(child, &status);
        OTIO_CHECK_STATUS(status, err);
        release_handle(child);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

template<typename Container, typename Child>
static int insert_child_impl(Container* container, int32_t index, Child* child, OtioError* err) {
    OTIO_NULL_CHECK_ERR(container, err, -1, "Container is null");
    OTIO_NULL_CHECK_ERR(child, err, -1, "Child is null");
    try {
        otio::ErrorStatus status;
        container->insert_child(index, child, &status);
        OTIO_CHECK_STATUS(status, err);
        release_handle(child);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

template<typename Container>
static int remove_child_impl(Container* container, int32_t index, OtioError* err) {
    OTIO_NULL_CHECK_ERR(container, err, -1, "Container is null");
    try {
        auto& children = container->children();
        if (index < 0 || static_cast<size_t>(index) >= children.size()) {
            set_error(err, 1, "Index out of bounds");
            return -1;
        }
        otio::ErrorStatus status;
        container->remove_child(index, &status);
        OTIO_CHECK_STATUS(status, err);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

template<typename Container>
static int clear_children_impl(Container* container, OtioError* err) {
    OTIO_NULL_CHECK_ERR(container, err, -1, "Container is null");
    try {
        container->clear_children();
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

// ============================================================================
// Parent navigation helpers (templates - must be before extern "C")
// ============================================================================

// Helper to determine parent type
template<typename T>
static int32_t get_parent_type_impl(T* obj) {
    if (!obj) return OTIO_PARENT_TYPE_NONE;
    try {
        auto parent = obj->parent();
        if (!parent) return OTIO_PARENT_TYPE_NONE;
        if (dynamic_cast<otio::Track*>(parent)) return OTIO_PARENT_TYPE_TRACK;
        if (dynamic_cast<otio::Stack*>(parent)) return OTIO_PARENT_TYPE_STACK;
        return OTIO_PARENT_TYPE_NONE;
    } catch (...) {
        return OTIO_PARENT_TYPE_NONE;
    }
}

template<typename T>
static void* get_parent_impl(T* obj) {
    if (!obj) return nullptr;
    try {
        return obj->parent();
    } catch (...) {
        return nullptr;
    }
}

// Helper to recursively find clips in a composition
static void find_clips_recursive(otio::Composition* comp, std::vector<otio::Clip*>& clips) {
    if (!comp) return;
    for (auto& child : comp->children()) {
        if (auto clip = dynamic_cast<otio::Clip*>(child.value)) {
            clips.push_back(clip);
        } else if (auto nested = dynamic_cast<otio::Composition*>(child.value)) {
            find_clips_recursive(nested, clips);
        }
    }
}

// ============================================================================
// C API Implementation
// ============================================================================

extern "C" {

// ----------------------------------------------------------------------------
// Timeline
// ----------------------------------------------------------------------------

OtioTimeline* otio_timeline_create(const char* name) {
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        auto tl = new otio::Timeline(name);
        tl->set_tracks(new otio::Stack());
        return reinterpret_cast<OtioTimeline*>(retain_for_handle(tl));
    )
}

void otio_timeline_free(OtioTimeline* tl) {
    if (tl) {
        try {
            OTIO_CAST(Timeline, timeline, tl);
            release_handle(timeline);
        } catch (...) {
            // Ignore exceptions during cleanup
        }
    }
}

int otio_timeline_set_global_start_time(OtioTimeline* tl, OtioRationalTime time, OtioError* err) {
    OTIO_NULL_CHECK_ERR(tl, err, -1, "Timeline is null");
    OTIO_TRY_INT(err,
        OTIO_CAST(Timeline, timeline, tl);
        timeline->set_global_start_time(to_otio_rt(time));
    )
}

OtioTrack* otio_timeline_add_video_track(OtioTimeline* tl, const char* name) {
    OTIO_NULL_CHECK(tl, nullptr);
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Timeline, timeline, tl);
        auto track = new otio::Track(name, std::nullopt, otio::Track::Kind::video);
        otio::ErrorStatus err;
        timeline->tracks()->append_child(track, &err);
        return reinterpret_cast<OtioTrack*>(track);
    )
}

OtioTrack* otio_timeline_add_audio_track(OtioTimeline* tl, const char* name) {
    OTIO_NULL_CHECK(tl, nullptr);
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Timeline, timeline, tl);
        auto track = new otio::Track(name, std::nullopt, otio::Track::Kind::audio);
        otio::ErrorStatus err;
        timeline->tracks()->append_child(track, &err);
        return reinterpret_cast<OtioTrack*>(track);
    )
}

OtioStack* otio_timeline_get_tracks(OtioTimeline* tl) {
    OTIO_NULL_CHECK(tl, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Timeline, timeline, tl);
        return reinterpret_cast<OtioStack*>(timeline->tracks());
    )
}

// ----------------------------------------------------------------------------
// Track
// ----------------------------------------------------------------------------

OtioTrack* otio_track_create_video(const char* name) {
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        auto track = new otio::Track(name, std::nullopt, otio::Track::Kind::video);
        return reinterpret_cast<OtioTrack*>(retain_for_handle(track));
    )
}

OtioTrack* otio_track_create_audio(const char* name) {
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        auto track = new otio::Track(name, std::nullopt, otio::Track::Kind::audio);
        return reinterpret_cast<OtioTrack*>(retain_for_handle(track));
    )
}

void otio_track_free(OtioTrack* track) {
    if (track) {
        try {
            OTIO_CAST(Track, t, track);
            release_handle(t);
        } catch (...) {
            // Ignore exceptions during cleanup
        }
    }
}

int otio_track_append_clip(OtioTrack* track, OtioClip* clip, OtioError* err) {
    return append_child_impl<otio::Track, otio::Clip>(
        reinterpret_cast<otio::Track*>(track),
        reinterpret_cast<otio::Clip*>(clip), err);
}

int otio_track_append_gap(OtioTrack* track, OtioGap* gap, OtioError* err) {
    return append_child_impl<otio::Track, otio::Gap>(
        reinterpret_cast<otio::Track*>(track),
        reinterpret_cast<otio::Gap*>(gap), err);
}

int otio_track_append_stack(OtioTrack* track, OtioStack* stack, OtioError* err) {
    return append_child_impl<otio::Track, otio::Stack>(
        reinterpret_cast<otio::Track*>(track),
        reinterpret_cast<otio::Stack*>(stack), err);
}

int32_t otio_track_children_count(OtioTrack* track) {
    return children_count_impl(reinterpret_cast<otio::Track*>(track));
}

int32_t otio_track_child_type(OtioTrack* track, int32_t index) {
    return child_type_impl(reinterpret_cast<otio::Track*>(track), index);
}

void* otio_track_child_at(OtioTrack* track, int32_t index) {
    return child_at_impl(reinterpret_cast<otio::Track*>(track), index);
}

int otio_track_remove_child(OtioTrack* track, int32_t index, OtioError* err) {
    return remove_child_impl(reinterpret_cast<otio::Track*>(track), index, err);
}

int otio_track_insert_clip(OtioTrack* track, int32_t index, OtioClip* clip, OtioError* err) {
    return insert_child_impl<otio::Track, otio::Clip>(
        reinterpret_cast<otio::Track*>(track), index,
        reinterpret_cast<otio::Clip*>(clip), err);
}

int otio_track_insert_gap(OtioTrack* track, int32_t index, OtioGap* gap, OtioError* err) {
    return insert_child_impl<otio::Track, otio::Gap>(
        reinterpret_cast<otio::Track*>(track), index,
        reinterpret_cast<otio::Gap*>(gap), err);
}

int otio_track_insert_stack(OtioTrack* track, int32_t index, OtioStack* stack, OtioError* err) {
    return insert_child_impl<otio::Track, otio::Stack>(
        reinterpret_cast<otio::Track*>(track), index,
        reinterpret_cast<otio::Stack*>(stack), err);
}

int otio_track_clear_children(OtioTrack* track, OtioError* err) {
    return clear_children_impl(reinterpret_cast<otio::Track*>(track), err);
}

// Helper to get composable type from pointer
static int32_t get_composable_type(otio::Composable* comp) {
    if (!comp) return -1;
    if (dynamic_cast<otio::Clip*>(comp)) return OTIO_CHILD_TYPE_CLIP;
    if (dynamic_cast<otio::Gap*>(comp)) return OTIO_CHILD_TYPE_GAP;
    if (dynamic_cast<otio::Stack*>(comp)) return OTIO_CHILD_TYPE_STACK;
    if (dynamic_cast<otio::Track*>(comp)) return OTIO_CHILD_TYPE_TRACK;
    if (dynamic_cast<otio::Transition*>(comp)) return OTIO_CHILD_TYPE_TRANSITION;
    return -1;
}

OtioNeighbors otio_track_neighbors_of(OtioTrack* track, int32_t child_index,
                                       int32_t gap_policy, OtioError* err) {
    OtioNeighbors result = {nullptr, -1, nullptr, -1};
    OTIO_NULL_CHECK_ERR(track, err, result, "Track is null");
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto& children = t->children();
        if (child_index < 0 || static_cast<size_t>(child_index) >= children.size()) {
            set_error(err, 1, "Index out of bounds");
            return result;
        }

        otio::ErrorStatus status;
        auto policy = static_cast<otio::Track::NeighborGapPolicy>(gap_policy);
        auto [left, right] = t->neighbors_of(children[child_index].value, &status, policy);

        if (otio::is_error(status)) {
            set_error(err, static_cast<int>(status.outcome), status.details.c_str());
            return result;
        }

        if (left) {
            result.left = left.value;
            result.left_type = get_composable_type(left.value);
        }
        if (right) {
            result.right = right.value;
            result.right_type = get_composable_type(right.value);
        }
        return result;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return result;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return result;
    }
}

// ----------------------------------------------------------------------------
// Clip
// ----------------------------------------------------------------------------

OtioClip* otio_clip_create(const char* name, OtioTimeRange source_range) {
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        auto clip = new otio::Clip(name, nullptr, to_otio_tr(source_range));
        return reinterpret_cast<OtioClip*>(retain_for_handle(clip));
    )
}

int otio_clip_set_media_reference(OtioClip* clip, OtioExternalRef* ref, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(ref, err, -1, "Reference is null");
    OTIO_TRY_INT(err,
        OTIO_CAST(Clip, c, clip);
        OTIO_CAST(ExternalReference, r, ref);
        c->set_media_reference(r);
        release_handle(r);
    )
}

char* otio_clip_get_name(OtioClip* clip) {
    OTIO_NULL_CHECK(clip, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Clip, c, clip);
        return safe_strdup(c->name());
    )
}

OtioTimeRange otio_clip_get_source_range(OtioClip* clip) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!clip) return zero;
    try {
        OTIO_CAST(Clip, c, clip);
        auto sr = c->source_range();
        if (sr.has_value()) {
            auto& range = sr.value();
            return OtioTimeRange{
                OtioRationalTime{range.start_time().value(), range.start_time().rate()},
                OtioRationalTime{range.duration().value(), range.duration().rate()}
            };
        }
    } catch (...) {
    }
    return zero;
}

OtioTimeRange otio_clip_available_range(OtioClip* clip, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    OTIO_NULL_CHECK_ERR(clip, err, zero, "Clip is null");
    try {
        OTIO_CAST(Clip, c, clip);
        otio::ErrorStatus status;
        auto range = c->available_range(&status);
        if (otio::is_error(status)) {
            set_error(err, static_cast<int>(status.outcome), status.details.c_str());
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return zero;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return zero;
    }
}

// ----------------------------------------------------------------------------
// Clip Multi-Reference Support
// ----------------------------------------------------------------------------

struct OtioStringIterator {
    std::vector<std::string> strings;
    size_t index;
    OtioStringIterator() : index(0) {}
};

OtioStringIterator* otio_clip_media_reference_keys(OtioClip* clip) {
    OTIO_NULL_CHECK(clip, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Clip, c, clip);
        auto iter = new OtioStringIterator();
        const auto& refs = c->media_references();
        for (const auto& pair : refs) {
            iter->strings.push_back(pair.first);
        }
        return iter;
    )
}

int32_t otio_string_iterator_count(OtioStringIterator* iter) {
    if (!iter) return 0;
    return static_cast<int32_t>(iter->strings.size());
}

char* otio_string_iterator_next(OtioStringIterator* iter) {
    if (!iter || iter->index >= iter->strings.size()) return nullptr;
    return safe_strdup(iter->strings[iter->index++]);
}

void otio_string_iterator_reset(OtioStringIterator* iter) {
    if (iter) iter->index = 0;
}

void otio_string_iterator_free(OtioStringIterator* iter) {
    delete iter;
}

char* otio_clip_active_media_reference_key(OtioClip* clip) {
    OTIO_NULL_CHECK(clip, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Clip, c, clip);
        return safe_strdup(c->active_media_reference_key());
    )
}

int otio_clip_set_active_media_reference_key(OtioClip* clip, const char* key, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(key, err, -1, "Key is null");
    try {
        OTIO_CAST(Clip, c, clip);
        c->set_active_media_reference_key(key);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_add_media_reference(OtioClip* clip, const char* key,
                                   void* ref, int32_t ref_type, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(key, err, -1, "Key is null");
    OTIO_NULL_CHECK_ERR(ref, err, -1, "Reference is null");
    try {
        OTIO_CAST(Clip, c, clip);
        otio::MediaReference* media_ref = nullptr;
        switch (ref_type) {
            case 0: // OTIO_REF_TYPE_EXTERNAL
                media_ref = reinterpret_cast<otio::ExternalReference*>(ref);
                break;
            case 1: // OTIO_REF_TYPE_MISSING
                media_ref = reinterpret_cast<otio::MissingReference*>(ref);
                break;
            case 2: // OTIO_REF_TYPE_GENERATOR
                media_ref = reinterpret_cast<otio::GeneratorReference*>(ref);
                break;
            case 3: // OTIO_REF_TYPE_IMAGE_SEQUENCE
                media_ref = reinterpret_cast<otio::ImageSequenceReference*>(ref);
                break;
            default:
                set_error(err, 1, "Unknown reference type");
                return -1;
        }
        // Get existing references and add the new one
        auto refs = c->media_references();
        // set_media_references clears the clip's map before refilling it, and
        // the clip holds the only reference to its current media references,
        // so retain them until the call returns or they are deleted and then
        // re-inserted as dangling pointers
        std::vector<Retainer<otio::MediaReference>> existing;
        for (auto const& entry: refs) {
            existing.emplace_back(entry.second);
        }
        refs[key] = media_ref;
        // Keep the current active key
        std::string active_key = c->active_media_reference_key();
        otio::ErrorStatus status;
        c->set_media_references(refs, active_key, &status);
        OTIO_CHECK_STATUS(status, err);
        release_handle(media_ref);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_has_media_reference(OtioClip* clip, const char* key) {
    if (!clip || !key) return 0;
    try {
        OTIO_CAST(Clip, c, clip);
        const auto& refs = c->media_references();
        return refs.find(key) != refs.end() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// ----------------------------------------------------------------------------
// Gap
// ----------------------------------------------------------------------------

OtioGap* otio_gap_create(OtioRationalTime duration) {
    OTIO_TRY_PTR(
        auto gap = new otio::Gap(otio::TimeRange(
            otio::RationalTime(0, duration.rate),
            to_otio_rt(duration)
        ));
        return reinterpret_cast<OtioGap*>(retain_for_handle(gap));
    )
}

char* otio_gap_get_name(OtioGap* gap) {
    OTIO_NULL_CHECK(gap, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Gap, g, gap);
        return safe_strdup(g->name());
    )
}

// ----------------------------------------------------------------------------
// ExternalReference
// ----------------------------------------------------------------------------

OtioExternalRef* otio_external_ref_create(const char* target_url) {
    OTIO_NULL_CHECK(target_url, nullptr);
    OTIO_TRY_PTR(
        auto ref = new otio::ExternalReference(target_url);
        return reinterpret_cast<OtioExternalRef*>(retain_for_handle(ref));
    )
}

int otio_external_ref_set_available_range(OtioExternalRef* ref, OtioTimeRange range, OtioError* err) {
    OTIO_NULL_CHECK_ERR(ref, err, -1, "Reference is null");
    OTIO_TRY_INT(err,
        OTIO_CAST(ExternalReference, r, ref);
        r->set_available_range(to_otio_tr(range));
    )
}

void otio_external_ref_free(OtioExternalRef* ref) {
    if (ref) {
        try {
            OTIO_CAST(ExternalReference, r, ref);
            release_handle(r);
        } catch (...) {
            // Ignore exceptions during cleanup
        }
    }
}

// ----------------------------------------------------------------------------
// Stack
// ----------------------------------------------------------------------------

OtioStack* otio_stack_create(const char* name) {
    OTIO_NULL_CHECK(name, nullptr);
    OTIO_TRY_PTR(
        auto stack = new otio::Stack(name);
        return reinterpret_cast<OtioStack*>(retain_for_handle(stack));
    )
}

void otio_stack_free(OtioStack* stack) {
    if (stack) {
        try {
            OTIO_CAST(Stack, s, stack);
            release_handle(s);
        } catch (...) {
            // Ignore exceptions during cleanup
        }
    }
}

int otio_stack_append_track(OtioStack* stack, OtioTrack* track, OtioError* err) {
    return append_child_impl<otio::Stack, otio::Track>(
        reinterpret_cast<otio::Stack*>(stack),
        reinterpret_cast<otio::Track*>(track), err);
}

int otio_stack_append_clip(OtioStack* stack, OtioClip* clip, OtioError* err) {
    return append_child_impl<otio::Stack, otio::Clip>(
        reinterpret_cast<otio::Stack*>(stack),
        reinterpret_cast<otio::Clip*>(clip), err);
}

int otio_stack_append_gap(OtioStack* stack, OtioGap* gap, OtioError* err) {
    return append_child_impl<otio::Stack, otio::Gap>(
        reinterpret_cast<otio::Stack*>(stack),
        reinterpret_cast<otio::Gap*>(gap), err);
}

int otio_stack_append_stack(OtioStack* stack, OtioStack* child, OtioError* err) {
    return append_child_impl<otio::Stack, otio::Stack>(
        reinterpret_cast<otio::Stack*>(stack),
        reinterpret_cast<otio::Stack*>(child), err);
}

int32_t otio_stack_children_count(OtioStack* stack) {
    return children_count_impl(reinterpret_cast<otio::Stack*>(stack));
}

int32_t otio_stack_child_type(OtioStack* stack, int32_t index) {
    return child_type_impl(reinterpret_cast<otio::Stack*>(stack), index);
}

void* otio_stack_child_at(OtioStack* stack, int32_t index) {
    return child_at_impl(reinterpret_cast<otio::Stack*>(stack), index);
}

int otio_stack_remove_child(OtioStack* stack, int32_t index, OtioError* err) {
    return remove_child_impl(reinterpret_cast<otio::Stack*>(stack), index, err);
}

int otio_stack_insert_track(OtioStack* stack, int32_t index, OtioTrack* track, OtioError* err) {
    return insert_child_impl<otio::Stack, otio::Track>(
        reinterpret_cast<otio::Stack*>(stack), index,
        reinterpret_cast<otio::Track*>(track), err);
}

int otio_stack_insert_clip(OtioStack* stack, int32_t index, OtioClip* clip, OtioError* err) {
    return insert_child_impl<otio::Stack, otio::Clip>(
        reinterpret_cast<otio::Stack*>(stack), index,
        reinterpret_cast<otio::Clip*>(clip), err);
}

int otio_stack_insert_gap(OtioStack* stack, int32_t index, OtioGap* gap, OtioError* err) {
    return insert_child_impl<otio::Stack, otio::Gap>(
        reinterpret_cast<otio::Stack*>(stack), index,
        reinterpret_cast<otio::Gap*>(gap), err);
}

int otio_stack_insert_stack(OtioStack* stack, int32_t index, OtioStack* child, OtioError* err) {
    return insert_child_impl<otio::Stack, otio::Stack>(
        reinterpret_cast<otio::Stack*>(stack), index,
        reinterpret_cast<otio::Stack*>(child), err);
}

int otio_stack_clear_children(OtioStack* stack, OtioError* err) {
    return clear_children_impl(reinterpret_cast<otio::Stack*>(stack), err);
}

char* otio_stack_get_name(OtioStack* stack) {
    OTIO_NULL_CHECK(stack, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Stack, s, stack);
        return safe_strdup(s->name());
    )
}

char* otio_track_get_name(OtioTrack* track) {
    OTIO_NULL_CHECK(track, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Track, t, track);
        return safe_strdup(t->name());
    )
}

// ----------------------------------------------------------------------------
// Serialization
// ----------------------------------------------------------------------------

int otio_timeline_write_to_file(OtioTimeline* tl, const char* path, OtioError* err) {
    OTIO_NULL_CHECK_ERR(tl, err, -1, "Timeline is null");
    OTIO_NULL_CHECK_ERR(path, err, -1, "Path is null");
    OTIO_TRY_INT(err,
        OTIO_CAST(Timeline, timeline, tl);
        otio::ErrorStatus status;
        bool success = timeline->to_json_file(path, &status);
        if (!success || otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
    )
}

OtioTimeline* otio_timeline_read_from_file(const char* path, OtioError* err) {
    if (!path) {
        set_error(err, 1, "Path is null");
        return nullptr;
    }
    try {
        otio::ErrorStatus status;
        auto result = otio::SerializableObject::from_json_file(path, &status);
        if (otio::is_error(status) || !result) {
            set_error(err, 1, status.full_description.c_str());
            return nullptr;
        }
        auto timeline = dynamic_cast<otio::Timeline*>(result);
        if (!timeline) {
            set_error(err, 1, "File does not contain a Timeline");
            Retainer<otio::SerializableObject> retainer(result);
            return nullptr;
        }
        return reinterpret_cast<OtioTimeline*>(retain_for_handle(timeline));
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return nullptr;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return nullptr;
    }
}

char* otio_timeline_to_json_string(OtioTimeline* tl, OtioError* err) {
    if (!tl) {
        set_error(err, 1, "Timeline is null");
        return nullptr;
    }
    try {
        auto timeline = reinterpret_cast<otio::Timeline*>(tl);
        otio::ErrorStatus status;
        std::string json = timeline->to_json_string(&status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return nullptr;
        }
        return safe_strdup(json);
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return nullptr;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return nullptr;
    }
}

OtioTimeline* otio_timeline_from_json_string(const char* json, OtioError* err) {
    if (!json) {
        set_error(err, 1, "JSON string is null");
        return nullptr;
    }
    try {
        otio::ErrorStatus status;
        auto result = otio::SerializableObject::from_json_string(json, &status);
        if (otio::is_error(status) || !result) {
            set_error(err, 1, status.full_description.c_str());
            return nullptr;
        }
        auto timeline = dynamic_cast<otio::Timeline*>(result);
        if (!timeline) {
            set_error(err, 1, "JSON does not contain a Timeline");
            Retainer<otio::SerializableObject> retainer(result);
            return nullptr;
        }
        return reinterpret_cast<OtioTimeline*>(retain_for_handle(timeline));
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return nullptr;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// Serialization with schema version targeting
// ----------------------------------------------------------------------------

int otio_timeline_write_to_file_with_schema_versions(
    OtioTimeline* tl,
    const char* path,
    const char** schema_names,
    const int64_t* schema_versions,
    int32_t count,
    OtioError* err
) {
    OTIO_NULL_CHECK_ERR(tl, err, -1, "Timeline is null");
    OTIO_NULL_CHECK_ERR(path, err, -1, "Path is null");

    // Build schema version map from arrays
    otio::schema_version_map version_map;
    if (schema_names && schema_versions && count > 0) {
        for (int32_t i = 0; i < count; i++) {
            if (schema_names[i]) {
                version_map[schema_names[i]] = schema_versions[i];
            }
        }
    }

    OTIO_TRY_INT(err,
        OTIO_CAST(Timeline, timeline, tl);
        otio::ErrorStatus status;
        bool success = timeline->to_json_file(
            path,
            &status,
            version_map.empty() ? nullptr : &version_map
        );
        if (!success || otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
    )
}

char* otio_timeline_to_json_string_with_schema_versions(
    OtioTimeline* tl,
    const char** schema_names,
    const int64_t* schema_versions,
    int32_t count,
    OtioError* err
) {
    OTIO_NULL_CHECK_ERR(tl, err, nullptr, "Timeline is null");

    // Build schema version map from arrays
    otio::schema_version_map version_map;
    if (schema_names && schema_versions && count > 0) {
        for (int32_t i = 0; i < count; i++) {
            if (schema_names[i]) {
                version_map[schema_names[i]] = schema_versions[i];
            }
        }
    }

    try {
        auto timeline = reinterpret_cast<otio::Timeline*>(tl);
        otio::ErrorStatus status;
        std::string json = timeline->to_json_string(
            &status,
            version_map.empty() ? nullptr : &version_map
        );
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return nullptr;
        }
        return safe_strdup(json);
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return nullptr;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// String memory management
// ----------------------------------------------------------------------------

void otio_free_string(char* str) {
    free(str);
}

// ----------------------------------------------------------------------------
// Metadata - using template helpers
// ----------------------------------------------------------------------------

void otio_timeline_set_metadata_string(OtioTimeline* tl, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Timeline*>(tl), key, value);
}

char* otio_timeline_get_metadata_string(OtioTimeline* tl, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Timeline*>(tl), key);
}

void otio_track_set_metadata_string(OtioTrack* track, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Track*>(track), key, value);
}

char* otio_track_get_metadata_string(OtioTrack* track, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Track*>(track), key);
}

void otio_clip_set_metadata_string(OtioClip* clip, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Clip*>(clip), key, value);
}

char* otio_clip_get_metadata_string(OtioClip* clip, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Clip*>(clip), key);
}

void otio_gap_set_metadata_string(OtioGap* gap, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Gap*>(gap), key, value);
}

char* otio_gap_get_metadata_string(OtioGap* gap, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Gap*>(gap), key);
}

void otio_stack_set_metadata_string(OtioStack* stack, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Stack*>(stack), key, value);
}

char* otio_stack_get_metadata_string(OtioStack* stack, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Stack*>(stack), key);
}

void otio_external_ref_set_metadata_string(OtioExternalRef* ref, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::ExternalReference*>(ref), key, value);
}

char* otio_external_ref_get_metadata_string(OtioExternalRef* ref, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::ExternalReference*>(ref), key);
}

// ----------------------------------------------------------------------------
// Marker
// ----------------------------------------------------------------------------

OtioMarker* otio_marker_create(const char* name, OtioTimeRange marked_range, const char* color) {
    OTIO_NULL_CHECK(name, nullptr);
    const char* c = color ? color : otio::Marker::Color::green;
    OTIO_TRY_PTR(
        auto marker = new otio::Marker(name, to_otio_tr(marked_range), c);
        return reinterpret_cast<OtioMarker*>(retain_for_handle(marker));
    )
}

void otio_marker_free(OtioMarker* marker) {
    if (marker) {
        try {
            OTIO_CAST(Marker, m, marker);
            release_handle(m);
        } catch (...) {
        }
    }
}

char* otio_marker_get_name(OtioMarker* marker) {
    OTIO_NULL_CHECK(marker, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Marker, m, marker);
        return safe_strdup(m->name());
    )
}

char* otio_marker_get_color(OtioMarker* marker) {
    OTIO_NULL_CHECK(marker, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Marker, m, marker);
        return safe_strdup(m->color());
    )
}

void otio_marker_set_color(OtioMarker* marker, const char* color) {
    if (!marker || !color) return;
    try {
        OTIO_CAST(Marker, m, marker);
        m->set_color(std::string(color));
    } catch (...) {
    }
}

OtioTimeRange otio_marker_get_marked_range(OtioMarker* marker) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!marker) return zero;
    try {
        OTIO_CAST(Marker, m, marker);
        auto range = m->marked_range();
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (...) {
        return zero;
    }
}

int otio_marker_set_marked_range(OtioMarker* marker, OtioTimeRange range, OtioError* err) {
    OTIO_NULL_CHECK_ERR(marker, err, -1, "Marker is null");
    OTIO_TRY_INT(err,
        OTIO_CAST(Marker, m, marker);
        m->set_marked_range(to_otio_tr(range));
    )
}

char* otio_marker_get_comment(OtioMarker* marker) {
    OTIO_NULL_CHECK(marker, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Marker, m, marker);
        return safe_strdup(m->comment());
    )
}

void otio_marker_set_comment(OtioMarker* marker, const char* comment) {
    if (!marker || !comment) return;
    try {
        OTIO_CAST(Marker, m, marker);
        m->set_comment(std::string(comment));
    } catch (...) {
    }
}

void otio_marker_set_metadata_string(OtioMarker* marker, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Marker*>(marker), key, value);
}

char* otio_marker_get_metadata_string(OtioMarker* marker, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Marker*>(marker), key);
}

// ----------------------------------------------------------------------------
// Effect
// ----------------------------------------------------------------------------

OtioEffect* otio_effect_create(const char* name, const char* effect_name) {
    const char* n = name ? name : "";
    const char* en = effect_name ? effect_name : "";
    OTIO_TRY_PTR(
        auto effect = new otio::Effect(n, en);
        return reinterpret_cast<OtioEffect*>(retain_for_handle(effect));
    )
}

void otio_effect_free(OtioEffect* effect) {
    if (effect) {
        try {
            OTIO_CAST(Effect, e, effect);
            release_handle(e);
        } catch (...) {
        }
    }
}

char* otio_effect_get_name(OtioEffect* effect) {
    OTIO_NULL_CHECK(effect, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Effect, e, effect);
        return safe_strdup(e->name());
    )
}

char* otio_effect_get_effect_name(OtioEffect* effect) {
    OTIO_NULL_CHECK(effect, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Effect, e, effect);
        return safe_strdup(e->effect_name());
    )
}

void otio_effect_set_effect_name(OtioEffect* effect, const char* effect_name) {
    if (!effect || !effect_name) return;
    try {
        OTIO_CAST(Effect, e, effect);
        e->set_effect_name(std::string(effect_name));
    } catch (...) {
    }
}

void otio_effect_set_metadata_string(OtioEffect* effect, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Effect*>(effect), key, value);
}

char* otio_effect_get_metadata_string(OtioEffect* effect, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Effect*>(effect), key);
}

// ----------------------------------------------------------------------------
// Transition
// ----------------------------------------------------------------------------

OtioTransition* otio_transition_create(const char* name, const char* transition_type,
                                       OtioRationalTime in_offset, OtioRationalTime out_offset) {
    const char* n = name ? name : "";
    const char* tt = transition_type ? transition_type : otio::Transition::Type::SMPTE_Dissolve;
    OTIO_TRY_PTR(
        auto transition = new otio::Transition(n, tt, to_otio_rt(in_offset), to_otio_rt(out_offset));
        return reinterpret_cast<OtioTransition*>(retain_for_handle(transition));
    )
}

void otio_transition_free(OtioTransition* transition) {
    if (transition) {
        try {
            OTIO_CAST(Transition, t, transition);
            release_handle(t);
        } catch (...) {
        }
    }
}

char* otio_transition_get_name(OtioTransition* transition) {
    OTIO_NULL_CHECK(transition, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Transition, t, transition);
        return safe_strdup(t->name());
    )
}

char* otio_transition_get_transition_type(OtioTransition* transition) {
    OTIO_NULL_CHECK(transition, nullptr);
    OTIO_TRY_PTR(
        OTIO_CAST(Transition, t, transition);
        return safe_strdup(t->transition_type());
    )
}

void otio_transition_set_transition_type(OtioTransition* transition, const char* transition_type) {
    if (!transition || !transition_type) return;
    try {
        OTIO_CAST(Transition, t, transition);
        t->set_transition_type(std::string(transition_type));
    } catch (...) {
    }
}

OtioRationalTime otio_transition_get_in_offset(OtioTransition* transition) {
    OtioRationalTime zero = {0, 1};
    if (!transition) return zero;
    try {
        OTIO_CAST(Transition, t, transition);
        auto rt = t->in_offset();
        return OtioRationalTime{rt.value(), rt.rate()};
    } catch (...) {
        return zero;
    }
}

void otio_transition_set_in_offset(OtioTransition* transition, OtioRationalTime offset) {
    if (!transition) return;
    try {
        OTIO_CAST(Transition, t, transition);
        t->set_in_offset(to_otio_rt(offset));
    } catch (...) {
    }
}

OtioRationalTime otio_transition_get_out_offset(OtioTransition* transition) {
    OtioRationalTime zero = {0, 1};
    if (!transition) return zero;
    try {
        OTIO_CAST(Transition, t, transition);
        auto rt = t->out_offset();
        return OtioRationalTime{rt.value(), rt.rate()};
    } catch (...) {
        return zero;
    }
}

void otio_transition_set_out_offset(OtioTransition* transition, OtioRationalTime offset) {
    if (!transition) return;
    try {
        OTIO_CAST(Transition, t, transition);
        t->set_out_offset(to_otio_rt(offset));
    } catch (...) {
    }
}

OtioRationalTime otio_transition_get_duration(OtioTransition* transition) {
    OtioRationalTime zero = {0, 1};
    if (!transition) return zero;
    try {
        OTIO_CAST(Transition, t, transition);
        otio::ErrorStatus status;
        auto rt = t->duration(&status);
        if (otio::is_error(status)) return zero;
        return OtioRationalTime{rt.value(), rt.rate()};
    } catch (...) {
        return zero;
    }
}

void otio_transition_set_metadata_string(OtioTransition* transition, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::Transition*>(transition), key, value);
}

char* otio_transition_get_metadata_string(OtioTransition* transition, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::Transition*>(transition), key);
}

int otio_track_append_transition(OtioTrack* track, OtioTransition* transition, OtioError* err) {
    return append_child_impl<otio::Track, otio::Transition>(
        reinterpret_cast<otio::Track*>(track),
        reinterpret_cast<otio::Transition*>(transition), err);
}

int otio_track_insert_transition(OtioTrack* track, int32_t index, OtioTransition* transition, OtioError* err) {
    return insert_child_impl<otio::Track, otio::Transition>(
        reinterpret_cast<otio::Track*>(track), index,
        reinterpret_cast<otio::Transition*>(transition), err);
}

// ----------------------------------------------------------------------------
// MissingReference
// ----------------------------------------------------------------------------

OtioMissingRef* otio_missing_ref_create(void) {
    OTIO_TRY_PTR(
        auto ref = new otio::MissingReference();
        return reinterpret_cast<OtioMissingRef*>(retain_for_handle(ref));
    )
}

void otio_missing_ref_free(OtioMissingRef* ref) {
    if (ref) {
        try {
            auto typed = reinterpret_cast<otio::MissingReference*>(ref);
            release_handle(typed);
        } catch (...) {
        }
    }
}

void otio_missing_ref_set_metadata_string(OtioMissingRef* ref, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::MissingReference*>(ref), key, value);
}

char* otio_missing_ref_get_metadata_string(OtioMissingRef* ref, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::MissingReference*>(ref), key);
}

// ----------------------------------------------------------------------------
// ImageSequenceReference
// ----------------------------------------------------------------------------

OtioImageSeqRef* otio_image_seq_ref_create(const char* target_url_base,
    const char* name_prefix, const char* name_suffix,
    int32_t start_frame, int32_t frame_step, double rate, int32_t frame_zero_padding) {
    const char* url = target_url_base ? target_url_base : "";
    const char* prefix = name_prefix ? name_prefix : "";
    const char* suffix = name_suffix ? name_suffix : "";
    OTIO_TRY_PTR(
        auto ref = new otio::ImageSequenceReference(
            url, prefix, suffix, start_frame, frame_step, rate, frame_zero_padding);
        return reinterpret_cast<OtioImageSeqRef*>(retain_for_handle(ref));
    )
}

void otio_image_seq_ref_free(OtioImageSeqRef* ref) {
    if (ref) {
        try {
            auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
            release_handle(typed);
        } catch (...) {
        }
    }
}

char* otio_image_seq_ref_get_target_url_base(OtioImageSeqRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return safe_strdup(typed->target_url_base());
    )
}

char* otio_image_seq_ref_get_name_prefix(OtioImageSeqRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return safe_strdup(typed->name_prefix());
    )
}

char* otio_image_seq_ref_get_name_suffix(OtioImageSeqRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return safe_strdup(typed->name_suffix());
    )
}

int32_t otio_image_seq_ref_get_start_frame(OtioImageSeqRef* ref) {
    if (!ref) return 1;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->start_frame();
    } catch (...) {
        return 1;
    }
}

int32_t otio_image_seq_ref_get_end_frame(OtioImageSeqRef* ref) {
    if (!ref) return 1;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->end_frame();
    } catch (...) {
        return 1;
    }
}

int32_t otio_image_seq_ref_get_frame_step(OtioImageSeqRef* ref) {
    if (!ref) return 1;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->frame_step();
    } catch (...) {
        return 1;
    }
}

double otio_image_seq_ref_get_rate(OtioImageSeqRef* ref) {
    if (!ref) return 1.0;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->rate();
    } catch (...) {
        return 1.0;
    }
}

int32_t otio_image_seq_ref_get_frame_zero_padding(OtioImageSeqRef* ref) {
    if (!ref) return 0;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->frame_zero_padding();
    } catch (...) {
        return 0;
    }
}

int32_t otio_image_seq_ref_get_missing_frame_policy(OtioImageSeqRef* ref) {
    if (!ref) return OTIO_MISSING_FRAME_ERROR;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return static_cast<int32_t>(typed->missing_frame_policy());
    } catch (...) {
        return OTIO_MISSING_FRAME_ERROR;
    }
}

void otio_image_seq_ref_set_target_url_base(OtioImageSeqRef* ref, const char* url_base) {
    if (!ref || !url_base) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_target_url_base(std::string(url_base));
    } catch (...) {
    }
}

void otio_image_seq_ref_set_name_prefix(OtioImageSeqRef* ref, const char* prefix) {
    if (!ref || !prefix) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_name_prefix(std::string(prefix));
    } catch (...) {
    }
}

void otio_image_seq_ref_set_name_suffix(OtioImageSeqRef* ref, const char* suffix) {
    if (!ref || !suffix) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_name_suffix(std::string(suffix));
    } catch (...) {
    }
}

void otio_image_seq_ref_set_start_frame(OtioImageSeqRef* ref, int32_t frame) {
    if (!ref) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_start_frame(frame);
    } catch (...) {
    }
}

void otio_image_seq_ref_set_frame_step(OtioImageSeqRef* ref, int32_t step) {
    if (!ref) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_frame_step(step);
    } catch (...) {
    }
}

void otio_image_seq_ref_set_rate(OtioImageSeqRef* ref, double rate) {
    if (!ref) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_rate(rate);
    } catch (...) {
    }
}

void otio_image_seq_ref_set_frame_zero_padding(OtioImageSeqRef* ref, int32_t padding) {
    if (!ref) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_frame_zero_padding(padding);
    } catch (...) {
    }
}

void otio_image_seq_ref_set_missing_frame_policy(OtioImageSeqRef* ref, int32_t policy) {
    if (!ref) return;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        otio::ImageSequenceReference::MissingFramePolicy p;
        switch (policy) {
            case OTIO_MISSING_FRAME_HOLD:
                p = otio::ImageSequenceReference::MissingFramePolicy::hold;
                break;
            case OTIO_MISSING_FRAME_BLACK:
                p = otio::ImageSequenceReference::MissingFramePolicy::black;
                break;
            default:
                p = otio::ImageSequenceReference::MissingFramePolicy::error;
                break;
        }
        typed->set_missing_frame_policy(p);
    } catch (...) {
    }
}

int32_t otio_image_seq_ref_number_of_images(OtioImageSeqRef* ref) {
    if (!ref) return 0;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        return typed->number_of_images_in_sequence();
    } catch (...) {
        return 0;
    }
}

int32_t otio_image_seq_ref_frame_for_time(OtioImageSeqRef* ref, OtioRationalTime time, OtioError* err) {
    if (!ref) {
        set_error(err, 1, "ImageSequenceReference is null");
        return 0;
    }
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        otio::ErrorStatus status;
        int32_t frame = typed->frame_for_time(to_otio_rt(time), &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return 0;
        }
        return frame;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return 0;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return 0;
    }
}

char* otio_image_seq_ref_target_url_for_image_number(OtioImageSeqRef* ref, int32_t image_number, OtioError* err) {
    if (!ref) {
        set_error(err, 1, "ImageSequenceReference is null");
        return nullptr;
    }
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        otio::ErrorStatus status;
        std::string url = typed->target_url_for_image_number(image_number, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return nullptr;
        }
        return safe_strdup(url);
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return nullptr;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return nullptr;
    }
}

int otio_image_seq_ref_set_available_range(OtioImageSeqRef* ref, OtioTimeRange range, OtioError* err) {
    OTIO_NULL_CHECK_ERR(ref, err, -1, "ImageSequenceReference is null");
    OTIO_TRY_INT(err,
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        typed->set_available_range(to_otio_tr(range));
    )
}

OtioTimeRange otio_image_seq_ref_get_available_range(OtioImageSeqRef* ref) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!ref) return zero;
    try {
        auto typed = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        auto range_opt = typed->available_range();
        if (!range_opt.has_value()) return zero;
        auto range = range_opt.value();
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (...) {
        return zero;
    }
}

int otio_clip_set_image_sequence_reference(OtioClip* clip, OtioImageSeqRef* ref, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto r = reinterpret_cast<otio::ImageSequenceReference*>(ref);
        c->set_media_reference(r);
        release_handle(r);
    )
}

void otio_image_seq_ref_set_metadata_string(OtioImageSeqRef* ref, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::ImageSequenceReference*>(ref), key, value);
}

char* otio_image_seq_ref_get_metadata_string(OtioImageSeqRef* ref, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::ImageSequenceReference*>(ref), key);
}

// ----------------------------------------------------------------------------
// GeneratorReference
// ----------------------------------------------------------------------------

OtioGeneratorRef* otio_generator_ref_create(const char* name, const char* generator_kind) {
    const char* n = name ? name : "";
    const char* gk = generator_kind ? generator_kind : "";
    OTIO_TRY_PTR(
        auto ref = new otio::GeneratorReference(n, gk);
        return reinterpret_cast<OtioGeneratorRef*>(retain_for_handle(ref));
    )
}

void otio_generator_ref_free(OtioGeneratorRef* ref) {
    if (ref) {
        try {
            auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
            release_handle(typed);
        } catch (...) {
        }
    }
}

char* otio_generator_ref_get_name(OtioGeneratorRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
        return safe_strdup(typed->name());
    )
}

char* otio_generator_ref_get_generator_kind(OtioGeneratorRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
        return safe_strdup(typed->generator_kind());
    )
}

void otio_generator_ref_set_generator_kind(OtioGeneratorRef* ref, const char* kind) {
    if (!ref || !kind) return;
    try {
        auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
        typed->set_generator_kind(std::string(kind));
    } catch (...) {
    }
}

int otio_generator_ref_set_available_range(OtioGeneratorRef* ref, OtioTimeRange range, OtioError* err) {
    OTIO_NULL_CHECK_ERR(ref, err, -1, "GeneratorReference is null");
    OTIO_TRY_INT(err,
        auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
        typed->set_available_range(to_otio_tr(range));
    )
}

OtioTimeRange otio_generator_ref_get_available_range(OtioGeneratorRef* ref) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!ref) return zero;
    try {
        auto typed = reinterpret_cast<otio::GeneratorReference*>(ref);
        auto range_opt = typed->available_range();
        if (!range_opt.has_value()) return zero;
        auto range = range_opt.value();
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (...) {
        return zero;
    }
}

void otio_generator_ref_set_metadata_string(OtioGeneratorRef* ref, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::GeneratorReference*>(ref), key, value);
}

char* otio_generator_ref_get_metadata_string(OtioGeneratorRef* ref, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::GeneratorReference*>(ref), key);
}

// ----------------------------------------------------------------------------
// LinearTimeWarp
// ----------------------------------------------------------------------------

OtioLinearTimeWarp* otio_linear_time_warp_create(const char* name, double time_scalar) {
    const char* n = name ? name : "";
    OTIO_TRY_PTR(
        auto effect = new otio::LinearTimeWarp(n, "", time_scalar);
        return reinterpret_cast<OtioLinearTimeWarp*>(retain_for_handle(effect));
    )
}

void otio_linear_time_warp_free(OtioLinearTimeWarp* effect) {
    if (effect) {
        try {
            auto typed = reinterpret_cast<otio::LinearTimeWarp*>(effect);
            release_handle(typed);
        } catch (...) {
        }
    }
}

char* otio_linear_time_warp_get_name(OtioLinearTimeWarp* effect) {
    OTIO_NULL_CHECK(effect, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::LinearTimeWarp*>(effect);
        return safe_strdup(typed->name());
    )
}

double otio_linear_time_warp_get_time_scalar(OtioLinearTimeWarp* effect) {
    if (!effect) return 1.0;
    try {
        auto typed = reinterpret_cast<otio::LinearTimeWarp*>(effect);
        return typed->time_scalar();
    } catch (...) {
        return 1.0;
    }
}

void otio_linear_time_warp_set_time_scalar(OtioLinearTimeWarp* effect, double scalar) {
    if (!effect) return;
    try {
        auto typed = reinterpret_cast<otio::LinearTimeWarp*>(effect);
        typed->set_time_scalar(scalar);
    } catch (...) {
    }
}

void otio_linear_time_warp_set_metadata_string(OtioLinearTimeWarp* effect, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::LinearTimeWarp*>(effect), key, value);
}

char* otio_linear_time_warp_get_metadata_string(OtioLinearTimeWarp* effect, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::LinearTimeWarp*>(effect), key);
}

// ----------------------------------------------------------------------------
// FreezeFrame
// ----------------------------------------------------------------------------

OtioFreezeFrame* otio_freeze_frame_create(const char* name) {
    const char* n = name ? name : "";
    OTIO_TRY_PTR(
        auto effect = new otio::FreezeFrame(n);
        return reinterpret_cast<OtioFreezeFrame*>(retain_for_handle(effect));
    )
}

void otio_freeze_frame_free(OtioFreezeFrame* effect) {
    if (effect) {
        try {
            auto typed = reinterpret_cast<otio::FreezeFrame*>(effect);
            release_handle(typed);
        } catch (...) {
        }
    }
}

char* otio_freeze_frame_get_name(OtioFreezeFrame* effect) {
    OTIO_NULL_CHECK(effect, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::FreezeFrame*>(effect);
        return safe_strdup(typed->name());
    )
}

void otio_freeze_frame_set_metadata_string(OtioFreezeFrame* effect, const char* key, const char* value) {
    set_metadata_string_impl(reinterpret_cast<otio::FreezeFrame*>(effect), key, value);
}

char* otio_freeze_frame_get_metadata_string(OtioFreezeFrame* effect, const char* key) {
    return get_metadata_string_impl(reinterpret_cast<otio::FreezeFrame*>(effect), key);
}

// ----------------------------------------------------------------------------
// Clip Marker/Effect attachment
// ----------------------------------------------------------------------------

int otio_clip_add_marker(OtioClip* clip, OtioMarker* marker, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(marker, err, -1, "Marker is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto m = reinterpret_cast<otio::Marker*>(marker);
        c->markers().push_back(m);
        release_handle(m);
    )
}

int32_t otio_clip_markers_count(OtioClip* clip) {
    if (!clip) return 0;
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        return static_cast<int32_t>(c->markers().size());
    } catch (...) {
        return 0;
    }
}

OtioMarker* otio_clip_marker_at(OtioClip* clip, int32_t index) {
    if (!clip) return nullptr;
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto& markers = c->markers();
        if (index < 0 || static_cast<size_t>(index) >= markers.size()) return nullptr;
        return reinterpret_cast<OtioMarker*>(markers[index].value);
    } catch (...) {
        return nullptr;
    }
}

int otio_clip_add_effect(OtioClip* clip, OtioEffect* effect, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(effect, err, -1, "Effect is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto e = reinterpret_cast<otio::Effect*>(effect);
        c->effects().push_back(e);
        release_handle(e);
    )
}

int32_t otio_clip_effects_count(OtioClip* clip) {
    if (!clip) return 0;
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        return static_cast<int32_t>(c->effects().size());
    } catch (...) {
        return 0;
    }
}

OtioEffect* otio_clip_effect_at(OtioClip* clip, int32_t index) {
    if (!clip) return nullptr;
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto& effects = c->effects();
        if (index < 0 || static_cast<size_t>(index) >= effects.size()) return nullptr;
        return reinterpret_cast<OtioEffect*>(effects[index].value);
    } catch (...) {
        return nullptr;
    }
}

int otio_clip_add_linear_time_warp(OtioClip* clip, OtioLinearTimeWarp* effect, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_NULL_CHECK_ERR(effect, err, -1, "LinearTimeWarp is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto e = reinterpret_cast<otio::LinearTimeWarp*>(effect);
        c->effects().push_back(e);
        release_handle(e);
    )
}

int otio_clip_set_missing_reference(OtioClip* clip, OtioMissingRef* ref, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto r = reinterpret_cast<otio::MissingReference*>(ref);
        c->set_media_reference(r);
        release_handle(r);
    )
}

int otio_clip_set_generator_reference(OtioClip* clip, OtioGeneratorRef* ref, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    OTIO_TRY_INT(err,
        auto c = reinterpret_cast<otio::Clip*>(clip);
        auto r = reinterpret_cast<otio::GeneratorReference*>(ref);
        c->set_media_reference(r);
        release_handle(r);
    )
}

// ----------------------------------------------------------------------------
// Track Marker attachment
// ----------------------------------------------------------------------------

int otio_track_add_marker(OtioTrack* track, OtioMarker* marker, OtioError* err) {
    OTIO_NULL_CHECK_ERR(track, err, -1, "Track is null");
    OTIO_NULL_CHECK_ERR(marker, err, -1, "Marker is null");
    OTIO_TRY_INT(err,
        auto t = reinterpret_cast<otio::Track*>(track);
        auto m = reinterpret_cast<otio::Marker*>(marker);
        t->markers().push_back(m);
        release_handle(m);
    )
}

int32_t otio_track_markers_count(OtioTrack* track) {
    if (!track) return 0;
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        return static_cast<int32_t>(t->markers().size());
    } catch (...) {
        return 0;
    }
}

OtioMarker* otio_track_marker_at(OtioTrack* track, int32_t index) {
    if (!track) return nullptr;
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto& markers = t->markers();
        if (index < 0 || static_cast<size_t>(index) >= markers.size()) return nullptr;
        return reinterpret_cast<OtioMarker*>(markers[index].value);
    } catch (...) {
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// Track kind
// ----------------------------------------------------------------------------

int32_t otio_track_get_kind(OtioTrack* track) {
    if (!track) return OTIO_TRACK_KIND_VIDEO;
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto kind = t->kind();
        if (kind == otio::Track::Kind::audio) return OTIO_TRACK_KIND_AUDIO;
        return OTIO_TRACK_KIND_VIDEO;
    } catch (...) {
        return OTIO_TRACK_KIND_VIDEO;
    }
}

void otio_track_set_kind(OtioTrack* track, int32_t kind) {
    if (!track) return;
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        if (kind == OTIO_TRACK_KIND_AUDIO) {
            t->set_kind(otio::Track::Kind::audio);
        } else {
            t->set_kind(otio::Track::Kind::video);
        }
    } catch (...) {
    }
}

// ----------------------------------------------------------------------------
// Time transforms
// ----------------------------------------------------------------------------

OtioTimeRange otio_track_range_of_child_at_index(OtioTrack* track, int32_t index, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!track) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Track is null", sizeof(err->message) - 1);
        }
        return zero;
    }
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto& children = t->children();
        if (index < 0 || static_cast<size_t>(index) >= children.size()) {
            if (err) {
                err->code = -1;
                strncpy(err->message, "Index out of bounds", sizeof(err->message) - 1);
            }
            return zero;
        }
        otio::ErrorStatus status;
        auto range = t->range_of_child_at_index(index, &status);
        if (otio::is_error(status)) {
            if (err) {
                err->code = static_cast<int>(status.outcome);
                strncpy(err->message, status.details.c_str(), sizeof(err->message) - 1);
            }
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        if (err) {
            err->code = -1;
            strncpy(err->message, e.what(), sizeof(err->message) - 1);
        }
        return zero;
    } catch (...) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Unknown error", sizeof(err->message) - 1);
        }
        return zero;
    }
}

OtioTimeRange otio_stack_range_of_child_at_index(OtioStack* stack, int32_t index, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!stack) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Stack is null", sizeof(err->message) - 1);
        }
        return zero;
    }
    try {
        auto s = reinterpret_cast<otio::Stack*>(stack);
        auto& children = s->children();
        if (index < 0 || static_cast<size_t>(index) >= children.size()) {
            if (err) {
                err->code = -1;
                strncpy(err->message, "Index out of bounds", sizeof(err->message) - 1);
            }
            return zero;
        }
        otio::ErrorStatus status;
        auto range = s->range_of_child_at_index(index, &status);
        if (otio::is_error(status)) {
            if (err) {
                err->code = static_cast<int>(status.outcome);
                strncpy(err->message, status.details.c_str(), sizeof(err->message) - 1);
            }
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        if (err) {
            err->code = -1;
            strncpy(err->message, e.what(), sizeof(err->message) - 1);
        }
        return zero;
    } catch (...) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Unknown error", sizeof(err->message) - 1);
        }
        return zero;
    }
}

OtioTimeRange otio_track_trimmed_range(OtioTrack* track, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!track) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Track is null", sizeof(err->message) - 1);
        }
        return zero;
    }
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        otio::ErrorStatus status;
        auto range = t->trimmed_range(&status);
        if (otio::is_error(status)) {
            if (err) {
                err->code = static_cast<int>(status.outcome);
                strncpy(err->message, status.details.c_str(), sizeof(err->message) - 1);
            }
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        if (err) {
            err->code = -1;
            strncpy(err->message, e.what(), sizeof(err->message) - 1);
        }
        return zero;
    } catch (...) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Unknown error", sizeof(err->message) - 1);
        }
        return zero;
    }
}

OtioTimeRange otio_stack_trimmed_range(OtioStack* stack, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!stack) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Stack is null", sizeof(err->message) - 1);
        }
        return zero;
    }
    try {
        auto s = reinterpret_cast<otio::Stack*>(stack);
        otio::ErrorStatus status;
        auto range = s->trimmed_range(&status);
        if (otio::is_error(status)) {
            if (err) {
                err->code = static_cast<int>(status.outcome);
                strncpy(err->message, status.details.c_str(), sizeof(err->message) - 1);
            }
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        if (err) {
            err->code = -1;
            strncpy(err->message, e.what(), sizeof(err->message) - 1);
        }
        return zero;
    } catch (...) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Unknown error", sizeof(err->message) - 1);
        }
        return zero;
    }
}

// ----------------------------------------------------------------------------
// ExternalReference additional accessors
// ----------------------------------------------------------------------------

char* otio_external_ref_get_target_url(OtioExternalRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::ExternalReference*>(ref);
        return safe_strdup(typed->target_url());
    )
}

OtioTimeRange otio_external_ref_get_available_range(OtioExternalRef* ref) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!ref) return zero;
    try {
        auto typed = reinterpret_cast<otio::ExternalReference*>(ref);
        auto range_opt = typed->available_range();
        if (!range_opt.has_value()) return zero;
        auto range = range_opt.value();
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (...) {
        return zero;
    }
}

char* otio_external_ref_get_name(OtioExternalRef* ref) {
    OTIO_NULL_CHECK(ref, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::ExternalReference*>(ref);
        return safe_strdup(typed->name());
    )
}

void otio_external_ref_set_name(OtioExternalRef* ref, const char* name) {
    if (!ref || !name) return;
    try {
        auto typed = reinterpret_cast<otio::ExternalReference*>(ref);
        typed->set_name(std::string(name));
    } catch (...) {
        // Ignore exceptions
    }
}

// ----------------------------------------------------------------------------
// Timeline additional accessors
// ----------------------------------------------------------------------------

char* otio_timeline_get_name(OtioTimeline* tl) {
    OTIO_NULL_CHECK(tl, nullptr);
    OTIO_TRY_PTR(
        auto typed = reinterpret_cast<otio::Timeline*>(tl);
        return safe_strdup(typed->name());
    )
}

OtioRationalTime otio_timeline_get_global_start_time(OtioTimeline* tl) {
    OtioRationalTime zero = {0, 1};
    if (!tl) return zero;
    try {
        auto typed = reinterpret_cast<otio::Timeline*>(tl);
        auto time_opt = typed->global_start_time();
        if (!time_opt.has_value()) return zero;
        auto time = time_opt.value();
        return OtioRationalTime{time.value(), time.rate()};
    } catch (...) {
        return zero;
    }
}

OtioTimeRange otio_timeline_get_duration(OtioTimeline* tl, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!tl) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Timeline is null", sizeof(err->message) - 1);
        }
        return zero;
    }
    try {
        auto typed = reinterpret_cast<otio::Timeline*>(tl);
        otio::ErrorStatus status;
        auto duration = typed->duration(&status);
        if (otio::is_error(status)) {
            if (err) {
                err->code = static_cast<int>(status.outcome);
                strncpy(err->message, status.details.c_str(), sizeof(err->message) - 1);
            }
            return zero;
        }
        // Return as a TimeRange with start at 0
        return OtioTimeRange{
            OtioRationalTime{0, duration.rate()},
            OtioRationalTime{duration.value(), duration.rate()}
        };
    } catch (const std::exception& e) {
        if (err) {
            err->code = -1;
            strncpy(err->message, e.what(), sizeof(err->message) - 1);
        }
        return zero;
    } catch (...) {
        if (err) {
            err->code = -1;
            strncpy(err->message, "Unknown error", sizeof(err->message) - 1);
        }
        return zero;
    }
}

// ----------------------------------------------------------------------------
// Edit Algorithms
// ----------------------------------------------------------------------------

int otio_track_overwrite(OtioTrack* track, OtioClip* clip,
    OtioTimeRange range, int remove_transitions, OtioError* err) {
    OTIO_NULL_CHECK_ERR(track, err, -1, "Track is null");
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        otio::algo::overwrite(c, t, to_otio_tr(range), remove_transitions != 0, nullptr, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        release_handle(c);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_track_insert_at_time(OtioTrack* track, OtioClip* clip,
    OtioRationalTime time, int remove_transitions, OtioError* err) {
    OTIO_NULL_CHECK_ERR(track, err, -1, "Track is null");
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        otio::algo::insert(c, t, to_otio_rt(time), remove_transitions != 0, nullptr, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        release_handle(c);
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_track_slice_at_time(OtioTrack* track, OtioRationalTime time,
    int remove_transitions, OtioError* err) {
    OTIO_NULL_CHECK_ERR(track, err, -1, "Track is null");
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        otio::ErrorStatus status;
        otio::algo::slice(t, to_otio_rt(time), remove_transitions != 0, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_slip(OtioClip* clip, OtioRationalTime delta, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::algo::slip(c, to_otio_rt(delta));
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_slide(OtioClip* clip, OtioRationalTime delta, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::algo::slide(c, to_otio_rt(delta));
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_trim(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        otio::algo::trim(c, to_otio_rt(delta_in), to_otio_rt(delta_out), nullptr, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_ripple(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        otio::algo::ripple(c, to_otio_rt(delta_in), to_otio_rt(delta_out), &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_clip_roll(OtioClip* clip, OtioRationalTime delta_in,
    OtioRationalTime delta_out, OtioError* err) {
    OTIO_NULL_CHECK_ERR(clip, err, -1, "Clip is null");
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        otio::algo::roll(c, to_otio_rt(delta_in), to_otio_rt(delta_out), &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

int otio_track_remove_at_time(OtioTrack* track, OtioRationalTime time,
    int fill_with_gap, OtioError* err) {
    OTIO_NULL_CHECK_ERR(track, err, -1, "Track is null");
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        otio::ErrorStatus status;
        otio::algo::remove(t, to_otio_rt(time), fill_with_gap != 0, nullptr, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return -1;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return -1;
    }
}

// ----------------------------------------------------------------------------
// Time coordinate transforms
// ----------------------------------------------------------------------------

// Helper to cast void* to Item* based on type
static otio::Item* cast_to_item(void* ptr, int32_t type) {
    if (!ptr) return nullptr;
    switch (type) {
        case OTIO_CHILD_TYPE_CLIP:
            return reinterpret_cast<otio::Clip*>(ptr);
        case OTIO_CHILD_TYPE_GAP:
            return reinterpret_cast<otio::Gap*>(ptr);
        case OTIO_CHILD_TYPE_TRACK:
            return reinterpret_cast<otio::Track*>(ptr);
        case OTIO_CHILD_TYPE_STACK:
            return reinterpret_cast<otio::Stack*>(ptr);
        default:
            return nullptr;
    }
}

OtioRationalTime otio_item_transformed_time(void* item, int32_t item_type,
    OtioRationalTime time, void* to_item, int32_t to_item_type, OtioError* err) {
    OtioRationalTime zero = {0, 1};

    otio::Item* from_item = cast_to_item(item, item_type);
    otio::Item* target_item = cast_to_item(to_item, to_item_type);

    if (!from_item) {
        set_error(err, 1, "Source item is null or invalid type");
        return zero;
    }
    if (!target_item) {
        set_error(err, 1, "Target item is null or invalid type");
        return zero;
    }

    try {
        otio::ErrorStatus status;
        auto result = from_item->transformed_time(to_otio_rt(time), target_item, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return zero;
        }
        return OtioRationalTime{result.value(), result.rate()};
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return zero;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return zero;
    }
}

OtioTimeRange otio_item_transformed_time_range(void* item, int32_t item_type,
    OtioTimeRange range, void* to_item, int32_t to_item_type, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};

    otio::Item* from_item = cast_to_item(item, item_type);
    otio::Item* target_item = cast_to_item(to_item, to_item_type);

    if (!from_item) {
        set_error(err, 1, "Source item is null or invalid type");
        return zero;
    }
    if (!target_item) {
        set_error(err, 1, "Target item is null or invalid type");
        return zero;
    }

    try {
        otio::ErrorStatus status;
        auto result = from_item->transformed_time_range(to_otio_tr(range), target_item, &status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{result.start_time().value(), result.start_time().rate()},
            OtioRationalTime{result.duration().value(), result.duration().rate()}
        };
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return zero;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return zero;
    }
}

OtioTimeRange otio_clip_range_in_parent(OtioClip* clip, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!clip) {
        set_error(err, 1, "Clip is null");
        return zero;
    }
    try {
        auto c = reinterpret_cast<otio::Clip*>(clip);
        otio::ErrorStatus status;
        auto range = c->range_in_parent(&status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return zero;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return zero;
    }
}

OtioTimeRange otio_gap_range_in_parent(OtioGap* gap, OtioError* err) {
    OtioTimeRange zero = {OtioRationalTime{0, 1}, OtioRationalTime{0, 1}};
    if (!gap) {
        set_error(err, 1, "Gap is null");
        return zero;
    }
    try {
        auto g = reinterpret_cast<otio::Gap*>(gap);
        otio::ErrorStatus status;
        auto range = g->range_in_parent(&status);
        if (otio::is_error(status)) {
            set_error(err, 1, status.full_description.c_str());
            return zero;
        }
        return OtioTimeRange{
            OtioRationalTime{range.start_time().value(), range.start_time().rate()},
            OtioRationalTime{range.duration().value(), range.duration().rate()}
        };
    } catch (const std::exception& e) {
        set_error(err, 1, e.what());
        return zero;
    } catch (...) {
        set_error(err, 1, "Unknown exception");
        return zero;
    }
}

// ----------------------------------------------------------------------------
// Parent navigation
// ----------------------------------------------------------------------------

int32_t otio_clip_get_parent_type(OtioClip* clip) {
    return get_parent_type_impl(reinterpret_cast<otio::Clip*>(clip));
}

void* otio_clip_get_parent(OtioClip* clip) {
    return get_parent_impl(reinterpret_cast<otio::Clip*>(clip));
}

int32_t otio_gap_get_parent_type(OtioGap* gap) {
    return get_parent_type_impl(reinterpret_cast<otio::Gap*>(gap));
}

void* otio_gap_get_parent(OtioGap* gap) {
    return get_parent_impl(reinterpret_cast<otio::Gap*>(gap));
}

int32_t otio_transition_get_parent_type(OtioTransition* transition) {
    return get_parent_type_impl(reinterpret_cast<otio::Transition*>(transition));
}

void* otio_transition_get_parent(OtioTransition* transition) {
    return get_parent_impl(reinterpret_cast<otio::Transition*>(transition));
}

int32_t otio_track_get_parent_type(OtioTrack* track) {
    return get_parent_type_impl(reinterpret_cast<otio::Track*>(track));
}

void* otio_track_get_parent(OtioTrack* track) {
    return get_parent_impl(reinterpret_cast<otio::Track*>(track));
}

int32_t otio_stack_get_parent_type(OtioStack* stack) {
    return get_parent_type_impl(reinterpret_cast<otio::Stack*>(stack));
}

void* otio_stack_get_parent(OtioStack* stack) {
    return get_parent_impl(reinterpret_cast<otio::Stack*>(stack));
}

// ----------------------------------------------------------------------------
// Track Iterator (filtered track lists)
// ----------------------------------------------------------------------------

struct OtioTrackIterator {
    std::vector<otio::Track*> tracks;
    size_t index;

    OtioTrackIterator() : index(0) {}
};

OtioTrackIterator* otio_timeline_video_tracks(OtioTimeline* tl) {
    if (!tl) return nullptr;
    try {
        auto timeline = reinterpret_cast<otio::Timeline*>(tl);
        auto iter = new OtioTrackIterator();
        for (auto* track : timeline->video_tracks()) {
            iter->tracks.push_back(track);
        }
        return iter;
    } catch (...) {
        return nullptr;
    }
}

OtioTrackIterator* otio_timeline_audio_tracks(OtioTimeline* tl) {
    if (!tl) return nullptr;
    try {
        auto timeline = reinterpret_cast<otio::Timeline*>(tl);
        auto iter = new OtioTrackIterator();
        for (auto* track : timeline->audio_tracks()) {
            iter->tracks.push_back(track);
        }
        return iter;
    } catch (...) {
        return nullptr;
    }
}

int32_t otio_track_iterator_count(OtioTrackIterator* iter) {
    if (!iter) return 0;
    return static_cast<int32_t>(iter->tracks.size());
}

OtioTrack* otio_track_iterator_next(OtioTrackIterator* iter) {
    if (!iter || iter->index >= iter->tracks.size()) return nullptr;
    return reinterpret_cast<OtioTrack*>(iter->tracks[iter->index++]);
}

void otio_track_iterator_reset(OtioTrackIterator* iter) {
    if (iter) iter->index = 0;
}

void otio_track_iterator_free(OtioTrackIterator* iter) {
    delete iter;
}

// ----------------------------------------------------------------------------
// Clip Iterator (find_clips search)
// ----------------------------------------------------------------------------

struct OtioClipIterator {
    std::vector<otio::Clip*> clips;
    size_t index;

    OtioClipIterator() : index(0) {}
};

OtioClipIterator* otio_track_find_clips(OtioTrack* track) {
    if (!track) return nullptr;
    try {
        auto t = reinterpret_cast<otio::Track*>(track);
        auto iter = new OtioClipIterator();

        // Iterate through children and collect clips
        for (auto& child : t->children()) {
            if (auto clip = dynamic_cast<otio::Clip*>(child.value)) {
                iter->clips.push_back(clip);
            }
        }
        return iter;
    } catch (...) {
        return nullptr;
    }
}

OtioClipIterator* otio_stack_find_clips(OtioStack* stack) {
    if (!stack) return nullptr;
    try {
        auto s = reinterpret_cast<otio::Stack*>(stack);
        auto iter = new OtioClipIterator();
        find_clips_recursive(s, iter->clips);
        return iter;
    } catch (...) {
        return nullptr;
    }
}

OtioClipIterator* otio_timeline_find_clips(OtioTimeline* timeline) {
    if (!timeline) return nullptr;
    try {
        auto tl = reinterpret_cast<otio::Timeline*>(timeline);
        auto iter = new OtioClipIterator();
        find_clips_recursive(tl->tracks(), iter->clips);
        return iter;
    } catch (...) {
        return nullptr;
    }
}

int32_t otio_clip_iterator_count(OtioClipIterator* iter) {
    if (!iter) return 0;
    return static_cast<int32_t>(iter->clips.size());
}

OtioClip* otio_clip_iterator_next(OtioClipIterator* iter) {
    if (!iter || iter->index >= iter->clips.size()) return nullptr;
    return reinterpret_cast<OtioClip*>(iter->clips[iter->index++]);
}

void otio_clip_iterator_reset(OtioClipIterator* iter) {
    if (iter) iter->index = 0;
}

void otio_clip_iterator_free(OtioClipIterator* iter) {
    delete iter;
}

} // extern "C"
