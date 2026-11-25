/*
 * The built UA101.driver, as Core Audio finds it: CFPlugIn reads its Info.plist and registers
 * one factory for the AudioServerPlugIn type, named UA101_Create. The Makefile checks that the
 * executable exports that name and nothing else.
 *
 * It doesn't load the code: an un-notarized bundle loaded by a program that isn't a developer
 * tool makes Gatekeeper ask on screen, and the load waits for the answer. The code itself is
 * tested linked in (tests/test_plugin.c). Touches no unit.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

#define FACTORY "4A228EEA-3A5E-4827-952C-54F49808C38A"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/UA101.driver";
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), true);
    CFPlugInRef plugin = url != NULL ? CFPlugInCreate(NULL, url) : NULL;
    CFBundleRef bundle;
    CFArrayRef factories;
    CFUUIDRef want = CFUUIDCreateFromString(NULL, CFSTR(FACTORY));
    CFDictionaryRef named;
    CFStringRef function = NULL;
    CFArrayRef services;
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
    factories = CFPlugInFindFactoriesForPlugInType(kAudioServerPlugInTypeUUID);
    if (factories == NULL || CFArrayGetCount(factories) != 1 ||
        !CFEqual(CFArrayGetValueAtIndex(factories, 0), want)) {
        printf("FAIL: want factory %s, and only it, for the AudioServerPlugIn type\n", FACTORY);
        ok = 0;
    }
    named = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFPlugInFactories"));
    if (named != NULL && CFGetTypeID(named) == CFDictionaryGetTypeID())
        function = CFDictionaryGetValue(named, CFSTR(FACTORY));
    if (function == NULL || !CFStringGetCString(function, text, sizeof(text), kCFStringEncodingUTF8) ||
        strcmp(text, "UA101_Create") != 0) {
        printf("FAIL: the factory function is \"%s\", want UA101_Create\n", text);
        ok = 0;
    }
    services = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioServerPlugIn_MachServices"));
    if (services == NULL || CFGetTypeID(services) != CFArrayGetTypeID() ||
        CFArrayGetCount(services) != 1 ||
        !CFEqual(CFArrayGetValueAtIndex(services, 0), CFSTR("io.github.delta90h.ua101d"))) {
        printf("FAIL: the sandbox must allow the helper's Mach service\n");
        ok = 0;
    }
    if (ok)
        printf("test_bundle: one factory, UA101_Create, for the AudioServerPlugIn type; "
               "may reach the helper\n");
    if (factories != NULL)
        CFRelease(factories);
    CFRelease(want);
    return ok ? 0 : 1;
}
