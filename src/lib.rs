//! # `otio-rs`
//!
//! Rust bindings to [OpenTimelineIO](https://opentimeline.io/) - an open-source
//! API and interchange format for editorial timeline information.
//!
//! ## Example
//!
//! ```no_run
//! use otio_rs::{Timeline, Track, Clip, RationalTime, TimeRange};
//!
//! let mut timeline = Timeline::new("My Timeline");
//! timeline.set_global_start_time(RationalTime::new(0.0, 24.0)).unwrap();
//!
//! let mut video_track = timeline.add_video_track("V1");
//!
//! let source_range = TimeRange::new(
//!     RationalTime::new(0.0, 24.0),
//!     RationalTime::new(48.0, 24.0), // 2 seconds at 24fps
//! );
//! let clip = Clip::new("My Clip", source_range);
//! video_track.append_clip(clip).unwrap();
//!
//! timeline.write_to_file(std::path::Path::new("output.otio")).unwrap();
//! ```

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
// FFI wrappers use unwrap for CString which can only panic on interior nulls
#![allow(clippy::missing_panics_doc)]

mod ffi {
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod macros;
mod traits;
pub use traits::HasMetadata;

mod types;
pub use types::*;

mod iterators;
use iterators::composable_from_ffi;
pub use iterators::{
    ClipRef, ClipSearchIter, Composable, GapRef, ParentRef, StackChildIter, StackRef,
    TrackChildIter, TrackIter, TrackRef, TransitionRef,
};

mod builders;
pub use builders::{ClipBuilder, ExternalReferenceBuilder, TimelineBuilder};

pub mod marker;
pub use marker::Marker;

mod effect;
pub use effect::Effect;

pub mod transition;
pub use transition::Transition;

mod missing_reference;
pub use missing_reference::MissingReference;

pub mod generator_reference;
pub use generator_reference::GeneratorReference;

pub mod image_sequence_reference;
pub use image_sequence_reference::ImageSequenceReference;

mod time_effect;
pub use time_effect::{FreezeFrame, LinearTimeWarp};

use std::ffi::{CStr, CString};
use std::path::Path;

/// Error type for OTIO operations.
#[derive(Debug)]
pub struct OtioError {
    pub code: i32,
    pub message: String,
}

impl std::fmt::Display for OtioError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "OTIO error {}: {}", self.code, self.message)
    }
}

impl std::error::Error for OtioError {}

impl From<ffi::OtioError> for OtioError {
    fn from(e: ffi::OtioError) -> Self {
        let message = unsafe {
            CStr::from_ptr(e.message.as_ptr())
                .to_string_lossy()
                .into_owned()
        };
        OtioError {
            code: e.code,
            message,
        }
    }
}

// ============================================================================
// FFI Helper Functions
// ============================================================================

/// Convert an FFI string pointer to a Rust String, freeing the pointer.
///
/// Returns an empty string if the pointer is null.
///
/// # Safety
///
/// The pointer must be either null or a valid C string allocated by the FFI layer.
pub(crate) fn ffi_string_to_rust(ptr: *mut std::ffi::c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    // SAFETY: We checked for null above, and the FFI contract guarantees
    // the pointer is a valid null-terminated C string.
    let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
    unsafe { ffi::otio_free_string(ptr) };
    result
}

/// Check if an FFI `RationalTime` represents an unset/sentinel value.
///
/// The FFI layer uses rate=1.0, value=0.0 as a sentinel for "not set".
#[allow(clippy::float_cmp)] // Sentinel value comparison is intentional
pub(crate) fn is_unset_rational_time(rt: &ffi::OtioRationalTime) -> bool {
    rt.rate == 1.0 && rt.value == 0.0
}

/// Check if an FFI `TimeRange` represents an unset/sentinel value.
///
/// The FFI layer uses duration.rate=1.0, duration.value=0.0 as a sentinel for "not set".
#[allow(clippy::float_cmp)] // Sentinel value comparison is intentional
pub(crate) fn is_unset_time_range(tr: &ffi::OtioTimeRange) -> bool {
    tr.duration.rate == 1.0 && tr.duration.value == 0.0
}

/// Convert an FFI `OtioTimeRange` to a Rust `TimeRange`.
pub(crate) fn time_range_from_ffi(ffi_range: &ffi::OtioTimeRange) -> TimeRange {
    TimeRange::new(
        RationalTime::new(ffi_range.start_time.value, ffi_range.start_time.rate),
        RationalTime::new(ffi_range.duration.value, ffi_range.duration.rate),
    )
}

// ============================================================================
// Core Types
// ============================================================================

/// A rational time value with a rate.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RationalTime {
    pub value: f64,
    pub rate: f64,
}

impl RationalTime {
    /// Create a new `RationalTime` with the given value and rate.
    #[must_use]
    pub fn new(value: f64, rate: f64) -> Self {
        Self { value, rate }
    }

    /// Create a `RationalTime` from seconds at the given rate.
    #[must_use]
    pub fn from_seconds(seconds: f64, rate: f64) -> Self {
        Self {
            value: seconds * rate,
            rate,
        }
    }

    /// Convert to seconds.
    #[must_use]
    pub fn to_seconds(self) -> f64 {
        self.value / self.rate
    }
}

impl From<RationalTime> for ffi::OtioRationalTime {
    fn from(rt: RationalTime) -> Self {
        ffi::OtioRationalTime {
            value: rt.value,
            rate: rt.rate,
        }
    }
}

/// A time range with start time and duration.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TimeRange {
    pub start_time: RationalTime,
    pub duration: RationalTime,
}

