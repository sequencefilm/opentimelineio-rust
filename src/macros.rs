//! Internal macros for reducing boilerplate in FFI wrapper code.
//!
//! These macros reduce ~400 lines of repetitive FFI code to ~100 lines.

/// Creates a new FFI error struct initialized to zero.
macro_rules! ffi_error {
    () => {
        crate::ffi::OtioError {
            code: 0,
            message: [0; 256],
        }
    };
}

/// Implements an append method that transfers ownership to C++.
///
/// # Usage
/// ```ignore
/// impl_append!(append_clip, Clip, otio_track_append_clip,
///     "Append a clip to this track.");
/// ```
macro_rules! impl_append {
    ($method:ident, $child_type:ty, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        ///
        /// # Errors
        ///
        /// Returns an error if the operation fails.
        pub fn $method(&mut self, child: $child_type) -> crate::Result<()> {
            let mut err = crate::macros::ffi_error!();
            let result = unsafe { crate::ffi::$ffi_fn(self.ptr, child.ptr, &mut err) };
            if result != 0 {
                return Err(err.into());
            }
            std::mem::forget(child);
            Ok(())
        }
    };
}

/// Implements an insert method that transfers ownership to C++.
///
/// # Usage
/// ```ignore
/// impl_insert!(insert_clip, Clip, otio_track_insert_clip,
///     "Insert a clip at the given index.");
/// ```
macro_rules! impl_insert {
    ($method:ident, $child_type:ty, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        ///
        /// # Errors
        ///
        /// Returns an error if the operation fails.
        #[allow(clippy::cast_possible_truncation)]
        #[allow(clippy::cast_possible_wrap)]
        pub fn $method(&mut self, index: usize, child: $child_type) -> crate::Result<()> {
            let mut err = crate::macros::ffi_error!();
            let result =
                unsafe { crate::ffi::$ffi_fn(self.ptr, index as i32, child.ptr, &mut err) };
            if result != 0 {
                return Err(err.into());
            }
            std::mem::forget(child);
            Ok(())
        }
    };
}

/// Implements `remove_child` method.
macro_rules! impl_remove_child {
    ($ffi_fn:ident) => {
        /// Remove a child at the given index.
        ///
        /// # Errors
        ///
        /// Returns an error if the index is out of bounds.
        #[allow(clippy::cast_possible_truncation)]
        #[allow(clippy::cast_possible_wrap)]
        pub fn remove_child(&mut self, index: usize) -> crate::Result<()> {
            let mut err = crate::macros::ffi_error!();
            let result = unsafe { crate::ffi::$ffi_fn(self.ptr, index as i32, &mut err) };
            if result != 0 {
                Err(err.into())
            } else {
                Ok(())
            }
        }
    };
}

/// Implements `clear_children` method.
macro_rules! impl_clear_children {
    ($ffi_fn:ident) => {
        /// Clear all children from this container.
        ///
        /// # Errors
        ///
        /// Returns an error if the children cannot be cleared.
        pub fn clear_children(&mut self) -> crate::Result<()> {
            let mut err = crate::macros::ffi_error!();
            let result = unsafe { crate::ffi::$ffi_fn(self.ptr, &mut err) };
            if result != 0 {
                Err(err.into())
            } else {
                Ok(())
            }
        }
    };
}

/// Implements `children_count` method.
macro_rules! impl_children_count {
    ($ffi_fn:ident) => {
        /// Get the number of children in this container.
        #[must_use]
        #[allow(clippy::cast_sign_loss)]
        pub fn children_count(&self) -> usize {
            let count = unsafe { crate::ffi::$ffi_fn(self.ptr) };
            count.max(0) as usize
        }
    };
}

/// Implements all Track child operations (append/insert clip, gap, stack, transition + remove/clear).
///
/// # Usage
/// ```ignore
/// impl Track {
///     impl_track_ops!();
///     // ... other methods
/// }
/// ```
macro_rules! impl_track_ops {
    () => {
        crate::macros::impl_append!(
            append_clip, Clip, otio_track_append_clip,
            "Append a clip to this track."
        );
        crate::macros::impl_append!(
            append_gap, Gap, otio_track_append_gap,
            "Append a gap to this track."
        );
        crate::macros::impl_append!(
            append_stack, Stack, otio_track_append_stack,
            "Append a stack to this track (for versioning/alternatives)."
        );
        crate::macros::impl_append!(
            append_transition, Transition, otio_track_append_transition,
            "Append a transition to this track."
        );

        crate::macros::impl_insert!(
            insert_clip, Clip, otio_track_insert_clip,
            "Insert a clip at the given index."
        );
        crate::macros::impl_insert!(
            insert_gap, Gap, otio_track_insert_gap,
            "Insert a gap at the given index."
        );
        crate::macros::impl_insert!(
            insert_stack, Stack, otio_track_insert_stack,
            "Insert a stack at the given index."
        );
        crate::macros::impl_insert!(
            insert_transition, Transition, otio_track_insert_transition,
            "Insert a transition at the given index."
        );

        crate::macros::impl_children_count!(otio_track_children_count);
        crate::macros::impl_remove_child!(otio_track_remove_child);
        crate::macros::impl_clear_children!(otio_track_clear_children);
    };
}

