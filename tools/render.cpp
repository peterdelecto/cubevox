// cubevox-render: loop in, harmony + unison processed mono 48 kHz f32 WAV out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../third_party/miniaudio.h"
#include "engine/harmony.h"
#include "engine/unison.h"

namespace {

struct TuningField {
  const char* name;
  float* value;
};

struct RenderParams {
  bool harmonyOn = false;
  bool unisonOn = false;
  cv::HarmonyParams harmony;
  cv::UnisonParams unison;
};

int usage() {
  std::fprintf(stderr,
               "usage: cubevox-render <in> <out.wav> [--depth <0..1>] [--on 0|1] "
               "[--tuning k=v ...]\n"
               "       [--harmony] [--key <0..11>] [--mix <0..1>] "
               "[--voice lower|low|fixed|high|higher=<0..3> ...]\n"
               "  unison runs only with --on 1 or --depth; at least one stage must run\n"
               "  k: baseDelayMs0 baseDelayMs1 lfoHz0 lfoHz1 swingMinMs swingMaxMs "
               "wetMaxDb detuneCents0 detuneCents1 windowMs\n");
  return 2;
}

bool parseFloat(const char* s, float* out) {
  char* end = nullptr;
  *out = std::strtof(s, &end);
  return end != s && *end == '\0';
}

bool applyTuning(cv::UnisonTuning& t, const char* kv) {
  const TuningField fields[] = {
      {"baseDelayMs0", &t.baseDelayMs[0]}, {"baseDelayMs1", &t.baseDelayMs[1]},
      {"lfoHz0", &t.lfoHz[0]},             {"lfoHz1", &t.lfoHz[1]},
      {"swingMinMs", &t.swingMinMs},       {"swingMaxMs", &t.swingMaxMs},
      {"wetMaxDb", &t.wetMaxDb},
      {"detuneCents0", &t.detuneCents[0]}, {"detuneCents1", &t.detuneCents[1]},
      {"windowMs", &t.windowMs},
  };
  const char* eq = std::strchr(kv, '=');
  if (!eq) return false;
  const size_t keyLen = static_cast<size_t>(eq - kv);
  for (const TuningField& f : fields) {
    if (std::strlen(f.name) == keyLen && std::strncmp(f.name, kv, keyLen) == 0)
      return parseFloat(eq + 1, f.value);
  }
  return false;
}

bool parseVoice(const char* kv, cv::HarmonySlot& slot) {
  const struct {
    const char* name;
    cv::HarmonyVoice voice;
  } names[] = {
      {"lower", cv::HarmonyVoice::Lower}, {"low", cv::HarmonyVoice::Low},
      {"fixed", cv::HarmonyVoice::Fixed}, {"high", cv::HarmonyVoice::High},
      {"higher", cv::HarmonyVoice::Higher},
  };
  const char* eq = std::strchr(kv, '=');
  if (!eq || eq[1] < '0' || eq[1] > '3' || eq[2] != '\0') return false;
  const size_t keyLen = static_cast<size_t>(eq - kv);
  for (const auto& n : names) {
    if (std::strlen(n.name) == keyLen && std::strncmp(n.name, kv, keyLen) == 0) {
      slot.voice = n.voice;
      slot.level = eq[1] - '0';
      return true;
    }
  }
  return false;
}

bool parseArgs(int argc, char** argv, const char** in, const char** out,
               RenderParams& rp) {
  cv::UnisonParams& p = rp.unison;
  bool haveDepth = false;
  bool haveOn = false;
  int voices = 0;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--depth") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &p.depth) || p.depth < 0.0f || p.depth > 1.0f)
        return false;
      haveDepth = true;
    } else if (std::strcmp(a, "--on") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0) return false;
      p.on = v[0] == '1';
      haveOn = true;
    } else if (std::strcmp(a, "--harmony") == 0) {
      rp.harmonyOn = true;
    } else if (std::strcmp(a, "--key") == 0 && i + 1 < argc) {
      float k = 0.0f;
      if (!parseFloat(argv[++i], &k) || k < 0.0f || k > 11.0f || k != std::floor(k))
        return false;
      rp.harmony.key = static_cast<int>(k);
    } else if (std::strcmp(a, "--mix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.harmony.mix) || rp.harmony.mix < 0.0f ||
          rp.harmony.mix > 1.0f)
        return false;
    } else if (std::strcmp(a, "--voice") == 0 && i + 1 < argc) {
      if (voices >= 2 || !parseVoice(argv[++i], rp.harmony.slots[voices])) return false;
      ++voices;
    } else if (std::strcmp(a, "--tuning") == 0) {
      int taken = 0;
      while (i + 1 < argc && argv[i + 1][0] != '-' && std::strchr(argv[i + 1], '=')) {
        if (!applyTuning(p.tuning, argv[++i])) return false;
        ++taken;
      }
      if (taken == 0) return false;
    } else if (a[0] != '-' && positional < 2) {
      (positional++ == 0 ? *in : *out) = a;
    } else {
      return false;
    }
  }
  if (positional != 2) return false;
  if (voices > 0 && !rp.harmonyOn) return false;
  // Unison runs only when asked for; --on 0 with --depth keeps it off.
  rp.unisonOn = haveOn ? p.on : haveDepth;
  p.on = rp.unisonOn;
  return rp.harmonyOn || rp.unisonOn;
}

int render(const char* inPath, const char* outPath, const RenderParams& rp) {
  ma_decoder dec;
  const ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 1, cv::kSampleRate);
  if (ma_decoder_init_file(inPath, &dcfg, &dec) != MA_SUCCESS) {
    std::fprintf(stderr, "cubevox-render: cannot decode '%s'\n", inPath);
    return 1;
  }

  ma_encoder enc;
  const ma_encoder_config ecfg =
      ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, cv::kSampleRate);
  if (ma_encoder_init_file(outPath, &ecfg, &enc) != MA_SUCCESS) {
    std::fprintf(stderr, "cubevox-render: cannot write '%s'\n", outPath);
    ma_decoder_uninit(&dec);
    return 1;
  }

  static cv::Harmony harmony;
  static cv::Unison unison;
  harmony.reset();
  unison.reset();

  float inBuf[cv::kBlock];
  float midBuf[cv::kBlock];
  float outBuf[cv::kBlock];
  int rc = 0;
  for (;;) {
    ma_uint64 got = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&dec, inBuf, cv::kBlock, &got);
    if (got > 0) {
      const int n = static_cast<int>(got);
      const float* src = inBuf;
      if (rp.harmonyOn) {
        harmony.process(src, midBuf, n, rp.harmony);
        src = midBuf;
      }
      if (rp.unisonOn) {
        unison.process(src, outBuf, n, rp.unison);
      } else {
        std::memcpy(outBuf, src, sizeof(float) * static_cast<size_t>(n));
      }
      ma_uint64 wrote = 0;
      if (ma_encoder_write_pcm_frames(&enc, outBuf, got, &wrote) != MA_SUCCESS ||
          wrote != got) {
        std::fprintf(stderr, "cubevox-render: write failed on '%s'\n", outPath);
        rc = 1;
        break;
      }
    }
    if (r == MA_AT_END || got == 0) break;
    if (r != MA_SUCCESS) {
      std::fprintf(stderr, "cubevox-render: decode error in '%s'\n", inPath);
      rc = 1;
      break;
    }
  }

  ma_encoder_uninit(&enc);
  ma_decoder_uninit(&dec);
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const char* in = nullptr;
  const char* out = nullptr;
  RenderParams rp;
  if (!parseArgs(argc, argv, &in, &out, rp)) return usage();
  return render(in, out, rp);
}
