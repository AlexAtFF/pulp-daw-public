#include "Script.h"
#include "PluginHost.h"
#include "pulp/Pads.h"
#include "Shortcuts.h"
#include "CurveView.h"
#include "CreativeViews.h"

ScriptRunner::ScriptRunner(MainComponent& main, juce::File logFile) : main_(main), log_(std::move(logFile)) {
    if (log_ != juce::File()) { log_.getParentDirectory().createDirectory(); log_.replaceWithText(""); }
}

void ScriptRunner::log(const juce::String& line) {
    if (log_ != juce::File()) log_.appendText(line + "\n");
}

bool ScriptRunner::load(const juce::File& script) {
    if (!script.existsAsFile()) { log("error: script not found: " + script.getFullPathName()); return false; }
    baseDir_ = script.getParentDirectory();
    lines_.addLines(script.loadFileAsString());
    return true;
}

bool ScriptRunner::saveSnapshot(juce::Component& c, const juce::File& file, float scale) {
    auto img = c.createComponentSnapshot(c.getLocalBounds(), true, scale);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, out);
}

void ScriptRunner::timerCallback() {
    stopTimer();
    while (next_ < lines_.size()) {
        const auto line = lines_[next_++].trim();
        if (line.isEmpty() || line.startsWithChar('#')) continue;
        if (line.startsWith("wait ")) { startTimer(juce::jmax(1, line.fromFirstOccurrenceOf(" ", false, false).getIntValue())); return; }
        if (!run(line)) return;
    }
    log("script finished, " + juce::String(errors_) + " errors");
}

// Sends a synthetic press / drag / release to whatever component is at a point, without
// moving the real mouse. Coordinates are relative to root.
static void sendMouse(juce::Component& root, juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods, bool drag, int clicks) {
    auto* target = root.getComponentAt(from.toInt());
    if (target == nullptr) target = &root;
    juce::Component::SafePointer<juce::Component> safe(target);
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    const auto p0 = target->getLocalPoint(&root, from);
    auto make = [&](juce::Point<float> p, bool dragged) {
        return juce::MouseEvent(src, p, mods, juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                                target, target, now, p0, now, clicks, dragged);
    };
    target->mouseDown(make(p0, false));
    if (drag && safe) {
        for (int i = 1; i <= 10 && safe; ++i) {
            const auto p = from + (to - from) * ((float)i / 10.0f);
            target->mouseDrag(make(target->getLocalPoint(&root, p), true));
        }
    }
    if (safe) target->mouseUp(make(target->getLocalPoint(&root, drag ? to : from), drag));
    if (safe && clicks == 2) target->mouseDoubleClick(make(p0, false));
}

