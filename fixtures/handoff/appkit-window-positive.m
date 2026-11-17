#include <stdbool.h>
#include <unistd.h>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <stdio.h>
#include <stdlib.h>

// Headless positive control for an offscreen AppKit window. Build with:
// xcrun clang -arch arm64 -fobjc-arc -Wall -Wextra -Werror \
//   -Wno-deprecated-declarations -framework AppKit -framework CoreGraphics \
//   fixtures/handoff/appkit-window-positive.m -o appkit-window-positive

@interface AcceptanceView : NSView
@end

@implementation AcceptanceView
- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;
  [[NSColor colorWithCalibratedRed:0.10 green:0.20 blue:0.30 alpha:1.0] setFill];
  NSRectFill(self.bounds);
  [[NSColor colorWithCalibratedRed:0.90 green:0.10 blue:0.20 alpha:1.0] setFill];
  NSRectFill(NSMakeRect(8, 8, 24, 24));
}
@end

int main(void) {
  @autoreleasepool {
    NSApplication *application = [NSApplication sharedApplication];
    if (![application setActivationPolicy:NSApplicationActivationPolicyAccessory]) {
      fputs("activation_policy=failed\n", stderr);
      return 2;
    }

    const NSRect frame = NSMakeRect(50000, 50000, 48, 48);
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (window == nil) {
      fputs("window=create_failed\n", stderr);
      return 3;
    }
    [window setReleasedWhenClosed:NO];
    [window setTitle:@"arm64-metal-appkit-positive"];
    [window setContentView:[[AcceptanceView alloc] initWithFrame:NSMakeRect(0, 0, 48, 48)]];
    [window orderFrontRegardless];
    [window displayIfNeeded];

    const CGWindowID windowID = (CGWindowID)[window windowNumber];
    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionAll, kCGNullWindowID);
    bool serverRecord = false;
    if (windows != NULL) {
      for (NSDictionary *entry in (__bridge NSArray *)windows) {
        if ([[entry objectForKey:(__bridge NSString *)kCGWindowNumber] unsignedIntValue] == windowID &&
            [[entry objectForKey:(__bridge NSString *)kCGWindowOwnerPID] intValue] == getpid()) {
          serverRecord = true;
          break;
        }
      }
      CFRelease(windows);
    }
    CGRect desktop = CGRectNull;
    for (NSScreen *screen in NSScreen.screens) {
      desktop = CGRectIsNull(desktop) ? screen.frame : CGRectUnion(desktop, screen.frame);
    }
    const bool offscreen = !CGRectIsNull(desktop) &&
                           !CGRectIntersectsRect(NSRectToCGRect(window.frame), desktop);
    if (windowID == 0 || !serverRecord || !offscreen) {
      fprintf(stderr, "windowserver_record=missing id=%u\n", windowID);
      [window close];
      return 4;
    }

    const NSRect bounds = NSMakeRect(0, 0, 48, 48);
    NSBitmapImageRep *bitmap = [window.contentView bitmapImageRepForCachingDisplayInRect:bounds];
    if (bitmap == nil) {
      fputs("render=bitmap_failed\n", stderr);
      [window close];
      return 5;
    }
    [window.contentView cacheDisplayInRect:bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const char *pngPath = getenv("APPKIT_RECEIPT_PNG");
    if (png == nil || pngPath == NULL || ![png writeToFile:[NSString stringWithUTF8String:pngPath] atomically:YES]) {
      fputs("render=write_failed\n", stderr);
      [window close];
      return 6;
    }
    const NSColor *background = [bitmap colorAtX:2 y:2];
    const NSColor *accent = [bitmap colorAtX:32 y:32];
    if (background == nil || accent == nil ||
        [background redComponent] < 0.05 || [accent redComponent] < 0.80) {
      fputs("render=pixel_assertion_failed\n", stderr);
      [window close];
      return 7;
    }

    NSEvent *posted = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                         location:NSZeroPoint
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:(NSInteger)windowID
                                          context:nil
                                          subtype:0x4d
                                            data1:0x4150504b
                                            data2:0];
    [application postEvent:posted atStart:YES];
    NSEvent *observed = [application nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
    if (observed == nil || observed.type != NSEventTypeApplicationDefined || observed.subtype != 0x4d) {
      fputs("event=dispatch_failed\n", stderr);
      [window close];
      return 8;
    }

    printf("windowserver_record=present id=%u offscreen=%d activation=accessory\n", windowID, offscreen);
    printf("render=png pixels=verified size=%ldx%ld\n", (long)bitmap.pixelsWide, (long)bitmap.pixelsHigh);
    printf("event=appkit-queue subtype=%d dispatched=1\n", observed.subtype);
    [window orderOut:nil];
    [window close];
    printf("shutdown=closed status=0\n");
    return 0;
  }
}
