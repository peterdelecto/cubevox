// Checks layouts and snapshot composition on the host. Covers the default layout, validity,
// recovery, settings, UI actions, toggles, bench overrides and output gain.

#include "support.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>

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
    // Autotune's row is the board's pair figure scaled by the host benches of the CPU pass and the
    // Autotune fix pass, with one extra worst point from the stress run, so it stays an estimate until
    // a board bench of the pair. Octave's row is the host bench of its rewrite; the stress run reads it
    // only together with the pitch stage. Distortion's row is the llvm-mca model of its stage-major
    // rewrite until a stress run with the Distortion sweeps reads it on the board. The Input gate's and
    // the Gate's rows are the model figures after the gate fix pass, and wait for the same run.
    check(d.cost.measured || layout.card[s] == Card::Autotune || layout.card[s] == Card::Octave ||
              layout.card[s] == Card::Distortion || layout.card[s] == Card::InputGate ||
              layout.card[s] == Card::Gate,
          "a default layout card's cost comes from the board");
  }
  const LayoutCost sums = layoutCost(layout);
  check(sums.avg <= costs::kBudgetAvgPercent, "default layout within the average line");
  check(sums.worst <= costs::kBudgetWorstPercent, "default layout within the worst-block line");
  check(costs::kAutotune.avg + costs::kTracker.avg == 7 && costs::kAutotune.worst + costs::kTracker.worst == 10,
        "Autotune's row and the tracker add to the pair's figure, 7 % mean and 10 % worst");
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

// The default layout with slot 2 empty, which holds no Autotune: the base of the tracker cases.
Layout withoutAutotune() {
  Layout layout = defaultLayout();
  layout.card[1] = Card::Empty;
  return layout;
}

// The pitch tracker's row counts once: with Autotune alone, with Crowd alone, and with both. A layout
// holding neither, and one holding only Octave, pays none. Octave without Autotune pays kPitchStage
// instead (checkPitchStageCost), so the layouts below that keep Octave in slot 3 without Autotune
// carry that term as well.
void checkTrackerCost() {
  const Layout noPitch = withoutAutotune();
  Layout autotune = noPitch;
  autotune.card[1] = Card::Autotune;
  Layout crowd = noPitch;
  crowd.card[1] = Card::Crowd;
  Layout both = autotune;
  both.card[3] = Card::Crowd;
  int cards = costs::kFixed.avg;
  for (const Card c : noPitch.card) cards += cardDesc(c).cost.avg;
  const int base = layoutCost(noPitch).avg;
  check(base == cards + costs::kPitchStage.avg, "tracker cost: Octave without Autotune or Crowd pays no tracker");
  check(layoutCost(autotune).avg == cards + costs::kAutotune.avg + costs::kTracker.avg, "tracker cost: Autotune pays it");
  check(layoutCost(crowd).avg == base + costs::kCrowd.avg + costs::kTracker.avg, "tracker cost: Crowd without Autotune pays it");
  check(layoutCost(both).avg == layoutCost(autotune).avg - costs::kUnison.avg + costs::kCrowd.avg,
        "tracker cost: Autotune and Crowd together pay it once");
  check(layoutCost(autotune).worst ==
            layoutCost(noPitch).worst - costs::kPitchStage.worst + costs::kAutotune.worst + costs::kTracker.worst,
        "tracker cost: the worst column counts it too");
}

// Formant in slot 3 in Octave's place pays the tracker without Autotune, and once beside it. Lo-fi and
// Chatter in slot 7 do not pay it.
void checkTrackerCostOtherCards() {
  const Layout noPitch = withoutAutotune();
  Layout autotune = noPitch;
  autotune.card[1] = Card::Autotune;
  const int base = layoutCost(noPitch).avg;
  const int gate = costs::kGate.avg;
  Layout formant = noPitch;
  formant.card[2] = Card::Formant;
  check(layoutCost(formant).avg == base - costs::kOctave.avg + costs::kFormant.avg + costs::kTracker.avg &&
            layoutCost(formant).worst ==
                layoutCost(noPitch).worst - costs::kOctave.worst + costs::kFormant.worst + costs::kTracker.worst,
        "tracker cost: Formant without Autotune pays it, in both columns");
  Layout formantBesideAutotune = autotune;
  formantBesideAutotune.card[2] = Card::Formant;
  check(layoutCost(formantBesideAutotune).avg == layoutCost(autotune).avg - costs::kOctave.avg + costs::kFormant.avg,
        "tracker cost: Formant and Autotune together pay it once");
  Layout lofi = noPitch;
  lofi.card[6] = Card::Lofi;
  Layout bloom = noPitch;
  bloom.card[6] = Card::Bloom;
  check(layoutCost(lofi).avg == base - gate + costs::kLofi.avg && layoutCost(bloom).avg == base - gate + costs::kBloom.avg,
        "tracker cost: Lo-fi and Chatter pay none");
}

// The default layout with Autotune and Octave out (slots 2 and 3 empty), the base of the stage cases.
Layout withoutPitchCards() {
  Layout layout = withoutAutotune();
  layout.card[2] = Card::Empty;
  return layout;
}

// A layout's sum is the fixed term, its cards' rows, the tracker row when `tracker` and the stage row
// when `stage`, in both columns.
void paysFor(const Layout& layout, bool tracker, bool stage, const char* what) {
  costs::Cost want = costs::kFixed;
  for (const Card c : layout.card) want = want + cardDesc(c).cost;
  if (tracker) want = want + costs::kTracker;
  if (stage) want = want + costs::kPitchStage;
  const LayoutCost got = layoutCost(layout);
  check(got.avg == want.avg && got.worst == want.worst, what);
}

