#include "core/layout.h"

Layout defaultLayout() {
  return {{Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
           Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring}};
}

bool layoutValid(const Layout& layout) {
  int avg = costs::kFixed.avg;
  int worst = costs::kFixed.worst;
  int reverbs = 0;
  for (const Card c : layout.card) {
    if (static_cast<int>(c) >= kCardCount) return false;
    const CardDesc& d = cardDesc(c);
    avg += d.cost.avg;
    worst += d.cost.worst;
    if (d.group == Group::Reverb) ++reverbs;
  }
  return reverbs <= 1 && avg <= costs::kBudgetAvgPercent && worst <= costs::kBudgetWorstPercent;
}
