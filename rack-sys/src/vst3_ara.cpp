// ARA (Audio Random Access) support for VST3 plugins hosted via rack.
//
// ARA is a VST3 extension protocol for deep audio editing (Melodyne pitch correction, etc.).
// The host provides audio content + callbacks, and the plugin analyzes and edits it.
//
// This file queries ARA interfaces from loaded VST3 plugins and forwards
// document controller operations through the ARA C API vtables.

#include "rack_vst3.h"

#ifdef RACK_HAS_ARA

// ARA SDK headers (C API only - no .cpp sources needed)
#include "ARAInterface.h"
#include "ARAVST3.h"

// VST3 SDK headers for COM interface queries
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <cstring>
#include <mutex>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Define ARA interface IIDs (DECLARE_CLASS_IID only declares; DEF_CLASS_IID defines the storage)
namespace ARA {
DEF_CLASS_IID (IMainFactory)
DEF_CLASS_IID (IPlugInEntryPoint)
DEF_CLASS_IID (IPlugInEntryPoint2)
}

// ============================================================================
// Internal: access to RackVST3Plugin internals
// ============================================================================

// Forward-declare the struct from vst3_instance.cpp so we can access its component.
// We only need the IComponent pointer for COM queries.
struct RackVST3Plugin;

// Implemented in vst3_instance.cpp — gives us access to the component for COM queries.
extern "C++" IPtr<IComponent>& rack_vst3_plugin_get_component(RackVST3Plugin* plugin);

// ============================================================================
// ARA Document Controller wrapper
// ============================================================================

struct RackAraDocumentController {
    ARA::ARADocumentControllerRef ref;
    const ARA::ARADocumentControllerInterface* iface;

    // Host callback interfaces (must outlive the document controller)
    ARA::ARAAudioAccessControllerInterface audio_access_iface;
    ARA::ARAArchivingControllerInterface archiving_iface;
    ARA::ARADocumentControllerHostInstance host_instance;

    // Rust-side callbacks (stored so C trampolines can reach them)
    RackAraHostCallbacks rust_callbacks;
};

// ============================================================================
// C trampoline functions: ARA calls these, they forward to Rust callbacks
// ============================================================================

static ARA::ARAAudioReaderHostRef trampoline_create_audio_reader(
    ARA::ARAAudioAccessControllerHostRef controllerHostRef,
    ARA::ARAAudioSourceHostRef audioSourceHostRef,
    ARA::ARABool use64BitSamples)
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (!doc->rust_callbacks.create_audio_reader) return nullptr;
    return reinterpret_cast<ARA::ARAAudioReaderHostRef>(
        doc->rust_callbacks.create_audio_reader(
            doc->rust_callbacks.context,
            reinterpret_cast<void*>(audioSourceHostRef),
            use64BitSamples ? 1 : 0));
}

static ARA::ARABool trampoline_read_audio_samples(
    ARA::ARAAudioAccessControllerHostRef controllerHostRef,
    ARA::ARAAudioReaderHostRef audioReaderHostRef,
    ARA::ARASamplePosition samplePosition,
    ARA::ARASampleCount samplesPerChannel,
    void* const buffers[])
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (!doc->rust_callbacks.read_audio_samples) return ARA::kARAFalse;
    int result = doc->rust_callbacks.read_audio_samples(
        doc->rust_callbacks.context,
        reinterpret_cast<void*>(audioReaderHostRef),
        samplePosition,
        samplesPerChannel,
        const_cast<void**>(buffers));
    return result ? ARA::kARATrue : ARA::kARAFalse;
}

static void trampoline_destroy_audio_reader(
    ARA::ARAAudioAccessControllerHostRef controllerHostRef,
    ARA::ARAAudioReaderHostRef audioReaderHostRef)
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (doc->rust_callbacks.destroy_audio_reader) {
        doc->rust_callbacks.destroy_audio_reader(
            doc->rust_callbacks.context,
            reinterpret_cast<void*>(audioReaderHostRef));
    }
}

static ARA::ARASize trampoline_get_archive_size(
    ARA::ARAArchivingControllerHostRef controllerHostRef,
    ARA::ARAArchiveReaderHostRef archiveReaderHostRef)
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (!doc->rust_callbacks.get_archive_size) return 0;
    return doc->rust_callbacks.get_archive_size(
        doc->rust_callbacks.context,
        reinterpret_cast<void*>(archiveReaderHostRef));
}

