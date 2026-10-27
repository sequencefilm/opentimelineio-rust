//! Ownership regressions across the FFI boundary.
//!
//! OpenTimelineIO deletes an object when its managed reference count returns
//! to 0, and several calls briefly retain and release the object they act on.
//! Most tests drive one of those calls and then keep using the object, so a
//! handle that does not hold its own reference shows up as a use-after-free
//! (a SIGSEGV on Linux, or a report under Valgrind or `AddressSanitizer`).

use otio_rs::{
    Clip, ExternalReference, HasMetadata, Neighbor, NeighborGapPolicy, RationalTime, TimeRange,
    Timeline, Track, Transition,
};

fn clip(name: &str, frames: f64) -> Clip {
    Clip::new(
        name,
        TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(frames, 24.0)),
    )
}

/// `to_json_string` wraps the timeline in a temporary `Retainer`; the
/// timeline must survive it, serialize the same way twice, and drop cleanly
#[test]
fn timeline_survives_serialization() {
    let mut timeline = Timeline::new("serialize");
    let mut track = timeline.add_video_track("V1");
    track.append_clip(clip("A", 48.0)).unwrap();
    drop(track);

    let first = timeline.to_json_string().unwrap();
    let second = timeline.to_json_string_with_schema_versions(&[]).unwrap();
    assert_eq!(first, second);
    assert_eq!(timeline.name(), "serialize");
    assert_eq!(timeline.find_clips().count(), 1);
}

/// A timeline loaded from JSON is handed out the same way as a created one
#[test]
fn loaded_timeline_survives_serialization() {
    let mut timeline = Timeline::new("roundtrip");
    let mut track = timeline.add_video_track("V1");
    track.append_clip(clip("A", 24.0)).unwrap();
    drop(track);
    let json = timeline.to_json_string().unwrap();

    let restored = Timeline::from_json_string(&json).unwrap();
    assert_eq!(restored.to_json_string().unwrap(), json);
    assert_eq!(restored.find_clips().count(), 1);
}

/// `overwrite` moves the clip into the track, so the shim releases the clip
/// handle's reference there; the track then owns the clip and the split
/// pieces of the clip it cut
#[test]
fn overwrite_hands_the_clip_to_the_track() {
    let mut track = Track::new_video("V1");
    track.append_clip(clip("A", 48.0)).unwrap();
    track
        .overwrite(
            clip("B", 12.0),
            TimeRange::new(RationalTime::new(12.0, 24.0), RationalTime::new(12.0, 24.0)),
            false,
        )
        .unwrap();
    assert_eq!(track.find_clips().count(), 3);
}

/// Adding a second media reference rebuilds the clip's reference map, which
/// must not delete the reference the clip already holds
#[test]
fn adding_a_media_reference_keeps_the_existing_one() {
    let mut timeline = Timeline::new("references");
    let mut track = timeline.add_video_track("V1");
    let mut c = clip("A", 24.0);
    c.set_media_reference(ExternalReference::new("file:///high.mov"))
        .unwrap();
    c.add_external_reference("proxy", ExternalReference::new("file:///proxy.mov"))
        .unwrap();
    track.append_clip(c).unwrap();
    drop(track);

    let json = timeline.to_json_string().unwrap();
    assert!(json.contains("file:///high.mov"), "{json}");
    assert!(json.contains("file:///proxy.mov"), "{json}");
}

/// Around a transition with no neighbor, OTIO makes up a gap that is not in
/// the track; the caller must own it, or it is deleted before Rust sees it
#[test]
fn generated_neighbor_gaps_are_owned() {
    let mut timeline = Timeline::new("neighbors");
    let mut track = timeline.add_video_track("V1");
    track
        .append_transition(Transition::new(
            "dissolve",
            "SMPTE_Dissolve",
            RationalTime::new(8.0, 24.0),
            RationalTime::new(12.0, 24.0),
        ))
        .unwrap();

    let neighbors = track.neighbors_of(0, NeighborGapPolicy::AroundTransitions).unwrap();
    let (Some(Neighbor::GeneratedGap(mut left)), Some(Neighbor::GeneratedGap(mut right))) =
        (neighbors.left, neighbors.right)
    else {
        panic!("expected generated gaps on both sides of a lone transition");
    };

    left.set_metadata("side", "left");
    right.set_metadata("side", "right");
    assert_eq!(left.get_metadata("side").as_deref(), Some("left"));
    assert_eq!(right.get_metadata("side").as_deref(), Some("right"));
    drop((left, right));

    assert_eq!(track.children_count(), 1);
    drop(track);
    timeline.to_json_string().unwrap();
}
