// smooth::step lands on its target bit for bit, never overshoots, and then holds. Engines compare
// settled smoothers against their targets (a settled-off stage is a plain copy), so a smoother that
// stalls a few ulps short keeps a stage running forever. No window, no audio device.

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "engine/smooth.h"

namespace {

struct Case {
  float from;
  float to;
};

constexpr Case kCases[] = {
    {0.0f, 1.0f}, {1.0f, 0.0f}, {0.3f, -0.7f}, {1e-3f, 0.0f}, {0.0f, 1e-7f}, {-80.0f, 0.0f}, {0.0f, -80.0f},
};

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

}  // namespace

int main() {
  bool ok = true;
  for (float sec : {0.001f, cv::smooth::kSmoothSec, 0.5f}) {
    const float a = sec == cv::smooth::kSmoothSec ? cv::smooth::kSmoothCoef : cv::smooth::coef(sec);
    // Twice the samples a one-pole needs to close 100 dB onto kSnap.
    const long limit = static_cast<long>(2.0 * std::log(100.0 / cv::smooth::kSnap) / a) + 10;
    long worst = 0;
    bool landed = true;
    for (const Case& c : kCases) {
      const long n = landing(c, a, limit);
      landed &= n > 0;
      if (n > worst) worst = n;
    }
    std::printf("%s smooth %.0f ms: every case lands exactly and holds, slowest after %ld steps (<= %ld)\n",
                landed ? "PASS" : "FAIL", static_cast<double>(sec * 1000.0f), worst, limit);
    ok &= landed;
  }
  // advanceBlock is advance() over kBlock steps in closed form. It agrees to within a millionth of the
  // distance moved, the step's snap, and the distance at which a step stops moving the float (the
  // loop snaps there; the closed form carries on to the snap). It never overshoots, and it lands
  // on the target and holds.
  bool block = true;
  double worstDiff = 0.0;
  for (const Case& c : kCases) {
    float closed = c.from, looped = c.from;
    const bool rising = c.to > c.from;
    bool landed = false;
    for (int b = 0; b < 4000 && !landed; ++b) {
      closed = cv::smooth::advanceBlock(closed, c.to);
      looped = cv::smooth::advance(looped, c.to, cv::smooth::kSmoothCoef, cv::kBlock);
      if (rising ? closed > c.to : closed < c.to) block = false;
      const double diff = std::fabs(static_cast<double>(closed) - looped);
      const double stall = std::fmax(std::fabs(c.from), std::fabs(c.to)) * 1.2e-7 / cv::smooth::kSmoothCoef;
      const double allowed = std::fabs(static_cast<double>(c.to) - c.from) * 1e-6 + 2.0 * cv::smooth::kSnap + stall;
      if (diff > allowed) block = false;
      if (diff / allowed > worstDiff) worstDiff = diff / allowed;
      landed = closed == c.to;
    }
    block &= landed && cv::smooth::advanceBlock(c.to, c.to) == c.to;
  }
  std::printf("%s smooth block: advanceBlock follows advance over kBlock steps (worst %.2f of the allowance), lands exactly and holds\n",
              block ? "PASS" : "FAIL", worstDiff);
  ok &= block;
  std::printf("%s smooth\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