static ARA::ARABool trampoline_read_bytes_from_archive(
    ARA::ARAArchivingControllerHostRef controllerHostRef,
    ARA::ARAArchiveReaderHostRef archiveReaderHostRef,
    ARA::ARASize position,
    ARA::ARASize length,
    ARA::ARAByte buffer[])
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (!doc->rust_callbacks.read_archive) return ARA::kARAFalse;
    int result = doc->rust_callbacks.read_archive(
        doc->rust_callbacks.context,
        reinterpret_cast<void*>(archiveReaderHostRef),
        position, length, buffer);
    return result ? ARA::kARATrue : ARA::kARAFalse;
}

static ARA::ARABool trampoline_write_bytes_to_archive(
    ARA::ARAArchivingControllerHostRef controllerHostRef,
    ARA::ARAArchiveWriterHostRef archiveWriterHostRef,
    ARA::ARASize position,
    ARA::ARASize length,
    const ARA::ARAByte buffer[])
{
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controllerHostRef);
    if (!doc->rust_callbacks.write_archive) return ARA::kARAFalse;
    int result = doc->rust_callbacks.write_archive(
        doc->rust_callbacks.context,
        reinterpret_cast<void*>(archiveWriterHostRef),
        position, length, buffer);
    return result ? ARA::kARATrue : ARA::kARAFalse;
}

static void trampoline_notify_archiving_progress(
    ARA::ARAArchivingControllerHostRef, float) {}

static void trampoline_notify_unarchiving_progress(
    ARA::ARAArchivingControllerHostRef, float) {}

// ============================================================================
// Helper: query ARA factory from a VST3 plugin
// ============================================================================

static const ARA::ARAFactory* query_ara_factory(RackVST3Plugin* plugin) {
    if (!plugin) return nullptr;

    auto& component = rack_vst3_plugin_get_component(plugin);
    if (!component) return nullptr;

    // Try IMainFactory first (factory-level, preferred)
    FUnknownPtr<ARA::IMainFactory> main_factory(component);
    if (main_factory) {
        return main_factory->getFactory();
    }

    // Try IPlugInEntryPoint (component-level, ARA 1.x compat)
    FUnknownPtr<ARA::IPlugInEntryPoint> entry_point(component);
    if (entry_point) {
        return entry_point->getFactory();
    }

    return nullptr;
}

// ============================================================================
// Extern "C" API implementation
// ============================================================================

