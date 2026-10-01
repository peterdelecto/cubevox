#include "open_panel.h"

#import <Cocoa/Cocoa.h>

std::string openFilePanel() {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  panel.allowedFileTypes = @[ @"wav", @"aiff", @"aif", @"mp3" ];

  if ([panel runModal] == NSModalResponseOK) {
    NSURL* url = panel.URLs.firstObject;
    if (url != nil) {
      return std::string(url.path.UTF8String);
    }
  }
  return std::string();
}

std::string chooseDirectoryPanel() {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = NO;
  panel.canChooseDirectories = YES;
  panel.canCreateDirectories = YES;
  panel.allowsMultipleSelection = NO;
  panel.prompt = @"Choose";

  if ([panel runModal] == NSModalResponseOK) {
    NSURL* url = panel.URLs.firstObject;
    if (url != nil) {
      return std::string(url.path.UTF8String);
    }
  }
  return std::string();
}
