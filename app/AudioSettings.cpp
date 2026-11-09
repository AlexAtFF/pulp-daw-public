#include "AudioSettings.h"

AudioSettings::AudioSettings(Session& s) : s_(s) {
    for (auto* c : {&type_, &output_, &input_, &rate_, &buffer_}) {
        addAndMakeVisible(c);
        c->onChange = [this] { if (!refreshing_) apply(); };
    }
    type_.setTooltip("Audio driver. ASIO gives the lowest latency; Windows Audio shares the device with other programs.");
    output_.setTooltip("Device the sound comes out of");
    input_.setTooltip("Device used for recording");
    rate_.setTooltip("Sample rate");
    buffer_.setTooltip("Buffer size: smaller means less delay but more CPU strain");
    panel_.setTooltip("Open the driver's own settings panel");
    panel_.onClick = [this] { if (auto* d = s_.devices.getCurrentAudioDevice()) if (d->hasControlPanel()) { d->showControlPanel(); refresh(); } };
    addAndMakeVisible(panel_);
    measure_.setTooltip("Latency calibration: connect an output to an input with a cable (or hold the microphone to a headphone), then click. "
                        "PULP plays three quiet clicks, hears them come back and stores the real delay for this device and buffer size, so takes land exactly in time.");
    measure_.onClick = [this] {
        error_.clear();
        info_ = "Measuring: three quiet clicks...";
        repaint();
        if (!s_.calibrate([safe = juce::Component::SafePointer<AudioSettings>(this)](int frames, float quality) {
                if (!safe) return;
                safe->refresh();
                if (frames < 0) safe->error_ = "the clicks did not come back on either input (match " + juce::String(quality, 2) + "). Connect an output to an input and try again; nothing was changed";
                safe->repaint();
            }))
            { error_ = "stop playback before measuring"; repaint(); }
    };
    forget_.setTooltip("Forget the measured latency for this device and go back to what the driver reports");
    forget_.onClick = [this] { s_.storeCalibration(-1); refresh(); };
    addAndMakeVisible(measure_);
    addAndMakeVisible(forget_);
    s_.devices.addChangeListener(this);
    refresh();
}

AudioSettings::~AudioSettings() { s_.devices.removeChangeListener(this); }