extern "C" {

int rack_vst3_plugin_has_ara(RackVST3Plugin* plugin) {
    return query_ara_factory(plugin) != nullptr ? 1 : 0;
}

const void* rack_vst3_plugin_get_ara_factory(RackVST3Plugin* plugin) {
    return reinterpret_cast<const void*>(query_ara_factory(plugin));
}

int rack_vst3_plugin_get_ara_factory_info(
    RackVST3Plugin* plugin,
    char* plugin_name, size_t name_size,
    char* manufacturer, size_t mfr_size,
    int32_t* highest_supported_api)
{
    const auto* factory = query_ara_factory(plugin);
    if (!factory) return RACK_VST3_ERROR_NOT_SUPPORTED;

    if (plugin_name && name_size > 0 && factory->plugInName) {
        strncpy(plugin_name, factory->plugInName, name_size - 1);
        plugin_name[name_size - 1] = '\0';
    }
    if (manufacturer && mfr_size > 0 && factory->manufacturerName) {
        strncpy(manufacturer, factory->manufacturerName, mfr_size - 1);
        manufacturer[mfr_size - 1] = '\0';
    }
    if (highest_supported_api) {
        *highest_supported_api = static_cast<int32_t>(factory->highestSupportedApiGeneration);
    }
    return RACK_VST3_OK;
}

int rack_vst3_ara_init(RackVST3Plugin* plugin) {
    const auto* factory = query_ara_factory(plugin);
    if (!factory) return RACK_VST3_ERROR_NOT_SUPPORTED;

    // Use ARA 2.0 if supported, otherwise 1.0
    ARA::ARAAPIGeneration desired = ARA::kARAAPIGeneration_2_0_Final;
    if (desired > factory->highestSupportedApiGeneration) {
        desired = factory->highestSupportedApiGeneration;
    }
    if (desired < factory->lowestSupportedApiGeneration) {
        return RACK_VST3_ERROR_NOT_SUPPORTED;
    }

    ARA::ARAInterfaceConfiguration config;
    memset(&config, 0, sizeof(config));
    config.structSize = sizeof(config);
    config.desiredApiGeneration = desired;
    config.assertFunctionAddress = nullptr;

    factory->initializeARAWithConfiguration(&config);
    return RACK_VST3_OK;
}

void rack_vst3_ara_uninit(RackVST3Plugin* plugin) {
    const auto* factory = query_ara_factory(plugin);
    if (factory && factory->uninitializeARA) {
        factory->uninitializeARA();
    }
}

void* rack_vst3_ara_create_document_controller(
    RackVST3Plugin* plugin,
    const RackAraHostCallbacks* callbacks,
    const char* document_name)
{
    if (!plugin || !callbacks) return nullptr;

    const auto* factory = query_ara_factory(plugin);
    if (!factory || !factory->createDocumentControllerWithDocument) return nullptr;

    auto* doc = new RackAraDocumentController();
    doc->rust_callbacks = *callbacks;

    // Set up audio access controller interface
    memset(&doc->audio_access_iface, 0, sizeof(doc->audio_access_iface));
    doc->audio_access_iface.structSize = sizeof(doc->audio_access_iface);
    doc->audio_access_iface.createAudioReaderForSource = trampoline_create_audio_reader;
    doc->audio_access_iface.readAudioSamples = trampoline_read_audio_samples;
    doc->audio_access_iface.destroyAudioReader = trampoline_destroy_audio_reader;

    // Set up archiving controller interface
    memset(&doc->archiving_iface, 0, sizeof(doc->archiving_iface));
    doc->archiving_iface.structSize = sizeof(doc->archiving_iface);
    doc->archiving_iface.getArchiveSize = trampoline_get_archive_size;
    doc->archiving_iface.readBytesFromArchive = trampoline_read_bytes_from_archive;
    doc->archiving_iface.writeBytesToArchive = trampoline_write_bytes_to_archive;
    doc->archiving_iface.notifyDocumentArchivingProgress = trampoline_notify_archiving_progress;
    doc->archiving_iface.notifyDocumentUnarchivingProgress = trampoline_notify_unarchiving_progress;

    // Set up host instance (required + optional interfaces)
    memset(&doc->host_instance, 0, sizeof(doc->host_instance));
    doc->host_instance.structSize = sizeof(doc->host_instance);
    doc->host_instance.audioAccessControllerHostRef =
        reinterpret_cast<ARA::ARAAudioAccessControllerHostRef>(doc);
    doc->host_instance.audioAccessControllerInterface = &doc->audio_access_iface;
    doc->host_instance.archivingControllerHostRef =
        reinterpret_cast<ARA::ARAArchivingControllerHostRef>(doc);
    doc->host_instance.archivingControllerInterface = &doc->archiving_iface;
    // Optional interfaces: NULL (not provided)
    doc->host_instance.contentAccessControllerHostRef = nullptr;
    doc->host_instance.contentAccessControllerInterface = nullptr;
    doc->host_instance.modelUpdateControllerHostRef = nullptr;
    doc->host_instance.modelUpdateControllerInterface = nullptr;
    doc->host_instance.playbackControllerHostRef = nullptr;
    doc->host_instance.playbackControllerInterface = nullptr;

    // Create document properties
    ARA::ARADocumentProperties doc_props;
    memset(&doc_props, 0, sizeof(doc_props));
    doc_props.structSize = sizeof(doc_props);
    doc_props.name = document_name;

    // Create the document controller
    const auto* instance = factory->createDocumentControllerWithDocument(
        &doc->host_instance, &doc_props);
    if (!instance || !instance->documentControllerRef || !instance->documentControllerInterface) {
        delete doc;
        return nullptr;
    }

    doc->ref = instance->documentControllerRef;
    doc->iface = instance->documentControllerInterface;
    return doc;
}

void rack_vst3_ara_destroy_document_controller(void* controller) {
    if (!controller) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->destroyDocumentController) {
        doc->iface->destroyDocumentController(doc->ref);
    }
    delete doc;
}