impl TimeRange {
    /// Create a new `TimeRange` with the given start time and duration.
    #[must_use]
    pub fn new(start_time: RationalTime, duration: RationalTime) -> Self {
        Self {
            start_time,
            duration,
        }
    }

    /// Get the end time of this range.
    #[must_use]
    pub fn end_time(&self) -> RationalTime {
        RationalTime::new(
            self.start_time.value + self.duration.value,
            self.start_time.rate,
        )
    }
}

impl From<TimeRange> for ffi::OtioTimeRange {
    fn from(tr: TimeRange) -> Self {
        ffi::OtioTimeRange {
            start_time: tr.start_time.into(),
            duration: tr.duration.into(),
        }
    }
}

/// A timeline is the top-level container for editorial content.
pub struct Timeline {
    ptr: *mut ffi::OtioTimeline,
}

impl std::fmt::Debug for Timeline {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Timeline")
            .field("name", &self.name())
            .finish()
    }
}

impl Timeline {
    /// Create a new timeline with the given name.
    #[must_use]
    pub fn new(name: &str) -> Self {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_timeline_create(c_name.as_ptr()) };
        Self { ptr }
    }

    /// Set the global start time of the timeline.
    ///
    /// # Errors
    ///
    /// Returns an error if the global start time cannot be set.
    pub fn set_global_start_time(&mut self, time: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_timeline_set_global_start_time(self.ptr, time.into(), &mut err) };
        if result != 0 {
            Err(err.into())
        } else {
            Ok(())
        }
    }

    /// Add a video track to the timeline.
    #[must_use]
    pub fn add_video_track(&mut self, name: &str) -> Track {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_timeline_add_video_track(self.ptr, c_name.as_ptr()) };
        Track { ptr, owned: false } // Timeline owns this track
    }

    /// Add an audio track to the timeline.
    #[must_use]
    pub fn add_audio_track(&mut self, name: &str) -> Track {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_timeline_add_audio_track(self.ptr, c_name.as_ptr()) };
        Track { ptr, owned: false } // Timeline owns this track
    }

    /// Write the timeline to a JSON file.
    ///
    /// # Errors
    ///
    /// Returns an error if the file cannot be written.
    pub fn write_to_file(&self, path: &Path) -> Result<()> {
        let c_path = CString::new(path.to_string_lossy().as_ref()).unwrap();
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_timeline_write_to_file(self.ptr, c_path.as_ptr(), &mut err) };
        if result != 0 {
            Err(err.into())
        } else {
            Ok(())
        }
    }

    /// Read a timeline from a JSON file.
    ///
    /// # Errors
    ///
    /// Returns an error if the file cannot be read or parsed.
    pub fn read_from_file(path: &Path) -> Result<Self> {
        let c_path = CString::new(path.to_string_lossy().as_ref()).unwrap();
        let mut err = macros::ffi_error!();
        let ptr = unsafe { ffi::otio_timeline_read_from_file(c_path.as_ptr(), &mut err) };
        if ptr.is_null() {
            Err(err.into())
        } else {
            Ok(Self { ptr })
        }
    }

    /// Serialize this timeline to a JSON string.
    ///
    /// # Errors
    ///
    /// Returns an error if the timeline cannot be serialized.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use otio_rs::Timeline;
    ///
    /// let timeline = Timeline::new("My Timeline");
    /// let json = timeline.to_json_string().unwrap();
    /// println!("Timeline JSON: {}", json);
    /// ```
    pub fn to_json_string(&self) -> Result<String> {
        let mut err = macros::ffi_error!();
        let ptr = unsafe { ffi::otio_timeline_to_json_string(self.ptr, &mut err) };
        if ptr.is_null() {
            return Err(err.into());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { ffi::otio_free_string(ptr) };
        Ok(result)
    }

    /// Write the timeline to a JSON file with schema version targeting.
    ///
    /// The `schema_versions` parameter specifies target schema versions for
    /// downgrading. Pass an empty slice for no downgrading (equivalent to `write_to_file`).
    ///
    /// # Arguments
    ///
    /// * `path` - Output file path
    /// * `schema_versions` - Slice of (`schema_name`, version) pairs
    ///
    /// # Errors
    ///
    /// Returns an error if the file cannot be written.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use otio_rs::Timeline;
    /// use std::path::Path;
    ///
    /// let timeline = Timeline::new("My Timeline");
    ///
    /// // Write with Clip schema downgraded to version 1
    /// timeline.write_to_file_with_schema_versions(
    ///     Path::new("output.otio"),
    ///     &[("Clip", 1)]
    /// ).unwrap();
    /// ```
    #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
    pub fn write_to_file_with_schema_versions(
        &self,
        path: &Path,
        schema_versions: &[(&str, i64)],
    ) -> Result<()> {
        let c_path = CString::new(path.to_string_lossy().as_ref()).unwrap();

        if schema_versions.is_empty() {
            // No schema versions specified, use regular write
            let mut err = macros::ffi_error!();
            let result =
                unsafe { ffi::otio_timeline_write_to_file(self.ptr, c_path.as_ptr(), &mut err) };
            return if result != 0 { Err(err.into()) } else { Ok(()) };
        }

        let names: Vec<CString> = schema_versions
            .iter()
            .map(|(name, _)| CString::new(*name).unwrap())
            .collect();
        let mut name_ptrs: Vec<*const std::ffi::c_char> =
            names.iter().map(|s| s.as_ptr()).collect();
        let versions: Vec<i64> = schema_versions.iter().map(|(_, v)| *v).collect();

        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_timeline_write_to_file_with_schema_versions(
                self.ptr,
                c_path.as_ptr(),
                name_ptrs.as_mut_ptr(),
                versions.as_ptr(),
                schema_versions.len() as i32,
                &mut err,
            )
        };
        if result != 0 {
            Err(err.into())
        } else {
            Ok(())
        }
    }

    /// Serialize the timeline to a JSON string with schema version targeting.
    ///
    /// The `schema_versions` parameter specifies target schema versions for
    /// downgrading. Pass an empty slice for no downgrading (equivalent to `to_json_string`).
    ///
    /// # Arguments
    ///
    /// * `schema_versions` - Slice of (`schema_name`, version) pairs
    ///
    /// # Errors
    ///
    /// Returns an error if serialization fails.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use otio_rs::Timeline;
    ///
    /// let timeline = Timeline::new("My Timeline");
    ///
    /// // Serialize with Clip schema downgraded to version 1
    /// let json = timeline.to_json_string_with_schema_versions(&[("Clip", 1)]).unwrap();
    /// ```
    #[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
    pub fn to_json_string_with_schema_versions(
        &self,
        schema_versions: &[(&str, i64)],
    ) -> Result<String> {
        if schema_versions.is_empty() {
            return self.to_json_string();
        }

        let names: Vec<CString> = schema_versions
            .iter()
            .map(|(name, _)| CString::new(*name).unwrap())
            .collect();
        let mut name_ptrs: Vec<*const std::ffi::c_char> =
            names.iter().map(|s| s.as_ptr()).collect();
        let versions: Vec<i64> = schema_versions.iter().map(|(_, v)| *v).collect();

        let mut err = macros::ffi_error!();
        let ptr = unsafe {
            ffi::otio_timeline_to_json_string_with_schema_versions(
                self.ptr,
                name_ptrs.as_mut_ptr(),
                versions.as_ptr(),
                schema_versions.len() as i32,
                &mut err,
            )
        };
        if ptr.is_null() {
            return Err(err.into());
        }
        let result = unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() };
        unsafe { ffi::otio_free_string(ptr) };
        Ok(result)
    }

    /// Deserialize a timeline from a JSON string.
    ///
    /// # Errors
    ///
    /// Returns an error if the JSON cannot be parsed or doesn't contain a timeline.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use otio_rs::Timeline;
    ///
    /// let json = r#"{"OTIO_SCHEMA": "Timeline.1", "name": "Test"}"#;
    /// let timeline = Timeline::from_json_string(json).unwrap();
    /// ```
    pub fn from_json_string(json: &str) -> Result<Self> {
        let c_json = CString::new(json).unwrap();
        let mut err = macros::ffi_error!();
        let ptr = unsafe { ffi::otio_timeline_from_json_string(c_json.as_ptr(), &mut err) };
        if ptr.is_null() {
            Err(err.into())
        } else {
            Ok(Self { ptr })
        }
    }

    /// Get the root stack (tracks container) for this timeline.
    ///
    /// The returned `StackRef` is a non-owning reference to the timeline's stack.
    /// Use `tracks().children()` to iterate over the tracks.
    #[must_use]
    pub fn tracks(&self) -> StackRef<'_> {
        let ptr = unsafe { ffi::otio_timeline_get_tracks(self.ptr) };
        StackRef::new(ptr)
    }

    /// Get the name of this timeline.
    #[must_use]
    pub fn name(&self) -> String {
        let ptr = unsafe { ffi::otio_timeline_get_name(self.ptr) };
        ffi_string_to_rust(ptr)
    }

    /// Get the global start time of this timeline.
    ///
    /// Returns `None` if no global start time has been set.
    #[must_use]
    pub fn global_start_time(&self) -> Option<RationalTime> {
        let rt = unsafe { ffi::otio_timeline_get_global_start_time(self.ptr) };
        if is_unset_rational_time(&rt) {
            return None;
        }
        Some(RationalTime::new(rt.value, rt.rate))
    }

    /// Get the duration of this timeline.
    ///
    /// The duration is computed from the timeline's tracks.
    ///
    /// # Errors
    ///
    /// Returns an error if the duration cannot be computed.
    pub fn duration(&self) -> Result<RationalTime> {
        let mut err = macros::ffi_error!();
        let range = unsafe { ffi::otio_timeline_get_duration(self.ptr, &mut err) };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(RationalTime::new(range.duration.value, range.duration.rate))
    }

    /// Get all video tracks in this timeline.
    ///
    /// Returns an iterator over video tracks only.
    #[must_use]
    pub fn video_tracks(&self) -> iterators::TrackIter<'_> {
        let ptr = unsafe { ffi::otio_timeline_video_tracks(self.ptr) };
        iterators::TrackIter::new(ptr)
    }

    /// Get all audio tracks in this timeline.
    ///
    /// Returns an iterator over audio tracks only.
    #[must_use]
    pub fn audio_tracks(&self) -> iterators::TrackIter<'_> {
        let ptr = unsafe { ffi::otio_timeline_audio_tracks(self.ptr) };
        iterators::TrackIter::new(ptr)
    }

    /// Find all clips in this timeline (recursively).
    ///
    /// Returns an iterator over all clips found in the timeline's tracks
    /// and any nested compositions.
    #[must_use]
    pub fn find_clips(&self) -> ClipSearchIter<'_> {
        let ptr = unsafe { ffi::otio_timeline_find_clips(self.ptr) };
        ClipSearchIter::new(ptr)
    }
}

