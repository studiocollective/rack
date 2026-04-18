// Objective-C++ helper for dispatching VST3 plugin calls to the main thread.
//
// JUCE-based plugins (Vital, etc.) may create NSWindow during setState,
// which requires the main thread. This helper dispatches the call to the
// main queue when called from a background thread, and catches any
// NSExceptions that the plugin throws.

#import <Foundation/Foundation.h>
#include <cstdio>
#include <dispatch/dispatch.h>

typedef int (*GenericFn)(void* context);

extern "C" int rack_vst3_dispatch_main(GenericFn fn, void* context) {
    __block int result = -1;

    void (^work)(void) = ^{
        @try {
            result = fn(context);
        } @catch (NSException* ex) {
            fprintf(stderr, "[rack] NSException name=%s reason=%s\n",
                    [[ex name] UTF8String],
                    [[ex reason] UTF8String]);
            result = -1;
        }
    };

    if ([NSThread isMainThread]) {
        work();
    } else {
        dispatch_sync(dispatch_get_main_queue(), work);
    }

    return result;
}
