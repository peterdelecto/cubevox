// Checks layouts and snapshot composition on the host. Covers the default layout, validity,
// recovery, settings, UI actions, toggles, bench overrides and output gain.

#include "support.h"

#include <cmath>
#include <cstring>

#include "core/compose.h"
#include "core/costs.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"
#include "ui/actions.h"
#include "ui/menu.h"

namespace {

void checkDefaultLayout() {
  const Card expected[kSlotCount] = {Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
                                     Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring};
  int reverbs = 0;
  const Layout layout = defaultLayout();
  for (int s = 0; s < kSlotCount; ++s) {
    check(layout.card[s] == expected[s], "default layout slot");
    const CardDesc& d = cardDesc(layout.card[s]);
    if (d.group == Group::Reverb) ++reverbs;
    // Autotune's row is the board's pair figure scaled by the CPU pass, an estimate until a board
    // bench of the pair.
    check(d.cost.measured || layout.card[s] == Card::Autotune, "a default layout card's cost comes from the board");
  }
  const LayoutCost sums = layoutCost(layout);
  check(sums.avg <= costs::kBudgetAvgPercent, "default layout within the average line");
  check(sums.worst <= costs::kBudgetWorstPercent, "default layout within the worst-block line");
  check(costs::kAutotune.avg + costs::kTracker.avg == 9 && costs::kAutotune.worst + costs::kTracker.worst == 11,
        "Autotune's row and the tracker add to the pair's estimate, 9 % mean and 11 % worst");
  check(reverbs == 1, "default layout has one reverb");
  check(cardDesc(Card::Empty).cost.avg == 0 && cardDesc(Card::Empty).cost.worst == 0, "empty card costs nothing");
  for (int i = 0; i < kCardCount; ++i) {
    const CardDesc& d = cardDesc(card(i));
    check(d.cost.worst >= d.cost.avg, "worst block is never under the mean");
  }
}

// The default layout with the Crowd card in slot 4 in place of Unison is accepted. Its sums come
// from the card rows (fixed term, the seven cards that stay, Crowd, and the tracker once), so a row
// that moves the layout past a line makes this check fail instead of passing on a recomputed verdict.
void checkCrowdLayout() {
  Layout layout = defaultLayout();
  layout.card[3] = Card::Crowd;
  const costs::Cost rows[] = {
      costs::kFixed,    costs::kInputGate,  costs::kAutotune,    costs::kOctave,  costs::kCrowd,
      costs::kSlapback, costs::kDistortion, costs::kReverbSpring, costs::kGate,   costs::kTracker,
  };
  int avg = 0;
  int worst = 0;
  for (const costs::Cost& row : rows) {
    avg += row.avg;
    worst += row.worst;
  }
  const LayoutCost sums = layoutCost(layout);
  check(sums.avg == avg && sums.worst == worst, "crowd layout: slot 4 with Unison out sums to its rows");
  check(avg <= costs::kBudgetAvgPercent && worst <= costs::kBudgetWorstPercent,
        "crowd layout: the row sums sit within both lines");
  const LayoutCheck verdict = layoutValid(layout);
  check(verdict.ok(), "crowd layout: slot 4 with Unison out is accepted");
}

// The pitch tracker's row counts once: with Autotune alone, with Crowd alone, and with both. A layout
// holding neither, and one holding only Octave, pays none.
void checkTrackerCost() {
  Layout noPitch = defaultLayout();
  noPitch.card[1] = Card::Empty;
  Layout autotune = noPitch;
  autotune.card[1] = Card::Autotune;
  Layout crowd = noPitch;
  crowd.card[1] = Card::Crowd;
  Layout both = autotune;
  both.card[3] = Card::Crowd;
  int cards = costs::kFixed.avg;
  for (const Card c : noPitch.card) cards += cardDesc(c).cost.avg;
  const int base = layoutCost(noPitch).avg;
  check(base == cards, "tracker cost: Octave without Autotune or Crowd pays none");
  check(layoutCost(autotune).avg == base + costs::kAutotune.avg + costs::kTracker.avg, "tracker cost: Autotune pays it");
  check(layoutCost(crowd).avg == base + costs::kCrowd.avg + costs::kTracker.avg, "tracker cost: Crowd without Autotune pays it");
  check(layoutCost(both).avg == layoutCost(autotune).avg - costs::kUnison.avg + costs::kCrowd.avg,
        "tracker cost: Autotune and Crowd together pay it once");
  check(layoutCost(autotune).worst == layoutCost(noPitch).worst + costs::kAutotune.worst + costs::kTracker.worst,
        "tracker cost: the worst column counts it too");
}

void checkLayoutValid() {
  check(layoutValid(defaultLayout()).ok(), "default layout is valid");
  char why[96];
  Layout bad = defaultLayout();
  bad.card[3] = static_cast<Card>(kCardCount);
  LayoutCheck c = layoutValid(bad);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && std::strcmp(why, "card id out of range in slot 4") == 0, "out-of-range card id is rejected");
  Layout twoReverbs = defaultLayout();
  twoReverbs.card[0] = Card::ReverbChasm;
  c = layoutValid(twoReverbs);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && c.error == LayoutError::TwoReverbs && std::strcmp(why, "two reverb cards") == 0,
        "second reverb card is rejected");
  Layout noReverb = defaultLayout();
  noReverb.card[7] = Card::Empty;
  c = layoutValid(noReverb);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && c.error == LayoutError::NoReverb && std::strcmp(why, "no reverb card") == 0,
        "a layout with no reverb card is rejected");
  Layout allEmpty;
  allEmpty.card.fill(Card::Empty);
  check(layoutValid(allEmpty).error == LayoutError::NoReverb, "an all-Empty layout is rejected for its missing reverb card");
  Layout chasm = defaultLayout();
  chasm.card[7] = Card::ReverbChasm;
  check(layoutValid(chasm).ok(), "a layout with Chasm as its one reverb card is accepted");
  Layout movedReverb = defaultLayout();
  movedReverb.card[0] = Card::ReverbSpring;
  movedReverb.card[7] = Card::Empty;
  check(layoutValid(movedReverb).ok(), "the one reverb card may sit in any slot");
  Layout twice = defaultLayout();
  twice.card[7] = twice.card[4];
  c = layoutValid(twice);
  layoutReason(c, why, sizeof why);
  check(!c.ok() && std::strcmp(why, "same card twice: slapback") == 0, "a card placed twice is rejected");
  Layout gaps = defaultLayout();
  gaps.card[0] = Card::Empty;
  gaps.card[1] = Card::Empty;
  check(layoutValid(gaps).ok(), "several Empty slots are accepted");
}