traits::impl_has_metadata!(Timeline, otio_timeline_set_metadata_string, otio_timeline_get_metadata_string);

impl Drop for Timeline {
    fn drop(&mut self) {
        unsafe { ffi::otio_timeline_free(self.ptr) }
    }
}

// Safety: Timeline is safe to send between threads
unsafe impl Send for Timeline {}

// ============================================================================
// Track Neighbor Types
// ============================================================================

/// Policy for including gaps when getting neighbors of a child in a track.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NeighborGapPolicy {
    /// Never include gaps as neighbors.
    #[default]
    Never = 0,
    /// Include gaps around transitions.
    AroundTransitions = 1,
}

/// The neighbors of a composable item in a track.
///
/// Returned by [`Track::neighbors_of`] to provide access to the items
/// immediately before and after a given child.
#[derive(Debug)]
pub struct Neighbors<'a> {
    /// The item before the queried child, if any.
    pub left: Option<Neighbor<'a>>,
    /// The item after the queried child, if any.
    pub right: Option<Neighbor<'a>>,
}

/// One side of [`Neighbors`].
#[derive(Debug)]
pub enum Neighbor<'a> {
    /// A child of the track, borrowed from it.
    Child(Composable<'a>),
    /// A gap that OpenTimelineIO made up to cover a transition's offset under
    /// [`NeighborGapPolicy::AroundTransitions`]. It is not in the track, so
    /// the caller owns it.
    GeneratedGap(Gap),
}