/// Implements all Stack child operations (append/insert track, clip, gap, stack + remove/clear).
///
/// # Usage
/// ```ignore
/// impl Stack {
///     impl_stack_ops!();
///     // ... other methods
/// }
/// ```
macro_rules! impl_stack_ops {
    () => {
        crate::macros::impl_append!(
            append_track, Track, otio_stack_append_track,
            "Append a track to this stack."
        );
        crate::macros::impl_append!(
            append_clip, Clip, otio_stack_append_clip,
            "Append a clip to this stack."
        );
        crate::macros::impl_append!(
            append_gap, Gap, otio_stack_append_gap,
            "Append a gap to this stack."
        );
        crate::macros::impl_append!(
            append_stack, Stack, otio_stack_append_stack,
            "Append a child stack to this stack."
        );

        crate::macros::impl_insert!(
            insert_track, Track, otio_stack_insert_track,
            "Insert a track at the given index."
        );
        crate::macros::impl_insert!(
            insert_clip, Clip, otio_stack_insert_clip,
            "Insert a clip at the given index."
        );
        crate::macros::impl_insert!(
            insert_gap, Gap, otio_stack_insert_gap,
            "Insert a gap at the given index."
        );
        crate::macros::impl_insert!(
            insert_stack, Stack, otio_stack_insert_stack,
            "Insert a child stack at the given index."
        );

        crate::macros::impl_children_count!(otio_stack_children_count);
        crate::macros::impl_remove_child!(otio_stack_remove_child);
        crate::macros::impl_clear_children!(otio_stack_clear_children);
    };
}

// ============================================================================
// Accessor Generation Macros
// ============================================================================

/// Generates a string getter method that calls an FFI function returning a malloc'd string.
///
/// # Usage
/// ```ignore
/// impl Marker {
///     impl_string_getter!(color, otio_marker_get_color, "Get the marker color.");
/// }
/// ```
macro_rules! impl_string_getter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        #[must_use]
        pub fn $method(&self) -> String {
            let ptr = unsafe { crate::ffi::$ffi_fn(self.ptr) };
            crate::ffi_string_to_rust(ptr)
        }
    };
}

/// Generates a string setter method.
///
/// # Usage
/// ```ignore
/// impl Marker {
///     impl_string_setter!(set_color, otio_marker_set_color, "Set the marker color.");
/// }
/// ```
macro_rules! impl_string_setter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        pub fn $method(&mut self, value: &str) {
            let c_value = std::ffi::CString::new(value).unwrap();
            unsafe { crate::ffi::$ffi_fn(self.ptr, c_value.as_ptr()) };
        }
    };
}

/// Generates a `TimeRange` getter method.
///
/// # Usage
/// ```ignore
/// impl Marker {
///     impl_time_range_getter!(marked_range, otio_marker_get_marked_range, "Get the marked range.");
/// }
/// ```
macro_rules! impl_time_range_getter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        #[must_use]
        pub fn $method(&self) -> crate::TimeRange {
            let ffi_range = unsafe { crate::ffi::$ffi_fn(self.ptr) };
            crate::time_range_from_ffi(&ffi_range)
        }
    };
}

/// Generates a `TimeRange` setter method with error handling.
///
/// # Usage
/// ```ignore
/// impl Marker {
///     impl_time_range_setter!(set_marked_range, otio_marker_set_marked_range, "Set the marked range.");
/// }
/// ```
macro_rules! impl_time_range_setter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        ///
        /// # Errors
        ///
        /// Returns an error if the range cannot be set.
        pub fn $method(&mut self, range: crate::TimeRange) -> crate::Result<()> {
            let mut err = crate::macros::ffi_error!();
            let result = unsafe { crate::ffi::$ffi_fn(self.ptr, range.into(), &mut err) };
            if result != 0 {
                Err(err.into())
            } else {
                Ok(())
            }
        }
    };
}

/// Generates a `RationalTime` getter method.
macro_rules! impl_rational_time_getter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        #[must_use]
        pub fn $method(&self) -> crate::RationalTime {
            let ffi_rt = unsafe { crate::ffi::$ffi_fn(self.ptr) };
            crate::RationalTime::new(ffi_rt.value, ffi_rt.rate)
        }
    };
}

/// Generates a `RationalTime` setter method.
macro_rules! impl_rational_time_setter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        pub fn $method(&mut self, time: crate::RationalTime) {
            unsafe { crate::ffi::$ffi_fn(self.ptr, time.into()) };
        }
    };
}

/// Generates a double getter method.
macro_rules! impl_double_getter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        #[must_use]
        pub fn $method(&self) -> f64 {
            unsafe { crate::ffi::$ffi_fn(self.ptr) }
        }
    };
}

/// Generates a double setter method.
macro_rules! impl_double_setter {
    ($method:ident, $ffi_fn:ident, $doc:expr) => {
        #[doc = $doc]
        pub fn $method(&mut self, value: f64) {
            unsafe { crate::ffi::$ffi_fn(self.ptr, value) };
        }
    };
}

// ============================================================================
// Exports
// ============================================================================

pub(crate) use ffi_error;
pub(crate) use impl_append;
pub(crate) use impl_children_count;
pub(crate) use impl_clear_children;
pub(crate) use impl_double_getter;
pub(crate) use impl_double_setter;
pub(crate) use impl_insert;
pub(crate) use impl_rational_time_getter;
pub(crate) use impl_rational_time_setter;
pub(crate) use impl_remove_child;
pub(crate) use impl_stack_ops;
pub(crate) use impl_string_getter;
pub(crate) use impl_string_setter;
pub(crate) use impl_time_range_getter;
pub(crate) use impl_time_range_setter;
pub(crate) use impl_track_ops;