// The pitch stage's own work, kPitchStage, counts once in both columns when Octave or Formant is placed
// and Autotune is not, and in no other layout. The default layout and MOB with Chasm place Autotune, so
// their sums stay the sums of their rows, the fixed term and the tracker.
void checkPitchStageCost() {
  const Layout none = withoutPitchCards();
  paysFor(none, false, false, "pitch stage cost: no Octave, Formant or Autotune pays none");

  Layout crowd = none;
  crowd.card[3] = Card::Crowd;
  paysFor(crowd, true, false, "pitch stage cost: Crowd alone pays the tracker and not the stage");

  Layout octave = none;
  octave.card[2] = Card::Octave;
  paysFor(octave, false, true, "pitch stage cost: Octave without Autotune pays it, in both columns");

  Layout formant = none;
  formant.card[2] = Card::Formant;
  paysFor(formant, true, true, "pitch stage cost: Formant without Autotune pays it beside the tracker");

  Layout bothCards = formant;
  bothCards.card[1] = Card::Octave;
  paysFor(bothCards, true, true, "pitch stage cost: Octave and Formant without Autotune pay it once");

  Layout octaveCrowd = octave;
  octaveCrowd.card[3] = Card::Crowd;
  paysFor(octaveCrowd, true, true, "pitch stage cost: Octave and Crowd without Autotune pay it once");

  Layout autotuneOctave = octave;
  autotuneOctave.card[1] = Card::Autotune;
  paysFor(autotuneOctave, true, false, "pitch stage cost: Autotune with Octave pays none");

  Layout autotuneFormant = formant;
  autotuneFormant.card[1] = Card::Autotune;
  paysFor(autotuneFormant, true, false, "pitch stage cost: Autotune with Formant pays none");
}

// The layouts that place Autotune pay no stage term: the default layout and MOB with Chasm. The rows
// and the term cover Octave alone, and the layouts that carry the term are accepted.
void checkPitchStageRows() {
  Layout heavy = defaultLayout();
  heavy.card[3] = Card::Crowd;
  heavy.card[7] = Card::ReverbChasm;
  paysFor(defaultLayout(), true, false, "pitch stage cost: the default layout pays none");
  paysFor(heavy, true, false, "pitch stage cost: MOB with Chasm pays none");

  // Octave alone benches at 2.5 % (15.9k cycles); its row and the stage term cover it, by about 3 points.
  check(costs::kOctave.avg + costs::kPitchStage.avg >= 3 && costs::kOctave.avg + costs::kPitchStage.avg <= 7,
        "pitch stage cost: Octave's row and the term cover the benched 2.5 % for Octave alone");
  Layout octave = withoutPitchCards();
  octave.card[2] = Card::Octave;
  Layout formant = withoutPitchCards();
  formant.card[2] = Card::Formant;
  check(layoutValid(octave).ok() && layoutValid(formant).ok(), "pitch stage cost: the layouts with the term are accepted");
}

// Formant in slot 3 in Octave's place, in the default layout and in MOB with Chasm: each sum is the
// Octave layout's with Formant's row for Octave's. Both layouts keep Autotune, so the tracker is paid
// once either way and Formant pays no stage term.
void checkSlot3Costs() {
  Layout octaveLight = defaultLayout();
  Layout octaveHeavy = octaveLight;
  octaveHeavy.card[3] = Card::Crowd;
  octaveHeavy.card[7] = Card::ReverbChasm;
  Layout formantLight = octaveLight;
  formantLight.card[2] = Card::Formant;
  Layout formantHeavy = octaveHeavy;
  formantHeavy.card[2] = Card::Formant;
  const auto swapped = [](const Layout& octave, const Layout& formant) {
    const LayoutCost o = layoutCost(octave);
    const LayoutCost f = layoutCost(formant);
    return f.avg == o.avg - costs::kOctave.avg + costs::kFormant.avg &&
           f.worst == o.worst - costs::kOctave.worst + costs::kFormant.worst;
  };
  check(swapped(octaveLight, formantLight), "slot 3 cost: Formant in Octave's place in the default layout sums to its rows");
  check(swapped(octaveHeavy, formantHeavy), "slot 3 cost: Formant with MOB and Chasm sums to its rows");
  check(layoutValid(formantLight).ok() && layoutValid(formantHeavy).ok(), "slot 3 cost: Formant is accepted in both layouts");

  // Yawn pays its own row, the tracker and the stage term as Formant does.
  Layout followerLight = formantLight;
  followerLight.card[2] = Card::FormantFollower;
  Layout followerHeavy = formantHeavy;
  followerHeavy.card[2] = Card::FormantFollower;
  const auto followed = [](const Layout& formant, const Layout& follower) {
    const LayoutCost f = layoutCost(formant);
    const LayoutCost w = layoutCost(follower);
    return w.avg == f.avg - costs::kFormant.avg + costs::kFormantFollower.avg &&
           w.worst == f.worst - costs::kFormant.worst + costs::kFormantFollower.worst;
  };
  check(followed(formantLight, followerLight) && followed(formantHeavy, followerHeavy),
        "slot 3 cost: Yawn in Formant's place sums to its rows in both layouts");
  check(layoutValid(followerLight).ok() && layoutValid(followerHeavy).ok(),
        "slot 3 cost: Yawn is accepted in both layouts");
  Layout followerAlone = withoutPitchCards();
  followerAlone.card[2] = Card::FormantFollower;
  paysFor(followerAlone, true, true, "pitch stage cost: Yawn without Autotune pays the tracker and the stage");

  // Formant and Yawn play one voice, so a layout holds one of them at most.
  Layout bothVoices = followerLight;
  bothVoices.card[6] = Card::Formant;
  const LayoutCheck twice = layoutValid(bothVoices);
  check(twice.error == LayoutError::SameCardTwice && twice.slot == 6 && twice.card == Card::Formant,
        "slot 3 cost: Formant beside Yawn is refused as the same card twice");
}

