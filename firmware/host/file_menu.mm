#include "file_menu.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <utility>

namespace {

std::function<void(const char*)> gOnOpen;

}  // namespace

@interface CubevoxFileMenuTarget : NSObject
- (void)openClip:(id)sender;
@end

@implementation CubevoxFileMenuTarget

- (void)openClip:(id)sender {
  (void)sender;
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  // What the decoder takes (miniaudio: wav and mp3); an offered type that failed to decode would
  // leave the old clip playing with nothing to say why.
  for (NSString* extension in @[ @"wav", @"mp3" ]) {
    UTType* type = [UTType typeWithFilenameExtension:extension];
    if (type != nil) [types addObject:type];
  }
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  panel.allowedContentTypes = types;
  if ([panel runModal] != NSModalResponseOK) return;
  NSURL* url = panel.URLs.firstObject;
  if (url == nil || !gOnOpen) return;
  gOnOpen(url.path.UTF8String);
}

@end

namespace host {

void installFileMenu(std::function<void(const char*)> onOpen) {
  NSMenu* mainMenu = NSApp.mainMenu;
  if (mainMenu == nil) return;
  gOnOpen = std::move(onOpen);

  static CubevoxFileMenuTarget* target = [[CubevoxFileMenuTarget alloc] init];
  NSMenuItem* open = [[NSMenuItem alloc] initWithTitle:@"Open…" action:@selector(openClip:) keyEquivalent:@"o"];
  open.target = target;  // the key equivalent defaults to Command
  NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
  [fileMenu addItem:open];
  NSMenuItem* fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
  fileItem.submenu = fileMenu;
  [mainMenu insertItem:fileItem atIndex:1];
}

}  // namespace host
