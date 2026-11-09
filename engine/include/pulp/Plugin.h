#pragma once
// The internal plugin interface: parameters, presets, state, latency, sidechain, note events.
// Built-in instruments and effects (and, later, hosted VST3 plugins) all sit behind it.
// See docs/PLUGIN_INTERFACE.md for the written contract.
#include "pulp/Core.h"
#include "pulp/Json.h"
#include <functional>

namespace pulp {

struct ParamInfo {
    std::string id;         // stable identifier used in project files and automation
    std::string name;       // shown in the interface
    float min = 0.0f, max = 1.0f, def = 0.0f;
    float skew = 1.0f;      // normalised = ((v-min)/(max-min))^(1/skew); >1 gives more travel to low values
    int steps = 0;          // 0 continuous; n > 0 means n discrete values (min..max integers)
    std::string unit;       // "dB", "Hz", "%", "ms", "st", ...
    std::vector<std::string> choices;   // names for stepped parameters
};

class Param {
public:
    explicit Param(ParamInfo i) : info(std::move(i)), value_(info.def) {}
    const ParamInfo info;
    float get() const { return value_.load(std::memory_order_relaxed); }
    void set(float v) { value_.store(clampT(v, info.min, info.max), std::memory_order_relaxed); }
    float getNorm() const { return toNorm(get()); }
    void setNorm(float n) { set(fromNorm(n)); }
    float toNorm(float v) const;
    float fromNorm(float n) const;
    int getInt() const { return (int)std::lround(get()); }
    bool getBool() const { return get() >= 0.5f; }
    std::string display() const { return displayFor(get()); }
    std::string displayFor(float v) const;
private:
    std::atomic<float> value_;
};

struct NoteEvent {
    // Kill and KillNote fade a voice out in a few milliseconds (cut groups, mono, polyphony limit).
    enum Type : unsigned char { Kill, KillNote, AllOff, On, Off, SlideTo };
    int offset = 0;             // sample within the quantum
    Type type = On;
    int key = 60;               // MIDI key number (60 shows as C5)
    float velocity = 0.8f;      // 0..1
    float pan = 0.0f;           // -1..1 per-note pan
    float modX = 0.5f;          // 0..1, mapped to filter cutoff by native instruments
    float modY = 0.5f;          // 0..1, mapped to resonance
    float fineCents = 0.0f;     // per-note fine pitch
    float release = 0.5f;       // 0..1 per-note release
    uint32 id = 0;              // matches an Off / SlideTo to its On
    int group = 0;              // note colour group; a SlideTo bends the sounding notes of its group
    float slideSamples = 0.0f;  // SlideTo: time to reach the new key. On: portamento time when glideFromKey >= 0
    float glideFromKey = -1.0f; // On: start at this key and glide to key (portamento)
};

struct ProcessContext {
    int numSamples = kQuantum;
    double sampleRate = 48000.0;
    double tempo = 140.0;
    double tickPos = 0.0;       // song or pattern position at the first sample
    bool playing = false;
    float* io[2] = {nullptr, nullptr};          // instrument: cleared, write into it. effect: process in place
    const float* sidechain[2] = {nullptr, nullptr};   // null when no sidechain source is routed
    const NoteEvent* events = nullptr;
    int numEvents = 0;
    float pitchBendSemis = 0.0f;                // channel pitch (knob and automation), smoothed by the host
};

struct Preset {
    std::string name;
    Json state;
};

// How a plugin wants its parameters grouped on its panel.
struct ParamGroup {
    std::string title;
    std::vector<std::string> ids;
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual const char* typeId() const = 0;        // stable id, e.g. "sampler"
    virtual const char* displayName() const = 0;   // original product name shown to the user
    virtual bool isInstrument() const { return false; }
    virtual bool wantsSidechain() const { return false; }

    // Called off the audio thread before the plugin is published, and again if the rate changes.
    virtual void prepare(double sampleRate) = 0;
    // Always called with exactly kQuantum samples, on the audio thread or a render thread.
    virtual void process(ProcessContext& ctx) = 0;
    virtual void reset() {}                        // audio thread: drop voices and clear delay lines
    virtual int latencySamples() const { return 0; }
    virtual double tailSeconds() const { return 0.0; }
    virtual int activeVoices() const { return 0; }