// The sums with each slot 7 card in the default layout and in MOB with Chasm, the heaviest layout the
// three share, from the card rows (fixed term, the seven cards that stay, the slot 7 card, and the
// tracker once).
void checkSlot7Costs() {
  struct Row {
    Card card;
    costs::Cost cost;
    const char* name;
  };
  const Row rows[] = {{Card::Gate, costs::kGate, "Gate"},
                      {Card::Lofi, costs::kLofi, "Lo-fi"},
                      {Card::Bloom, costs::kBloom, "Chatter"}};
  const costs::Cost defaultRest = costs::kFixed + costs::kInputGate + costs::kAutotune + costs::kOctave + costs::kUnison +
                                  costs::kSlapback + costs::kDistortion + costs::kReverbSpring + costs::kTracker;
  const costs::Cost heavyRest = costs::kFixed + costs::kInputGate + costs::kAutotune + costs::kOctave + costs::kCrowd +
                                costs::kSlapback + costs::kDistortion + costs::kReverbChasm + costs::kTracker;
  for (const Row& r : rows) {
    Layout light = defaultLayout();
    light.card[6] = r.card;
    Layout heavy = light;
    heavy.card[3] = Card::Crowd;
    heavy.card[7] = Card::ReverbChasm;
    const LayoutCost lightSum = layoutCost(light);
    const LayoutCost heavySum = layoutCost(heavy);
    const std::string name = std::string("slot 7 cost: ") + r.name;
    check(lightSum.avg == defaultRest.avg + r.cost.avg && lightSum.worst == defaultRest.worst + r.cost.worst,
          (name + " in the default layout sums to its rows").c_str());
    check(heavySum.avg == heavyRest.avg + r.cost.avg && heavySum.worst == heavyRest.worst + r.cost.worst,
          (name + " with MOB and Chasm sums to its rows").c_str());
    check(layoutValid(light).ok() && layoutValid(heavy).ok(), (name + " is accepted in both layouts").c_str());
  }

  // With the stress-run rows no layout the registry allows goes over a line. The heaviest set, Input
  // gate, Formant without Autotune (which pays the pitch stage), Unison, MOB, Distortion, Gate, Chasm and
  // Chatter, sits inside both lines, and layoutValid accepts it.
  const Layout heavyBloom = {{Card::InputGate, Card::Formant, Card::Unison, Card::Crowd, Card::Gate, Card::Distortion,
                              Card::Bloom, Card::ReverbChasm}};
  const LayoutCost heavyCost = layoutCost(heavyBloom);
  check(heavyCost.avg <= costs::kBudgetAvgPercent && heavyCost.worst <= costs::kBudgetWorstPercent &&
            layoutValid(heavyBloom).ok(),
        "slot 7 cost: the heaviest set with MOB, Chasm and Chatter sits inside both lines and layoutValid accepts it");
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
  panel.knob[3] = {0.25f, 7.0f};
  ChainSnapshot snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(!snap.params.crowd.on && !snap.params.pitchFx.crowdOn, "compose: Crowd card off leaves both flags off");
  check(std::fabs(snap.params.crowd.width - 0.25f) < 1e-6f && snap.params.crowd.voicesStep == 7,
        "compose: Crowd knob A is DIVERGE and knob B is the VOICES step");
  panel.toggle[3] = true;
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.crowd.on && snap.params.pitchFx.crowdOn && !snap.params.pitchFx.autotune.on &&
            !snap.params.pitchFx.octave.on,
        "compose: Crowd card on sets its flag and the pitch stage's, and no other pitch toggle");
  check(snap.slotOn[3], "compose: the Crowd card's slot reads on");
}

// Formant in slot 3 reads its knobs and toggle from slot 3's pots.
void checkComposeSlot3Formant() {
  PanelReading panel = panelWithToggles(false);
  Layout layout = defaultLayout();

  layout.card[2] = Card::Formant;
  panel.knob[2] = {4.0f, 0.4f};  // SHIFT position 4 is -3 st
  ChainSnapshot snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(!snap.params.pitchFx.formant.on && snap.params.pitchFx.formant.shiftSt == -3 &&
            std::fabs(snap.params.pitchFx.formant.mix - 0.4f) < 1e-6f,
        "compose: Formant in slot 3 reads SHIFT and MIX from slot 3's pots, off with its toggle");
  panel.toggle[2] = true;
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.pitchFx.formant.on && snap.slotOn[2] && !snap.params.pitchFx.autotune.on &&
            !snap.params.pitchFx.octave.on && !snap.params.pitchFx.crowdOn,
        "compose: Formant's toggle sets formant.on alone, beside Autotune and Octave left off");
  layout.card[2] = Card::Octave;
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.pitchFx.octave.on && !snap.params.pitchFx.formant.on,
        "compose: Octave back in slot 3 sets octave.on and leaves Formant off");
  check(!snap.params.follower.placed, "compose: with Octave in slot 3 no envelope drives SHIFT");

  layout.card[2] = Card::FormantFollower;
  panel.knob[2] = {0.75f, 0.6f};
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.follower.placed && snap.params.pitchFx.formant.on && !snap.params.pitchFx.octave.on &&
            std::fabs(snap.params.follower.sensitivity - 0.75f) < 1e-6f && std::fabs(snap.params.follower.rangeSt - 1.2f) < 1e-5f,
        "compose: Yawn in slot 3 reads SENSITIVITY and RANGE from slot 3's pots and drives the Formant voice");
}