void AudioSettings::refresh() {
    refreshing_ = true;
    auto& dm = s_.devices;
    type_.clear(juce::dontSendNotification);
    output_.clear(juce::dontSendNotification);
    input_.clear(juce::dontSendNotification);
    rate_.clear(juce::dontSendNotification);
    buffer_.clear(juce::dontSendNotification);
    outNames_.clear();
    inNames_.clear();
    info_.clear();
    const bool live = !s_.usingNullDevice();
    for (auto* c : {&type_, &output_, &input_, &rate_, &buffer_}) c->setEnabled(live);
    panel_.setEnabled(false);
    if (!live) { info_ = "Running in silent test mode: no audio device is open."; forget_.setEnabled(s_.calibratedLatency() >= 0); refreshing_ = false; resized(); repaint(); return; }

    int id = 1;
    juce::AudioIODeviceType* cur = nullptr;
    for (auto* t : dm.getAvailableDeviceTypes()) {
        type_.addItem(t->getTypeName(), id);
        if (t->getTypeName() == dm.getCurrentAudioDeviceType()) { type_.setSelectedId(id, juce::dontSendNotification); cur = t; }
        ++id;
    }
    const auto setup = dm.getAudioDeviceSetup();
    if (cur) {
        const auto outs = cur->getDeviceNames(false), ins = cur->getDeviceNames(true);
        for (int i = 0; i < outs.size(); ++i) output_.addItem(outs[i], i + 1);
        output_.setText(setup.outputDeviceName, juce::dontSendNotification);
        input_.addItem("(none)", 1000);
        for (int i = 0; i < ins.size(); ++i) input_.addItem(ins[i], i + 1);
        input_.setText(setup.inputDeviceName.isEmpty() ? "(none)" : setup.inputDeviceName, juce::dontSendNotification);
        input_.setEnabled(cur->hasSeparateInputsAndOutputs());
    }
    if (auto* d = dm.getCurrentAudioDevice()) {
        int i = 1;
        for (double r : d->getAvailableSampleRates()) {
            rate_.addItem(juce::String(r, 0) + " Hz", i);
            if (std::abs(r - d->getCurrentSampleRate()) < 1.0) rate_.setSelectedId(i, juce::dontSendNotification);
            ++i;
        }
        i = 1;
        for (int b : d->getAvailableBufferSizes()) {
            buffer_.addItem(juce::String(b) + " samples  (" + juce::String(1000.0 * b / d->getCurrentSampleRate(), 1) + " ms)", i);
            if (b == d->getCurrentBufferSizeSamples()) buffer_.setSelectedId(i, juce::dontSendNotification);
            ++i;
        }
        outNames_ = d->getOutputChannelNames();
        inNames_ = d->getInputChannelNames();
        outActive_ = d->getActiveOutputChannels();
        inActive_ = d->getActiveInputChannels();
        panel_.setEnabled(d->hasControlPanel());
        const double sr = d->getCurrentSampleRate();
        info_ = "Output latency " + juce::String(1000.0 * d->getOutputLatencyInSamples() / sr, 1) + " ms,  input latency "
              + juce::String(1000.0 * d->getInputLatencyInSamples() / sr, 1) + " ms,  " + juce::String(d->getCurrentBitDepth()) + " bit.   Underruns so far: "
              + juce::String(s_.xruns());
    } else info_ = "No audio device is open.";
    measure_.setEnabled(true);
    forget_.setEnabled(s_.calibratedLatency() >= 0);
    refreshing_ = false;
    resized();
    repaint();
}

void AudioSettings::apply() {
    auto& dm = s_.devices;
    error_.clear();
    const auto wantedType = type_.getText();
    if (wantedType.isNotEmpty() && wantedType != dm.getCurrentAudioDeviceType()) {
        const auto err = s_.openDevice(wantedType, {});
        if (err.isNotEmpty()) error_ = err;
        refresh();
        return;
    }
    auto setup = dm.getAudioDeviceSetup();
    juce::AudioIODeviceType* cur = nullptr;
    for (auto* t : dm.getAvailableDeviceTypes()) if (t->getTypeName() == dm.getCurrentAudioDeviceType()) cur = t;
    const bool deviceChanged = output_.getText() != setup.outputDeviceName;
    setup.outputDeviceName = output_.getText();
    if (cur && !cur->hasSeparateInputsAndOutputs()) setup.inputDeviceName = setup.outputDeviceName;
    else setup.inputDeviceName = input_.getSelectedId() == 1000 ? juce::String() : input_.getText();
    if (auto* d = dm.getCurrentAudioDevice(); d && !deviceChanged) {
        const auto rates = d->getAvailableSampleRates();
        const auto bufs = d->getAvailableBufferSizes();
        if (rate_.getSelectedId() > 0 && rate_.getSelectedId() <= rates.size()) setup.sampleRate = rates[rate_.getSelectedId() - 1];
        if (buffer_.getSelectedId() > 0 && buffer_.getSelectedId() <= bufs.size()) setup.bufferSize = bufs[buffer_.getSelectedId() - 1];
        setup.useDefaultOutputChannels = false;
        setup.useDefaultInputChannels = false;
        setup.outputChannels = outActive_;
        setup.inputChannels = inActive_;
    } else {
        setup.useDefaultOutputChannels = true;
        setup.useDefaultInputChannels = true;
    }
    const auto err = dm.setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) error_ = err;
    refresh();
}