int rack_vst3_ara_bind_to_document(
    RackVST3Plugin* plugin,
    void* controller,
    uint32_t roles)
{
    if (!plugin || !controller) return RACK_VST3_ERROR_INVALID_PARAM;

    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    auto& component = rack_vst3_plugin_get_component(plugin);
    if (!component) return RACK_VST3_ERROR_NOT_INITIALIZED;

    // Try IPlugInEntryPoint2 (ARA 2.0)
    FUnknownPtr<ARA::IPlugInEntryPoint2> entry2(component);
    if (entry2) {
        auto assigned = static_cast<ARA::ARAPlugInInstanceRoleFlags>(roles);
        // knownRoles = all roles we know about
        auto known = static_cast<ARA::ARAPlugInInstanceRoleFlags>(
            ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole);
        entry2->bindToDocumentControllerWithRoles(doc->ref, known, assigned);
        return RACK_VST3_OK;
    }

    // Fallback: IPlugInEntryPoint (ARA 1.x)
    FUnknownPtr<ARA::IPlugInEntryPoint> entry1(component);
    if (entry1) {
        entry1->bindToDocumentController(doc->ref);
        return RACK_VST3_OK;
    }

    return RACK_VST3_ERROR_NOT_SUPPORTED;
}

// ============================================================================
// Document Model Operations
// ============================================================================

void* rack_vst3_ara_create_audio_source(
    void* controller,
    void* host_ref,
    const char* name,
    const char* persistent_id,
    int64_t sample_count,
    double sample_rate,
    int32_t channel_count)
{
    if (!controller) return nullptr;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (!doc->iface || !doc->iface->createAudioSource) return nullptr;

    ARA::ARAAudioSourceProperties props;
    memset(&props, 0, sizeof(props));
    props.structSize = sizeof(props);
    props.name = name;
    props.persistentID = persistent_id;
    props.sampleCount = sample_count;
    props.sampleRate = sample_rate;
    props.channelCount = channel_count;
    props.merits64BitSamples = ARA::kARAFalse;

    auto ref = doc->iface->createAudioSource(
        doc->ref,
        reinterpret_cast<ARA::ARAAudioSourceHostRef>(host_ref),
        &props);
    return reinterpret_cast<void*>(ref);
}

int rack_vst3_ara_enable_audio_source_access(void* controller, void* source, int enable) {
    if (!controller || !source) return RACK_VST3_ERROR_INVALID_PARAM;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (!doc->iface || !doc->iface->enableAudioSourceSamplesAccess) return RACK_VST3_ERROR_NOT_SUPPORTED;

    doc->iface->enableAudioSourceSamplesAccess(
        doc->ref,
        reinterpret_cast<ARA::ARAAudioSourceRef>(source),
        enable ? ARA::kARATrue : ARA::kARAFalse);
    return RACK_VST3_OK;
}

void* rack_vst3_ara_create_audio_modification(
    void* controller,
    void* audio_source,
    void* host_ref,
    const char* name,
    const char* persistent_id)
{
    if (!controller || !audio_source) return nullptr;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (!doc->iface || !doc->iface->createAudioModification) return nullptr;

    ARA::ARAAudioModificationProperties props;
    memset(&props, 0, sizeof(props));
    props.structSize = sizeof(props);
    props.name = name;
    props.persistentID = persistent_id;

    auto ref = doc->iface->createAudioModification(
        doc->ref,
        reinterpret_cast<ARA::ARAAudioSourceRef>(audio_source),
        reinterpret_cast<ARA::ARAAudioModificationHostRef>(host_ref),
        &props);
    return reinterpret_cast<void*>(ref);
}