// Each slot 7 card reads its knobs and toggle from slot 7's pots, and the Chatter card's word key
// follows the placement of the Input gate.
void checkComposeSlot7Cards() {
  PanelReading panel = panelWithToggles(false);
  Layout layout = defaultLayout();

  layout.card[6] = Card::Lofi;
  panel.toggle[6] = true;
  panel.knob[6] = {0.25f, 0.75f};
  ChainSnapshot snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.lofi.on && std::fabs(snap.params.lofi.tone - 0.25f) < 1e-6f &&
            std::fabs(snap.params.lofi.amount - 0.75f) < 1e-6f && !snap.params.pitchFx.formant.on,
        "compose: Lo-fi in slot 7 reads TONE and AMOUNT, and Formant's flag is not set by the slot");

  layout.card[6] = Card::Bloom;
  panel.knob[6] = {0.5f, 0.6f};
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.bloom.on && std::fabs(snap.params.bloom.lengthMs - std::sqrt(20.0f * 3000.0f)) < 1e-2f &&
            std::fabs(snap.params.bloom.amount - 0.6f) < 1e-6f && snap.params.bloom.keyFromGate,
        "compose: the Chatter card in slot 7 reads LENGTH and AMOUNT, and keys on the Input gate the default layout places");

  layout.card[0] = Card::Empty;
  snap = composeSnapshot(layout, panel, Settings(), BenchOverrides());
  check(snap.params.bloom.on && !snap.params.bloom.keyFromGate,
        "compose: with no Input gate placed the Chatter card listens to its own input");
}