void checkLayoutReasons() {
  char why[96] = "stale";
  layoutReason(LayoutCheck(), why, sizeof why);
  check(why[0] == '\0', "reason: an ok check gives an empty string");
  LayoutCheck over;
  over.error = LayoutError::OverWorst;
  over.percent = 97;
  layoutReason(over, why, sizeof why);
  check(std::strcmp(why, "over CPU budget (worst 97 %)") == 0, "budget reason names the worst line");
  over.error = LayoutError::OverAvg;
  over.percent = 80;
  layoutReason(over, why, sizeof why);
  check(std::strcmp(why, "over CPU budget (mean 80 %)") == 0, "budget reason names the mean line");
  layoutReason(over, why, 0);  // no room: must not write
  check(std::strcmp(why, "over CPU budget (mean 80 %)") == 0, "reason: a zero-size buffer is left alone");
}

void checkLayoutName() {
  check(std::strcmp(layoutDisplayName(""), "Default") == 0, "name: an empty stored name shows Default");
  check(std::strcmp(layoutDisplayName(nullptr), "Default") == 0, "name: a missing stored name shows Default");
  check(std::strcmp(layoutDisplayName("Stage rig"), "Stage rig") == 0, "name: a stored name shows as is");
}

PanelReading panelWithToggles(bool on) {
  PanelReading r;
  r.toggle.fill(on);
  return r;
}

void checkComposeToggles() {
  const Layout layout = defaultLayout();
  const ChainSnapshot off = composeSnapshot(layout, panelWithToggles(false), Settings(), BenchOverrides());
  check(!off.params.distortion.on && !off.params.unison.on && !off.params.reverb.on && !off.params.gate.on &&
            !off.params.pitchFx.autotune.on && !off.params.pitchFx.octave.on,
        "toggles off leave every stage off");
  const ChainSnapshot on = composeSnapshot(layout, panelWithToggles(true), Settings(), BenchOverrides());
  check(on.params.distortion.on && on.params.unison.on && on.params.reverb.on && on.params.gate.on &&
            on.params.pitchFx.autotune.on && on.params.pitchFx.octave.on,
        "toggles on turn every stage on");
  check(on.layout.card == layout.card, "snapshot carries the layout");
}

