/*
 * The built UA101MIDI.plugin, as the MIDI server finds it: CFPlugIn reads its Info.plist and
 * registers one factory for the MIDI driver type, named UA101MIDI_Create; the bundle carries the
 * device's picture. The Makefile checks that the executable exports that name and nothing else.
 *
 * As tests/test_bundle.c, it doesn't load the code (an un-notarized bundle loaded by a program
 * that isn't a developer tool makes Gatekeeper ask on screen). The code itself is tested linked
 * in (tests/test_midiplugin.c). Touches no unit and no MIDI server.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/MIDIDriver.h>
#include <stdio.h>
#include <string.h>

#include "../src/midiplugin/ua101_midiplugin.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/UA101MIDI.plugin";
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), true);
    CFPlugInRef plugin = url != NULL ? CFPlugInCreate(NULL, url) : NULL;
    CFBundleRef bundle;
    CFArrayRef factories;
    CFUUIDRef want = CFUUIDCreateFromString(NULL, CFSTR(UA101_MIDIPLUGIN_FACTORY));
    CFDictionaryRef named;
    CFStringRef function = NULL, id;
    CFURLRef picture;
    char text[128] = "";
    int ok = 1;

    if (plugin == NULL) {
        printf("FAIL: %s isn't a plug-in bundle\n", path);
        return 1;
    }
    bundle = CFPlugInGetBundle(plugin);
    if (CFBundleIsExecutableLoaded(bundle)) {
        printf("FAIL: registering loaded the code\n");
        ok = 0;
    }
    factories = CFPlugInFindFactoriesForPlugInType(kMIDIDriverTypeID);
    if (factories == NULL || CFArrayGetCount(factories) != 1 ||
        !CFEqual(CFArrayGetValueAtIndex(factories, 0), want)) {
        printf("FAIL: want factory %s, and only it, for the MIDI driver type\n",
               UA101_MIDIPLUGIN_FACTORY);
        ok = 0;
    }
    named = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFPlugInFactories"));
    if (named != NULL && CFGetTypeID(named) == CFDictionaryGetTypeID())
        function = CFDictionaryGetValue(named, CFSTR(UA101_MIDIPLUGIN_FACTORY));
    if (function == NULL ||
        !CFStringGetCString(function, text, sizeof(text), kCFStringEncodingUTF8) ||
        strcmp(text, "UA101MIDI_Create") != 0) {
        printf("FAIL: the factory function is \"%s\", want UA101MIDI_Create\n", text);
        ok = 0;
    }
    id = CFBundleGetIdentifier(bundle);
    if (id == NULL || !CFEqual(id, CFSTR(UA101_MIDIPLUGIN_BUNDLE_ID))) {
        printf("FAIL: the bundle's identifier must be %s: the code finds its picture by it\n",
               UA101_MIDIPLUGIN_BUNDLE_ID);
        ok = 0;
    }
    picture = CFBundleCopyResourceURL(bundle, CFSTR("UA-101"), CFSTR("png"), NULL);
    if (picture == NULL) {
        printf("FAIL: the device's picture UA-101.png isn't in the bundle\n");
        ok = 0;
    } else {
        CFRelease(picture);
    }
    if (ok)
        printf("test_midibundle: one factory, UA101MIDI_Create, for the MIDI driver type; "
               "the picture is there\n");
    if (factories != NULL)
        CFRelease(factories);
    CFRelease(want);
    return ok ? 0 : 1;
}