// composeSnapshotInto builds the snapshot in an object the caller owns, for the firmware's main loop, which
// keeps it off the shared stack. It matches composeSnapshot, and an object that held another layout keeps
// nothing from it.
void checkComposeInto() {
  auto into = std::make_unique<ChainSnapshot>();
  Layout bloom = defaultLayout();
  bloom.card[6] = Card::Bloom;
  composeSnapshotInto(*into, bloom, panelWithToggles(true), Settings(), BenchOverrides());
  const ChainSnapshot want = composeSnapshot(bloom, panelWithToggles(true), Settings(), BenchOverrides());
  check(into->layout == bloom && into->params.bloom.on && into->params.bloom.keyFromGate && into->slotOn == want.slotOn &&
            into->outputGain == want.outputGain && into->params.bloom.lengthMs == want.params.bloom.lengthMs,
        "composeInto: a snapshot built in place equals the one composeSnapshot returns");

  Layout other = defaultLayout();
  other.card[0] = Card::Empty;  // no Input gate, so nothing keys the Chatter card
  const PanelReading off = panelWithToggles(false);
  composeSnapshotInto(*into, other, off, Settings(), BenchOverrides());
  const ChainSnapshot fresh = composeSnapshot(other, off, Settings(), BenchOverrides());
  check(into->layout == other && !into->params.bloom.on && !into->params.bloom.keyFromGate && !into->params.lofi.on &&
            into->slotOn == fresh.slotOn && into->params.gate.on == fresh.params.gate.on,
        "composeInto: a reused object keeps nothing from the layout before");
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

  // The POLISH flags force a stage off against a menu that says on, and leave it on when clear.
  Settings voxOn;
  voxOn.voxEq = VoxEq::On;
  Settings voxCuts;
  voxCuts.voxEq = VoxEq::Cuts;
  snap = composeSnapshot(layout, panelWithToggles(true), Settings(), BenchOverrides());
  check(snap.params.comp.on && !snap.params.eq.on, "the default settings run POLISH COMP and leave VOX EQ off");
  snap = composeSnapshot(layout, panelWithToggles(true), voxOn, BenchOverrides());
  check(snap.params.eq.on && !snap.params.eq.cuts, "VOX EQ ON runs the stage without the cuts");
  snap = composeSnapshot(layout, panelWithToggles(true), voxCuts, BenchOverrides());
  check(snap.params.eq.on && snap.params.eq.cuts, "VOX EQ ON+CUTS runs the stage with the cuts");
  bench = BenchOverrides();
  bench.compOff = true;
  snap = composeSnapshot(layout, panelWithToggles(true), voxCuts, bench);
  check(!snap.params.comp.on && snap.params.eq.on, "compOff forces only POLISH COMP off");
  bench = BenchOverrides();
  bench.eqOff = true;
  snap = composeSnapshot(layout, panelWithToggles(true), voxCuts, bench);
  check(snap.params.comp.on && !snap.params.eq.on, "eqOff forces only VOX EQ off");

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

// The force-on mask turns a slot on against a toggle that reads off, and a force-off bit still wins.
// slots_live and knobs_live hand the panel back.
void checkComposeBenchLevers() {
  const Layout layout = defaultLayout();  // slot index 5 is Distortion, 3 Unison, 2 Octave
  const PanelReading toggleOff = panelWithToggles(false);

  BenchOverrides bench;
  bench.slotsForcedOn = 1u << 5;
  ChainSnapshot snap = composeSnapshot(layout, toggleOff, Settings(), bench);
  check(snap.params.distortion.on && snap.slotOn[5] && !snap.params.unison.on && !snap.slotOn[3],
        "slotsForcedOn bit turns only that slot on against an off toggle");

  bench.slotsForcedOn = 0xFF;
  snap = composeSnapshot(layout, toggleOff, Settings(), bench);
  check(snap.params.distortion.on && snap.params.unison.on && snap.params.reverb.on && snap.params.gate.on &&
            snap.params.pitchFx.autotune.on && snap.params.pitchFx.octave.on,
        "all eight slots forced on turn every stage on");

  bench.slotsOff = 1u << 5;
  snap = composeSnapshot(layout, toggleOff, Settings(), bench);
  check(!snap.params.distortion.on && !snap.slotOn[5] && snap.params.unison.on,
        "a force-off bit wins over the same slot's force-on bit");

  bench.slotsLive();
  check(bench.slotsOff == 0 && bench.slotsForcedOn == 0, "slotsLive clears both masks");
  snap = composeSnapshot(layout, toggleOff, Settings(), bench);
  check(!snap.params.distortion.on && !snap.params.unison.on && !snap.slotOn[5] && !snap.slotOn[3],
        "slotsLive lets an off toggle turn the slot off again");
  snap = composeSnapshot(layout, panelWithToggles(true), Settings(), bench);
  check(snap.params.distortion.on && snap.params.unison.on, "slotsLive lets an on toggle turn the slot on");

  // Octave semitones is stepped (25 steps, slot index 2, knob index 0), Unison depth continuous
  // (slot index 3, knob index 1).
  const PanelReading low;
  bench = BenchOverrides();
  bench.pinKnob(2, 0, 50);
  snap = composeSnapshot(layout, low, Settings(), bench);
  check(snap.params.pitchFx.octave.semitones == 0, "a stepped pin at 50 % lands on the middle step");
  bench.pinKnob(2, 0, 0);
  check(composeSnapshot(layout, low, Settings(), bench).params.pitchFx.octave.semitones == -12,
        "a stepped pin at 0 % lands on the first step");
  bench.pinKnob(2, 0, 100);
  check(composeSnapshot(layout, low, Settings(), bench).params.pitchFx.octave.semitones == 12,
        "a stepped pin at 100 % lands on the last step");
  bench.pinKnob(2, 0, 55);  // 0.55 * 24 = 13.2 rounds to step 13, which is +1 semitone
  check(composeSnapshot(layout, low, Settings(), bench).params.pitchFx.octave.semitones == 1,
        "a stepped pin rounds to the nearest step");

  ChainParams expect;
  bench.pinKnob(3, 1, 25);
  applyKnob(Card::Unison, 1, 0.25f, expect);
  snap = composeSnapshot(layout, low, Settings(), bench);
  check(std::fabs(snap.params.unison.depth - expect.unison.depth) < 1e-6f, "a continuous pin at 25 % sits at 0.25");
  PanelReading mixed;
  mixed.knob[2][1] = 0.4f;  // Octave MIX, the other knob of the pinned Octave semitones
  snap = composeSnapshot(layout, mixed, Settings(), bench);
  check(std::fabs(snap.params.pitchFx.octave.mix - 0.4f) < 1e-6f && snap.params.pitchFx.octave.semitones == 1,
        "a pin on one knob leaves the card's other knob on the panel");

  // A per-knob pin wins over pin-at-top; an unpinned knob still goes to the top.
  bench = BenchOverrides();
  bench.knobsPinned = true;
  bench.pinKnob(2, 0, 50);
  snap = composeSnapshot(layout, low, Settings(), bench);
  ChainParams top;
  applyKnob(Card::Unison, 1, 1.0f, top);
  check(snap.params.pitchFx.octave.semitones == 0 && std::fabs(snap.params.unison.depth - top.unison.depth) < 1e-6f,
        "a per-knob pin wins over pin-at-top and the other knobs stay at the top");

  bench.knobsLive();
  check(!bench.knobsPinned && bench.knobPin[2][0] == kNoPin, "knobsLive clears the pin-at-top and every per-knob pin");
  snap = composeSnapshot(layout, low, Settings(), bench);
  ChainParams zero;
  applyKnob(Card::Unison, 1, 0.0f, zero);
  check(snap.params.pitchFx.octave.semitones == -12 && std::fabs(snap.params.unison.depth - zero.unison.depth) < 1e-6f,
        "knobsLive hands the knobs back to the panel");
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

void checkUnisonCrowdSlot() {
  Layout layout = defaultLayout();
  check(unisonCrowdSlot(layout) == 3, "unisonCrowdSlot: the default layout has Unison in slot 4");
  layout.card[3] = Card::Crowd;
  check(unisonCrowdSlot(layout) == 3, "unisonCrowdSlot: Crowd counts as well");
  layout.card[3] = Card::Empty;
  check(unisonCrowdSlot(layout) == -1, "unisonCrowdSlot: -1 when no slot holds either");
  layout.card[5] = Card::Crowd;
  check(unisonCrowdSlot(layout) == 5, "unisonCrowdSlot: the slot that holds it, wherever it is");
  layout.card[1] = Card::Unison;
  check(unisonCrowdSlot(layout) == 1, "unisonCrowdSlot: the first slot when both cards are placed");
}

void checkSwapUnisonCrowd() {
  Settings s;
  const Settings before = s;

  // The default layout: the outcome follows layoutValid, whatever the Crowd cost row says.
  Layout layout = defaultLayout();
  Layout crowd = layout;
  crowd.card[3] = Card::Crowd;
  const bool crowdFits = layoutValid(crowd).ok();
  check(ui::applyAction(UiAction::SwapUnisonCrowd, s, layout) ==
                (crowdFits ? ui::ApplyResult::LayoutChanged : ui::ApplyResult::None) &&
            layout == (crowdFits ? crowd : defaultLayout()),
        "action: SwapUnisonCrowd on the default layout follows layoutValid");

  // A sparse layout that holds Crowd under any cost row: Unison to Crowd and back.
  Layout sparse = {{Card::Empty, Card::Empty, Card::Empty, Card::Unison, Card::Empty, Card::Empty, Card::Empty,
                    Card::ReverbSpring}};
  Layout sparseCrowd = sparse;
  sparseCrowd.card[3] = Card::Crowd;
  check(layoutValid(sparse).ok() && layoutValid(sparseCrowd).ok(), "action: the sparse layouts pass the placement rules");
  Layout swap = sparse;
  check(ui::applyAction(UiAction::SwapUnisonCrowd, s, swap) == ui::ApplyResult::LayoutChanged && swap == sparseCrowd,
        "action: SwapUnisonCrowd turns Unison into Crowd in its slot and touches no other slot");
  check(ui::applyAction(UiAction::SwapUnisonCrowd, s, swap) == ui::ApplyResult::LayoutChanged && swap == sparse,
        "action: and turns Crowd back into Unison");

  Layout moved = sparse;  // the card in slot 6
  moved.card[3] = Card::Empty;
  moved.card[5] = Card::Unison;
  check(ui::applyAction(UiAction::SwapUnisonCrowd, s, moved) == ui::ApplyResult::LayoutChanged &&
            moved.card[5] == Card::Crowd && moved.card[3] == Card::Empty,
        "action: SwapUnisonCrowd swaps in the slot that holds the card, wherever it is");

  Layout neither = sparse;
  neither.card[3] = Card::Empty;
  const Layout neitherBefore = neither;
  check(ui::applyAction(UiAction::SwapUnisonCrowd, s, neither) == ui::ApplyResult::None && neither == neitherBefore,
        "action: SwapUnisonCrowd changes nothing when no slot holds either card");

  Layout both = sparse;
  both.card[1] = Card::Crowd;
  const Layout bothBefore = both;
  check(layoutValid(both).ok() && ui::applyAction(UiAction::SwapUnisonCrowd, s, both) == ui::ApplyResult::None &&
            both == bothBefore,
        "action: SwapUnisonCrowd changes nothing when the other card would repeat");

  check(s.outputLevel == before.outputLevel && s.inputJack == before.inputJack && s.polishComp == before.polishComp &&
            s.voxEq == before.voxEq && s.topSwitch == before.topSwitch && s.debug == before.debug,
        "action: SwapUnisonCrowd changes no setting");
}

void checkOctaveFormantSlot() {
  Layout layout = defaultLayout();
  check(octaveFormantSlot(layout) == 2, "octaveFormantSlot: the default layout has Octave in slot 3");
  layout.card[2] = Card::Formant;
  check(octaveFormantSlot(layout) == 2, "octaveFormantSlot: Formant counts as well");
  layout.card[2] = Card::Empty;
  check(octaveFormantSlot(layout) == -1, "octaveFormantSlot: -1 when no slot holds either");
  layout.card[5] = Card::Formant;
  check(octaveFormantSlot(layout) == 5, "octaveFormantSlot: the slot that holds it, wherever it is");
  layout.card[1] = Card::Octave;
  check(octaveFormantSlot(layout) == 1, "octaveFormantSlot: the first slot when both are placed");
  layout.card[0] = Card::FormantFollower;
  check(octaveFormantSlot(layout) == 0, "octaveFormantSlot: Yawn counts as well");
}

void checkSwapOctaveFormant() {
  Settings s;
  const Settings before = s;

  // The default layout has room for each card, so every step of the cycle is taken.
  Layout layout = defaultLayout();
  Layout formant = layout;
  formant.card[2] = Card::Formant;
  Layout follower = layout;
  follower.card[2] = Card::FormantFollower;
  check(layoutValid(formant).ok() && layoutValid(follower).ok(),
        "action: the default layout with Formant or Yawn in slot 3 passes the placement rules");
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, layout) == ui::ApplyResult::LayoutChanged && layout == formant,
        "action: SwapOctaveFormant turns Octave into Formant in its slot and touches no other slot");
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, layout) == ui::ApplyResult::LayoutChanged && layout == follower,
        "action: then Formant into Yawn");
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, layout) == ui::ApplyResult::LayoutChanged && layout == defaultLayout(),
        "action: and Yawn back into Octave");

  Layout moved = defaultLayout();  // Octave in slot 6, with Empty in slot 3
  moved.card[2] = Card::Empty;
  moved.card[5] = Card::Octave;
  Layout movedFormant = moved;
  movedFormant.card[5] = Card::Formant;
  check(layoutValid(moved).ok() && ui::applyAction(UiAction::SwapOctaveFormant, s, moved) == ui::ApplyResult::LayoutChanged &&
            moved == movedFormant,
        "action: SwapOctaveFormant swaps in the slot that holds the card, wherever it is");

  Layout neither = defaultLayout();
  neither.card[2] = Card::Empty;
  const Layout neitherBefore = neither;
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, neither) == ui::ApplyResult::None && neither == neitherBefore,
        "action: SwapOctaveFormant changes nothing when no slot holds either card");

  // Formant in slot 2 and Octave in slot 3: the finder names slot 2, and the cycle passes over Octave,
  // which would repeat.
  Layout both = defaultLayout();
  both.card[1] = Card::Formant;
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, both) == ui::ApplyResult::LayoutChanged &&
            both.card[1] == Card::FormantFollower && both.card[2] == Card::Octave,
        "action: SwapOctaveFormant turns Formant into Yawn beside Octave");
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, both) == ui::ApplyResult::LayoutChanged &&
            both.card[1] == Card::Formant && both.card[2] == Card::Octave,
        "action: and passes over Octave, which would repeat, back to Formant");

  // Octave in slot 2 and Formant in slot 3: Formant would repeat, and Yawn plays Formant's voice.
  Layout repeat = defaultLayout();
  repeat.card[1] = Card::Octave;
  repeat.card[2] = Card::Formant;
  const Layout repeatBefore = repeat;
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, repeat) == ui::ApplyResult::None && repeat == repeatBefore,
        "action: SwapOctaveFormant changes nothing when each other card would repeat");

  // A layout the box would not load stays as it is. No layout goes over a CPU line with the stress-run
  // rows, so this one holds two reverb cards, which the swap cannot cure.
  Layout unloadable = {{Card::InputGate, Card::ReverbSpring, Card::Formant, Card::Crowd, Card::Slapback, Card::Distortion,
                        Card::Unison, Card::ReverbChasm}};
  Layout withOctave = unloadable;
  withOctave.card[2] = Card::Octave;
  check(!layoutValid(unloadable).ok() && !layoutValid(withOctave).ok(),
        "action: the layout with two reverb cards would not load with Formant or with Octave");
  const Layout unloadableBefore = unloadable;
  check(ui::applyAction(UiAction::SwapOctaveFormant, s, unloadable) == ui::ApplyResult::None && unloadable == unloadableBefore,
        "action: SwapOctaveFormant leaves a layout the box would not load as it is");

  check(s.outputLevel == before.outputLevel && s.inputJack == before.inputJack && s.polishComp == before.polishComp &&
            s.voxEq == before.voxEq && s.topSwitch == before.topSwitch && s.debug == before.debug,
        "action: SwapOctaveFormant changes no setting");
}