bool ScriptRunner::run(const juce::String& line) {
    auto args = juce::StringArray::fromTokens(line, " ", "\"");
    for (auto& a : args) a = a.unquoted();
    const auto cmd = args[0];
    auto& s = main_.session();
    auto fail = [&](const juce::String& why) { ++errors_; log("error: " + why + "  [" + line + "]"); };
    auto file = [&](const juce::String& p) { return juce::File::isAbsolutePath(p) ? juce::File(p) : baseDir_.getChildFile(p); };
    auto modsFrom = [&](int firstArg) {
        int flags = juce::ModifierKeys::leftButtonModifier;
        for (int i = firstArg; i < args.size(); ++i) {
            if (args[i] == "right") flags = (flags & ~juce::ModifierKeys::leftButtonModifier) | juce::ModifierKeys::rightButtonModifier;
            else if (args[i] == "ctrl") flags |= juce::ModifierKeys::ctrlModifier;
            else if (args[i] == "shift") flags |= juce::ModifierKeys::shiftModifier;
            else if (args[i] == "alt") flags |= juce::ModifierKeys::altModifier;
        }
        return juce::ModifierKeys(flags);
    };

    if (cmd == "quit") {
        log("quit, " + juce::String(errors_) + " errors");
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return false;
    }
    // The commands are one chain of "else if" cut in two, because the compiler limits how deep such a chain may go.
    bool firstHalf = true;
    if (cmd == "play") s.engine.play();
    else if (cmd == "stop") s.engine.stop();
    else if (cmd == "key") {
        auto desc = line.fromFirstOccurrenceOf(" ", false, false).trim();
        if (desc.endsWithIgnoreCase("space")) desc += "bar";
        const auto k = juce::KeyPress::createFromDescription(desc);
        if (!k.isValid()) fail("bad key");
        else {
            // deliver to the focused component first, like a real key press, then up its parents
            bool used = false;
            for (auto* c = juce::Component::getCurrentlyFocusedComponent(); c != nullptr && !used; c = c->getParentComponent()) used = c->keyPressed(k);
            if (!used) used = main_.handleKey(k);
            if (!used) log("note: key not handled: " + k.getTextDescription());
        }
    }
    else if (cmd == "keyto" && args.size() >= 3) {          // keyto <target> <key>: as if that window had the focus
        auto desc = line.fromFirstOccurrenceOf(args[1], false, false).trim();
        if (desc.endsWithIgnoreCase("space")) desc += "bar";
        const auto k = juce::KeyPress::createFromDescription(desc);
        auto* c = main_.findTarget(args[1]);
        if (!k.isValid() || !c) fail("bad key or target");
        else {
            bool used = false;
            for (; c != nullptr && !used; c = c->getParentComponent()) used = c->keyPressed(k);
            if (!used) log("note: key not handled: " + k.getTextDescription());
        }
    }
    else if (cmd == "set" && args.size() >= 3) {
        const double v = args[2].getDoubleValue();
        if (args[1] == "tempo") s.setTempo(v);
        else if (args[1] == "master") s.project.global->masterVolume.set((float)v);
        else if (args[1] == "song") s.setSongMode(v != 0.0);
        else if (args[1] == "pattern") s.selectPattern((int)v);
        else if (args[1] == "channel") s.selectChannel((int)v);
        else fail("unknown setting");
    }
    else if (cmd == "param" && args.size() >= 4) {          // param <channel> <id> <value>: strip or instrument parameter
        if (auto* ch = s.channel(args[1].getIntValue())) {
            auto* p = ch->live->stripParam(args[2].toStdString());
            if (!p) p = ch->live->plugin->param(args[2].toStdString());
            if (p) { p->set(args[3].getFloatValue()); s.commit("Script"); }
            else fail("no such parameter");
        } else fail("no such channel");
    }
    else if ((cmd == "click" || cmd == "dblclick") && args.size() >= 4) {
        if (auto* c = main_.findTarget(args[1])) {
            const juce::Point<float> p(args[2].getFloatValue(), args[3].getFloatValue());
            sendMouse(*c, p, p, modsFrom(4), false, cmd == "dblclick" ? 2 : 1);
        } else fail("unknown target " + args[1]);
    }
    else if (cmd == "drag" && args.size() >= 6) {
        if (auto* c = main_.findTarget(args[1]))
            sendMouse(*c, {args[2].getFloatValue(), args[3].getFloatValue()}, {args[4].getFloatValue(), args[5].getFloatValue()}, modsFrom(6), true, 1);
        else fail("unknown target " + args[1]);
    }
    else if (cmd == "drop" && args.size() >= 2) {           // drop <file> [x y]: as if dragged onto the channel rack
        main_.rack().dropFiles({file(args[1]).getFullPathName()}, {args[2].getIntValue(), args[3].getIntValue()});
    }
    else if (cmd == "show" || cmd == "hide") {
        if (auto* w = main_.window(args[1])) { if (cmd == "show") w->showAndRaise(); else w->setVisible(false); }
        else fail("unknown window " + args[1]);
    }
    else if (cmd == "move" && args.size() >= 6) {
        if (auto* w = main_.window(args[1])) w->setBounds(args[2].getIntValue(), args[3].getIntValue(), args[4].getIntValue(), args[5].getIntValue());
        else fail("unknown window " + args[1]);
    }
    else if (cmd == "detach" || cmd == "dock") {
        if (auto* w = main_.window(args[1])) w->setDetached(cmd == "detach");
        else fail("unknown window " + args[1]);
    }
    else if (cmd == "dismiss") juce::PopupMenu::dismissAllActiveMenus();
    else if (cmd == "dialog" && args.size() >= 2) {         // dialog info | dialog set <field> <value> | dialog ok | dialog cancel: the window that is asking for something
        auto* w = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        if (!w) fail("no dialog is open");
        else if (args[1] == "set" && args.size() >= 4) { if (auto* ed = w->getTextEditor(args[2])) ed->setText(args[3]); else fail("no such field"); }
        else if (args[1] == "ok") w->exitModalState(1);
        else if (args[1] == "cancel") w->exitModalState(0);
        else log("dialog: " + w->getName() + " with " + juce::String(w->getNumChildComponents()) + " parts");
    }
    else if (cmd == "note" && args.size() >= 3) {           // note on <key> [velocity] | note off <key>
        if (args[1] == "on") s.liveNoteOn(args[2].getIntValue(), args.size() > 3 ? args[3].getFloatValue() : pulp::kDefaultVelocity);
        else s.liveNoteOff(args[2].getIntValue());
    }
    else if (cmd == "piano") log(main_.pianoRoll().describe());
    else if (cmd == "playlist") log(main_.playlist().describe());
    else if (cmd == "mixer") log(main_.mixer().describe());
    // ---- recording ----
    else if (cmd == "testinput") s.testInput = args[1] != "off";
    else if (cmd == "countin" && args.size() >= 2) { s.countInBars = juce::jlimit(0, 2, args[1].getIntValue()); s.notify(); }
    else if (cmd == "mixinput" && args.size() >= 3) {       // mixinput <track> <0|1|2|3> [monitor]
        auto& t = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumInserts, args[1].getIntValue())];
        t.input = juce::jlimit(0, 3, args[2].getIntValue());
        t.monitor = args[3] == "monitor";
        s.commit("Track input");
    }
    else if (cmd == "audio") {
        juce::String out = "audio recording=" + juce::String((int)s.audioRecording()) + " latency=" + juce::String(s.recordingLatencyFrames())
                         + " inputPeak=" + juce::String(s.engine.inputPeak(0), 3) + " inputDb=" + juce::String(pulp::gainToDb(s.engine.inputPeak(0)), 1) + "/" + juce::String(pulp::gainToDb(s.engine.inputPeak(1)), 1)
                         + " waiting=" + juce::String((int)s.engine.waitingForInput()) + " misses=" + juce::String(s.engine.streamMisses())
                         + " items=" + juce::String((int)s.project.audios.size()) + " :";
        for (auto& c : s.project.clips)
            if (c.type == pulp::Clip::AudioClip) {
                const auto* a = s.project.audioById(c.audioId);
                out << " [t" << c.track << " start=" << c.start << " len=" << c.length << " skip=" << c.sourceOffsetFrames << " mix=" << c.mixerTrack
                    << " frames=" << (a && a->valid() ? juce::String(a->frames()) : juce::String("?")) << " ch=" << (a ? a->channels() : 0)
                    << (a && a->stream ? " disk" : "") << (c.take > 0 ? " take=" + juce::String(c.take) + "/g" + juce::String(c.takeGroup) : juce::String()) << (c.muted ? " muted" : "")
                    << (c.sync ? " sync" : "") << (a && a->bpm > 0.0 ? " bpm=" + juce::String(a->bpm, 2) : juce::String())
                    << (s.project.clipNeedsProcessing(c) ? " ratio=" + juce::String(s.project.clipLengthRatio(c), 4) + " mode=" + juce::String(c.stretchMode) : juce::String()) << "]";
            }
        log(out);
        s.engine.resetInputPeak();
    }
    else if (cmd == "addaudio" && args.size() >= 2) {       // addaudio <file> [track] [tick]: as if dropped on the playlist
        if (s.addAudioFile(file(args[1]), args[2].getIntValue(), args[3].getIntValue()) < 0) fail("cannot load audio");
    }
    else if (cmd == "recopt" && args.size() >= 3) {         // recopt punch|loop|wait|replace on|off
        const bool on = args[2] != "off";
        if (args[1] == "punch") s.recOpt.punch = on;
        else if (args[1] == "loop") s.recOpt.loopTakes = on;
        else if (args[1] == "wait") s.recOpt.waitForInput = on;
        else if (args[1] == "replace") s.recOpt.replaceNotes = on;
        else fail("unknown record option");
        s.notify();
    }
    else if (cmd == "metrosound" && args.size() >= 2) s.engine.setMetronomeSound(args[1].getIntValue());
    else if (cmd == "loopregion" && args.size() >= 3) { s.project.loopStart = args[1].getLargeIntValue(); s.project.loopEnd = args[2].getLargeIntValue(); s.commit("Loop region"); }
    else if (cmd == "seek" && args.size() >= 2) s.seek(args[1].getDoubleValue());
    else if (cmd == "testloop" && args.size() >= 2) s.testLoopFrames = args[1].getIntValue();
    else if (cmd == "calibrate") {                          // measure the round trip; the script waits for the answer
        scriptBusy_ = true;
        if (!s.calibrate([this, &s](int frames, float q) {
                scriptBusy_ = false;
                log("calibration frames=" + juce::String(frames) + " quality=" + juce::String(q, 2) + " offset=" + juce::String(s.recordingLatencyFrames()));
            })) { scriptBusy_ = false; fail("cannot calibrate now"); }
    }
    else if (cmd == "waitbusy") { if (scriptBusy_) { --next_; startTimer(50); return false; } }
    else if (cmd == "clipstretch" && args.size() >= 6) {    // clipstretch <clip> <mode> <sync 0|1> <length multiplier> <cents>
        const int i = args[1].getIntValue();
        if (i < 0 || i >= (int)s.project.clips.size()) fail("no such clip");
        else { s.project.setClipStretch(i, args[2].getIntValue(), args[3].getIntValue() != 0, args[4].getFloatValue(), args[5].getFloatValue()); s.commit("Stretch"); }
    }
    else if (cmd == "audiotempo" && args.size() >= 3) {     // audiotempo <audio id> <bpm|detect>
        const int id = args[1].getIntValue();
        if (args[2] == "detect") log("tempo detected: " + juce::String(s.project.detectAudioTempo(id), 3));
        else for (auto& a : s.project.audios) if (a.id == id) a.bpm = args[2].getDoubleValue();
        s.commit("Audio tempo");
    }
    // ---- automation, controllers, MIDI ----
    else if (cmd == "autoclip" && args.size() >= 2) {       // autoclip <address> [track] [start] [length]
        if (s.project.findParam(args[1].toStdString()) == nullptr) fail("no such control");
        else {
            auto& c = s.project.addAutomationClip(args[1].toStdString(), args[2].getIntValue(), args[3].getLargeIntValue(), args.size() > 4 ? args[4].getLargeIntValue() : 3840);
            log("automation clip curve=" + juce::String(c.autoId));
            s.commit("Create automation clip");
        }
    }
    else if (cmd == "autopoints" && args.size() >= 3) {     // autopoints <curve id> tick,value[,shape[,tension]] ...
        if (auto* a = s.project.autoById(args[1].getIntValue())) {
            a->points.clear();
            for (int i = 2; i < args.size(); ++i) {
                const auto f = juce::StringArray::fromTokens(args[i], ",", "");
                a->points.push_back({f[0].getLargeIntValue(), f[1].getFloatValue(), f[3].getFloatValue(), f[2].getIntValue()});
            }
            s.commit("Automation points");
        } else fail("no such curve");
    }
    else if (cmd == "autolfo" && args.size() >= 6) {        // autolfo <curve id> <on> <shape> <steps per cycle> <depth>
        if (auto* a = s.project.autoById(args[1].getIntValue())) {
            a->lfo.on = args[2].getIntValue() != 0; a->lfo.shape = args[3].getIntValue(); a->lfo.periodSteps = args[4].getFloatValue(); a->lfo.depth = args[5].getFloatValue();
            s.commit("Automation LFO");
        } else fail("no such curve");
    }
    else if (cmd == "events" && args.size() >= 2) {         // events <address> tick,value[,ramp] ...: this control's events in the current pattern
        auto& ev = s.project.pattern().events[args[1].toStdString()];
        ev.clear();
        for (int i = 2; i < args.size(); ++i) {
            const auto f = juce::StringArray::fromTokens(args[i], ",", "");
            ev.push_back({f[0].getIntValue(), f[1].getFloatValue(), f[2].getIntValue() != 0});
        }
        pulp::sortEvents(ev);
        if (ev.empty()) s.project.pattern().events.erase(args[1].toStdString());
        s.commit("Events");
    }
    else if (cmd == "link" && args.size() >= 5) {           // link <address> ctl <track> <slot> <output> [min max smooth formula] | link <address> midi <channel|-1> <cc> [min max]
        pulp::ParamLink l;
        l.target = args[1].toStdString();
        if (s.project.findParam(l.target) == nullptr) fail("no such control");
        else {
            const bool ctl = args[2] == "ctl";
            l.kind = ctl ? pulp::ParamLink::Controller : pulp::ParamLink::Midi;
            if (ctl) { l.track = args[3].getIntValue(); l.slot = args[4].getIntValue(); l.output = args[5].getIntValue(); }
            else { l.midiChannel = args[3].getIntValue(); l.cc = args[4].getIntValue(); }
            const int extra = ctl ? 6 : 5;
            if (args.size() > extra + 1) { l.min = args[extra].getFloatValue(); l.max = args[extra + 1].getFloatValue(); }
            if (args.size() > extra + 2) l.smoothMs = args[extra + 2].getFloatValue();
            if (args.size() > extra + 3) l.formula = args.joinIntoString(" ", extra + 3).toStdString();
            s.setLink(l);
        }
    }
    else if (cmd == "unlink" && args.size() >= 2) s.removeLink(args[1].toStdString());
    else if (cmd == "learn" && args.size() >= 2) { if (auto* p = s.project.findParam(args[1].toStdString())) s.beginMidiLearn(p); else fail("no such control"); }
    else if (cmd == "midiin" && args.size() >= 3) {         // as if a MIDI device had sent it: cc <channel> <controller> <value> | on <key> <velocity> | off <key> | bend <-1..1>
        if (args[1] == "cc") s.handleMidi(juce::MidiMessage::controllerEvent(juce::jlimit(1, 16, args[2].getIntValue()), args[3].getIntValue(), args[4].getIntValue()));
        else if (args[1] == "on") s.handleMidi(juce::MidiMessage::noteOn(1, args[2].getIntValue(), (juce::uint8)juce::jlimit(1, 127, args.size() > 3 ? args[3].getIntValue() : 100)));
        else if (args[1] == "off") s.handleMidi(juce::MidiMessage::noteOff(1, args[2].getIntValue()));
        else if (args[1] == "bend") s.handleMidi(juce::MidiMessage::pitchWheel(1, juce::jlimit(0, 16383, 8192 + (int)(args[2].getFloatValue() * 8191.0f))));
        else fail("unknown MIDI message");
    }
    else if (cmd == "paramval" && args.size() >= 2) {
        if (auto* p = s.project.findParam(args[1].toStdString())) log("param " + args[1] + " = " + juce::String(p->get(), 4) + " norm " + juce::String(p->getNorm(), 4) + "  (" + juce::String(s.project.paramLabel(args[1].toStdString())) + ": " + juce::String(p->display()) + ")");
        else fail("no such control");
    }
    else if ((cmd == "knobdown" || cmd == "knobset" || cmd == "knobup") && args.size() >= 2) {      // a control being moved by hand
        if (auto* p = s.project.findParam(args[1].toStdString())) {
            if (cmd == "knobdown") ParamUi::touch(p, 0);
            else if (cmd == "knobset") { p->setNorm(args[2].getFloatValue()); ParamUi::touch(p, 1); }
            else ParamUi::touch(p, 2);
        } else fail("no such control");
    }
    else if (cmd == "menuitem" && args.size() >= 3) {       // menuitem <address> <0 automation clip | 1 link | 2 events | 3 remove link>: the control's right-click menu
        if (auto* p = s.project.findParam(args[1].toStdString()); p && ParamUi::menuChosen) ParamUi::menuChosen(p, args[2].getIntValue());
        else fail("no such control");
    }
    else if (cmd == "showevents" && args.size() >= 2) main_.showEvents(args[1].toStdString());
    else if (cmd == "showlink" && args.size() >= 2) main_.showLink(args[1].toStdString());
    else if (cmd == "settingspage" && args.size() >= 2) main_.showSettingsPage(args[1].getIntValue());
    else if (cmd == "eventinfo") log(main_.eventEditor().describe());
    else if (cmd == "autos") {
        juce::String out = "automation curves=" + juce::String((int)s.project.autos.size()) + " links=" + juce::String((int)s.project.links.size()) + " learning=" + juce::String((int)s.midiLearning()) + " :";
        for (auto& a : s.project.autos) {
            out << " [" << a.id << " " << a.target << " '" << a.name << "'";
            for (auto& p : a.points) out << " " << p.tick << "=" << juce::String(p.value, 3) << (p.shape ? "s" + juce::String(p.shape) : juce::String()) << (p.tension != 0.0f ? "t" + juce::String(p.tension, 2) : juce::String());
            out << (a.lfo.on ? " lfo" : "") << "]";
        }
        for (auto& l : s.project.links)
            out << " {" << l.target << (l.kind == pulp::ParamLink::Midi ? " midi ch" + juce::String(l.midiChannel) + " cc" + juce::String(l.cc) : " ctl " + juce::String(l.track) + "/" + juce::String(l.slot) + "/" + juce::String(l.output))
                << " " << juce::String(l.min, 2) << ".." << juce::String(l.max, 2) << (l.formula.empty() ? juce::String() : " f=" + juce::String(l.formula)) << "}";
        for (auto& kv : s.project.pattern().events) out << " <" << kv.first << " " << (int)kv.second.size() << " events>";
        log(out);
    }
    else if (cmd == "mididevices") {
        juce::String out = "midi devices:";
        for (auto& d : s.midiDevices()) out << " [" << d.name << (d.notes ? " notes" : "") << (d.controllers ? " controllers" : "") << (d.open ? "" : " CLOSED") << "]";
        log(out + " last=" + s.lastMidiText);
    }
    // ---- hosted plugins ----
    else if (cmd == "vstscan") {                            // vstscan [all] [file ...]: no files = the standard folders
        juce::StringArray files;
        bool all = false;
        for (int i = 1; i < args.size(); ++i) { if (args[i] == "all") all = true; else files.add(juce::File::isAbsolutePath(args[i]) ? args[i] : file(args[i]).getFullPathName()); }
        scriptBusy_ = true;
        main_.settings().plugins().scan(all, files, [this](int found, int failed) {
            scriptBusy_ = false;
            log("vst scan: found " + juce::String(found) + ", failed " + juce::String(failed));
        });
        if (!PluginHost::get().scanning()) { scriptBusy_ = false; fail("the scan did not start"); }
    }
    else if (cmd == "vstlist") {
        juce::String out = "vst plugins:";
        for (auto& e : PluginHost::get().plugins()) out << "\n  " << e.name << " | " << e.maker << " | " << (e.instrument ? "instrument" : "effect");
        for (auto& b : PluginHost::get().blacklist()) out << "\n  FAILED " << juce::File::createFileWithoutCheckingPath(b).getFileName();
        log(out + "\n  status: " + main_.settings().plugins().status());
    }
    else if (cmd == "vstfx" && args.size() >= 4) {          // vstfx <track> <slot> <part of the plugin's name>
        const auto id = PluginHost::get().findByName(args[3]);
        if (id.empty() || !main_.mixer().setEffect(args[1].getIntValue(), args[2].getIntValue(), id)) fail("could not load the plugin " + args[3]);
    }
    else if (cmd == "vstchan" && args.size() >= 2) {        // vstchan <part of the plugin's name>
        const auto id = PluginHost::get().findByName(args[1]);
        if (id.empty() || s.addInstrumentChannel(id) < 0) fail("could not load the plugin " + args[1]);
    }
    else if ((cmd == "vstinfo" || cmd == "vsttouch") && args.size() >= 3) {      // vstinfo <track|ch> <slot|channel index>; vsttouch <track|ch> <slot> <param> <value>
        std::shared_ptr<pulp::Plugin> p;
        if (args[1] == "ch") { if (auto* ch = s.channel(args[2].getIntValue())) p = ch->live->plugin; }
        else p = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumMixerTracks - 1, args[1].getIntValue())].slots[juce::jlimit(0, pulp::kNumSlots - 1, args[2].getIntValue())];
        auto* hp = dynamic_cast<HostedPlugin*>(p.get());
        if (!hp) fail("no hosted plugin there" + (p ? " (it is " + juce::String(p->displayName()) + ")" : juce::String()));
        else if (cmd == "vsttouch" && args.size() >= 5) hp->simulateTouch(args[3].getIntValue(), args[4].getFloatValue());
        else {
            juce::String out = "vst: " + juce::String(hp->displayName()) + " by " + hp->description().manufacturerName + (hp->isInstrument() ? " (instrument)" : " (effect)")
                             + " params=" + juce::String((int)hp->params().size()) + "/" + juce::String(hp->totalParameters()) + " latency=" + juce::String(hp->latencySamples())
                             + " tail=" + juce::String(hp->tailSeconds(), 2) + " ins=" + juce::String(hp->instance().getTotalNumInputChannels())
                             + " outs=" + juce::String(hp->instance().getTotalNumOutputChannels());
            for (int i = 0; i < juce::jmin(8, (int)hp->params().size()); ++i)
                out << "\n  " << hp->params()[(size_t)i]->info.id << " " << hp->params()[(size_t)i]->info.name << " = " << juce::String(hp->params()[(size_t)i]->get(), 4) << " (" << hp->valueText(i) << ")";
            log(out);
        }
    }
    else if (cmd == "curve" && args.size() >= 2) {           // curve info | curve add <x> <y> | curve drag <x> <y> <to x> <to y>: the open waveshaper's curve, in -1..1
        BendCurveView* v = nullptr;
        for (auto* c : main_.customViews()) if (auto* b = dynamic_cast<BendCurveView*>(c)) v = b;
        if (!v) fail("no curve editor is open");
        else {
            auto at = [&](float x, float y) { const auto a = v->getLocalBounds().toFloat().reduced(8.0f); const float side = juce::jmin(a.getWidth(), a.getHeight()); const auto p = juce::Rectangle<float>(side, side).withCentre(a.getCentre()); return juce::Point<float>(p.getX() + (x + 1.0f) * 0.5f * p.getWidth(), p.getBottom() - (y + 1.0f) * 0.5f * p.getHeight()); };
            if (args[1] == "add" && args.size() >= 4) { const auto p = at(args[2].getFloatValue(), args[3].getFloatValue()); sendMouse(*v, p, p, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), false, 2); }
            else if (args[1] == "drag" && args.size() >= 6) {       // from x y to x y
                const auto from = at(args[2].getFloatValue(), args[3].getFloatValue()), to = at(args[4].getFloatValue(), args[5].getFloatValue());
                sendMouse(*v, from, to, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), true, 1);
            }
            log(v->describe());
        }
    }
    else if (cmd == "rewind" && args.size() >= 2) {         // rewind info | rewind add time|volume <place 0..1> <height 0..1>: the open Rewind's curves
        RewindCurveView* v = nullptr;
        for (auto* c : main_.customViews()) if (auto* r = dynamic_cast<RewindCurveView*>(c)) v = r;
        if (!v) fail("no Rewind editor is open");
        else {
            if (args[1] == "add" && args.size() >= 5) {
                const auto p = v->plot(args[2] == "time" ? 0 : 1);
                const juce::Point<float> at(p.getX() + args[3].getFloatValue() * p.getWidth(), p.getBottom() - args[4].getFloatValue() * p.getHeight());
                sendMouse(*v, at, at, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), false, 2);
            }
            log(v->describe());
        }
    }
    else if (cmd == "gaugeinfo") { juce::String out; for (auto* v : main_.analysers()) out << v->describe() << "; "; log(out.isEmpty() ? juce::String("no analyser is open") : out); }
    else if (cmd == "vstwin") log(PluginHost::get().describeEditors());
    else if (cmd == "vstshot" && args.size() >= 2) { if (!PluginHost::get().captureWindow(args.size() > 2 ? args[2].getIntValue() : 0, file(args[1]))) fail("could not capture a plugin window"); }
    else if (cmd == "vstclose") PluginHost::get().closeWindows();
    else if (cmd == "openchan" && args.size() >= 2) main_.openChannel(args[1].getIntValue());
    else if (cmd == "midisong" && args.size() >= 3) {      // midisong import|export <file>: a whole MIDI file
        if (args[1] == "import") { const auto what = s.importMidiSong(file(args[2])); if (what.isEmpty()) fail("could not import"); else log("midi import: " + what); }
        else if (!s.exportMidiSong(file(args[2]))) fail("could not export");
    }
    else if (cmd == "sf2" && args.size() >= 2) {           // sf2 <file>: list its presets; sf2 <file> <n>: preset n (from 1) as a channel
        if (args.size() == 2) log("soundfont presets: " + s.soundFontPresets(file(args[1])).joinIntoString(" | "));
        else if (s.addSoundFontChannel(file(args[1]), args[2].getIntValue() - 1) < 0) fail("could not load that preset");
    }
    else if (cmd == "shortcut" && args.size() >= 2) {       // shortcut list | reset | <command> <key, as "ctrl + K"> | <command> none
        auto& sc = Shortcuts::get();
        if (args[1] == "list") log(main_.settings().shortcuts().describe());
        else if (args[1] == "reset") sc.resetAll();
        else {
            int index = -1;
            for (int i = 0; i < (int)sc.commands().size(); ++i) if (sc.commands()[(size_t)i].id == args[1]) index = i;
            if (index < 0 || args.size() < 3) fail("no such command");
            else {
                const auto loser = sc.assign(index, args[2] == "none" ? juce::KeyPress() : juce::KeyPress::createFromDescription(args[2]));
                log("shortcut " + args[1] + " = " + Shortcuts::describe(sc.commands()[(size_t)index].key) + (loser.isEmpty() ? juce::String() : " (taken from " + loser + ")"));
            }
        }
    }
    else if (cmd == "midiout") log(s.midiOutLog() + " outputs: " + s.midiOutputNames().joinIntoString(", "));
    else if (cmd == "tour" && args.size() >= 2) {           // tour start | next | back | close | info
        if (args[1] == "start") main_.startTour();
        else if (!main_.tour()) fail("the tour is not showing");
        else if (args[1] == "next") main_.tour()->go(main_.tour()->step() + 1);
        else if (args[1] == "back") main_.tour()->go(main_.tour()->step() - 1);
        else if (args[1] == "close") main_.endTour(false);
        if (main_.tour()) log(main_.tour()->describe());
    }
    else firstHalf = false;
    if (firstHalf) return true;
    if (cmd == "timesig" && args.size() >= 3) { s.setTimeSignature(args[1].getIntValue(), args[2].getIntValue()); log("time signature " + juce::String(s.project.timeSigNum) + "/" + juce::String(s.project.timeSigDen) + ", a bar is " + juce::String(s.project.ticksPerBar()) + " ticks"); }
    else if (cmd == "browseopen" && args.size() >= 2) {    // browseopen <item> [<item inside it> ...]: open part of the Browser's tree
        juce::StringArray path(args);
        path.remove(0);
        if (!main_.browser().openPath(path)) fail("no such item in the Browser");
    }
    else if (cmd == "browse" && args.size() >= 3) {        // browse instrument|effect <type> [preset]: as if double-clicked in the Browser's plugin database
        const int preset = args.size() > 3 ? args[3].getIntValue() : -1;
        if (args[1] == "effect") { if (s.onUseEffect) s.onUseEffect(args[2].toStdString(), preset); }
        else if (s.useInstrument(args[2].toStdString(), preset) < 0) fail("no such instrument");
    }
    // ---- pads and slices ----
    else if (cmd == "padload" && args.size() >= 3) {        // padload <pad 1..16> <file> [layer]: a sound on a pad of the current channel
        if (!s.setPadSample(s.currentChannel, args[1].getIntValue() - 1, file(args[2]), args.size() > 3)) fail("could not load the pad");
    }
    else if (cmd == "padkit" && args.size() >= 2) log("pads filled: " + juce::String(s.loadPadKit(s.currentChannel, file(args[1]))));
    else if (cmd == "padinfo") {
        auto* ch = s.current();
        auto* kit = ch ? dynamic_cast<pulp::PadKit*>(ch->live->plugin.get()) : nullptr;
        if (!kit) fail("the current channel is not a pad sampler");
        else {
            juce::String out = "pads:";
            for (int i = 0; i < pulp::PadKit::kPads; ++i) if (kit->layerCount(i) > 0) out << " [" << (i + 1) << " " << kit->padName(i) << (kit->layerCount(i) > 1 ? " x" + juce::String(kit->layerCount(i)) : juce::String()) << "]";
            log(out);
        }
    }
    else if (cmd == "chop" && args.size() >= 2) {           // chop <file>: a new channel that slices a loop; chop write: its notes into the pattern; chop info
        auto* ch = s.current();
        auto* chop = ch ? dynamic_cast<pulp::Chop*>(ch->live->plugin.get()) : nullptr;
        if (args[1] == "write") log("slice notes written: " + juce::String(s.writeSliceNotes(s.currentChannel)));
        else if (args[1] == "info") {
            if (!chop || !chop->sample()) fail("the current channel has no loop");
            else {
                juce::String out = "chop: " + juce::String((int)chop->slices().size()) + " slices, " + juce::String(chop->beats(), 2) + " beats, at";
                for (auto st : chop->slices()) out << " " << juce::String((double)st / chop->sample()->sampleRate, 3);
                log(out);
            }
        }
        else if (args[1] == "detect") { if (chop) { chop->detect(); s.commit("Slice the loop"); } else fail("the current channel is not a slicer"); }
        else if (s.addChopChannel(file(args[1])) < 0) fail("could not slice that file");
    }
    else if (cmd == "buildinst" && args.size() >= 2) {      // buildinst <file> [file ...]: a multisample instrument from recordings of single notes
        juce::StringArray files;
        for (int i = 1; i < args.size(); ++i) files.add(file(args[i]).getFullPathName());
        if (s.buildInstrumentChannel(files) < 0) fail("could not build an instrument");
        else log("built instrument:\n" + s.lastBuildReport.trimEnd());
    }
    else if (cmd == "loadinst" && args.size() >= 2) { if (s.addInstrumentFileChannel(file(args[1])) < 0) fail("could not load the instrument"); }
    else if (cmd == "usetake" && args.size() >= 2) { s.project.useTake(args[1].getIntValue()); s.commit("Use take"); }
    else if (cmd == "splitclip" && args.size() >= 3) { if (s.project.splitClip(args[1].getIntValue(), args[2].getLargeIntValue()) < 0) fail("cannot split there"); else s.commit("Slice clips"); }
    else if (cmd == "ed" && args.size() >= 2) {             // ed <command> ...: the newest open Splice editor
        if (!main_.spliceCommand(args, baseDir_, [this](const juce::String& m) { log(m); })) fail("editor command failed");
    }
    // ---- files, undo, backups, export ----
    else if (cmd == "save" && args.size() >= 2) { if (!s.saveProject(file(args[1]))) fail("save failed"); }
    else if (cmd == "open" && args.size() >= 2) { if (!s.openProject(file(args[1]))) fail("open failed"); }
    else if (cmd == "new") { if (args.size() >= 2) s.newFromTemplate(args[1] == "demo" ? -1 : args[1].getIntValue()); else s.newProject(); }      // new [template number | demo]
    else if (cmd == "saveversion") { if (!s.saveNewVersion()) fail("no file to version"); else log("saved " + s.projectFile.getFileName()); }
    else if (cmd == "pack" && args.size() >= 2) { if (!s.packProject(file(args[1]))) fail("pack failed"); }
    else if (cmd == "undo") s.undoToggle();
    else if (cmd == "undostep") s.undoStep();
    else if (cmd == "redo") s.redoStep();
    else if (cmd == "history") {
        juce::String h = "history pos=" + juce::String(s.historyPos) + " dirty=" + juce::String((int)s.dirty) + " title=" + s.projectTitle() + " :";
        for (int i = juce::jmax(0, (int)s.history.size() - 12); i < (int)s.history.size(); ++i) h << " [" << i << (i == s.historyPos ? "*" : "") << " " << s.history[(size_t)i].name << "]";
        log(h);
    }
    else if (cmd == "autosave") { if (!s.autosaveNow()) fail("autosave failed"); }
    else if (cmd == "backups") {
        const auto b = s.listBackups();
        juce::String out = "backups crashed=" + juce::String((int)s.crashedLastTime) + " count=" + juce::String(b.size());
        if (!b.isEmpty()) out << " newest=" << b[0].getFileName().upToFirstOccurrenceOf("__", false, false);
        log(out);
    }
    else if (cmd == "recover") { const auto b = s.listBackups(); if (b.isEmpty() || !s.restoreBackup(b[0])) fail("nothing to recover"); }
    else if (cmd == "crash") { log("crashing on purpose"); volatile int* p = nullptr; *p = 1; }
    else if (cmd == "export" && args.size() >= 2) {         // export <file> [song|pattern] [rate] [bits] [leave|cut|wrap]
        Session::ExportOptions o;
        o.song = args[2] != "pattern";
        if (args[3].getDoubleValue() > 0) o.sampleRate = args[3].getDoubleValue();
        if (args[4].getIntValue() > 0) o.bits = args[4].getIntValue();
        o.tail = args[5] == "cut" ? 1 : (args[5] == "wrap" ? 2 : 0);
        o.stems = args[6] == "stems";       // the format is the file's ending: wav, flac, mp3, ogg
        const auto f = file(args[1]);
        exportBusy_ = true;
        main_.exportTo(f, o, [this, f, stems = o.stems](juce::String err) {
            exportBusy_ = false;
            if (err.isNotEmpty()) log("export error: " + err);
            else if (stems) log("exported stems: " + juce::String(f.getParentDirectory().findChildFiles(juce::File::findFiles, false, f.getFileNameWithoutExtension() + " - *" + f.getFileExtension()).size()) + " files");
            else log("exported " + f.getFileName() + " " + juce::String(f.getSize()) + " bytes");
        });
    }
    else if (cmd == "waitexport") {                         // hold the script until the export has finished
        if (exportBusy_) { --next_; startTimer(100); return false; }
    }
    else if (cmd == "mixsel" && args.size() >= 2) main_.mixer().selectTrack(args[1].getIntValue());
    else if (cmd == "mixmove" && args.size() >= 3) {        // mixmove <track> left|right: as the track menu's Move left / Move right
        if (!main_.mixer().moveTrack(args[1].getIntValue(), args[2] == "left" ? -1 : 1)) fail("that track cannot move there");
    }
    else if (cmd == "fxmove" && args.size() >= 4) {         // fxmove <track> <slot> <to slot>: two slots change places
        if (!s.project.swapSlots(args[1].getIntValue(), args[2].getIntValue(), args[3].getIntValue())) fail("no such slot");
        else s.commit("Move effect");
    }
    else if (cmd == "fx" && args.size() >= 4) {             // fx <track> <slot> <type|none>
        if (!main_.mixer().setEffect(args[1].getIntValue(), args[2].getIntValue(), args[3] == "none" ? std::string() : args[3].toStdString())) fail("could not set effect");
    }
    else if (cmd == "openfx" && args.size() >= 3) {
        auto& t = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumMixerTracks - 1, args[1].getIntValue())];
        auto p = t.slots[juce::jlimit(0, pulp::kNumSlots - 1, args[2].getIntValue())];
        if (p) main_.openEffect(p, p->displayName()); else fail("empty slot");
    }
    else if (cmd == "fxparam" && args.size() >= 5) {        // fxparam <track> <slot> <id> <value>
        auto& t = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumMixerTracks - 1, args[1].getIntValue())];
        auto p = t.slots[juce::jlimit(0, pulp::kNumSlots - 1, args[2].getIntValue())];
        auto* prm = p ? p->param(args[3].toStdString()) : nullptr;
        // #n is the nth parameter (hosted plugins name theirs by number)
        if (p && args[3].startsWith("#") && args[3].substring(1).getIntValue() < (int)p->params().size()) prm = p->params()[(size_t)args[3].substring(1).getIntValue()].get();
        if (prm) { prm->set(args[4].getFloatValue()); s.commit("Script"); } else fail("no such effect parameter");
    }
    else if (cmd == "mixparam" && args.size() >= 4) {       // mixparam <track> <id> <value>
        auto* prm = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumMixerTracks - 1, args[1].getIntValue())].live->param(args[2].toStdString());
        if (prm) { prm->set(args[3].getFloatValue()); s.commit("Script"); } else fail("no such mixer parameter");
    }
    else if (cmd == "route" && args.size() >= 3) {          // route <from> <to> [sc|off]
        const int from = args[1].getIntValue(), to = args[2].getIntValue();
        if (!s.project.setRoute(from, to, args[3] != "off", args[3] == "sc")) log("route refused " + args[1] + " -> " + args[2]);
        s.commit("Script");
    }
    else if (cmd == "pltool" && args.size() >= 2) main_.playlist().setTool(args[1].getIntValue());
    else if (cmd == "pltrack" && args.size() >= 3) {        // pltrack <track> group|ungroup|collapse|expand
        const int t = args[1].getIntValue();
        if (t < 0 || t >= pulp::kNumPlaylistTracks) fail("no such track");
        else if (args[2] == "group" || args[2] == "ungroup") { s.project.tracks[(size_t)t].grouped = args[2] == "group" && t > 0; s.commit("Track group"); }
        else if (args[2] == "collapse" || args[2] == "expand") {
            if (!s.project.trackIsGroupHead(t)) fail("track " + juce::String(t) + " is not the top of a group");
            else main_.playlist().setCollapsed(t, args[2] == "collapse");
        }
        else fail("pltrack <track> group|ungroup|collapse|expand");
    }
    else if (cmd == "pltracks") log(main_.playlist().describeTracks(args.size() >= 2 ? args[1].getIntValue() : 8));
    else if (cmd == "pianoset" && args.size() >= 3) {       // pianoset stamp|scaleroot|scale|wave <n>
        auto& pr = main_.pianoRoll();
        const int v = args[2].getIntValue();
        if (args[1] == "stamp") pr.stampChord = v;
        else if (args[1] == "scaleroot") pr.scaleRoot = v;
        else if (args[1] == "scale") pr.scaleIndex = v;
        else if (args[1] == "wave") pr.setBackdrop(v != 0);
        else fail("unknown piano roll setting");
        pr.repaint();
    }
    else if (cmd == "fxpreset" && args.size() >= 4) {       // fxpreset <track> <slot> <index>: load an effect's factory preset
        auto& t = s.project.mixer[(size_t)juce::jlimit(0, pulp::kNumMixerTracks - 1, args[1].getIntValue())];
        auto p = t.slots[juce::jlimit(0, pulp::kNumSlots - 1, args[2].getIntValue())];
        const auto ps = p ? p->factoryPresets() : std::vector<pulp::Preset>();
        const int i = args[3].getIntValue();
        if (i >= 0 && i < (int)ps.size()) { p->loadState(ps[(size_t)i].state); s.commit("Preset"); log("preset: " + juce::String(ps[(size_t)i].name)); }
        else fail("no such preset");
    }
    else if (cmd == "preset" && args.size() >= 3) {         // preset <channel> <index>: load an instrument's factory preset
        if (auto* ch = s.channel(args[1].getIntValue())) {
            const auto ps = ch->live->plugin->factoryPresets();
            const int i = args[2].getIntValue();
            if (i >= 0 && i < (int)ps.size()) { ch->live->plugin->loadState(ps[(size_t)i].state); s.commit("Preset"); log("preset: " + juce::String(ps[(size_t)i].name)); }
            else fail("no such preset");
        } else fail("no such channel");
    }
    else if (cmd == "tool" && args.size() >= 2) main_.pianoRoll().runTool(args[1].getIntValue());      // piano roll tool by number
    else if (cmd == "ptool" && args.size() >= 2) main_.pianoRoll().setTool(args[1].getIntValue());     // piano roll mouse tool
    else if (cmd == "toolset" && args.size() >= 3) {        // toolset <setting> <value>: a setting of the piano roll tool panel that is open
        if (!main_.pianoRoll().setToolParam(args[1], args[2].getFloatValue())) fail("no tool panel is open, or it has no such setting");
    }
    else if (cmd == "record") { s.recording = args[1] != "off"; s.notify(); }
    else if (cmd == "addchannel" && args.size() >= 2) { if (s.addInstrumentChannel(args[1].toStdString()) < 0) fail("unknown instrument"); }
    else if (cmd == "midi" && args.size() >= 3) {
        if (args[1] == "import") main_.pianoRoll().importMidi(file(args[2]));
        else main_.pianoRoll().exportMidi(file(args[2]));
    }
    else if (cmd == "preview" && args.size() >= 2) { if (!s.previewFile(file(args[1]))) fail("cannot preview"); }
    else if (cmd == "shot" && args.size() >= 3) {           // shot <target> <file> [scale]
        if (auto* c = main_.findTarget(args[1])) {
            if (!saveSnapshot(*c, file(args[2]), args.size() > 3 ? args[3].getFloatValue() : 1.0f)) fail("could not write image");
        } else fail("unknown target " + args[1]);
    }
    else if (cmd == "paintbench" && args.size() >= 2) {     // paintbench <target> [times]: how long the window takes to draw itself (software renderer)
        if (auto* c = main_.findTarget(args[1])) {
            const int n = juce::jlimit(1, 200, args.size() > 2 ? args[2].getIntValue() : 20);
            juce::Image img(juce::Image::ARGB, juce::jmax(1, c->getWidth()), juce::jmax(1, c->getHeight()), true);
            double worst = 0.0, total = 0.0;
            for (int i = 0; i < n; ++i) {
                juce::Graphics g(img);
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                c->paintEntireComponent(g, true);
                const double dt = juce::Time::getMillisecondCounterHiRes() - t0;
                total += dt;
                worst = juce::jmax(worst, dt);
            }
            log("paint " + args[1] + " " + juce::String(c->getWidth()) + "x" + juce::String(c->getHeight()) + ": " + juce::String(total / n, 2) + " ms on average, worst "
                + juce::String(worst, 2) + " ms (a frame at 60 a second has 16.7 ms)");
        } else fail("unknown target " + args[1]);
    }
    else if (cmd == "hint" && args.size() >= 4) {           // hint <target> x y: the hint text of whatever is there
        if (auto* c = main_.findTarget(args[1])) {
            auto* under = c->getComponentAt(args[2].getIntValue(), args[3].getIntValue());
            log("hint: " + MainComponent::hintFor(under ? under : c));
        } else fail("unknown target " + args[1]);
    }
    else if (cmd == "windows") {
        juce::String w;
        for (auto* n : {"rack", "chan", "settings", "piano", "playlist", "mixer", "events", "link"})   // windows that do not exist yet are skipped
            if (auto* win = main_.window(n)) w << n << "=" << (win->isVisible() ? 1 : 0) << (win->isDetached() ? "d " : " ");
        log("windows " + w);
    }
    else if (cmd == "state") {
        log("state playing=" + juce::String((int)s.engine.isPlaying()) + " tick=" + juce::String(s.engine.positionTicks(), 1)
            + " tempo=" + juce::String(s.project.global->tempo.get(), 2) + " voices=" + juce::String(s.engine.activeVoices())
            + " xruns=" + juce::String(s.xruns()) + " cpu=" + juce::String(s.engine.cpuLoad(), 3)
            + " channels=" + juce::String((int)s.project.channels.size()) + " patterns=" + juce::String((int)s.project.patterns.size())
            + " pattern=" + juce::String(s.project.currentPattern) + " channel=" + juce::String(s.currentChannel)
            + " song=" + juce::String((int)s.project.songMode) + " preview=" + juce::String((int)s.engine.preview.isPlaying()));
    }
    else if (cmd == "steps") {                              // one line per channel: x = step set, . = empty
        for (auto& ch : s.project.channels) {
            juce::String row;
            const auto* notes = s.project.pattern().find(ch.id);
            for (int i = 0; i < s.project.pattern().steps; ++i) row << ((notes && pulp::Project::hasStep(*notes, i)) ? "x" : ".");
            log("steps " + row + "  " + juce::String(ch.name) + (ch.live->mute.getBool() ? "  (muted)" : ""));
        }
    }
    else if (cmd == "dump" && args.size() >= 2) file(args[1]).replaceWithText(juce::String(s.project.toJson().dump(1)));
    else if (cmd == "devices") log(s.describeDevices());
    else if (cmd == "device" && args.size() >= 2) {
        const auto err = s.openDevice(args[1], args[2], args[3].getDoubleValue(), args[4].getIntValue());
        log("device: " + (err.isEmpty() ? s.deviceSummary() : "FAILED " + err));
        if (err.isNotEmpty()) ++errors_;
    }
    else if (cmd == "log") log(line.fromFirstOccurrenceOf(" ", false, false));
    else fail("unknown command");
    return true;
}
