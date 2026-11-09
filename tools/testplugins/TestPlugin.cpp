// Two tiny VST3 plugins that exist only so PULP's plugin hosting can be checked on any PC:
//   PULP Test Tone   an instrument: a sine per held note (level follows velocity and the Level parameter)
//   PULP Test Delay  an effect: delays the sound by exactly 256 samples and says so (latency), with a gain
// They are not shipped in dist\PULP. Built by "build.bat testplugins".
#include <juce_audio_processors/juce_audio_processors.h>

#ifndef PULP_TEST_SYNTH
 #define PULP_TEST_SYNTH 0
#endif

class TestPlugin : public juce::AudioProcessor {
public:
    static constexpr int kDelay = 256;

    TestPlugin()
       #if PULP_TEST_SYNTH
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
        addParameter(level_ = new juce::AudioParameterFloat(juce::ParameterID{"level", 1}, "Level", 0.0f, 1.0f, 0.5f));
        addParameter(octave_ = new juce::AudioParameterInt(juce::ParameterID{"octave", 1}, "Octave", -2, 2, 0));
       #else
        : juce::AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                                .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
        addParameter(level_ = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 2.0f, 1.0f));
        addParameter(flip_ = new juce::AudioParameterBool(juce::ParameterID{"flip", 1}, "Flip polarity", false));
        setLatencySamples(kDelay);
       #endif
    }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return PULP_TEST_SYNTH != 0; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && (PULP_TEST_SYNTH != 0 || layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay(double sampleRate, int) override {
        rate_ = sampleRate;
        for (auto& v : voices_) v = {};
        for (auto& line : delay_) std::fill(std::begin(line), std::end(line), 0.0f);
        pos_ = 0;
        bend_ = 0.0;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        juce::ScopedNoDenormals noDenormals;
        const int n = buffer.getNumSamples();
       #if PULP_TEST_SYNTH
        buffer.clear();
        auto it = midi.begin();
        for (int i = 0; i < n; ++i) {
            for (; it != midi.end() && (*it).samplePosition <= i; ++it) {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) {
                    Voice* free = nullptr;
                    for (auto& v : voices_) if (!v.on && free == nullptr) free = &v;
                    if (free) *free = {true, m.getNoteNumber(), m.getFloatVelocity(), 0.0};
                } else if (m.isNoteOff()) {
                    for (auto& v : voices_) if (v.on && v.key == m.getNoteNumber()) v.on = false;
                } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
                    for (auto& v : voices_) v.on = false;
                } else if (m.isPitchWheel()) bend_ = (m.getPitchWheelValue() - 8192) / 8192.0 * 2.0;
            }
            float out = 0.0f;
            for (auto& v : voices_) {
                if (!v.on) continue;
                const double hz = 440.0 * std::pow(2.0, (v.key - 69 + 12 * octave_->get() + bend_) / 12.0);
                out += (float)std::sin(v.phase) * v.velocity * level_->get();
                v.phase += juce::MathConstants<double>::twoPi * hz / rate_;
                if (v.phase > juce::MathConstants<double>::twoPi) v.phase -= juce::MathConstants<double>::twoPi;
            }
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample(ch, i, out);
        }
       #else
        juce::ignoreUnused(midi);
        const float gain = level_->get() * (flip_->get() ? -1.0f : 1.0f);
        for (int i = 0; i < n; ++i) {
            for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch) {
                const float in = buffer.getSample(ch, i);
                buffer.setSample(ch, i, delay_[ch][pos_] * gain);
                delay_[ch][pos_] = in;
            }
            pos_ = (pos_ + 1) % kDelay;
        }
       #endif
    }

    // state: the parameters, and a note that proves the state made the round trip
    void getStateInformation(juce::MemoryBlock& dest) override {
        juce::XmlElement xml("PULPTEST");
        for (auto* p : getParameters()) xml.setAttribute("p" + juce::String(p->getParameterIndex()), (double)p->getValue());
        xml.setAttribute("memo", memo_);
        copyXmlToBinary(xml, dest);
    }
    void setStateInformation(const void* data, int size) override {
        if (auto xml = getXmlFromBinary(data, size)) {
            for (auto* p : getParameters())
                p->setValueNotifyingHost((float)xml->getDoubleAttribute("p" + juce::String(p->getParameterIndex()), p->getDefaultValue()));
            memo_ = xml->getStringAttribute("memo");
        }
    }

private:
    struct Voice { bool on = false; int key = 60; float velocity = 0.0f; double phase = 0.0; };
    Voice voices_[16];
    float delay_[2][kDelay] = {};
    int pos_ = 0;
    double rate_ = 48000.0, bend_ = 0.0;
    juce::AudioParameterFloat* level_ = nullptr;
    juce::AudioParameterInt* octave_ = nullptr;
    juce::AudioParameterBool* flip_ = nullptr;
    juce::String memo_ = "pulp";
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestPlugin(); }