// Growl is Distortion's alternate in slot 6. The layout takes it, pays the tracker once for it (or once
// with Autotune), and the slot 6 action swaps the two cards in the slot that holds either.
void checkGrowlSlot6() {
  Settings s;
  Layout layout = defaultLayout();
  check(distortionGrowlSlot(layout) == 5, "distortionGrowlSlot: the default layout has Distortion in slot 6");
  Layout growl = layout;
  growl.card[5] = Card::Growl;
  check(distortionGrowlSlot(growl) == 5, "distortionGrowlSlot: Growl counts as well");
  check(layoutValid(growl).ok(), "growl: the default layout with Growl in slot 6 passes the placement rules");
  paysFor(growl, true, false, "growl: Growl in slot 6 adds its row, and the tracker stays paid once");

  Layout alone = withoutPitchCards();
  alone.card[5] = Card::Growl;
  check(layoutValid(alone).ok(), "growl: a layout with Growl and no other pitch card passes the placement rules");
  paysFor(alone, true, false, "growl: Growl alone pays the tracker once and no pitch stage row");

  check(ui::applyAction(UiAction::SwapDistortionGrowl, s, layout) == ui::ApplyResult::LayoutChanged && layout == growl,
        "action: SwapDistortionGrowl turns Distortion into Growl in its slot and touches no other slot");
  check(ui::applyAction(UiAction::SwapDistortionGrowl, s, layout) == ui::ApplyResult::LayoutChanged && layout == defaultLayout(),
        "action: and turns Growl back into Distortion");
  Layout neither = defaultLayout();
  neither.card[5] = Card::Empty;
  const Layout neitherBefore = neither;
  check(ui::applyAction(UiAction::SwapDistortionGrowl, s, neither) == ui::ApplyResult::None && neither == neitherBefore,
        "action: SwapDistortionGrowl changes nothing when no slot holds either card");

  PanelReading panel = panelWithToggles(false);
  panel.knob[5] = {0.25f, 0.75f};
  panel.toggle[5] = true;
  const ChainSnapshot snap = composeSnapshot(growl, panel, Settings(), BenchOverrides());
  check(snap.params.growl.on && snap.params.pitchFx.growlOn && !snap.params.distortion.on &&
            std::fabs(snap.params.growl.growl - 0.25f * 0.80f) < 1e-6f &&
            std::fabs(snap.params.growl.drive - 0.25f * 0.65f) < 1e-6f && std::fabs(snap.params.growl.mix - 0.75f) < 1e-6f,
        "compose: Growl on in slot 6 sets its flag and the pitch stage's, GROWL from the upper pot and MIX from the lower");
}

