CC = clang
# The oldest macOS every program and plug-in is built for (2026-11-22): macOS 14
# Sonoma, on Apple silicon only. Newer calls must be guarded (the compilers stop on an unguarded
# one). app/Info.plist, both plug-ins' Info.plist and the installer package say the same.
MACOS_MIN = 14.0
ARCH = arm64
CFLAGS = -std=gnu11 -O2 -g -Wall -Wextra -arch $(ARCH) -mmacosx-version-min=$(MACOS_MIN) \
		-Werror=unguarded-availability -Werror=unguarded-availability-new
BUILD = build

.PHONY: all test tsan probe stream settings midi helper plugin agent agent-test midiplugin linktest \
		clean app drv \
		halcheck app-test app-pictures user-app user-pictures manual pkg

all: test probe stream settings midi helper plugin agent midiplugin drv

test: $(BUILD)/test_desc $(BUILD)/test_stream $(BUILD)/test_control $(BUILD)/test_resample \
		$(BUILD)/test_midi $(BUILD)/ua101tonerate $(BUILD)/ua101fixrate $(BUILD)/ua101wavcheck \
		$(BUILD)/ua101jackmap $(BUILD)/test_framering $(BUILD)/test_clock $(BUILD)/test_timeline \
		$(BUILD)/test_shared $(BUILD)/test_convert $(BUILD)/test_plugin $(BUILD)/test_service \
		$(BUILD)/test_bundle $(BUILD)/test_ctlengine $(BUILD)/test_ctlhelper $(BUILD)/test_midigate \
		$(BUILD)/test_midiplugin $(BUILD)/test_midibundle $(BUILD)/test_midilamp \
		$(BUILD)/test_drvset plugin midiplugin
	./$(BUILD)/test_drvset
	./$(BUILD)/test_midigate
	./$(BUILD)/test_midilamp
	./$(BUILD)/test_midiplugin
	./$(BUILD)/test_midibundle $(BUILD)/UA101MIDI.plugin
	./$(BUILD)/test_ctlengine
	./$(BUILD)/test_ctlhelper
	./$(BUILD)/test_desc
	./$(BUILD)/test_stream
	./$(BUILD)/test_control
	./$(BUILD)/test_resample
	./$(BUILD)/test_midi
	./$(BUILD)/test_framering
	./$(BUILD)/test_clock
	./$(BUILD)/test_timeline
	./$(BUILD)/test_shared
	./$(BUILD)/test_convert
	./$(BUILD)/test_plugin
	./$(BUILD)/test_service
	./$(BUILD)/test_bundle $(BUILD)/UA101.driver
	test "$$(nm -gU $(BUILD)/UA101.driver/Contents/MacOS/UA101 | awk '{print $$3}')" = _UA101_Create
	bash tests/test_tonerate.sh $(BUILD)
	bash tests/test_wavcheck.sh $(BUILD)
	bash tests/test_jackmap.sh $(BUILD)

probe: $(BUILD)/ua101probe

stream: $(BUILD)/ua101stream $(BUILD)/ua101wavcheck $(BUILD)/ua101fixrate $(BUILD)/ua101tonerate \
		$(BUILD)/ua101macrate $(BUILD)/ua101jackmap

settings: $(BUILD)/ua101settings

midi: $(BUILD)/ua101midi

$(BUILD)/test_desc: tests/test_desc.c src/ua101_desc.c src/ua101_desc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_desc.c src/ua101_desc.c

$(BUILD)/test_stream: tests/test_stream.c src/ua101_stream.c src/ua101_stream.h src/ua101_analysis.c src/ua101_analysis.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_stream.c src/ua101_stream.c src/ua101_analysis.c

CTL_ENGINE = src/ua101_ctlengine.c src/ua101_ctlmsg.c src/ua101_ctlbuild.c src/ua101_ctlsim.c src/ua101_control.c
$(BUILD)/test_ctlengine: tests/test_ctlengine.c $(CTL_ENGINE) src/ua101_ctlengine.h src/ua101_ctlmsg.h \
		src/ua101_ctlsim.h src/ua101_control.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DUA101_CTLENGINE_TESTING -o $@ tests/test_ctlengine.c $(CTL_ENGINE) -lm

$(BUILD)/test_control: tests/test_control.c src/ua101_control.c src/ua101_control.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_control.c src/ua101_control.c

