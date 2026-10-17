// Small decisions the sketch makes every pass, kept pure so the host test covers them.

#pragma once

// Decides when a changed layout, setting or panel is handed to the audio interrupt. Before every pot
// has been read the knob values are zero, so a publish waits and stays requested.
class PublishGate {
 public:
  // A publish is needed even though no knob moved (new layout, bench override).
  void markStale() { stale_ = true; }

  // True when the caller should publish now. `panelSeeded` is whether every pot has been read;
  // `changed` is whether the panel or a setting changed this pass.
  bool take(bool panelSeeded, bool changed) {
    if (!changed && !stale_) return false;
    stale_ = !panelSeeded;
    return panelSeeded;
  }

 private:
  bool stale_ = false;
};

// Tells the menu to repaint when something else that owned the canvas lets go of it.
class CanvasHandoff {
 public:
  // `otherOwns` is whether the other owner holds the canvas this pass. True once, on the first
  // pass after it lets go.
  bool update(bool otherOwns) {
    const bool released = wasOwned_ && !otherOwns;
    wasOwned_ = otherOwns;
    return released;
  }

 private:
  bool wasOwned_ = false;
};