// The Crowd card's toggle turns on its stage and the pitch stage's feed, and its knobs reach the stage.
void checkComposeCrowd() {
  Layout layout = defaultLayout();
  layout.card[3] = Card::Crowd;
  PanelReading panel = panelWithToggles(false);
  panel.knob[3] = {0.25f, 0.75f};
  ChainSnapshot snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(!snap.params.crowd.on && !snap.params.pitchFx.crowdOn, "compose: Crowd card off leaves both flags off");
  check(std::fabs(snap.params.crowd.width - 0.25f) < 1e-6f && std::fabs(snap.params.crowd.voices - 0.75f) < 1e-6f,
        "compose: Crowd knob A is WIDTH and knob B is VOICES");
  panel.toggle[3] = true;
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.crowd.on && snap.params.pitchFx.crowdOn && !snap.params.pitchFx.autotune.on &&
            !snap.params.pitchFx.octave.on,
        "compose: Crowd card on sets its flag and the pitch stage's, and no other pitch toggle");
  check(snap.slotOn[3], "compose: the Crowd card's slot reads on");
}

void checkComposeBench() {
  const Layout layout = defaultLayout();  // slot 6 (index 5) is Distortion
  BenchOverrides bench;
  bench.slotsOff = 1u << 5;
  ChainSnapshot snap = composeSnapshot(layout, panelWithToggles(true), Settings(), bench);
  check(!snap.params.distortion.on && snap.params.unison.on, "slotsOff bit forces only that slot off");

  bench = BenchOverrides();
  bench.reverbChasm = true;
  snap = composeSnapshot(layout, panelWithToggles(true), Settings(), bench);
  check(snap.params.reverb.engine == cv::kReverbChasm, "reverbChasm selects the chasm engine");

  // Octave semitones (stepped, slot index 2) and Unison depth (continuous, index 3) read the top position.
  PanelReading low;
  bench = BenchOverrides();
  bench.knobsPinned = true;
  const ChainSnapshot pinned = composeSnapshot(layout, low, Settings(), bench);
  ChainParams expect;
  applyKnob(Card::Octave, 0, 24.0f, expect);
  applyKnob(Card::Unison, 1, 1.0f, expect);
  check(pinned.params.pitchFx.octave.semitones == expect.pitchFx.octave.semitones &&
            pinned.params.pitchFx.octave.semitones == 12,
        "pinned stepped knob sits on its last step");
  check(std::fabs(pinned.params.unison.depth - expect.unison.depth) < 1e-6f, "pinned continuous knob sits at 1.0");
  const ChainSnapshot live = composeSnapshot(layout, low, Settings(), BenchOverrides());
  check(live.params.pitchFx.octave.semitones == -12, "unpinned stepped knob follows the panel");
}

void checkOutputGain() {
  Settings s;
  const BenchOverrides none;
  check(composeSnapshot(defaultLayout(), PanelReading(), s, none).outputGain == 1.0f, "LINE gain is unity");
  s.outputLevel = OutputLevel::Mic;
  const float g = composeSnapshot(defaultLayout(), PanelReading(), s, none).outputGain;
  check(std::fabs(g - 0.0316f) < 1e-3f, "MIC gain is -30 dB linear");
}

void checkSettings() {
  const Settings s;
  check(s.inputJack == InputJack::Mic, "input jack defaults to MIC");
  check(s.outputLevel == OutputLevel::Line, "output defaults to LINE");
  check(outputTrimDb(s) == 0.0f, "LINE output trim is 0 dB");
  Settings mic;
  mic.outputLevel = OutputLevel::Mic;
  check(std::fabs(outputTrimDb(mic) - (-30.0f)) < 1e-6f, "MIC output trim is -30 dB");
}

