#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define REQUIRE(cond) do { if (!(cond)) { std::cerr << "FAIL at " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; std::exit(1); } } while (0)

namespace {

struct MemOStream {
    clap_ostream_t stream;
    std::string data;
};

int64_t memWrite(const clap_ostream_t* s, const void* buf, uint64_t size) {
    auto* m = reinterpret_cast<MemOStream*>(s->ctx);
    m->data.append(static_cast<const char*>(buf), static_cast<size_t>(size));
    return static_cast<int64_t>(size);
}

struct MemIStream {
    clap_istream_t stream;
    const std::string* data = nullptr;
    size_t pos = 0;
};

int64_t memRead(const clap_istream_t* s, void* buf, uint64_t size) {
    auto* m = reinterpret_cast<MemIStream*>(s->ctx);
    const size_t avail = m->data->size() - m->pos;
    if (avail == 0) return 0;
    const size_t n = std::min<size_t>(avail, static_cast<size_t>(size));
    std::memcpy(buf, m->data->data() + m->pos, n);
    m->pos += n;
    return static_cast<int64_t>(n);
}

struct SingleNoteEvents {
    clap_input_events_t list;
    clap_event_note_t noteOn;
    bool hasNote = false;
};

uint32_t evSize(const clap_input_events_t* l) {
    return static_cast<const SingleNoteEvents*>(l->ctx)->hasNote ? 1 : 0;
}

const clap_event_header_t* evGet(const clap_input_events_t* l, uint32_t idx) {
    auto* s = static_cast<const SingleNoteEvents*>(l->ctx);
    return (idx == 0 && s->hasNote) ? &s->noteOn.header : nullptr;
}

bool outEvTryPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "--vst3") == 0) {
        const char* vst3Path = argv[2];
        std::cout << "[test_vst3_smoke] Loading " << vst3Path << "...\n";
#ifdef _WIN32
        HMODULE mod = LoadLibraryA(vst3Path);
        if (!mod) {
            std::cerr << "LoadLibraryA failed: error " << GetLastError() << "\n";
            return 1;
        }
        using InitFn = bool(*)();
        using ExitFn = bool(*)();
        using FactoryFn = void*(*)();
        auto initDll = reinterpret_cast<InitFn>(GetProcAddress(mod, "InitDll"));
        auto exitDll = reinterpret_cast<ExitFn>(GetProcAddress(mod, "ExitDll"));
        auto getFactory = reinterpret_cast<FactoryFn>(GetProcAddress(mod, "GetPluginFactory"));
        REQUIRE(initDll != nullptr && exitDll != nullptr && getFactory != nullptr);
        REQUIRE(initDll());
        void* factory = getFactory();
        REQUIRE(factory != nullptr);
        std::cout << "[test_vst3_smoke] InitDll + GetPluginFactory OK (factory=" << factory << ")\n";
        REQUIRE(exitDll());
        FreeLibrary(mod);
#else
        void* mod = dlopen(vst3Path, RTLD_NOW | RTLD_LOCAL);
        REQUIRE(mod != nullptr);
        auto getFactory = reinterpret_cast<void*(*)()>(dlsym(mod, "GetPluginFactory"));
        REQUIRE(getFactory != nullptr);
        REQUIRE(getFactory() != nullptr);
        dlclose(mod);
#endif
        std::cout << "[test_vst3_smoke] VST3 SMOKE PASSED\n";
        return 0;
    }

    std::string clapPathStr = argc > 1 ? argv[1] : "./GASFREE.clap";
#if defined(__APPLE__)
    // On macOS a .clap is a bundle directory; load its executable like a host would.
    {
        std::filesystem::path p(clapPathStr);
        if (std::filesystem::is_directory(p))
            clapPathStr = (p / "Contents" / "MacOS" / p.stem()).string();
    }
#endif
    const char* clapPath = clapPathStr.c_str();
    std::cout << "[test_clap_host] Loading " << clapPath << "...\n";

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(clapPath);
    if (!handle) {
        std::cerr << "LoadLibraryA failed: error " << GetLastError() << "\n";
        return 1;
    }
    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(handle, "clap_entry"));