impl Neighbor<'_> {
    fn from_ffi(ptr: *mut std::ffi::c_void, child_type: i32, generated: i32) -> Option<Self> {
        if generated != 0 {
            // the shim gave the generated gap a reference that this Gap now owns
            return (!ptr.is_null()).then(|| Neighbor::GeneratedGap(Gap { ptr: ptr.cast() }));
        }

        composable_from_ffi(ptr, child_type).map(Neighbor::Child)
    }
}

/// A track contains clips, gaps, and other items.
///
/// Tracks can be created standalone or added to a Timeline. When created
/// standalone, the Track owns its memory. When added to a Timeline or Stack,
/// ownership transfers to the parent.
pub struct Track {
    ptr: *mut ffi::OtioTrack,
    owned: bool,
}

impl std::fmt::Debug for Track {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Track")
            .field("kind", &self.kind())
            .field("children_count", &self.children_count())
            .finish()
    }
}

impl Track {
    /// Create a new video track with the given name.
    #[must_use]
    pub fn new_video(name: &str) -> Self {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_track_create_video(c_name.as_ptr()) };
        Self { ptr, owned: true }
    }

    /// Create a new audio track with the given name.
    #[must_use]
    pub fn new_audio(name: &str) -> Self {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_track_create_audio(c_name.as_ptr()) };
        Self { ptr, owned: true }
    }

    // Child operations generated by macro
    macros::impl_track_ops!();

    /// Iterate over children of this track.
    ///
    /// Returns an iterator of `Composable` items (clips, gaps, stacks).
    #[must_use]
    pub fn children(&self) -> TrackChildIter<'_> {
        TrackChildIter::new(self.ptr)
    }

    /// Get the kind of this track (video or audio).
    #[must_use]
    pub fn kind(&self) -> TrackKind {
        let kind = unsafe { ffi::otio_track_get_kind(self.ptr) };
        if kind == 1 {
            TrackKind::Audio
        } else {
            TrackKind::Video
        }
    }

    /// Set the kind of this track.
    pub fn set_kind(&mut self, kind: TrackKind) {
        let kind_val = match kind {
            TrackKind::Video => 0,
            TrackKind::Audio => 1,
        };
        unsafe { ffi::otio_track_set_kind(self.ptr, kind_val) };
    }

    /// Add a marker to this track.
    ///
    /// # Errors
    ///
    /// Returns an error if the marker cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_marker(&mut self, marker: Marker) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_track_add_marker(self.ptr, marker.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(marker);
        Ok(())
    }

    /// Get the number of markers on this track.
    #[must_use]
    #[allow(clippy::cast_sign_loss)]
    pub fn markers_count(&self) -> usize {
        let count = unsafe { ffi::otio_track_markers_count(self.ptr) };
        count.max(0) as usize
    }

    /// Get the range of a child at the given index within this track.
    ///
    /// This returns the time range of the child relative to the track's
    /// start time, taking into account all preceding children.
    ///
    /// # Errors
    ///
    /// Returns an error if the index is out of bounds.
    #[allow(clippy::cast_possible_wrap, clippy::cast_possible_truncation)]
    pub fn range_of_child_at_index(&self, index: usize) -> Result<TimeRange> {
        let mut err = macros::ffi_error!();
        let range = unsafe {
            ffi::otio_track_range_of_child_at_index(self.ptr, index as i32, &mut err)
        };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(time_range_from_ffi(&range))
    }

    /// Get the trimmed range of this track.
    ///
    /// The trimmed range is computed from the children of the track.
    ///
    /// # Errors
    ///
    /// Returns an error if the range cannot be computed.
    pub fn trimmed_range(&self) -> Result<TimeRange> {
        let mut err = macros::ffi_error!();
        let range = unsafe { ffi::otio_track_trimmed_range(self.ptr, &mut err) };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(time_range_from_ffi(&range))
    }

    /// Get the parent stack of this track.
    ///
    /// Returns `None` if the track is not attached to a stack.
    #[must_use]
    pub fn parent(&self) -> Option<StackRef<'_>> {
        iterators::get_track_parent(self.ptr)
    }

    /// Find all clips in this track.
    ///
    /// Returns an iterator over all clips that are direct children of this track.
    /// For a recursive search through nested compositions, use `find_clips()` on
    /// the containing Stack or Timeline instead.
    #[must_use]
    pub fn find_clips(&self) -> ClipSearchIter<'_> {
        let ptr = unsafe { ffi::otio_track_find_clips(self.ptr) };
        ClipSearchIter::new(ptr)
    }

    /// Get the neighbors of a child at the given index.
    ///
    /// Returns the items immediately before and after the child at `index`.
    /// The `policy` parameter controls whether gaps should be included as neighbors.
    ///
    /// # Arguments
    ///
    /// * `index` - The index of the child to get neighbors for
    /// * `policy` - Policy for including gaps as neighbors
    ///
    /// # Errors
    ///
    /// Returns an error if the index is out of bounds.
    ///
    /// # Example
    ///
    /// ```no_run
    /// use otio_rs::{Track, NeighborGapPolicy};
    ///
    /// let track = Track::new_video("V1");
    /// // ... add some clips ...
    /// if let Ok(neighbors) = track.neighbors_of(1, NeighborGapPolicy::Never) {
    ///     if let Some(left) = neighbors.left {
    ///         println!("Left neighbor exists");
    ///     }
    /// }
    /// ```
    #[allow(clippy::cast_possible_wrap, clippy::cast_possible_truncation)]
    pub fn neighbors_of(&self, index: usize, policy: NeighborGapPolicy) -> Result<Neighbors<'_>> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_track_neighbors_of(self.ptr, index as i32, policy as i32, &mut err)
        };
        if err.code != 0 {
            return Err(err.into());
        }

        let left = Neighbor::from_ffi(result.left, result.left_type, result.left_generated);
        let right = Neighbor::from_ffi(result.right, result.right_type, result.right_generated);

        Ok(Neighbors { left, right })
    }

    // =========================================================================
    // Edit Algorithms
    // =========================================================================

    /// Overwrite content in this track at the specified range with a new clip.
    ///
    /// This is equivalent to a 3-point edit in NLE software. The clip is placed
    /// at the specified range, replacing any existing content.
    ///
    /// # Arguments
    ///
    /// * `clip` - The clip to insert (ownership transfers to the track)
    /// * `range` - The time range to overwrite
    /// * `remove_transitions` - Whether to remove transitions that intersect the range
    ///
    /// # Errors
    ///
    /// Returns an error if the overwrite operation fails.
    pub fn overwrite(
        &mut self,
        clip: Clip,
        range: TimeRange,
        remove_transitions: bool,
    ) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_track_overwrite(
                self.ptr,
                clip.ptr,
                range.into(),
                i32::from(remove_transitions),
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(clip);
        Ok(())
    }

    /// Insert a clip at a specific time, shifting subsequent items.
    ///
    /// This splits any item at the insertion point and pushes all subsequent
    /// items later in the track to make room for the new clip.
    ///
    /// # Arguments
    ///
    /// * `clip` - The clip to insert (ownership transfers to the track)
    /// * `time` - The time at which to insert
    /// * `remove_transitions` - Whether to remove transitions that intersect the time
    ///
    /// # Errors
    ///
    /// Returns an error if the insert operation fails.
    pub fn insert_at_time(
        &mut self,
        clip: Clip,
        time: RationalTime,
        remove_transitions: bool,
    ) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_track_insert_at_time(
                self.ptr,
                clip.ptr,
                time.into(),
                i32::from(remove_transitions),
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(clip);
        Ok(())
    }

    /// Slice (split) the track at a specific time point.
    ///
    /// This creates a cut at the specified time, splitting any item that
    /// spans that point into two items.
    ///
    /// # Arguments
    ///
    /// * `time` - The time at which to slice
    /// * `remove_transitions` - Whether to remove transitions that intersect the time
    ///
    /// # Errors
    ///
    /// Returns an error if the slice operation fails.
    pub fn slice_at_time(&mut self, time: RationalTime, remove_transitions: bool) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_track_slice_at_time(
                self.ptr,
                time.into(),
                i32::from(remove_transitions),
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Remove the item at a specific time.
    ///
    /// # Arguments
    ///
    /// * `time` - The time at which to remove
    /// * `fill_with_gap` - If true, fills the removed space with a gap; otherwise
    ///   subsequent items are concatenated
    ///
    /// # Errors
    ///
    /// Returns an error if the remove operation fails.
    pub fn remove_at_time(&mut self, time: RationalTime, fill_with_gap: bool) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_track_remove_at_time(
                self.ptr,
                time.into(),
                i32::from(fill_with_gap),
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }
}