void checkApplyAction() {
  Settings s;
  Layout layout = defaultLayout();
  check(ui::applyAction(UiAction::ToggleOutput, s, layout) == ui::ApplyResult::SettingsChanged &&
            s.outputLevel == OutputLevel::Mic,
        "action: ToggleOutput flips LINE to MIC");
  check(ui::applyAction(UiAction::ToggleOutput, s, layout) == ui::ApplyResult::SettingsChanged &&
            s.outputLevel == OutputLevel::Line,
        "action: ToggleOutput flips back");
  check(ui::applyAction(UiAction::ToggleInputJack, s, layout) == ui::ApplyResult::SettingsChanged &&
            s.inputJack == InputJack::Line,
        "action: ToggleInputJack flips MIC to LINE");
  check(ui::applyAction(UiAction::ToggleInputJack, s, layout) == ui::ApplyResult::SettingsChanged &&
            s.inputJack == InputJack::Mic,
        "action: ToggleInputJack flips back");
  check(ui::applyAction(UiAction::ToggleDebug, s, layout) == ui::ApplyResult::SettingsChanged && s.debug,
        "action: ToggleDebug turns DEBUG on");
  check(ui::applyAction(UiAction::ToggleDebug, s, layout) == ui::ApplyResult::SettingsChanged && !s.debug,
        "action: ToggleDebug turns it off again");
  const Settings before = s;
  check(ui::applyAction(UiAction::EnterDfu, s, layout) == ui::ApplyResult::EnterDfu && s.outputLevel == before.outputLevel &&
            s.inputJack == before.inputJack,
        "action: EnterDfu asks for the reset and changes nothing");
  check(ui::applyAction(UiAction::None, s, layout) == ui::ApplyResult::None && s.inputJack == before.inputJack,
        "action: None does nothing");
  check(layout == defaultLayout(), "action: the setting actions leave the layout alone");

  check(ui::applyAction(UiAction::SwapReverb, s, layout) == ui::ApplyResult::LayoutChanged &&
            layout.card[7] == Card::ReverbChasm && s.outputLevel == before.outputLevel && s.debug == before.debug,
        "action: SwapReverb changes the layout and no setting");
}

void checkReverbSlot() {
  Layout layout = defaultLayout();
  check(reverbSlot(layout) == 7, "reverbSlot: the default layout has its reverb card in slot 8");
  layout.card[7] = Card::ReverbChasm;
  check(reverbSlot(layout) == 7, "reverbSlot: Chasm counts as a reverb card");
  layout.card[7] = Card::Empty;
  check(reverbSlot(layout) == -1, "reverbSlot: -1 when no slot holds one");
  layout.card[2] = Card::ReverbChasm;
  check(reverbSlot(layout) == 2, "reverbSlot: the slot that holds it, wherever it is");
}

void checkCrowdTestLayout() {
  const Layout base = defaultLayout();
  const Layout test = crowdTestLayout();
  check(test.card[1] == Card::Crowd, "crowd test layout: Crowd sits in slot 2");
  for (int s = 0; s < kSlotCount; ++s) {
    if (s != 1) check(test.card[s] == base.card[s], "crowd test layout: every other slot is the default");
  }
}

void checkRecoverLayout() {
  Layout good = defaultLayout();
  good.card[0] = Card::Empty;
  Layout kept = good;
  const Recovery ok = recoverLayout(kept);
  check(!ok.replaced && ok.why.ok() && kept == good, "recover: a valid layout is kept as it is");

  Layout bad = defaultLayout();
  bad.card[0] = Card::ReverbChasm;
  const Recovery twoReverbs = recoverLayout(bad);
  check(twoReverbs.replaced && twoReverbs.why.error == LayoutError::TwoReverbs && bad == defaultLayout(),
        "recover: an invalid layout becomes the whole default and keeps the reason");

  Layout outOfRange = defaultLayout();
  outOfRange.card[5] = static_cast<Card>(kCardCount);
  const Recovery badCard = recoverLayout(outOfRange);
  check(badCard.replaced && badCard.why.error == LayoutError::BadCard && badCard.why.slot == 5 &&
            outOfRange == defaultLayout(),
        "recover: a bad card id is replaced and names its slot");

  Layout noReverb = defaultLayout();
  noReverb.card[7] = Card::Empty;
  const Recovery missing = recoverLayout(noReverb);
  check(missing.replaced && missing.why.error == LayoutError::NoReverb && noReverb == defaultLayout(),
        "recover: a layout with no reverb card becomes the default");
}

}  // namespace

void checkLayoutAndCompose() {
  checkDefaultLayout();
  checkCrowdTestLayout();
  checkRecoverLayout();
  checkSettings();
  checkLayoutValid();
  checkCrowdLayout();
  checkTrackerCost();
  checkLayoutReasons();
  checkLayoutName();
  checkApplyAction();
  checkReverbSlot();
  checkComposeToggles();
  checkComposeCrowd();
  checkComposeBench();
  checkOutputGain();
}