void* rack_vst3_ara_create_playback_region(
    void* controller,
    void* audio_modification,
    void* host_ref,
    double start_in_mod,
    double duration_in_mod,
    double start_in_playback,
    double duration_in_playback)
{
    if (!controller || !audio_modification) return nullptr;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (!doc->iface || !doc->iface->createPlaybackRegion) return nullptr;

    ARA::ARAPlaybackRegionProperties props;
    memset(&props, 0, sizeof(props));
    props.structSize = sizeof(props);
    props.transformationFlags = 0; // No transformation by default
    props.startInModificationTime = start_in_mod;
    props.durationInModificationTime = duration_in_mod;
    props.startInPlaybackTime = start_in_playback;
    props.durationInPlaybackTime = duration_in_playback;

    auto ref = doc->iface->createPlaybackRegion(
        doc->ref,
        reinterpret_cast<ARA::ARAAudioModificationRef>(audio_modification),
        reinterpret_cast<ARA::ARAPlaybackRegionHostRef>(host_ref),
        &props);
    return reinterpret_cast<void*>(ref);
}

void rack_vst3_ara_destroy_playback_region(void* controller, void* region) {
    if (!controller || !region) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->destroyPlaybackRegion) {
        doc->iface->destroyPlaybackRegion(doc->ref,
            reinterpret_cast<ARA::ARAPlaybackRegionRef>(region));
    }
}

void rack_vst3_ara_destroy_audio_modification(void* controller, void* modification) {
    if (!controller || !modification) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->destroyAudioModification) {
        doc->iface->destroyAudioModification(doc->ref,
            reinterpret_cast<ARA::ARAAudioModificationRef>(modification));
    }
}

void rack_vst3_ara_destroy_audio_source(void* controller, void* source) {
    if (!controller || !source) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->destroyAudioSource) {
        doc->iface->destroyAudioSource(doc->ref,
            reinterpret_cast<ARA::ARAAudioSourceRef>(source));
    }
}

void rack_vst3_ara_begin_editing(void* controller) {
    if (!controller) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->beginEditing) {
        doc->iface->beginEditing(doc->ref);
    }
}

void rack_vst3_ara_end_editing(void* controller) {
    if (!controller) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->endEditing) {
        doc->iface->endEditing(doc->ref);
    }
}

void rack_vst3_ara_notify_model_updates(void* controller) {
    if (!controller) return;
    auto* doc = reinterpret_cast<RackAraDocumentController*>(controller);
    if (doc->iface && doc->iface->notifyModelUpdates) {
        doc->iface->notifyModelUpdates(doc->ref);
    }
}

} // extern "C"

#else // !RACK_HAS_ARA — stub implementations when ARA SDK is not available

extern "C" {

int rack_vst3_plugin_has_ara(RackVST3Plugin*) { return 0; }
const void* rack_vst3_plugin_get_ara_factory(RackVST3Plugin*) { return nullptr; }
int rack_vst3_plugin_get_ara_factory_info(RackVST3Plugin*, char*, size_t, char*, size_t, int32_t*) { return RACK_VST3_ERROR_NOT_SUPPORTED; }
int rack_vst3_ara_init(RackVST3Plugin*) { return RACK_VST3_ERROR_NOT_SUPPORTED; }
void rack_vst3_ara_uninit(RackVST3Plugin*) {}
void* rack_vst3_ara_create_document_controller(RackVST3Plugin*, const RackAraHostCallbacks*, const char*) { return nullptr; }
void rack_vst3_ara_destroy_document_controller(void*) {}
int rack_vst3_ara_bind_to_document(RackVST3Plugin*, void*, uint32_t) { return RACK_VST3_ERROR_NOT_SUPPORTED; }
void* rack_vst3_ara_create_audio_source(void*, void*, const char*, const char*, int64_t, double, int32_t) { return nullptr; }
int rack_vst3_ara_enable_audio_source_access(void*, void*, int) { return RACK_VST3_ERROR_NOT_SUPPORTED; }
void* rack_vst3_ara_create_audio_modification(void*, void*, void*, const char*, const char*) { return nullptr; }
void* rack_vst3_ara_create_playback_region(void*, void*, void*, double, double, double, double) { return nullptr; }
void rack_vst3_ara_destroy_playback_region(void*, void*) {}
void rack_vst3_ara_destroy_audio_modification(void*, void*) {}
void rack_vst3_ara_destroy_audio_source(void*, void*) {}
void rack_vst3_ara_begin_editing(void*) {}
void rack_vst3_ara_end_editing(void*) {}
void rack_vst3_ara_notify_model_updates(void*) {}

} // extern "C"

#endif // RACK_HAS_ARA