void AudioSettings::resized() {
    auto r = getLocalBounds().reduced(10);
    auto row = [&](juce::ComboBox& c) {
        auto line = r.removeFromTop(26);
        c.setBounds(line.withTrimmedLeft(110));
        r.removeFromTop(6);
    };
    row(type_); row(output_); row(input_); row(rate_); row(buffer_);
    panel_.setBounds(r.removeFromTop(24).withTrimmedLeft(110).withWidth(170));
    r.removeFromTop(10);
    infoArea_ = r.removeFromBottom(40);
    auto lat = r.removeFromBottom(26);
    forget_.setBounds(lat.removeFromRight(64).reduced(0, 1));
    lat.removeFromRight(4);
    measure_.setBounds(lat.removeFromRight(120).reduced(0, 1));
    latencyArea_ = lat;
    r.removeFromBottom(6);
    auto lists = r;
    outArea_ = lists.removeFromLeft(lists.getWidth() / 2 - 4);
    lists.removeFromLeft(8);
    inArea_ = lists;
}

void AudioSettings::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    g.setColour(theme::textDim);
    g.setFont(theme::font(12.5f));
    const char* labels[5] = {"Driver", "Output device", "Input device", "Sample rate", "Buffer size"};
    juce::ComboBox* boxes[5] = {&type_, &output_, &input_, &rate_, &buffer_};
    for (int i = 0; i < 5; ++i) g.drawText(labels[i], 10, boxes[i]->getY(), 96, boxes[i]->getHeight(), juce::Justification::centredLeft, false);

    auto list = [&](juce::Rectangle<int> area, const juce::String& title, const juce::StringArray& names, const juce::BigInteger& active) {
        g.setColour(theme::accent2);
        g.setFont(theme::font(11.0f, true));
        g.drawText(title, area.removeFromTop(18), juce::Justification::centredLeft, false);
        theme::insetBox(g, area.toFloat());
        g.setFont(theme::font(12.0f));
        auto rows = area.reduced(4);
        for (int i = 0; i < names.size() && rows.getHeight() >= 18; ++i) {
            auto line = rows.removeFromTop(18);
            auto box = line.removeFromLeft(18).reduced(3);
            g.setColour(theme::panelHi);
            g.fillRoundedRectangle(box.toFloat(), 2.0f);
            if (active[i]) { g.setColour(theme::good); g.fillRoundedRectangle(box.toFloat().reduced(2.0f), 1.5f); }
            g.setColour(theme::text);
            g.drawText(juce::String(i + 1) + ": " + names[i], line.withTrimmedLeft(4), juce::Justification::centredLeft, true);
        }
    };
    list(outArea_, "ACTIVE OUTPUTS", outNames_, outActive_);
    list(inArea_, "ACTIVE INPUTS", inNames_, inActive_);

    g.setColour(theme::text);
    g.setFont(theme::font(12.0f));
    g.drawFittedText(latencyText(), latencyArea_, juce::Justification::centredLeft, 2);
    g.setColour(error_.isNotEmpty() ? theme::bad : theme::textDim);
    g.setFont(theme::font(12.0f));
    g.drawFittedText(error_.isNotEmpty() ? "Could not apply: " + error_ : info_, infoArea_, juce::Justification::topLeft, 2);
}

juce::String AudioSettings::latencyText() const {
    const double sr = s_.engine.sampleRate();
    const int used = s_.recordingLatencyFrames();
    const int measured = s_.calibratedLatency();
    return "Recording offset: " + juce::String(used) + " samples (" + juce::String(1000.0 * used / sr, 1) + " ms), "
         + (measured >= 0 ? juce::String("measured on this device") : juce::String("as the driver reports it"));
}

void AudioSettings::mouseDown(const juce::MouseEvent& e) {
    if (s_.usingNullDevice()) return;
    auto hit = [&](juce::Rectangle<int> area, const juce::StringArray& names, juce::BigInteger& active) {
        auto rows = area.withTrimmedTop(18).reduced(4);
        if (!rows.contains(e.getPosition())) return false;
        const int i = (e.y - rows.getY()) / 18;
        if (i < 0 || i >= names.size()) return false;
        active.setBit(i, !active[i]);
        return true;
    };
    if (hit(outArea_, outNames_, outActive_) || hit(inArea_, inNames_, inActive_)) apply();
}