    // State beyond the parameter values (loaded samples, drawn curves).
    virtual Json saveExtra() const { return Json(); }
    virtual void loadExtra(const Json&, const std::string& projectDir) {}
    virtual std::vector<Preset> factoryPresets() const { return {}; }
    virtual std::vector<ParamGroup> paramGroups() const { return {}; }    // empty = one group with everything

    // Read-back for a plugin's display, safe to call from the interface thread while audio runs.
    virtual int readScope(float* dst, int n) const { return 0; }          // most recent output samples (mono), for spectrum views
    virtual float meter(int index) const { return 0.0f; }                 // plugin-defined, e.g. 0 = gain reduction in dB

    // Controllers: a plugin can put out control values (0..1, as of the last quantum it processed) that
    // parameters are linked to. Safe to read from any thread.
    virtual int numControlOutputs() const { return 0; }
    virtual float controlOutput(int index) const { return 0.0f; }
    virtual const char* controlOutputName(int index) const { return ""; }

    // Told before an export renders through this plugin (true) so that a plugin which behaves differently when
    // it need not keep up with real time can do so. Built-in plugins sound the same either way and ignore it.
    virtual void setOffline(bool) {}

    virtual Json saveState() const;
    virtual void loadState(const Json& j, const std::string& projectDir = {});

    Param* param(const std::string& id) const;
    const std::vector<std::unique_ptr<Param>>& params() const { return params_; }

protected:
    Param* add(ParamInfo info);
    Param* addFloat(const char* id, const char* name, float min, float max, float def, const char* unit = "", float skew = 1.0f);
    Param* addChoice(const char* id, const char* name, std::vector<std::string> choices, int def);
    Param* addBool(const char* id, const char* name, bool def);
    std::vector<std::unique_ptr<Param>> params_;
};

// Base for native instruments: splits each quantum at event offsets so note timing is exact.
class Instrument : public Plugin {
public:
    bool isInstrument() const override { return true; }
    void process(ProcessContext& ctx) final;
protected:
    virtual void beginBlock(ProcessContext& ctx) {}
    virtual void handleEvent(const NoteEvent& e, ProcessContext& ctx) = 0;
    // Add n samples of output into L and R (they start cleared).
    virtual void render(float* L, float* R, int n, ProcessContext& ctx) = 0;
};

// Stands in for a plugin that cannot be created on this machine (a hosted plugin that is not
// installed, or any type this build does not know). It keeps the saved state untouched, so opening
// and saving the project loses nothing; as an effect it lets the sound through, as an instrument it is silent.
class MissingPlugin : public Plugin {
public:
    MissingPlugin(std::string typeId, bool instrument) : type_(std::move(typeId)), instrument_(instrument) { name_ = "Missing: " + type_; }
    const char* typeId() const override { return type_.c_str(); }
    const char* displayName() const override { return name_.c_str(); }
    bool isInstrument() const override { return instrument_; }
    void prepare(double) override {}
    void process(ProcessContext&) override {}
    Json saveState() const override { return state_; }
    void loadState(const Json& j, const std::string& = {}) override {
        state_ = j;
        const std::string name = j["extra"]["name"].str("");     // hosted plugins save what they are called
        if (!name.empty()) name_ = "Missing: " + name;
    }
private:
    std::string type_, name_;
    bool instrument_;
    Json state_;
};

// Registry of built-in plugin types.
struct PluginType {
    std::string id, name, category;
    bool instrument = false;
    std::function<std::shared_ptr<Plugin>()> create;
};
class PluginRegistry {
public:
    static PluginRegistry& global();
    void add(PluginType t);
    const std::vector<PluginType>& types() const { return types_; }
    std::shared_ptr<Plugin> create(const std::string& id) const;
    const PluginType* find(const std::string& id) const;
    // Plugins that live outside the engine (hosted VST3): the program supplies a function that makes one from
    // its id. It is asked for every id the registry itself does not know. May return null.
    void setExternalFactory(std::function<std::shared_ptr<Plugin>(const std::string& id)> f) { external_ = std::move(f); }
    // Like create, but never null: an unknown type becomes a MissingPlugin that preserves its state.
    std::shared_ptr<Plugin> createOrMissing(const std::string& id, bool instrument) const;
private:
    PluginRegistry();
    std::vector<PluginType> types_;
    std::function<std::shared_ptr<Plugin>(const std::string&)> external_;
};

} // namespace pulp
