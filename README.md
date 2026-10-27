# otio-rs

Rust bindings to [OpenTimelineIO](https://opentimeline.io/) - an open-source API and interchange format for editorial timeline information.

[![Test](https://github.com/lmcgartland/opentimelineio-rust/actions/workflows/test.yml/badge.svg)](https://github.com/lmcgartland/opentimelineio-rust/actions/workflows/test.yml)

## Status

This crate provides FFI bindings to the C++ OpenTimelineIO library (v0.17.0) via a thin C shim layer.

**Coverage:** ~95% of core types, ~90% of methods

## Features

- **Timeline creation and manipulation** - Create timelines, tracks, clips, gaps, and stacks
- **Edit algorithms** - NLE-style editing operations (overwrite, insert, slice, slip, slide, trim, ripple, roll)
- **Iteration support** - Iterate over children of tracks and stacks with type-safe `Composable` enum
- **Track filtering** - Get video-only or audio-only tracks from a timeline
- **Track neighbors** - Get adjacent items before/after a child in a track
- **Time transforms** - Convert times between different coordinate spaces in the hierarchy
- **Available range** - Get the available range from a clip's media reference
- **String serialization** - Serialize/deserialize timelines to/from JSON strings
- **Builder pattern** - Fluent API for constructing clips, timelines, and references
- **Metadata support** - Get/set string metadata on all OTIO objects via `HasMetadata` trait
- **Markers and effects** - Add markers, linear time warps, and freeze frames
- **Transitions** - Cross-dissolves and other transition types
- **Media references** - External references, image sequences, generators, and missing references
- **Multi-reference clips** - Multiple media references per clip with key-based selection
- **File I/O** - Read and write `.otio` JSON files
- **Schema version targeting** - Export with older schema versions for compatibility
- **Flexible linking** - Use vendored (bundled) or system-installed OpenTimelineIO

## Prerequisites

### All Platforms
- **Rust 1.70+** - Install via [rustup](https://rustup.rs/)
- **CMake 3.18+** - Required for building OpenTimelineIO
- **C++17 compiler** - clang or gcc

### macOS
```bash
# Install Xcode command line tools (includes clang)
xcode-select --install

# Install CMake via Homebrew
brew install cmake
```

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y cmake clang libstdc++-12-dev
```

### Windows
- Install [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with C++ workload
- Install [CMake](https://cmake.org/download/)

## Installation

Add this to your `Cargo.toml`:

```toml
[dependencies]
otio-rs = { git = "https://github.com/lmcgartland/opentimelineio-rust" }
```

### Feature Flags

| Feature | Default | Description |
|---------|---------|-------------|
| `vendored` | Yes | Build and link bundled OpenTimelineIO from source |
| `system` | No | Link against system-installed OpenTimelineIO via pkg-config |

To use system-installed OpenTimelineIO instead of vendored:
```toml
[dependencies]
otio-rs = { git = "https://github.com/lmcgartland/opentimelineio-rust", default-features = false, features = ["system"] }
```

## Quick Start

```rust
use otio_rs::{Timeline, Clip, RationalTime, TimeRange, ExternalReference};
use std::path::Path;

fn main() -> otio_rs::Result<()> {
    // Create a new timeline
    let mut timeline = Timeline::new("My Timeline");
    timeline.set_global_start_time(RationalTime::new(0.0, 24.0))?;

    // Add a video track
    let mut video_track = timeline.add_video_track("V1");

    // Create a clip with a 2-second source range at 24fps
    let source_range = TimeRange::new(
        RationalTime::new(0.0, 24.0),
        RationalTime::new(48.0, 24.0),
    );
    let mut clip = Clip::new("My Clip", source_range);

    // Set media reference
    let mut media_ref = ExternalReference::new("/path/to/media.mov");
    media_ref.set_available_range(TimeRange::new(
        RationalTime::new(0.0, 24.0),
        RationalTime::new(240.0, 24.0), // 10 seconds
    ))?;
    clip.set_media_reference(media_ref)?;

    // Add clip to track
    video_track.append_clip(clip)?;

    // Write to file
    timeline.write_to_file(Path::new("output.otio"))?;

    // Read back
    let _loaded = Timeline::read_from_file(Path::new("output.otio"))?;

    Ok(())
}
```

## Edit Algorithms

Perform NLE-style editing operations:

```rust
use otio_rs::{Timeline, Track, Clip, RationalTime, TimeRange};

let mut timeline = Timeline::new("Edit Demo");
let mut track = timeline.add_video_track("V1");

// Add some clips
let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(48.0, 24.0));
track.append_clip(Clip::new("Clip A", range))?;
track.append_clip(Clip::new("Clip B", range))?;
track.append_clip(Clip::new("Clip C", range))?;

// Overwrite content at a specific range (3-point edit)
let new_clip = Clip::new("Insert", range);
let overwrite_range = TimeRange::new(RationalTime::new(24.0, 24.0), RationalTime::new(24.0, 24.0));
track.overwrite(new_clip, overwrite_range, false)?;

// Insert at a time, shifting subsequent items
let insert_clip = Clip::new("Inserted", range);
track.insert_at_time(insert_clip, RationalTime::new(48.0, 24.0), false)?;

// Slice (split) at a time point
track.slice_at_time(RationalTime::new(36.0, 24.0), false)?;

// Remove item at time (with optional gap fill)
track.remove_at_time(RationalTime::new(0.0, 24.0), true)?;
```

Clip-level edit operations:

```rust
// Get a mutable clip reference
let mut clip = Clip::new("My Clip", range);

// Slip: shift media content without changing position/duration
clip.slip(RationalTime::new(12.0, 24.0))?;

// Slide: move clip position, adjusting adjacent items
clip.slide(RationalTime::new(-6.0, 24.0))?;

// Trim: adjust in/out points
clip.trim(
    RationalTime::new(6.0, 24.0),   // trim in by 6 frames
    RationalTime::new(-6.0, 24.0),  // trim out by 6 frames
)?;

// Ripple: adjust duration, shifting subsequent clips
clip.ripple(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(12.0, 24.0),  // extend out by 12 frames
)?;

// Roll: adjust edit point between adjacent clips
clip.roll(
    RationalTime::new(6.0, 24.0),
    RationalTime::new(0.0, 24.0),
)?;
```

## String Serialization

Serialize and deserialize timelines to/from JSON strings:

```rust
use otio_rs::Timeline;

// Create a timeline
let timeline = Timeline::new("My Timeline");

// Serialize to JSON string
let json = timeline.to_json_string()?;
println!("JSON: {}", json);

// Deserialize from JSON string
let restored = Timeline::from_json_string(&json)?;
assert_eq!(restored.name(), "My Timeline");
```

## Schema Version Targeting

Export timelines with older schema versions for compatibility with older OTIO readers:

```rust
use otio_rs::Timeline;
use std::path::Path;

let timeline = Timeline::new("My Timeline");
// ... add tracks, clips, etc.

// Serialize to JSON string with Clip schema downgraded to version 1
let json = timeline.to_json_string_with_schema_versions(&[("Clip", 1)])?;

// Or write to file with schema targeting
timeline.write_to_file_with_schema_versions(
    Path::new("output.otio"),
    &[("Clip", 1), ("Timeline", 1)],
)?;
```

## Image Sequences

Work with VFX image sequences (EXR, DPX, TIFF, etc.):

```rust
use otio_rs::{ImageSequenceReference, Clip, RationalTime, TimeRange};
use otio_rs::image_sequence_reference::MissingFramePolicy;

// Create a reference to an EXR sequence: shot_0001.exr, shot_0002.exr, ...
let mut seq = ImageSequenceReference::new(
    "/path/to/render/",  // target_url_base
    "shot_",             // name_prefix
    ".exr",              // name_suffix
    1001,                // start_frame
    1,                   // frame_step
    24.0,                // rate (fps)
    4,                   // frame_zero_padding (e.g., 0001)
);

seq.set_available_range(TimeRange::new(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(100.0, 24.0), // 100 frames
))?;

seq.set_missing_frame_policy(MissingFramePolicy::Hold);

// Get URL for a specific image
let url = seq.target_url_for_image_number(0)?; // "/path/to/render/shot_1001.exr"

// Attach to a clip
let mut clip = Clip::new("VFX Shot", TimeRange::new(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(100.0, 24.0),
));
clip.set_image_sequence_reference(seq)?;
```

## Time Transforms

Convert times between different coordinate spaces:

```rust
use otio_rs::{Timeline, RationalTime, TimeRange};

let timeline = Timeline::read_from_file(std::path::Path::new("input.otio"))?;

// Find clips and get their position in parent
for clip in timeline.find_clips() {
    // Get the clip's range in the parent track's coordinate space
    let range_in_parent = clip.range_in_parent()?;
    println!("Clip '{}' is at {:?} in parent", clip.name(), range_in_parent);
}
```

## Markers

Add markers to clips and tracks:

```rust
use otio_rs::{Clip, Marker, marker, RationalTime, TimeRange};

let mut clip = Clip::new("My Clip", TimeRange::new(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(48.0, 24.0),
));

// Add markers with different colors
let marker = Marker::new(
    "Important moment",
    TimeRange::new(RationalTime::new(12.0, 24.0), RationalTime::new(1.0, 24.0)),
    marker::colors::RED,
);
clip.add_marker(marker)?;

// Iterate markers
for marker in clip.markers() {
    println!("Marker: {} at {:?}", marker.name(), marker.marked_range());
}
```

## Effects

Add time effects to clips:

```rust
use otio_rs::{Clip, LinearTimeWarp, FreezeFrame, RationalTime, TimeRange};

let mut clip = Clip::new("My Clip", TimeRange::new(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(48.0, 24.0),
));

// Add a slow-motion effect (50% speed)
let slow_mo = LinearTimeWarp::new("Slow Motion", 0.5);
clip.add_effect(slow_mo)?;

// Or add a freeze frame
let freeze = FreezeFrame::new("Freeze");
clip.add_effect(freeze)?;
```

## Transitions

Add transitions between clips:

```rust
use otio_rs::{Track, Clip, Transition, RationalTime, TimeRange};
use otio_rs::transition::TransitionType;

let mut track = Track::new_video("V1");
let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(48.0, 24.0));

track.append_clip(Clip::new("Clip A", range))?;

// Add a 12-frame cross dissolve
let transition = Transition::new(
    "Dissolve",
    TransitionType::SMPTE_Dissolve,
    RationalTime::new(6.0, 24.0),  // in_offset
    RationalTime::new(6.0, 24.0),  // out_offset
);
track.append_transition(transition)?;

track.append_clip(Clip::new("Clip B", range))?;
```

## Builder Pattern

Use builders for a fluent construction API:

```rust
use otio_rs::{ClipBuilder, TimelineBuilder, ExternalReferenceBuilder, RationalTime, TimeRange};

let timeline = TimelineBuilder::new("My Project")
    .global_start_time(RationalTime::new(0.0, 24.0))
    .metadata("author", "Jane Doe")
    .build()?;

let clip = ClipBuilder::new(
    "Interview",
    TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(48.0, 24.0)),
)
.media_reference(
    ExternalReferenceBuilder::new("/media/interview.mov")
        .available_range(TimeRange::new(
            RationalTime::new(0.0, 24.0),
            RationalTime::new(720.0, 24.0),
        ))
        .metadata("codec", "ProRes")
        .build()?,
)
.metadata("speaker", "John Smith")
.build()?;
```

## Iteration

Iterate over track and stack children:

```rust
use otio_rs::{Timeline, Composable};

let timeline = Timeline::read_from_file(std::path::Path::new("input.otio"))?;

for child in timeline.tracks().children() {
    match child {
        Composable::Track(track_ref) => {
            println!("Track: {}", track_ref.name());
            for item in track_ref.children() {
                match item {
                    Composable::Clip(clip) => println!("  Clip: {}", clip.name()),
                    Composable::Gap(gap) => println!("  Gap: {}", gap.name()),
                    Composable::Stack(stack) => println!("  Stack: {}", stack.name()),
                    Composable::Track(track) => println!("  Track: {}", track.name()),
                    Composable::Transition(trans) => println!("  Transition: {}", trans.name()),
                }
            }
        }
        _ => {}
    }
}
```

## Track Filtering

Get video or audio tracks from a timeline:

```rust
use otio_rs::{Timeline, TrackKind};

let mut timeline = Timeline::new("My Timeline");
timeline.add_video_track("V1");
timeline.add_video_track("V2");
timeline.add_audio_track("A1");

// Get only video tracks
for track in timeline.video_tracks() {
    println!("Video track: {}", track.name());
    assert_eq!(track.kind(), TrackKind::Video);
}

// Get only audio tracks
for track in timeline.audio_tracks() {
    println!("Audio track: {}", track.name());
}

// ExactSizeIterator support
let video_count = timeline.video_tracks().len();
```

## Track Neighbors

Get the neighbors of a child item in a track:

```rust
use otio_rs::{Timeline, Clip, RationalTime, TimeRange, NeighborGapPolicy, Composable, Neighbor};

let mut timeline = Timeline::new("Test");
let mut track = timeline.add_video_track("V1");

let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(24.0, 24.0));
track.append_clip(Clip::new("A", range))?;
track.append_clip(Clip::new("B", range))?;
track.append_clip(Clip::new("C", range))?;

