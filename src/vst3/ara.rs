//! Safe Rust wrappers for ARA (Audio Random Access) support in VST3 plugins.
//!
//! ARA is a VST3 extension protocol for deep audio editing (Melodyne, etc.).
//! The host provides audio content via callbacks, and the plugin analyzes/edits it.

use super::ffi;
use super::util::map_error;
use std::ffi::{c_void, CString};
use std::marker::PhantomData;
use std::ptr::NonNull;

/// ARA plugin instance role flags.
#[derive(Debug, Clone, Copy)]
pub struct AraRoles(u32);

impl AraRoles {
    pub const PLAYBACK_RENDERER: u32 = 1 << 0;
    pub const EDITOR_RENDERER: u32 = 1 << 1;
    pub const EDITOR_VIEW: u32 = 1 << 2;

    /// All roles (playback + editor renderer + editor view).
    pub fn all() -> Self {
        Self(Self::PLAYBACK_RENDERER | Self::EDITOR_RENDERER | Self::EDITOR_VIEW)
    }

    /// Playback renderer only.
    pub fn playback() -> Self {
        Self(Self::PLAYBACK_RENDERER)
    }

    pub fn bits(&self) -> u32 {
        self.0
    }
}

/// Information about an ARA-capable plugin's factory.
#[derive(Debug, Clone)]
pub struct AraFactoryInfo {
    pub plugin_name: String,
    pub manufacturer: String,
    pub highest_supported_api: i32,
}

/// An ARA document controller, managing the ARA document model.
///
/// Owns the underlying C handle and destroys it on drop.
pub struct AraDocumentController {
    handle: NonNull<c_void>,
    _not_sync: PhantomData<*const ()>,
}

// Safety: ARA document controllers can be moved between threads
// but must not be accessed concurrently.
unsafe impl Send for AraDocumentController {}

impl AraDocumentController {
    /// Create from a raw handle. The caller must ensure the handle is valid.
    ///
    /// # Safety
    /// `handle` must be a valid, non-null document controller handle
    /// returned by `rack_vst3_ara_create_document_controller`.
    pub(crate) unsafe fn from_raw(handle: NonNull<c_void>) -> Self {
        Self {
            handle,
            _not_sync: PhantomData,
        }
    }

    /// Begin an editing session (bracket model changes).
    pub fn begin_editing(&self) {
        unsafe { ffi::rack_vst3_ara_begin_editing(self.handle.as_ptr()) }
    }

    /// End an editing session.
    pub fn end_editing(&self) {
        unsafe { ffi::rack_vst3_ara_end_editing(self.handle.as_ptr()) }
    }

    /// Notify the plugin about pending model updates.
    pub fn notify_model_updates(&self) {
        unsafe { ffi::rack_vst3_ara_notify_model_updates(self.handle.as_ptr()) }
    }

    /// Create an audio source in the ARA document.
    pub fn create_audio_source(
        &self,
        host_ref: *mut c_void,
        name: &str,
        persistent_id: &str,
        sample_count: i64,
        sample_rate: f64,
        channel_count: i32,
    ) -> Option<AraAudioSource> {
        let c_name = CString::new(name).ok()?;
        let c_id = CString::new(persistent_id).ok()?;
        let ptr = unsafe {
            ffi::rack_vst3_ara_create_audio_source(
                self.handle.as_ptr(),
                host_ref,
                c_name.as_ptr(),
                c_id.as_ptr(),
                sample_count,
                sample_rate,
                channel_count,
            )
        };
        NonNull::new(ptr).map(|handle| AraAudioSource {
            handle,
            controller: self.handle,
        })
    }

    /// Enable or disable sample access on an audio source.
    pub fn enable_audio_source_access(
        &self,
        source: &AraAudioSource,
        enable: bool,
    ) -> crate::Result<()> {
        let rc = unsafe {
            ffi::rack_vst3_ara_enable_audio_source_access(
                self.handle.as_ptr(),
                source.handle.as_ptr(),
                if enable { 1 } else { 0 },
            )
        };
        if rc != ffi::RACK_VST3_OK {
            return Err(map_error(rc));
        }
        Ok(())
    }

    /// Create an audio modification for an audio source.
    pub fn create_audio_modification(
        &self,
        source: &AraAudioSource,
        host_ref: *mut c_void,
        name: &str,
        persistent_id: &str,
    ) -> Option<AraAudioModification> {
        let c_name = CString::new(name).ok()?;
        let c_id = CString::new(persistent_id).ok()?;
        let ptr = unsafe {
            ffi::rack_vst3_ara_create_audio_modification(
                self.handle.as_ptr(),
                source.handle.as_ptr(),
                host_ref,
                c_name.as_ptr(),
                c_id.as_ptr(),
            )
        };
        NonNull::new(ptr).map(|handle| AraAudioModification {
            handle,
            controller: self.handle,
        })
    }

    /// Create a playback region for an audio modification.
    pub fn create_playback_region(
        &self,
        modification: &AraAudioModification,
        host_ref: *mut c_void,
        start_in_mod: f64,
        duration_in_mod: f64,
        start_in_playback: f64,
        duration_in_playback: f64,
    ) -> Option<AraPlaybackRegion> {
        let ptr = unsafe {
            ffi::rack_vst3_ara_create_playback_region(
                self.handle.as_ptr(),
                modification.handle.as_ptr(),
                host_ref,
                start_in_mod,
                duration_in_mod,
                start_in_playback,
                duration_in_playback,
            )
        };
        NonNull::new(ptr).map(|handle| AraPlaybackRegion {
            handle,
            controller: self.handle,
        })
    }

    /// Get the raw handle (for binding operations).
    pub(crate) fn raw(&self) -> *mut c_void {
        self.handle.as_ptr()
    }
}

impl Drop for AraDocumentController {
    fn drop(&mut self) {
        unsafe { ffi::rack_vst3_ara_destroy_document_controller(self.handle.as_ptr()) }
    }
}

/// An audio source in the ARA document model.
pub struct AraAudioSource {
    handle: NonNull<c_void>,
    controller: NonNull<c_void>,
}

unsafe impl Send for AraAudioSource {}

impl Drop for AraAudioSource {
    fn drop(&mut self) {
        unsafe {
            ffi::rack_vst3_ara_destroy_audio_source(
                self.controller.as_ptr(),
                self.handle.as_ptr(),
            )
        }
    }
}

/// An audio modification in the ARA document model.
pub struct AraAudioModification {
    handle: NonNull<c_void>,
    controller: NonNull<c_void>,
}

unsafe impl Send for AraAudioModification {}

impl Drop for AraAudioModification {
    fn drop(&mut self) {
        unsafe {
            ffi::rack_vst3_ara_destroy_audio_modification(
                self.controller.as_ptr(),
                self.handle.as_ptr(),
            )
        }
    }
}

/// A playback region in the ARA document model.
pub struct AraPlaybackRegion {
    handle: NonNull<c_void>,
    controller: NonNull<c_void>,
}

unsafe impl Send for AraPlaybackRegion {}

impl Drop for AraPlaybackRegion {
    fn drop(&mut self) {
        unsafe {
            ffi::rack_vst3_ara_destroy_playback_region(
                self.controller.as_ptr(),
                self.handle.as_ptr(),
            )
        }
    }
}