$(BUILD)/test_resample: tests/test_resample.c src/ua101_resample.c src/ua101_resample.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DUA101_RESAMPLE_TESTING -o $@ tests/test_resample.c src/ua101_resample.c

$(BUILD)/test_clock: tests/test_clock.c src/ua101_clock.c src/ua101_clock.h tests/data/ua101-bus-stamps-20s.txt \
		tests/data/ua101-bus-stamps-duplex-20s.txt tests/data/ua101-bus-stamps-crossing-40s.txt
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_clock.c src/ua101_clock.c

$(BUILD)/test_timeline: tests/test_timeline.c tests/data/ua101-48k-capture-transfers.txt src/ua101_timeline.c src/ua101_timeline.h src/ua101_clock.c \
		src/ua101_clock.h src/ua101_framering.c src/ua101_framering.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_timeline.c src/ua101_timeline.c \
		src/ua101_clock.c src/ua101_framering.c

# The stress test races two threads on purpose, so it uses the address checker, not the thread one.
$(BUILD)/test_framering: tests/test_framering.c src/ua101_framering.c src/ua101_framering.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -pthread -o $@ tests/test_framering.c src/ua101_framering.c

$(BUILD)/test_shared: tests/test_shared.c src/ua101_shared.c src/ua101_shared.h src/ua101_framering.c \
		src/ua101_framering.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -pthread -o $@ tests/test_shared.c src/ua101_shared.c \
		src/ua101_framering.c

$(BUILD)/test_convert: tests/test_convert.c src/ua101_convert.c src/ua101_convert.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_convert.c src/ua101_convert.c

# The plug-in as Core Audio drives it, with a fake helper link and a fake host, in one process.
PLUGIN_CORE = src/ua101_convert.c src/ua101_shared.c src/ua101_framering.c src/ua101_resample.c \
		src/ua101_steer.c
PLUGIN_HEADERS = src/ua101_convert.h src/ua101_link.h src/ua101_shared.h src/ua101_framering.h \
		src/ua101_resample.h src/ua101_steer.h src/ua101_drvset.h