// Get neighbors of clip B (index 1)
let neighbors = track.neighbors_of(1, NeighborGapPolicy::Never)?;

if let Some(Neighbor::Child(Composable::Clip(left))) = neighbors.left {
    println!("Left neighbor: {}", left.name()); // "A"
}
if let Some(Neighbor::Child(Composable::Clip(right))) = neighbors.right {
    println!("Right neighbor: {}", right.name()); // "C"
}
```

With `NeighborGapPolicy::AroundTransitions`, a transition at either end of the
track gets a `Neighbor::GeneratedGap`: a gap OpenTimelineIO made up to cover the
transition's offset. It is not in the track, and dropping it frees it.

## Available Range

Get the available range from a clip's media reference:

```rust
use otio_rs::{Clip, ExternalReference, RationalTime, TimeRange};

let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(48.0, 24.0));
let mut clip = Clip::new("My Clip", range);

let mut media_ref = ExternalReference::new("/path/to/media.mov");
media_ref.set_available_range(TimeRange::new(
    RationalTime::new(0.0, 24.0),
    RationalTime::new(1000.0, 24.0), // Full media is 1000 frames
))?;
clip.set_media_reference(media_ref)?;

// Get available range from the media reference
let available = clip.available_range()?;
println!("Available duration: {} frames", available.duration.value);
```

## Metadata

All OTIO objects support string metadata via the `HasMetadata` trait:

```rust
use otio_rs::{Clip, RationalTime, TimeRange, HasMetadata};