traits::impl_has_metadata!(Track, otio_track_set_metadata_string, otio_track_get_metadata_string);

impl Drop for Track {
    fn drop(&mut self) {
        if self.owned {
            unsafe { ffi::otio_track_free(self.ptr) }
        }
    }
}

// Safety: Track is safe to send between threads
unsafe impl Send for Track {}

/// A clip represents a segment of media.
pub struct Clip {
    ptr: *mut ffi::OtioClip,
}

impl std::fmt::Debug for Clip {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Clip")
            .field("name", &self.name())
            .finish()
    }
}

impl Clip {
    /// Get the name of this clip.
    #[must_use]
    pub fn name(&self) -> String {
        let ptr = unsafe { ffi::otio_clip_get_name(self.ptr) };
        ffi_string_to_rust(ptr)
    }

    /// Create a new clip with the given name and source range.
    #[must_use]
    pub fn new(name: &str, source_range: TimeRange) -> Self {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_clip_create(c_name.as_ptr(), source_range.into()) };
        Self { ptr }
    }

    /// Set the media reference for this clip.
    ///
    /// # Errors
    ///
    /// Returns an error if the media reference cannot be set.
    pub fn set_media_reference(&mut self, reference: ExternalReference) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_clip_set_media_reference(self.ptr, reference.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference); // Clip now owns the reference - only forget on success
        Ok(())
    }

    /// Set a missing reference for this clip (for offline/placeholder clips).
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be set.
    #[allow(clippy::forget_non_drop)]
    pub fn set_missing_reference(&mut self, reference: MissingReference) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_clip_set_missing_reference(self.ptr, reference.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Set a generator reference for this clip (for generated content).
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be set.
    #[allow(clippy::forget_non_drop)]
    pub fn set_generator_reference(&mut self, reference: GeneratorReference) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_clip_set_generator_reference(self.ptr, reference.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Set an image sequence reference for this clip (for VFX image sequences).
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be set.
    #[allow(clippy::forget_non_drop)]
    pub fn set_image_sequence_reference(
        &mut self,
        reference: ImageSequenceReference,
    ) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_set_image_sequence_reference(self.ptr, reference.ptr, &mut err)
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Get the available range of this clip's media.
    ///
    /// This is the range of media that is available from the media reference,
    /// which may differ from the `source_range` (the portion actually used).
    ///
    /// # Errors
    ///
    /// Returns an error if the clip has no media reference or the range cannot
    /// be computed.
    pub fn available_range(&self) -> Result<TimeRange> {
        let mut err = macros::ffi_error!();
        let range = unsafe { ffi::otio_clip_available_range(self.ptr, &mut err) };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(time_range_from_ffi(&range))
    }

    // =========================================================================
    // Multi-Reference Support
    // =========================================================================

    /// Get the active media reference key.
    ///
    /// OTIO clips can have multiple media references (e.g., for different
    /// resolutions or proxy versions). This returns the key of the currently
    /// active reference.
    #[must_use]
    pub fn active_media_reference_key(&self) -> String {
        ffi_string_to_rust(unsafe { ffi::otio_clip_active_media_reference_key(self.ptr) })
    }

    /// Set the active media reference key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key of the media reference to make active
    ///
    /// # Errors
    ///
    /// Returns an error if the key does not exist in the clip's media references.
    pub fn set_active_media_reference_key(&mut self, key: &str) -> Result<()> {
        let c_key = CString::new(key).unwrap();
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_set_active_media_reference_key(self.ptr, c_key.as_ptr(), &mut err)
        };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Get all media reference keys.
    ///
    /// Returns a list of all keys in the clip's media reference map.
    #[must_use]
    #[allow(clippy::cast_sign_loss)]
    pub fn media_reference_keys(&self) -> Vec<String> {
        let iter = unsafe { ffi::otio_clip_media_reference_keys(self.ptr) };
        if iter.is_null() {
            return Vec::new();
        }
        let count = unsafe { ffi::otio_string_iterator_count(iter) } as usize;
        let mut keys = Vec::with_capacity(count);
        loop {
            let ptr = unsafe { ffi::otio_string_iterator_next(iter) };
            if ptr.is_null() {
                break;
            }
            keys.push(ffi_string_to_rust(ptr));
        }
        unsafe { ffi::otio_string_iterator_free(iter) };
        keys
    }

    /// Check if a media reference exists for the given key.
    #[must_use]
    pub fn has_media_reference(&self, key: &str) -> bool {
        let c_key = CString::new(key).unwrap();
        unsafe { ffi::otio_clip_has_media_reference(self.ptr, c_key.as_ptr()) != 0 }
    }

    /// Add an external reference with a key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key to associate with this reference
    /// * `reference` - The external reference to add (ownership transfers)
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be added.
    pub fn add_external_reference(&mut self, key: &str, reference: ExternalReference) -> Result<()> {
        let c_key = CString::new(key).unwrap();
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_add_media_reference(
                self.ptr,
                c_key.as_ptr(),
                reference.ptr.cast(),
                0, // OTIO_REF_TYPE_EXTERNAL
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Add a missing reference with a key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key to associate with this reference
    /// * `reference` - The missing reference to add (ownership transfers)
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_missing_reference(&mut self, key: &str, reference: MissingReference) -> Result<()> {
        let c_key = CString::new(key).unwrap();
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_add_media_reference(
                self.ptr,
                c_key.as_ptr(),
                reference.ptr.cast(),
                1, // OTIO_REF_TYPE_MISSING
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Add a generator reference with a key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key to associate with this reference
    /// * `reference` - The generator reference to add (ownership transfers)
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_generator_reference(&mut self, key: &str, reference: GeneratorReference) -> Result<()> {
        let c_key = CString::new(key).unwrap();
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_add_media_reference(
                self.ptr,
                c_key.as_ptr(),
                reference.ptr.cast(),
                2, // OTIO_REF_TYPE_GENERATOR
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Add an image sequence reference with a key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key to associate with this reference
    /// * `reference` - The image sequence reference to add (ownership transfers)
    ///
    /// # Errors
    ///
    /// Returns an error if the reference cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_image_sequence_reference(
        &mut self,
        key: &str,
        reference: ImageSequenceReference,
    ) -> Result<()> {
        let c_key = CString::new(key).unwrap();
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_add_media_reference(
                self.ptr,
                c_key.as_ptr(),
                reference.ptr.cast(),
                3, // OTIO_REF_TYPE_IMAGE_SEQUENCE
                &mut err,
            )
        };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(reference);
        Ok(())
    }

    /// Add a marker to this clip.
    ///
    /// # Errors
    ///
    /// Returns an error if the marker cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_marker(&mut self, marker: Marker) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_clip_add_marker(self.ptr, marker.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(marker);
        Ok(())
    }

    /// Get the number of markers on this clip.
    #[must_use]
    #[allow(clippy::cast_sign_loss)]
    pub fn markers_count(&self) -> usize {
        let count = unsafe { ffi::otio_clip_markers_count(self.ptr) };
        count.max(0) as usize
    }

    /// Add an effect to this clip.
    ///
    /// # Errors
    ///
    /// Returns an error if the effect cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_effect(&mut self, effect: Effect) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_clip_add_effect(self.ptr, effect.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(effect);
        Ok(())
    }

    /// Add a linear time warp effect to this clip.
    ///
    /// # Errors
    ///
    /// Returns an error if the effect cannot be added.
    #[allow(clippy::forget_non_drop)]
    pub fn add_linear_time_warp(&mut self, effect: LinearTimeWarp) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_clip_add_linear_time_warp(self.ptr, effect.ptr, &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        std::mem::forget(effect);
        Ok(())
    }

    /// Get the number of effects on this clip.
    #[must_use]
    #[allow(clippy::cast_sign_loss)]
    pub fn effects_count(&self) -> usize {
        let count = unsafe { ffi::otio_clip_effects_count(self.ptr) };
        count.max(0) as usize
    }

    // =========================================================================
    // Edit Algorithms
    // =========================================================================

    /// Slip the clip's media content by a time delta.
    ///
    /// Slipping adjusts which portion of the source media is shown without
    /// changing the clip's position or duration in the track. The media
    /// "slides" under the clip boundaries.
    ///
    /// # Arguments
    ///
    /// * `delta` - The time amount to slip (positive = later in source, negative = earlier)
    ///
    /// # Errors
    ///
    /// Returns an error if the slip operation fails.
    pub fn slip(&mut self, delta: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_clip_slip(self.ptr, delta.into(), &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Slide the clip's position in the track.
    ///
    /// Sliding moves the clip earlier or later in the track, adjusting the
    /// duration of the previous item to compensate. The clip's content and
    /// duration remain unchanged.
    ///
    /// # Arguments
    ///
    /// * `delta` - The time amount to slide (positive = later, negative = earlier)
    ///
    /// # Errors
    ///
    /// Returns an error if the slide operation fails.
    pub fn slide(&mut self, delta: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe { ffi::otio_clip_slide(self.ptr, delta.into(), &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Trim the clip's in and out points.
    ///
    /// Trimming adjusts the source range boundaries without affecting other
    /// clips in the track. Empty space created is filled with a gap.
    ///
    /// # Arguments
    ///
    /// * `delta_in` - Adjustment to the in point (positive = trim later, negative = extend earlier)
    /// * `delta_out` - Adjustment to the out point (positive = extend later, negative = trim earlier)
    ///
    /// # Errors
    ///
    /// Returns an error if the trim operation fails.
    pub fn trim(&mut self, delta_in: RationalTime, delta_out: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_trim(self.ptr, delta_in.into(), delta_out.into(), &mut err)
        };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Ripple edit the clip's duration.
    ///
    /// Rippling adjusts the clip's duration and propagates the change through
    /// the rest of the track - subsequent clips move to accommodate the change.
    ///
    /// # Arguments
    ///
    /// * `delta_in` - Adjustment to the in point
    /// * `delta_out` - Adjustment to the out point
    ///
    /// # Errors
    ///
    /// Returns an error if the ripple operation fails.
    pub fn ripple(&mut self, delta_in: RationalTime, delta_out: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result = unsafe {
            ffi::otio_clip_ripple(self.ptr, delta_in.into(), delta_out.into(), &mut err)
        };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }

    /// Roll the edit point between this clip and adjacent clips.
    ///
    /// Rolling moves the edit point between clips without changing the
    /// overall duration of the combined clips. One clip gets longer while
    /// the adjacent clip gets shorter.
    ///
    /// # Arguments
    ///
    /// * `delta_in` - Adjustment to the in point (affects previous clip's out)
    /// * `delta_out` - Adjustment to the out point (affects next clip's in)
    ///
    /// # Errors
    ///
    /// Returns an error if the roll operation fails.
    pub fn roll(&mut self, delta_in: RationalTime, delta_out: RationalTime) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_clip_roll(self.ptr, delta_in.into(), delta_out.into(), &mut err) };
        if result != 0 {
            return Err(err.into());
        }
        Ok(())
    }
}

traits::impl_has_metadata!(Clip, otio_clip_set_metadata_string, otio_clip_get_metadata_string);

impl Drop for Clip {
    fn drop(&mut self) {
        unsafe { ffi::otio_clip_free(self.ptr) }
    }
}

/// A gap represents empty space in a track.
pub struct Gap {
    ptr: *mut ffi::OtioGap,
}

impl std::fmt::Debug for Gap {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Gap").finish()
    }
}

impl Gap {
    /// Create a new gap with the given duration.
    #[must_use]
    pub fn new(duration: RationalTime) -> Self {
        let ptr = unsafe { ffi::otio_gap_create(duration.into()) };
        Self { ptr }
    }
}

traits::impl_has_metadata!(Gap, otio_gap_set_metadata_string, otio_gap_get_metadata_string);

impl Drop for Gap {
    fn drop(&mut self) {
        unsafe { ffi::otio_gap_free(self.ptr) }
    }
}

/// An external reference points to a media file.
pub struct ExternalReference {
    ptr: *mut ffi::OtioExternalRef,
}

impl std::fmt::Debug for ExternalReference {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ExternalReference")
            .field("target_url", &self.target_url())
            .finish()
    }
}

impl ExternalReference {
    /// Create a new external reference with the given URL.
    #[must_use]
    pub fn new(target_url: &str) -> Self {
        let c_url = CString::new(target_url).unwrap();
        let ptr = unsafe { ffi::otio_external_ref_create(c_url.as_ptr()) };
        Self { ptr }
    }

    /// Get the name of this external reference.
    #[must_use]
    pub fn name(&self) -> String {
        let ptr = unsafe { ffi::otio_external_ref_get_name(self.ptr) };
        ffi_string_to_rust(ptr)
    }

    /// Set the name of this external reference.
    pub fn set_name(&mut self, name: &str) {
        let c_name = CString::new(name).unwrap();
        unsafe { ffi::otio_external_ref_set_name(self.ptr, c_name.as_ptr()) };
    }

    /// Set the available range for this media reference.
    ///
    /// # Errors
    ///
    /// Returns an error if the available range cannot be set.
    pub fn set_available_range(&mut self, range: TimeRange) -> Result<()> {
        let mut err = macros::ffi_error!();
        let result =
            unsafe { ffi::otio_external_ref_set_available_range(self.ptr, range.into(), &mut err) };
        if result != 0 {
            Err(err.into())
        } else {
            Ok(())
        }
    }

    /// Get the target URL of this media reference.
    #[must_use]
    pub fn target_url(&self) -> String {
        let ptr = unsafe { ffi::otio_external_ref_get_target_url(self.ptr) };
        ffi_string_to_rust(ptr)
    }

    /// Get the available range of this media reference.
    ///
    /// Returns `None` if no available range has been set.
    #[must_use]
    pub fn available_range(&self) -> Option<TimeRange> {
        let range = unsafe { ffi::otio_external_ref_get_available_range(self.ptr) };
        if is_unset_time_range(&range) {
            return None;
        }
        Some(time_range_from_ffi(&range))
    }
}

traits::impl_has_metadata!(ExternalReference, otio_external_ref_set_metadata_string, otio_external_ref_get_metadata_string);

impl Drop for ExternalReference {
    fn drop(&mut self) {
        unsafe { ffi::otio_external_ref_free(self.ptr) }
    }
}

/// A stack is a composition that layers its children.
///
/// Stacks are used for:
/// - Timeline's root tracks container
/// - Nested compositions within tracks (for versioning/alternatives)
/// - Clip stacks for layered effects
pub struct Stack {
    ptr: *mut ffi::OtioStack,
}

impl std::fmt::Debug for Stack {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Stack")
            .field("name", &self.name())
            .field("children_count", &self.children_count())
            .finish()
    }
}

impl Stack {
    /// Get the name of this stack.
    #[must_use]
    pub fn name(&self) -> String {
        let ptr = unsafe { ffi::otio_stack_get_name(self.ptr) };
        ffi_string_to_rust(ptr)
    }

    /// Create a new stack with the given name.
    #[must_use]
    pub fn new(name: &str) -> Self {
        let c_name = CString::new(name).unwrap();
        let ptr = unsafe { ffi::otio_stack_create(c_name.as_ptr()) };
        Self { ptr }
    }

    // Child operations generated by macro
    macros::impl_stack_ops!();

    /// Iterate over children of this stack.
    ///
    /// Returns an iterator of `Composable` items (clips, gaps, stacks, tracks).
    #[must_use]
    pub fn children(&self) -> StackChildIter<'_> {
        StackChildIter::new(self.ptr)
    }

    /// Get the range of a child at the given index within this stack.
    ///
    /// For stacks, all children typically start at the same time (they layer
    /// rather than sequence like tracks).
    ///
    /// # Errors
    ///
    /// Returns an error if the index is out of bounds.
    #[allow(clippy::cast_possible_wrap, clippy::cast_possible_truncation)]
    pub fn range_of_child_at_index(&self, index: usize) -> Result<TimeRange> {
        let mut err = macros::ffi_error!();
        let range = unsafe {
            ffi::otio_stack_range_of_child_at_index(self.ptr, index as i32, &mut err)
        };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(time_range_from_ffi(&range))
    }

    /// Get the trimmed range of this stack.
    ///
    /// The trimmed range is the union of all children's ranges.
    ///
    /// # Errors
    ///
    /// Returns an error if the range cannot be computed.
    pub fn trimmed_range(&self) -> Result<TimeRange> {
        let mut err = macros::ffi_error!();
        let range = unsafe { ffi::otio_stack_trimmed_range(self.ptr, &mut err) };
        if err.code != 0 {
            return Err(err.into());
        }
        Ok(time_range_from_ffi(&range))
    }

    /// Get the parent stack of this stack.
    ///
    /// Returns `None` if this stack is not nested within another stack.
    #[must_use]
    pub fn parent(&self) -> Option<StackRef<'_>> {
        iterators::get_stack_parent(self.ptr)
    }

    /// Find all clips in this stack (recursively).
    ///
    /// Returns an iterator over all clips found in this stack and its nested
    /// compositions (tracks and nested stacks).
    #[must_use]
    pub fn find_clips(&self) -> ClipSearchIter<'_> {
        let ptr = unsafe { ffi::otio_stack_find_clips(self.ptr) };
        ClipSearchIter::new(ptr)
    }
}

traits::impl_has_metadata!(Stack, otio_stack_set_metadata_string, otio_stack_get_metadata_string);

impl Drop for Stack {
    fn drop(&mut self) {
        unsafe { ffi::otio_stack_free(self.ptr) }
    }
}

// Safety: Stack is safe to send between threads
unsafe impl Send for Stack {}
