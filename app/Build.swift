// Which build this is. One switch, given to the Swift compiler by the Makefile:
//
//   (none)                       the developer build in build/: everything, with the pretend
//                                unit, the exact-value items, the bytes on screen, --selftest,
//                                --render DIR and --quit-after N
//   -D UA101_USER                the end-user build in build/user/: the UA-101 only, plain words
//                                on screen, and only --quit-after N (the build's own check,
//                                which attaches to nothing)
//   -D UA101_USER -D UA101_PICTURES
//                                the end-user screens drawn with a stand-in unit (--render DIR
//                                only), for the user and the manual; never handed out
//
// Whatever the build, the record file keeps its full detail (bytes, times, every change and
// its read-back): the user uses the end-user build every day, and the record is what
// development and QA read (a design choice, 2026-11-21).
enum Build {
    #if UA101_USER
    static let isEndUser = true
    #else
    static let isEndUser = false
    #endif
    #if UA101_PICTURES
    static let name = "end-user (pictures)"
    #elseif UA101_USER
    static let name = "end-user"
    #else
    static let name = "developer"
    #endif
}