let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(48.0, 24.0));
let mut clip = Clip::new("My Clip", range);

clip.set_metadata("external_id", "abc123");
clip.set_metadata("notes", "Director's preferred take");

assert_eq!(clip.get_metadata("external_id"), Some("abc123".to_string()));
```

## Modify Operations

Insert, remove, and clear children:

```rust
use otio_rs::{Track, Clip, Gap, RationalTime, TimeRange};

let mut track = Track::new_video("V1");
let range = TimeRange::new(RationalTime::new(0.0, 24.0), RationalTime::new(24.0, 24.0));

// Append items
track.append_clip(Clip::new("Clip A", range))?;
track.append_clip(Clip::new("Clip B", range))?;

// Insert at specific index
track.insert_gap(1, Gap::new(RationalTime::new(12.0, 24.0)))?;

// Remove by index
track.remove_child(0)?;

// Clear all children
track.clear_children()?;
```

## Building from Source

### 1. Clone the Repository

```bash
# Clone with submodules (required!)
git clone --recursive https://github.com/lmcgartland/opentimelineio-rust
cd opentimelineio-rust
```

If you already cloned without `--recursive`:
```bash
git submodule update --init --recursive
```

### 2. Build

```bash
# Debug build
cargo build

# Release build (recommended for production)
cargo build --release
```

The first build will take several minutes as it compiles the OpenTimelineIO C++ library.

### 3. Run Tests

```bash
# Run all tests
cargo test