void checkSlot7CardSlot() {
  Layout layout = defaultLayout();
  check(slot7CardSlot(layout) == 6, "slot7CardSlot: the default layout has Gate in slot 7");
  for (const Card c : {Card::Lofi, Card::Bloom}) {
    layout.card[6] = c;
    check(slot7CardSlot(layout) == 6, "slot7CardSlot: Lo-fi and Chatter count as well");
  }
  layout.card[6] = Card::Formant;
  check(slot7CardSlot(layout) == -1, "slot7CardSlot: Formant does not count");
  layout.card[6] = Card::Empty;
  check(slot7CardSlot(layout) == -1, "slot7CardSlot: -1 when no slot holds one of the three");
  layout.card[4] = Card::Bloom;
  check(slot7CardSlot(layout) == 4, "slot7CardSlot: the slot that holds it, wherever it is");
  layout.card[1] = Card::Lofi;
  check(slot7CardSlot(layout) == 1, "slot7CardSlot: the first slot when several are placed");
}

// The slot 7 action walks Gate, Lo-fi, Chatter and wraps, in the slot that holds the card and no other,
// and passes over a card layoutValid refuses.
void checkCycleSlot7Card() {
  Settings s;
  const Settings before = s;
  Layout layout = defaultLayout();
  bool others = true;
  for (const Card want : {Card::Lofi, Card::Bloom, Card::Gate, Card::Lofi}) {
    const Layout last = layout;
    const bool changed = ui::applyAction(UiAction::CycleSlot7Card, s, layout) == ui::ApplyResult::LayoutChanged;
    for (int slot = 0; slot < kSlotCount; ++slot) others = others && (slot == 6 || layout.card[slot] == last.card[slot]);
    check(changed && layout.card[6] == want, "action: CycleSlot7Card moves slot 7 on round Gate, Lo-fi, Chatter and back");
  }
  check(others && s.outputLevel == before.outputLevel && s.inputJack == before.inputJack &&
            s.polishComp == before.polishComp && s.voxEq == before.voxEq && s.topSwitch == before.topSwitch &&
            s.debug == before.debug,
        "action: CycleSlot7Card touches no other slot and no setting");

  Layout moved = defaultLayout();  // the card in slot 3, Octave's, with Gate gone from slot 7
  moved.card[2] = Card::Lofi;
  moved.card[6] = Card::Empty;
  check(ui::applyAction(UiAction::CycleSlot7Card, s, moved) == ui::ApplyResult::LayoutChanged &&
            moved.card[2] == Card::Bloom && moved.card[6] == Card::Empty,
        "action: CycleSlot7Card cycles in the slot that holds the card, wherever it is");

  Layout none = defaultLayout();
  none.card[6] = Card::Empty;
  const Layout noneBefore = none;
  check(ui::applyAction(UiAction::CycleSlot7Card, s, none) == ui::ApplyResult::None && none == noneBefore,
        "action: CycleSlot7Card changes nothing when no slot holds one of the three");

  // All three placed: every other card would repeat, so both candidates are refused.
  Layout all = defaultLayout();
  all.card[1] = Card::Lofi;
  all.card[2] = Card::Bloom;
  const Layout allBefore = all;
  check(ui::applyAction(UiAction::CycleSlot7Card, s, all) == ui::ApplyResult::None && all == allBefore,
        "action: CycleSlot7Card changes nothing when each other card would repeat");

  // Chatter already sits in slot 8, so the layout refuses it in slot 7 and the cycle passes it over. No
  // layout goes over a CPU line with the stress-run rows, so a repeat is the refusal that remains. The
  // expected cards follow layoutValid, not a fixed verdict.
  Layout tight = {{Card::ReverbSpring, Card::InputGate, Card::Unison, Card::Crowd, Card::Slapback, Card::Distortion,
                   Card::Gate, Card::Bloom}};
  Layout withBloom = tight;
  withBloom.card[6] = Card::Bloom;
  check(layoutValid(tight).ok() && !layoutValid(withBloom).ok(),
        "action: the layout fits with Gate in slot 7 and refuses a second Chatter");
  Layout atLofi = tight;
  atLofi.card[6] = Card::Lofi;
  check(ui::applyAction(UiAction::CycleSlot7Card, s, atLofi) == ui::ApplyResult::LayoutChanged && atLofi == tight,
        "action: CycleSlot7Card passes over Chatter and wraps from Lo-fi to Gate");
  check(ui::applyAction(UiAction::CycleSlot7Card, s, atLofi) == ui::ApplyResult::LayoutChanged &&
            atLofi.card[6] == Card::Lofi,
        "action: and goes on from Gate to Lo-fi");
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
  checkTrackerCostOtherCards();
  checkPitchStageCost();
  checkPitchStageRows();
  checkSlot3Costs();
  checkSlot7Costs();
  checkLayoutReasons();
  checkLayoutName();
  checkApplyAction();
  checkReverbSlot();
  checkUnisonCrowdSlot();
  checkSwapUnisonCrowd();
  checkOctaveFormantSlot();
  checkSwapOctaveFormant();
  checkGrowlSlot6();
  checkSlot7CardSlot();
  checkCycleSlot7Card();
  checkComposeToggles();
  checkComposeCrowd();
  checkComposeSlot3Formant();
  checkComposeSlot7Cards();
  checkComposeInto();
  checkComposeBench();
  checkComposeBenchLevers();
  checkOutputGain();
}