#else
    void* handle = dlopen(clapPath, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
#endif
    REQUIRE(entry != nullptr);
    REQUIRE(clap_version_is_compatible(entry->clap_version));
    REQUIRE(entry->init(clapPath));

    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    REQUIRE(factory != nullptr);
    REQUIRE(factory->get_plugin_count(factory) == 1);

    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    REQUIRE(desc != nullptr);
    std::cout << "[test_clap_host] Found plugin: " << desc->name << " (" << desc->id << ") v" << desc->version << "\n";

    clap_host_t host{};
    host.clap_version = CLAP_VERSION_INIT;
    host.name = "gasfree-test-host";
    host.vendor = "gasfree";
    host.version = "0.1.0";
    host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    host.request_restart = [](const clap_host_t*) {};
    host.request_process = [](const clap_host_t*) {};
    host.request_callback = [](const clap_host_t*) {};

    const clap_plugin_t* plug = factory->create_plugin(factory, &host, desc->id);
    REQUIRE(plug != nullptr);
    REQUIRE(plug->init(plug));

    // Check extensions
    auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS));
    auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(plug->get_extension(plug, CLAP_EXT_NOTE_PORTS));
    auto* params = static_cast<const clap_plugin_params_t*>(plug->get_extension(plug, CLAP_EXT_PARAMS));
    auto* state = static_cast<const clap_plugin_state_t*>(plug->get_extension(plug, CLAP_EXT_STATE));
    auto* gui = static_cast<const clap_plugin_gui_t*>(plug->get_extension(plug, CLAP_EXT_GUI));
    REQUIRE(audioPorts && notePorts && params && state && gui);
    REQUIRE(audioPorts->count(plug, true) == 1);
    REQUIRE(audioPorts->count(plug, false) == 1);
    REQUIRE(notePorts->count(plug, true) == 1);
    REQUIRE(params->count(plug) == 17);
#ifdef _WIN32
    REQUIRE(gui->is_api_supported(plug, CLAP_WINDOW_API_WIN32, false));
#elif defined(__APPLE__)
    REQUIRE(gui->is_api_supported(plug, CLAP_WINDOW_API_COCOA, false));
#else
    REQUIRE(gui->is_api_supported(plug, CLAP_WINDOW_API_X11, false));
#endif

    // Activate & process audio with CLAP_EVENT_NOTE_ON
    REQUIRE(plug->activate(plug, 48000.0, 64, 512));
    REQUIRE(plug->start_processing(plug));

    std::vector<float> inL(256, 0.0f), inR(256, 0.0f), outL(256, 0.0f), outR(256, 0.0f);
    float* inPtrs[2] = {inL.data(), inR.data()};
    float* outPtrs[2] = {outL.data(), outR.data()};

    clap_audio_buffer_t inBuf{};
    inBuf.data32 = inPtrs;
    inBuf.channel_count = 2;

    clap_audio_buffer_t outBuf{};
    outBuf.data32 = outPtrs;
    outBuf.channel_count = 2;

    SingleNoteEvents evs{};
    evs.list.ctx = &evs;
    evs.list.size = evSize;
    evs.list.get = evGet;
    evs.noteOn.header.size = sizeof(clap_event_note_t);
    evs.noteOn.header.time = 0;
    evs.noteOn.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    evs.noteOn.header.type = CLAP_EVENT_NOTE_ON;
    evs.noteOn.header.flags = 0;
    evs.noteOn.note_id = 1;
    evs.noteOn.port_index = 0;
    evs.noteOn.channel = 0;
    evs.noteOn.key = 60;
    evs.noteOn.velocity = 1.0;

    clap_output_events_t outEvs{nullptr, outEvTryPush};

    clap_event_transport_t transport{};
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
    transport.tempo = 125.0;

    double energy = 0.0;
    for (int blk = 0; blk < 64; ++blk) {
        evs.hasNote = (blk == 0);
        clap_process_t proc{};
        proc.steady_time = blk * 256;
        proc.frames_count = 256;
        proc.transport = &transport;
        proc.audio_inputs = &inBuf;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &outBuf;
        proc.audio_outputs_count = 1;
        proc.in_events = &evs.list;
        proc.out_events = &outEvs;

        auto status = plug->process(plug, &proc);
        REQUIRE(status == CLAP_PROCESS_CONTINUE);
        for (int i = 0; i < 256; ++i) {
            REQUIRE(std::isfinite(outL[i]) && std::isfinite(outR[i]));
            energy += outL[i] * outL[i] + outR[i] * outR[i];
        }
    }
    const double rms = std::sqrt(energy / (64.0 * 256.0 * 2.0));
    std::cout << "[test_clap_host] Processed 64 blocks with CLAP_EVENT_NOTE_ON: rms=" << rms << "\n";
    REQUIRE(rms > 0.01 && rms < 1.5);

    // State save & reload round-trip
    MemOStream os{};
    os.stream.ctx = &os;
    os.stream.write = memWrite;
    REQUIRE(state->save(plug, &os.stream));
    std::cout << "[test_clap_host] Saved state chunk (" << os.data.size() << " bytes)\n";
    REQUIRE(os.data.size() > 100);

    MemIStream is{};
    is.data = &os.data;
    is.stream.ctx = &is;
    is.stream.read = memRead;
    REQUIRE(state->load(plug, &is.stream));
    std::cout << "[test_clap_host] State load round-trip OK\n";

    plug->stop_processing(plug);
    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif

    std::cout << "[test_clap_host] ALL CLAP HOST CHECKS PASSED\n";
    return 0;
}