# Run memory stress tests (for leak detection)
cargo test --test memory -- --ignored --test-threads=1
```

### 4. Run Examples

```bash
cargo run --example dummy      # Basic timeline creation
cargo run --example iterate    # Iteration demo
cargo run --example modify     # Insert/remove operations
cargo run --example builder    # Builder pattern demo
```

## Memory Leak Testing

The library includes stress tests for memory leak detection:

```bash
# Run stress tests (requires valgrind on Linux)
./scripts/check_memory.sh

# Or run manually
cargo test --test memory -- --ignored --test-threads=1
```

## How It Works

### Binding Architecture

This library uses a three-layer binding approach:

```
+-------------------------------------+
|         Rust Safe API               |  <- src/lib.rs, builders.rs, iterators.rs
|    (Timeline, Clip, Track, etc.)    |
+-------------------------------------+
|        Generated FFI Bindings       |  <- bindgen output (build.rs)
|     (otio_timeline_create, etc.)    |
+-------------------------------------+
|           C Shim Layer              |  <- shim/otio_shim.h + otio_shim.cpp
|     (extern "C" wrapper functions)  |
+-------------------------------------+
|      OpenTimelineIO C++ Library     |  <- vendor/OpenTimelineIO
|   (otio::Timeline, otio::Clip...)   |
+-------------------------------------+
```

**Why a C shim?** OpenTimelineIO is a C++ library with templates, inheritance, and smart pointers. Rust's `bindgen` can only generate bindings to C APIs. The shim layer:
- Exposes a pure C API with opaque pointer handles
- Catches C++ exceptions and converts them to error codes
- Manages memory ownership explicitly

### Memory Ownership

Objects follow clear ownership rules across the FFI boundary:

| Scenario | Ownership |
|----------|-----------|
| `Timeline::new()` | Rust owns the Timeline |
| `timeline.add_video_track()` | Timeline owns the Track (returns non-owning handle) |
| `Clip::new()` | Rust owns the Clip |
| `track.append_clip(clip)` | Track takes ownership (Clip consumed via `mem::forget`) |
| Iterator items (`ClipRef`, `TrackRef`) | Non-owning references (lifetime tied to parent) |
| `Neighbor::GeneratedGap` from `track.neighbors_of()` | Rust owns the Gap (OpenTimelineIO made it up; it is not in the track) |

Each owning handle holds one OpenTimelineIO managed reference, taken when the shim creates or loads the object and released by its `Drop`. OpenTimelineIO deletes an object as soon as its reference count returns to 0, and calls such as `to_json_string` briefly retain and release the object they act on, so a handle without its own reference would be deleted by the first such call.

When appending/inserting children, the parent takes its own reference, the shim releases the handle's reference, and Rust's destructor is bypassed:

```rust
pub fn append_clip(&mut self, child: Clip) -> Result<()> {
    // ... FFI call transfers ownership to C++ ...
    std::mem::forget(child);  // Don't run Rust Drop
    Ok(())
}
```

### Thread Safety

Types implement `Send` but not `Sync`:
- **Safe:** Moving a Timeline to another thread
- **Unsafe:** Sharing a Timeline between threads simultaneously

Use `Arc<Mutex<Timeline>>` for shared access across threads.

## Project Structure

```
otio-rs/
├── Cargo.toml          # Rust package manifest
├── build.rs            # Build script (CMake + bindgen)
├── src/
│   ├── lib.rs          # Core types (Timeline, Track, Clip, Gap, Stack)
│   ├── types.rs        # Type aliases (Result)
│   ├── traits.rs       # HasMetadata trait
│   ├── iterators.rs    # Iteration support (Composable enum, *Ref types)
│   ├── builders.rs     # Builder pattern (ClipBuilder, TimelineBuilder)
│   ├── macros.rs       # Internal macros reducing FFI boilerplate
│   ├── marker.rs       # Marker type and color constants
│   ├── effect.rs       # Effect wrapper
│   ├── time_effect.rs  # LinearTimeWarp, FreezeFrame
│   ├── transition.rs   # Transition type
│   ├── image_sequence_reference.rs  # VFX image sequences
│   ├── generator_reference.rs       # Synthetic media generators
│   └── missing_reference.rs         # Placeholder for missing media
├── shim/
│   ├── otio_shim.h     # C interface header (~400 functions)
│   ├── otio_shim.cpp   # C++ implementation wrapping OTIO
│   └── CMakeLists.txt  # CMake build configuration
├── vendor/
│   └── OpenTimelineIO/ # Git submodule (v0.17.0)
├── scripts/
│   ├── check_memory.sh # Valgrind memory leak detection
│   └── valgrind.supp   # Valgrind suppressions
├── examples/
│   ├── dummy.rs        # Basic usage
│   ├── iterate.rs      # Iteration example
│   ├── modify.rs       # Insert/remove operations
│   └── builder.rs      # Builder pattern
└── tests/
    ├── extended_features.rs  # Comprehensive feature tests
    ├── timeline_iteration.rs # Track filtering, neighbors, available_range tests
    ├── memory.rs             # Memory leak stress tests
    ├── error_handling.rs     # FFI error propagation tests
    ├── roundtrip.rs          # File I/O tests
    ├── metadata.rs           # Metadata tests
    ├── nested.rs             # Nested structure tests
    ├── iteration.rs          # Iteration tests
    ├── modify_operations.rs  # Insert/remove tests
    └── builders.rs           # Builder pattern tests
```

## Troubleshooting

### "could not find native static library"
Ensure CMake completed successfully. Check the build output for CMake errors.

### "opentimelineio/timeline.h not found"
Make sure submodules are initialized:
```bash
git submodule update --init --recursive
```

### Build takes too long
The first build compiles the entire OpenTimelineIO C++ library. Subsequent builds are incremental and much faster. Use `cargo build --release` for optimized builds.

### macOS: "xcrun: error: invalid active developer path"
Install Xcode command line tools:
```bash
xcode-select --install
```

### Using system OpenTimelineIO
If using the `system` feature, ensure OpenTimelineIO is installed and discoverable via pkg-config:
```bash
pkg-config --modversion opentimelineio
```

## License

Licensed under the Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <http://www.apache.org/licenses/LICENSE-2.0>).