$(BUILD)/test_plugin: tests/test_plugin.c src/plugin/ua101_plugin.c $(PLUGIN_CORE) $(PLUGIN_HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_plugin.c src/plugin/ua101_plugin.c \
		$(PLUGIN_CORE) -framework CoreAudio -framework CoreFoundation

# The real plug-in, its real XPC link and the helper's real service in one process, with a
# pretend unit in place of the USB engine and of the bus and sleep watcher: unplugging,
# switching off, sleep and wake, and a helper that doesn't answer, with no unit needed.
SERVICE_TEST = tests/test_service.c src/plugin/ua101_plugin.c src/plugin/ua101_link_xpc.c \
		src/ua101_service.c src/ua101_control.c src/ua101_ctlmsg.c src/ua101_midigate.c \
		src/ua101_midi.c
$(BUILD)/test_service: $(SERVICE_TEST) src/plugin/ua101_link_xpc.h src/ua101_service.h \
		src/ua101_engine.h src/ua101_watch.h src/ua101_midiport.h src/ua101_control.h \
		src/ua101_midigate.h src/ua101_midi.h src/ua101_midilamp.h \
		$(PLUGIN_CORE) $(PLUGIN_HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(SERVICE_TEST) $(PLUGIN_CORE) \
		-framework CoreAudio -framework CoreFoundation

# The control app's way to the unit with no unit: the app's real engine and its real end of the
# link, the helper's real service, and a pretend MIDI interface with the pretend UA-101 behind
# it. It checks that the settings request is the only message that can reach the cable.
CTLHELPER_TEST = tests/test_ctlhelper.c src/ua101_service.c src/ua101_ctllink.c app/panel_shim.c \
		$(CTL_ENGINE) src/ua101_shared.c src/ua101_framering.c src/ua101_midigate.c \
		src/ua101_midi.c src/ua101_midilink.c
CTLHELPER_HEADERS = src/ua101_service.h src/ua101_ctllink.h src/ua101_link.h src/ua101_midiport.h \
		src/ua101_midigate.h src/ua101_midi.h src/ua101_midilink.h src/ua101_midilamp.h \
		src/ua101_engine.h src/ua101_watch.h src/ua101_shared.h src/ua101_framering.h \
		app/panel_shim.h src/ua101_ctlengine.h src/ua101_ctlmsg.h src/ua101_ctlsim.h src/ua101_control.h
$(BUILD)/test_ctlhelper: $(CTLHELPER_TEST) $(CTLHELPER_HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DUA101_SERVICE_TESTING -o $@ $(CTLHELPER_TEST) -lm

# The compiler's thread checker on the tests that use more than one thread: the clock's sequence
# lock, the frame ring, and the plug-in with its I/O cycle on a thread of its own beside hellos
# and property calls. It reports fields two threads touch without a lock or an atomic. It does
# not look inside memcpy, so the frame ring's frames, raced on purpose, don't show.
tsan: tests/test_shared.c tests/test_framering.c tests/test_plugin.c src/plugin/ua101_plugin.c \
		$(SERVICE_TEST) $(PLUGIN_CORE) $(PLUGIN_HEADERS) $(CTLHELPER_TEST) $(CTLHELPER_HEADERS) \
		$(MIDIPLUGIN_TEST) $(MIDIPLUGIN_HEADERS)
	mkdir -p $(BUILD)/tsan
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -pthread -o $(BUILD)/tsan/test_shared \
		tests/test_shared.c src/ua101_shared.c src/ua101_framering.c
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -pthread -o $(BUILD)/tsan/test_framering \
		tests/test_framering.c src/ua101_framering.c
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -o $(BUILD)/tsan/test_plugin tests/test_plugin.c \
		src/plugin/ua101_plugin.c $(PLUGIN_CORE) -framework CoreAudio -framework CoreFoundation
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -o $(BUILD)/tsan/test_service $(SERVICE_TEST) \
		$(PLUGIN_CORE) -framework CoreAudio -framework CoreFoundation
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -DUA101_SERVICE_TESTING -o $(BUILD)/tsan/test_ctlhelper $(CTLHELPER_TEST) -lm
	$(CC) -std=gnu11 -O1 -g -fsanitize=thread -DUA101_MIDIPLUGIN_TESTING -o $(BUILD)/tsan/test_midiplugin \
		$(MIDIPLUGIN_TEST) -framework CoreMIDI -framework CoreFoundation
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_shared
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_framering
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_plugin
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_service
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_ctlhelper
	TSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/tsan/test_midiplugin

# Needs the unit: the plug-in, its XPC link and the helper's service in one process.
linktest: $(BUILD)/ua101linktest

$(BUILD)/ua101linktest: tests/ua101linktest.c src/plugin/ua101_plugin.c src/plugin/ua101_link_xpc.c \
		src/plugin/ua101_link_xpc.h src/ua101_service.c src/ua101_service.h src/ua101_engine.c \
		src/ua101_engine.h src/ua101_timeline.c src/ua101_clock.c src/ua101_stream.c src/ua101_desc.c \
		src/ua101_watch.c src/ua101_watch.h src/ua101_midiport.c src/ua101_midiport.h \
		src/ua101_control.c src/ua101_control.h src/ua101_midigate.c src/ua101_midigate.h \
		src/ua101_midi.c src/ua101_midi.h $(PLUGIN_CORE) $(PLUGIN_HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ tests/ua101linktest.c src/plugin/ua101_plugin.c \
		src/plugin/ua101_link_xpc.c src/ua101_service.c src/ua101_engine.c src/ua101_timeline.c \
		src/ua101_clock.c src/ua101_stream.c src/ua101_desc.c src/ua101_watch.c src/ua101_midiport.c \
		src/ua101_control.c src/ua101_ctlmsg.c src/ua101_midigate.c src/ua101_midi.c $(PLUGIN_CORE) \
		-framework IOKit -framework CoreAudio -framework CoreFoundation

$(BUILD)/test_bundle: tests/test_bundle.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_bundle.c -framework CoreAudio -framework CoreFoundation

# The bundle Core Audio loads. Signed ad hoc here; install/install.sh installs it (INSTALL.md).
plugin: $(BUILD)/UA101.driver/Contents/MacOS/UA101

$(BUILD)/UA101.driver/Contents/MacOS/UA101: src/plugin/ua101_plugin.c src/plugin/ua101_link_xpc.c \
		src/plugin/ua101_link_xpc.h src/plugin/Info.plist $(PLUGIN_CORE) $(PLUGIN_HEADERS)
	mkdir -p $(BUILD)/UA101.driver/Contents/MacOS
	cp src/plugin/Info.plist $(BUILD)/UA101.driver/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -fvisibility=hidden -o $@ src/plugin/ua101_plugin.c \
		src/plugin/ua101_link_xpc.c $(PLUGIN_CORE) -framework CoreAudio -framework CoreFoundation
	codesign --force -s - $(BUILD)/UA101.driver

# The gate between MIDI clients and the unit's MIDI sockets, with no Apple APIs. Built with the
# switch that lets the test leave out one check at a time; the helper is built without it.
$(BUILD)/test_midigate: tests/test_midigate.c src/ua101_midigate.c src/ua101_midigate.h src/ua101_midi.c \
		src/ua101_midi.h src/ua101_control.c src/ua101_control.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DUA101_MIDIGATE_TESTING -o $@ tests/test_midigate.c \
		src/ua101_midigate.c src/ua101_midi.c src/ua101_control.c

# The driver's settings: the lists of delays and the measure of spare room (src/ua101_drvset.h),
# on a real frame ring with time moved by hand.
$(BUILD)/test_drvset: tests/test_drvset.c src/ua101_drvset.h src/ua101_framering.c src/ua101_framering.h \
		src/ua101_shared.c src/ua101_shared.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_drvset.c src/ua101_framering.c \
		src/ua101_shared.c

# The control app's MIDI lamps, the helper's pacing (src/ua101_midilamp.h), with time moved by hand.
$(BUILD)/test_midilamp: tests/test_midilamp.c src/ua101_midilamp.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_midilamp.c

$(BUILD)/test_midi: tests/test_midi.c src/ua101_midi.c src/ua101_midi.h src/ua101_control.c src/ua101_control.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_midi.c src/ua101_midi.c src/ua101_control.c

$(BUILD)/ua101fixrate: src/ua101fixrate.c src/ua101_resample.c src/ua101_resample.h src/ua101_stream.c src/ua101_stream.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101fixrate.c src/ua101_resample.c src/ua101_stream.c

# Apple marks parts of IOUSBLib as deprecated, but they remain the documented
# route for user-space USB, so those warnings are hidden.
$(BUILD)/ua101probe: src/ua101probe.c src/ua101_desc.c src/ua101_desc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ src/ua101probe.c src/ua101_desc.c \
		-framework IOKit -framework CoreFoundation

helper: $(BUILD)/ua101d

$(BUILD)/ua101d: src/ua101d.c src/ua101_engine.c src/ua101_engine.h src/ua101_drvset.h src/ua101_shared.c src/ua101_shared.h \
		src/ua101_framering.c src/ua101_framering.h src/ua101_timeline.c src/ua101_timeline.h \
		src/ua101_clock.c src/ua101_clock.h src/ua101_stream.c src/ua101_stream.h src/ua101_desc.c \
		src/ua101_desc.h src/ua101_service.c src/ua101_service.h src/ua101_link.h src/ua101_watch.c \
		src/ua101_watch.h src/ua101_midiport.c src/ua101_midiport.h src/ua101_control.c src/ua101_control.h \
		src/ua101_ctlmsg.c src/ua101_ctlmsg.h src/ua101_midigate.c src/ua101_midigate.h \
		src/ua101_midi.c src/ua101_midi.h src/ua101_midilamp.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ src/ua101d.c src/ua101_engine.c src/ua101_shared.c \
		src/ua101_framering.c src/ua101_timeline.c src/ua101_clock.c src/ua101_stream.c src/ua101_desc.c \
		src/ua101_service.c src/ua101_watch.c src/ua101_midiport.c src/ua101_control.c src/ua101_ctlmsg.c \
		src/ua101_midigate.c src/ua101_midi.c \
		-framework IOKit -framework CoreFoundation
	@# The helper classes messages (src/ua101_ctlmsg.c) but must not be able to build one that
	@# sets anything: that code (src/ua101_ctlbuild.c) stays out.
	@! nm $@ | grep -q ua101_ctlmsg_build
	@# Nor is it built with the switches that let a test leave out one of its checks.
	@! nm $@ | grep -q 'ua101_service_test_\|ua101_midigate_test_'

# The per-user agent: the unit's MIDI sockets as the MIDI port "UA-101 MIDI", through the helper.
# It has no USB code: IOKit is not linked.
agent: $(BUILD)/ua101midiagent

AGENT_CORE = src/ua101_midiports.c src/ua101_midilink.c src/ua101_midi.c
AGENT_HEADERS = src/ua101_midiports.h src/ua101_midilink.h src/ua101_midi.h src/ua101_link.h \
		src/ua101_shared.h
$(BUILD)/ua101midiagent: src/ua101midiagent.c $(AGENT_CORE) $(AGENT_HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101midiagent.c $(AGENT_CORE) -framework CoreMIDI -framework CoreFoundation
	@# It must not be able to reach the unit by itself.
	@! otool -L $@ | grep -q IOKit

# The agent's CoreMIDI side with no unit: the agent's code, the helper's real service and a
# pretend unit in one process, with this test as the MIDI app. It needs the logged-in user's
# MIDI server, so it is not part of `make test`. Its ports are private and hidden from apps.
AGENT_TEST = tests/test_midiagent.c $(AGENT_CORE) src/ua101_service.c src/ua101_control.c \
		src/ua101_ctlmsg.c src/ua101_midigate.c src/ua101_shared.c src/ua101_framering.c
$(BUILD)/test_midiagent: $(AGENT_TEST) $(AGENT_HEADERS) src/ua101_service.h src/ua101_midigate.h \
		src/ua101_midiport.h src/ua101_engine.h src/ua101_watch.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(AGENT_TEST) \
		-framework CoreMIDI -framework CoreFoundation

agent-test: $(BUILD)/test_midiagent
	./$(BUILD)/test_midiagent

# The CoreMIDI driver plug-in the user's MIDI server loads, which
# shows the UA-101 as a MIDI device in Audio MIDI Setup. A MIDI client of the helper, as the
# agent is: no USB code (IOKit is not linked), none of the helper's port or gate code, and it
# exports only its factory. Signed ad hoc here.
midiplugin: $(BUILD)/UA101MIDI.plugin/Contents/MacOS/UA101MIDI

MIDIPLUGIN_CORE = src/midiplugin/ua101_midiplugin.c src/ua101_midilink.c src/ua101_midi.c
MIDIPLUGIN_HEADERS = src/midiplugin/ua101_midiplugin.h src/ua101_midilink.h src/ua101_midi.h \
		src/ua101_link.h src/ua101_shared.h
$(BUILD)/UA101MIDI.plugin/Contents/MacOS/UA101MIDI: $(MIDIPLUGIN_CORE) $(MIDIPLUGIN_HEADERS) \
		src/midiplugin/Info.plist app/AppIcon.png
	rm -rf $(BUILD)/UA101MIDI.plugin
	mkdir -p $(BUILD)/UA101MIDI.plugin/Contents/MacOS $(BUILD)/UA101MIDI.plugin/Contents/Resources
	cp src/midiplugin/Info.plist $(BUILD)/UA101MIDI.plugin/Contents/Info.plist
	sips -s format png -z 128 128 app/AppIcon.png \
		--out $(BUILD)/UA101MIDI.plugin/Contents/Resources/UA-101.png >/dev/null
	@# Linked beside the bundle, so its debug symbols (UA101MIDI.dSYM) stay outside it.
	$(CC) $(CFLAGS) -bundle -fvisibility=hidden -o $(BUILD)/UA101MIDI $(MIDIPLUGIN_CORE) \
		-framework CoreMIDI -framework CoreFoundation
	cp $(BUILD)/UA101MIDI $@
	@# It must not be able to reach the unit by itself, nor hold any of the helper's code.
	@! otool -L $@ | grep -q IOKit
	@! nm $@ | grep -q 'ua101_midiport_\|ua101_midigate_\|ua101_service_\|ua101_midiplugin_test'
	test "$$(nm -gU $@ | awk '{print $$3}')" = _UA101MIDI_Create
	@# macOS puts Finder information on a new folder named .plugin (and iCloud and sips add more),
	@# which codesign refuses. So the bundle is signed outside iCloud under a plain folder name,
	@# then renamed and copied back; if the tag comes back after signing, only a strict check minds.
	@d=$$(mktemp -d) && ditto --noextattr --noqtn $(BUILD)/UA101MIDI.plugin "$$d/UA101MIDI.stage" && \
		xattr -cr "$$d/UA101MIDI.stage" && codesign --force -s - "$$d/UA101MIDI.stage" && \
		mv "$$d/UA101MIDI.stage" "$$d/UA101MIDI.plugin" && rm -rf $(BUILD)/UA101MIDI.plugin && \
		ditto --noextattr --noqtn "$$d/UA101MIDI.plugin" $(BUILD)/UA101MIDI.plugin && rm -rf "$$d" && \
		echo "signed $(BUILD)/UA101MIDI.plugin"
	codesign --verify $(BUILD)/UA101MIDI.plugin

# The plug-in's code linked into a test with the helper's real service, a pretend unit and a
# pretend MIDI server (tests/test_midiplugin.c). Needs no MIDI server and no unit.
MIDIPLUGIN_TEST = tests/test_midiplugin.c $(MIDIPLUGIN_CORE) src/ua101_service.c src/ua101_control.c \
		src/ua101_ctlmsg.c src/ua101_midigate.c src/ua101_shared.c src/ua101_framering.c
$(BUILD)/test_midiplugin: $(MIDIPLUGIN_TEST) $(MIDIPLUGIN_HEADERS) src/ua101_service.h \
		src/ua101_midilamp.h src/ua101_midigate.h src/ua101_midiport.h src/ua101_engine.h src/ua101_watch.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DUA101_MIDIPLUGIN_TESTING -o $@ $(MIDIPLUGIN_TEST) \
		-framework CoreMIDI -framework CoreFoundation

$(BUILD)/test_midibundle: tests/test_midibundle.c src/midiplugin/ua101_midiplugin.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_midibundle.c -framework CoreMIDI -framework CoreFoundation

$(BUILD)/ua101stream: src/ua101stream.c src/ua101_stream.c src/ua101_stream.h src/ua101_desc.c src/ua101_desc.h \
		src/ua101_timeline.c src/ua101_timeline.h src/ua101_clock.c src/ua101_clock.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ src/ua101stream.c src/ua101_stream.c src/ua101_desc.c \
		src/ua101_timeline.c src/ua101_clock.c -framework IOKit -framework CoreFoundation

$(BUILD)/ua101settings: src/ua101settings.c src/ua101_control.c src/ua101_control.h src/ua101_desc.c src/ua101_desc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ src/ua101settings.c src/ua101_control.c src/ua101_desc.c \
		-framework IOKit -framework CoreFoundation

$(BUILD)/ua101midi: src/ua101midi.c src/ua101_midi.c src/ua101_midi.h src/ua101_desc.c src/ua101_desc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -o $@ src/ua101midi.c src/ua101_midi.c src/ua101_desc.c \
		-framework IOKit -framework CoreMIDI -framework CoreFoundation

$(BUILD)/ua101wavcheck: src/ua101wavcheck.c src/ua101_stream.c src/ua101_stream.h src/ua101_analysis.c src/ua101_analysis.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101wavcheck.c src/ua101_stream.c src/ua101_analysis.c

# For the jack map: which output reached which input, with no Apple APIs.
$(BUILD)/ua101jackmap: src/ua101jackmap.c src/ua101_stream.c src/ua101_stream.h src/ua101_analysis.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101jackmap.c src/ua101_stream.c

# For the cable test: tone files and frequency measurement, with no Apple APIs.
$(BUILD)/ua101tonerate: src/ua101tonerate.c src/ua101_stream.c src/ua101_stream.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101tonerate.c src/ua101_stream.c

# For the cable test: plays the tone through the Mac's headphone jack and times it.
$(BUILD)/ua101macrate: src/ua101macrate.c src/ua101_stream.c src/ua101_stream.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101macrate.c src/ua101_stream.c -framework CoreAudio -framework CoreFoundation

# The driver's settings and what its last run measured, through Core Audio: reads only, unless
# asked to set one (src/ua101drv.c). Nothing goes to the unit.
drv: $(BUILD)/ua101drv

$(BUILD)/ua101drv: src/ua101drv.c src/ua101_drvclient.c src/ua101_drvclient.h src/ua101_drvset.h \
		src/ua101_shared.h src/ua101_framering.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ua101drv.c src/ua101_drvclient.c -framework CoreAudio -framework CoreFoundation

# A silent check through Core Audio with the driver installed: build/halcheck 30
halcheck: $(BUILD)/halcheck

$(BUILD)/halcheck: tools/halcheck.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< -framework CoreAudio -framework CoreFoundation

clean:
	rm -rf $(BUILD)

# ---- The control panel (SwiftUI on the C engine). Pretend unit only: it has no way to the real one.
# Not part of `make test`, so the C tests don't need the Swift compiler.
SWIFTC = swiftc -target $(ARCH)-apple-macos$(MACOS_MIN)
APP_SWIFT = $(wildcard app/*.swift)
APP_OBJ = $(BUILD)/app-obj
APP_BUNDLE = $(BUILD)/UA-101 Control.app

$(BUILD)/ua101panel: $(APP_SWIFT) app/panel_shim.c app/panel_shim.h app/Bridge.h $(CTL_ENGINE) \
		src/ua101_ctlengine.h src/ua101_ctlmsg.h src/ua101_ctlsim.h src/ua101_control.h \
		src/ua101_ctllink.c src/ua101_ctllink.h src/ua101_link.h \
		src/ua101_drvclient.c src/ua101_drvclient.h src/ua101_drvset.h
	@mkdir -p $(APP_OBJ)
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/panel_shim.o app/panel_shim.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_ctlengine.o src/ua101_ctlengine.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_ctlmsg.o src/ua101_ctlmsg.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_ctlbuild.o src/ua101_ctlbuild.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_ctlsim.o src/ua101_ctlsim.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_control.o src/ua101_control.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_ctllink.o src/ua101_ctllink.c
	$(CC) $(CFLAGS) -c -o $(APP_OBJ)/ua101_drvclient.o src/ua101_drvclient.c
	$(SWIFTC) -swift-version 5 -O -import-objc-header app/Bridge.h -o $@ $(APP_SWIFT) $(APP_OBJ)/*.o \
		-framework CoreAudio

# ---- The user manual: manual/pages → a help book and a PDF, both copied into
# the app by `app` and `user-app`. build.py checks first that every menu and button name of the
# end-user app is in it and no old name is; makepdf prints the pages to an A4 PDF. The pictures
# are the end-user ones (`make user-pictures`), never taken from the real unit.
MANUAL = $(BUILD)/manual
MANUAL_DEPS = $(wildcard manual/pages/*.html) manual/style.css manual/build.py \
		app/PanelApp.swift app/MixerViews.swift app/Theme.swift $(wildcard manual/pictures/*.png)

$(MANUAL)/makepdf: manual/makepdf.swift
	@mkdir -p $(MANUAL)
	$(SWIFTC) -O -o $@ manual/makepdf.swift

$(MANUAL)/.built: $(MANUAL_DEPS) $(MANUAL)/makepdf
	python3 manual/build.py $(MANUAL)
	$(MANUAL)/makepdf $(MANUAL)/print/manual.html "$(MANUAL)/UA-101 Control Manual.pdf"
	@touch $@

manual: $(MANUAL)/.built

app: $(BUILD)/ua101panel $(MANUAL)/.built
	@mkdir -p "$(APP_BUNDLE)/Contents/MacOS"
	@# A running copy of the app keeps its file: never write over the binary in place (macOS then
	@# kills the next start with "Code Signature Invalid"); remove it first so the copy is a new file.
	rm -f "$(APP_BUNDLE)/Contents/MacOS/ua101panel"
	cp $(BUILD)/ua101panel "$(APP_BUNDLE)/Contents/MacOS/ua101panel"
	cp app/Info.plist "$(APP_BUNDLE)/Contents/Info.plist"
	@mkdir -p "$(APP_BUNDLE)/Contents/Resources"
	cp app/AppIcon.icns "$(APP_BUNDLE)/Contents/Resources/AppIcon.icns"
	rm -rf "$(APP_BUNDLE)/Contents/Resources/UA-101 Control.help"
	cp -R "$(MANUAL)/UA-101 Control.help" "$(APP_BUNDLE)/Contents/Resources/"
	cp "$(MANUAL)/UA-101 Control Manual.pdf" "$(APP_BUNDLE)/Contents/Resources/"

app-test: $(BUILD)/ua101panel
	./$(BUILD)/ua101panel --selftest

app-pictures: $(BUILD)/ua101panel
	./$(BUILD)/ua101panel --render $(BUILD)/pictures

# ---- The end-user build: the same sources with one switch, -D UA101_USER
# (app/Build.swift): no pretend unit, no exact-value items, plain words on screen, no
# --selftest and no --render; the record keeps its full detail. The self-test, the pictures'
# bench and the stand-in helper are not compiled in. `user-pictures` draws the end-user
# windows with a stand-in unit, for the user and the manual; it is never handed out.
USER_BUILD = $(BUILD)/user
USER_BUNDLE = $(USER_BUILD)/UA-101 Control.app
USER_SWIFT = $(filter-out app/SelfTest.swift app/Render.swift app/FakeHelper.swift,$(APP_SWIFT))
PICTURES_BUILD = $(BUILD)/user-pictures
PICTURES_SWIFT = $(filter-out app/SelfTest.swift,$(APP_SWIFT))
PANEL_C = app/panel_shim.c src/ua101_ctlengine.c src/ua101_ctlmsg.c src/ua101_ctlbuild.c src/ua101_ctlsim.c \
		src/ua101_control.c src/ua101_ctllink.c src/ua101_drvclient.c
PANEL_DEPS = $(APP_SWIFT) $(PANEL_C) app/panel_shim.h app/Bridge.h \
		src/ua101_ctlengine.h src/ua101_ctlmsg.h src/ua101_ctlsim.h src/ua101_control.h \
		src/ua101_ctllink.h src/ua101_link.h src/ua101_drvclient.h src/ua101_drvset.h

$(USER_BUILD)/ua101panel: $(PANEL_DEPS)
	@mkdir -p $(USER_BUILD)
	$(SWIFTC) -swift-version 5 -O -D UA101_USER -import-objc-header app/Bridge.h -o $@ $(USER_SWIFT) \
		$(patsubst %,$(USER_BUILD)/%.o,$(notdir $(basename $(PANEL_C)))) -framework CoreAudio -Xlinker -dead_strip

$(USER_BUILD)/ua101panel: $(patsubst %,$(USER_BUILD)/%.o,$(notdir $(basename $(PANEL_C))))

$(USER_BUILD)/%.o: src/%.c
	@mkdir -p $(USER_BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(USER_BUILD)/panel_shim.o: app/panel_shim.c app/panel_shim.h
	@mkdir -p $(USER_BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

user-app: $(USER_BUILD)/ua101panel $(MANUAL)/.built
	@mkdir -p "$(USER_BUNDLE)/Contents/MacOS" "$(USER_BUNDLE)/Contents/Resources"
	rm -f "$(USER_BUNDLE)/Contents/MacOS/ua101panel"
	cp $(USER_BUILD)/ua101panel "$(USER_BUNDLE)/Contents/MacOS/ua101panel"
	cp app/Info.plist "$(USER_BUNDLE)/Contents/Info.plist"
	cp app/AppIcon.icns "$(USER_BUNDLE)/Contents/Resources/AppIcon.icns"
	rm -rf "$(USER_BUNDLE)/Contents/Resources/UA-101 Control.help"
	cp -R "$(MANUAL)/UA-101 Control.help" "$(USER_BUNDLE)/Contents/Resources/"
	cp "$(MANUAL)/UA-101 Control Manual.pdf" "$(USER_BUNDLE)/Contents/Resources/"

$(PICTURES_BUILD)/ua101panel: $(PANEL_DEPS) $(patsubst %,$(USER_BUILD)/%.o,$(notdir $(basename $(PANEL_C))))
	@mkdir -p $(PICTURES_BUILD)
	$(SWIFTC) -swift-version 5 -O -D UA101_USER -D UA101_PICTURES -import-objc-header app/Bridge.h -o $@ $(PICTURES_SWIFT) \
		$(patsubst %,$(USER_BUILD)/%.o,$(notdir $(basename $(PANEL_C)))) -framework CoreAudio

user-pictures: $(PICTURES_BUILD)/ua101panel
	./$(PICTURES_BUILD)/ua101panel --render manual/pictures

# ---- The installer package: "build/UA-101 Driver.pkg", for any Mac with Apple
# silicon and macOS $(MACOS_MIN) or later. It holds the helper and its launchd file, both
# plug-ins, the MIDI agent (kept as the way back, not started), the uninstaller and the end-user
# app; its scripts do what install/install.sh does. Built only, never installed by make.
# Option A (a design choice, 2026-11-22): ad hoc signatures, the package unsigned. Option B
# later is one step: give CODESIGN_ID, INSTALLER_ID and NOTARY_PROFILE (install/pkg/build-pkg.sh).
APP_VERSION := $(shell /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' app/Info.plist)
LINK_VERSION := $(shell awk '/define UA101_LINK_VERSION/ {print $$3}' src/ua101_link.h)
PKG_VERSION = $(APP_VERSION).$(LINK_VERSION)

pkg: helper plugin midiplugin agent user-app
	CODESIGN_ID="$(CODESIGN_ID)" INSTALLER_ID="$(INSTALLER_ID)" NOTARY_PROFILE="$(NOTARY_PROFILE)" \
		sh install/pkg/build-pkg.sh $(BUILD) $(MACOS_MIN) $(PKG_VERSION)
