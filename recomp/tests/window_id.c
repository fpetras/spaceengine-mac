// Prints the on-screen window numbers that belong to a process, for use with
// `screencapture -l <number>` when checking what a window really shows.
// With a second argument, lists every window of the process, on screen or not.
// usage: window_id <pid> [all]
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int pid = atoi(argv[1]);
    CFArrayRef list = CGWindowListCopyWindowInfo(argc > 2 ? kCGWindowListOptionAll : kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef w = CFArrayGetValueAtIndex(list, i);
        int owner = 0, number = 0;
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowOwnerPID), kCFNumberIntType, &owner);
        CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowNumber), kCFNumberIntType, &number);
        if (owner != pid) continue;
        if (argc > 2) {
            CGRect r = CGRectZero;
            int on = 0, layer = 0;
            CFBooleanRef onscreen = CFDictionaryGetValue(w, kCGWindowIsOnscreen);
            if (onscreen) on = CFBooleanGetValue(onscreen);
            CFNumberGetValue(CFDictionaryGetValue(w, kCGWindowLayer), kCFNumberIntType, &layer);
            CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(w, kCGWindowBounds), &r);
            printf("%d onscreen=%d layer=%d %.0fx%.0f at %.0f,%.0f\n", number, on, layer, r.size.width, r.size.height, r.origin.x, r.origin.y);
        } else printf("%d\n", number);
    }
    return 0;
}
