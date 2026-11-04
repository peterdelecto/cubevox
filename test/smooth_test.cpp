// smooth::step lands on its target bit for bit, never overshoots, and then holds. Engines compare
// settled smoothers against their targets (a settled-off stage is a plain copy), so a smoother that
// stalls a few ulps short keeps a stage running forever. The closed form lands on the loop's block,
// the 20 ms literals match libm, and the clamp maps NaN to its lower bound. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <utility>

#include "engine/smooth.h"

namespace {

struct Case {
  float from;
  float to;
};

constexpr Case kCases[] = {
    {0.0f, 1.0f}, {1.0f, 0.0f}, {0.3f, -0.7f}, {1e-3f, 0.0f}, {0.0f, 1e-7f}, {-80.0f, 0.0f}, {0.0f, -80.0f},
};

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// Steps until x equals the target, at most `limit` steps. Returns the step count, or -1 when x
// overshoots, never lands, or moves again after landing.
long landing(Case c, float a, long limit) {
  float x = c.from;
  const bool rising = c.to > c.from;
  for (long n = 1; n <= limit; ++n) {
    x = cv::smooth::step(x, c.to, a);
    if (rising ? x > c.to : x < c.to) return -1;
    if (x != c.to) continue;
    for (int k = 0; k < 1000; ++k) {
      if (cv::smooth::step(x, c.to, a) != c.to) return -1;
    }
    return n;
  }
  return -1;
}

bool testLanding() {
  bool ok = true;
  for (float sec : {0.001f, cv::smooth::kSmoothSec, 0.5f}) {
    const float a = cv::smooth::coef(sec);
    // Twice the samples a one-pole needs to close 100 dB onto kSnap.
    const long limit = static_cast<long>(2.0 * std::log(100.0 / cv::smooth::kSnap) / a) + 10;
    long worst = 0;
    bool landed = true;
    for (const Case& c : kCases) {
      const long n = landing(c, a, limit);
      landed &= n > 0;
      if (n > worst) worst = n;
    }
    char name[48];
    std::snprintf(name, sizeof name, "smooth %.0f ms", static_cast<double>(sec * 1000.0f));
    ok &= report(name, landed, "every case lands exactly and holds, slowest after %.0f steps (<= %.0f)",
                 static_cast<double>(worst), static_cast<double>(limit));
  }
  return ok;
}

// The 20 ms literals sit within a few ulps of what libm gives, and the three forms agree.
bool testConstants() {
  using namespace cv::smooth;
  const double aRel = std::fabs(static_cast<double>(kSmoothCoef) / coef(kSmoothSec) - 1.0);
  const double kRel = std::fabs(static_cast<double>(kSmoothKeepBlock) / keep(kSmoothSec, cv::kBlock) - 1.0);
  const bool forms = blockCoef(kSmoothSec, 1) == coef(kSmoothSec) &&
                     blockCoef(kSmoothSec, cv::kBlock) == 1.0f - keep(kSmoothSec, cv::kBlock);
  const bool instant = coef(0.0f) == 1.0f && coef(-1.0f) == 1.0f && keep(0.0f, cv::kBlock) == 0.0f &&
                       blockCoef(0.0f, cv::kBlock) == 1.0f;
  // 1 - exp(-1/960) rounds near 1, so a libm one ulp off moves the coefficient by 6e-5 relative.
  bool ok = report("smooth constants", aRel < 1e-4 && kRel < 2e-7,
                   "kSmoothCoef off libm by %.2g (< 1e-4), kSmoothKeepBlock by %.2g (< 2e-7)", aRel, kRel);
  ok &= report("smooth forms", forms && instant, "coef, blockCoef and keep agree and 0 s is instant=%.0f",
               (forms && instant) ? 1.0 : 0.0);
  return ok;
}

// advance(..., 1) is step, a smoother on its target returns the target itself, and the pair form equals
// two single advances whether either, both or neither sits on its target.
bool testAdvance() {
  using namespace cv::smooth;
  const float a = kSmoothCoef;
  bool ok = true;
  for (const Case& c : kCases) ok &= advance(c.from, c.to, a, 1) == step(c.from, c.to, a);
  ok &= advance(0.0f, -0.0f, a, 64) == 0.0f && std::signbit(advance(0.0f, -0.0f, a, 64));
  ok &= advance(0.5f, 0.5f, a, 0) == 0.5f;
  for (const Case& p : kCases) {
    for (const Case& q : kCases) {
      for (int settle = 0; settle < 4; ++settle) {
        float x = p.from, y = q.from;
        if (settle & 1) x = p.to;
        if (settle & 2) y = q.to;
        float wantX = advance(x, p.to, a, 64), wantY = advance(y, q.to, a, 64);
        advance(x, p.to, y, q.to, a, 64);
        ok &= x == wantX && y == wantY;
      }
    }
  }
  return report("smooth advance", ok, "one step is step(), a settled smoother returns its target, pairs match singles=%.0f",
                ok ? 1.0 : 0.0);
}

// advanceBy is advance() over n steps in closed form. It agrees to within a millionth of the distance
// moved, the step's snap, and the distance at which a step stops moving the float. It never
// overshoots, and it lands on the block where the loop lands, give or take one.
bool testClosedForm() {
  using namespace cv::smooth;
  bool ok = true;
  double worstDiff = 0.0;
  int worstGap = 0;
  for (const Case& c : kCases) {
    float closed = c.from, looped = c.from;
    const bool rising = c.to > c.from;
    int closedAt = -1, loopedAt = -1;
    for (int b = 0; b < 4000 && (closedAt < 0 || loopedAt < 0); ++b) {
      closed = advanceBlock(closed, c.to);
      looped = advance(looped, c.to, kSmoothCoef, cv::kBlock);
      if (rising ? closed > c.to : closed < c.to) ok = false;
      const double diff = std::fabs(static_cast<double>(closed) - looped);
      const double stall = std::fmax(std::fabs(c.from), std::fabs(c.to)) * 1.2e-7 / kSmoothCoef;
      const double allowed = std::fabs(static_cast<double>(c.to) - c.from) * 1e-6 + 2.0 * kSnap + stall;
      if (diff > allowed) ok = false;
      if (diff / allowed > worstDiff) worstDiff = diff / allowed;
      if (closedAt < 0 && closed == c.to) closedAt = b;
      if (loopedAt < 0 && looped == c.to) loopedAt = b;
    }
    ok &= closedAt >= 0 && loopedAt >= 0 && advanceBlock(c.to, c.to) == c.to;
    if (closedAt >= 0 && loopedAt >= 0) worstGap = std::max(worstGap, std::abs(closedAt - loopedAt));
  }
  ok &= report("smooth block", ok && worstGap <= 1,
               "advanceBlock follows advance over kBlock steps (worst %.2f of the allowance) and lands within %.0f block",
               worstDiff, static_cast<double>(worstGap));
  // Any n and any time constant: 17 steps of a 500 ms smoother, one step of 1 ms and 480 steps of 20 ms.
  // The reference is the exact exponential. The loop sits off it by the rounding of coef() itself, which
  // is 1 - exp(x) near 1.
  double worstExact = 0.0;
  for (const auto& nSec : {std::pair<int, float>{17, 0.5f}, {1, 0.001f}, {480, 0.02f}}) {
    const float a = coef(nSec.second);
    const float decay = keep(nSec.second, nSec.first);
    const double exact = 1.0 - std::exp(-static_cast<double>(nSec.first) / (static_cast<double>(nSec.second) * cv::kSampleRate));
    worstExact = std::fmax(worstExact, std::fabs(static_cast<double>(advanceBy(0.0f, 1.0f, a, decay)) - exact));
  }
  return report("smooth advanceBy", worstExact < 5e-8, "closed form sits %.2g from the exact exponential for other n and time constants (< 5e-8)",
                worstExact) &&
         ok;
}

bool testClamp() {
  using namespace cv::smooth;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const bool ok = clamp01(nan) == 0.0f && clamp01(-inf) == 0.0f && clamp01(inf) == 1.0f && clamp01(-0.25f) == 0.0f &&
                  clamp01(1.5f) == 1.0f && clamp01(0.25f) == 0.25f && clamp01(-0.0f) == 0.0f;
  return report("smooth clamp01", ok, "limits to [0, 1] and NaN reads as 0=%.0f", ok ? 1.0 : 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testLanding();
  ok &= testConstants();
  ok &= testAdvance();
  ok &= testClosedForm();
  ok &= testClamp();
  std::printf("%s smooth\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
